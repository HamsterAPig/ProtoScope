#include "record_query.hpp"
#include "sqlite_database.hpp"
#include "query_functions.hpp"
#include "record_export.hpp"
#include "protoscope/data/table.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <queue>

namespace protoscope::storage {
namespace {
constexpr data::ValueLimits valueLimits{32U*1024U*1024U,16,131072};
void checkStop(std::stop_token stop)
{
    if (stop.stop_requested()) throw std::runtime_error("record query canceled");
}
std::size_t schemaBytes(const data::Schema& schema)
{
    std::size_t bytes=sizeof(schema)+schema.dataset.capacity()+schema.fields.capacity()*sizeof(data::Field);
    for (const auto& field:schema.fields) bytes+=field.name.capacity();
    return bytes;
}
struct Cursor {
    const RecordSource& source;
    sqlite::Database db;
    std::unique_ptr<sqlite::Statement> statement;
    std::stop_token stop;
    std::int64_t id{0},receivedUs{0};
    std::uint64_t localSchema{0};
    int sortType{-1};
    data::Value sortValue;
    bool valid{false};

    Cursor(const RecordSource& input,const Query& query,std::stop_token token)
        :source(input),db(input.path,true),stop(token)
    {
        registerQueryFunctions(db.get());
        sqlite3_progress_handler(db.get(),1000,[](void* value) {
            return static_cast<std::stop_token*>(value)->stop_requested() ? 1:0;
        },&stop);
        db.exec("PRAGMA cache_size=-64; BEGIN");
        const bool fields=query.sort || !query.conditions.empty();
        std::string sql="SELECT r.id,r.received_us,r.schema_id";
        if (query.sort) sql+=",ps_field_type(r.payload,s.definition,?),ps_field(r.payload,s.definition,?)";
        else sql+=",NULL,NULL";
        sql+=" FROM records r ";
        if (fields) sql+="JOIN schemas s ON s.id=r.schema_id ";
        sql+="WHERE r.id<=? AND (?='' OR r.dataset=?) AND (?=0 OR r.device=?) "
             "AND (?=0 OR r.received_us>=?) AND (?=0 OR r.received_us<=?) ";
        if (!query.conditions.empty()) sql+="AND ps_matches(r.payload,s.definition,?) ";
        sql+="ORDER BY ";
        if (query.sort) sql+=query.sort->descending ? "4 DESC,5 DESC,":"4 ASC,5 ASC,";
        sql+="r.received_us,r.id";
        statement=std::make_unique<sqlite::Statement>(db,sql.c_str());
        int parameter=1;
        if (query.sort) {statement->text(parameter++,query.sort->field);statement->text(parameter++,query.sort->field);}
        statement->integer(parameter++,input.cutoff);
        statement->text(parameter++,query.dataset);statement->text(parameter++,query.dataset);
        statement->integer(parameter++,query.device.has_value());statement->text(parameter++,query.device.value_or(""));
        statement->integer(parameter++,query.fromUs.has_value());statement->integer(parameter++,query.fromUs.value_or(0));
        statement->integer(parameter++,query.toUs.has_value());statement->integer(parameter++,query.toUs.value_or(0));
        if (!query.conditions.empty())
            statement->blob(parameter++,data::encodeValue(data::conditionsValue(query.conditions),{128U*1024U,4}));
    }
    std::uint64_t globalId() const
    {
        if (id<=0 || source.idBase<0 || id>INT64_MAX-source.idBase)
            throw std::runtime_error("invalid global record ID");
        return static_cast<std::uint64_t>(source.idBase+id);
    }
    void advance()
    {
        checkStop(stop);
        valid=statement->row();
        sortValue={};
        if (!valid) return;
        id=statement->integer(0);receivedUs=statement->integer(1);
        localSchema=static_cast<std::uint64_t>(statement->integer(2));
        sortType=statement->type(3)==SQLITE_NULL ? -1:static_cast<int>(statement->integer(3));
        switch(sortType) {
        case -1:case 0:break;
        case 1:sortValue={statement->integer(4)};break;
        case 2:sortValue={statement->real(4)};break;
        case 3:sortValue={statement->integer(4)!=0};break;
        case 4:sortValue={statement->text(4)};break;
        case 5:sortValue={statement->blob(4)};break;
        default:throw std::runtime_error("invalid query sort key type");
        }
    }
    std::size_t keyBytes() const
    {
        if (const auto* text=std::get_if<std::string>(&sortValue.value)) return sizeof(Cursor)+text->capacity();
        if (const auto* bytes=std::get_if<data::Bytes>(&sortValue.value)) return sizeof(Cursor)+bytes->capacity();
        return sizeof(Cursor);
    }
    data::Record record(const RecordSnapshot& snapshot)
    {
        sqlite::Statement payload(db,"SELECT payload FROM records WHERE id=?");
        payload.integer(1,id);
        if (!payload.row()) throw std::runtime_error("snapshot record missing");
        auto record=data::recordFromValue(data::decodeValue(payload.blob(0),valueLimits));
        if (record.schemaVersion!=localSchema || record.receivedAtUs!=receivedUs)
            throw std::runtime_error("record index differs from payload");
        record.schemaVersion=source.schemaIds.at(localSchema);
        data::validateRecord(snapshot.schemas.at(record.schemaVersion),record);
        return record;
    }
};
class Merge {
public:
    Merge(const RecordSnapshot& snapshot,const Query& query,std::size_t budget,std::stop_token stop)
        :snapshot_(snapshot),query_(query),budget_(budget),heap_([this](auto a,auto b){return before(b,a);})
    {
        // 每卷只保留一个排序键，先合并键再加载命中行；不为 offset 或整段历史建立数组。
        for (const auto& source:snapshot.sources) {
            checkStop(stop);
            if (!source.cutoff) continue;
            if (!query.dataset.empty() && std::none_of(source.schemaIds.begin(),source.schemaIds.end(),
                [&](const auto& entry){return snapshot.schemas.at(entry.second).dataset==query.dataset;})) continue;
            auto cursor=std::make_unique<Cursor>(source,query,stop);
            cursor->advance();
            if (!cursor->valid) continue;
            addBytes(cursor->keyBytes());
            cursors_.push_back(std::move(cursor));
            heap_.push(cursors_.size()-1);
        }
    }
    bool valid() const {return !heap_.empty();}
    std::pair<data::Record,std::uint64_t> current()
    {
        auto& cursor=*cursors_.at(heap_.top());
        return {cursor.record(snapshot_),cursor.globalId()};
    }
    void advance()
    {
        const auto index=heap_.top();heap_.pop();
        auto& cursor=*cursors_[index];
        bytes_-=cursor.keyBytes();
        cursor.advance();
        if (cursor.valid) {addBytes(cursor.keyBytes());heap_.push(index);}
        else cursors_[index].reset();
    }
private:
    bool before(std::size_t a,std::size_t b) const
    {
        const auto& left=*cursors_[a];const auto& right=*cursors_[b];
        if (query_.sort) {
            const auto order=left.sortType!=right.sortType ? (left.sortType<right.sortType ? -1:1):
                data::compareValues(left.sortValue,right.sortValue);
            if (order) return query_.sort->descending ? order>0:order<0;
        }
        if (left.receivedUs!=right.receivedUs) return left.receivedUs<right.receivedUs;
        return left.globalId()<right.globalId();
    }
    void addBytes(std::size_t bytes)
    {
        if (bytes>budget_-bytes_) throw std::runtime_error("cross-volume cursor memory budget exceeded");
        bytes_+=bytes;
    }
    const RecordSnapshot& snapshot_;
    const Query& query_;
    std::size_t budget_,bytes_{0};
    std::vector<std::unique_ptr<Cursor>> cursors_;
    std::priority_queue<std::size_t,std::vector<std::size_t>,std::function<bool(std::size_t,std::size_t)>> heap_;
};
}
RecordQueryService::RecordQueryService(std::filesystem::path active,VolumeCatalog& catalog,std::size_t budget)
    :active_(std::move(active)),catalog_(catalog),memoryBudget_(budget)
{
    sqlite::Database db(active_,true);
    sqlite::Statement rows(db,"SELECT id,definition FROM schemas");
    std::map<std::uint64_t,data::Schema> schemas;
    std::size_t bytes=0;
    while (rows.row()) {
        auto schema=data::schemaFromValue(data::decodeValue(rows.blob(1),valueLimits));
        const auto size=schemaBytes(schema);
        if (size>valueLimits.maxBytes-bytes) throw std::runtime_error("active schema metadata exceeds query budget");
        bytes+=size;
        schemas.emplace(static_cast<std::uint64_t>(rows.integer(0)),std::move(schema));
    }
    activeSchemas_=catalog_.registerSchemas(schemas);
}
std::shared_ptr<const RecordSnapshot> RecordQueryService::snapshot(std::optional<std::int64_t> token)
{
    std::lock_guard lock(mutex_);
    if (token) {
        const auto found=snapshots_.find(*token);
        if (found==snapshots_.end()) throw std::runtime_error("record snapshot expired or belongs to another session; refresh required");
        return found->second;
    }
    while (snapshots_.size()>=128) {
        const auto unused=std::find_if(snapshots_.begin(),snapshots_.end(),[](const auto& item){return item.second.use_count()==1;});
        if (unused==snapshots_.end()) throw std::runtime_error("too many pinned record snapshots");
        snapshots_.erase(unused);
    }
    auto result=std::make_shared<RecordSnapshot>();
    result->volumes=catalog_.pinAll();
    result->schemas=catalog_.schemas();
    std::size_t bytes=0;
    for (const auto& [id,schema]:result->schemas) {
        const auto size=schemaBytes(schema);
        if (size>valueLimits.maxBytes-bytes) throw std::runtime_error("snapshot schema metadata exceeds budget");
        bytes+=size;
    }
    {
        sqlite::Database active(active_,true);
        sqlite::Statement high(active,"SELECT coalesce(max(id),0) FROM records");
        high.row();
        result->sources.push_back({active_,high.integer(0),0,activeSchemas_});
    }
    for (const auto& volume:result->volumes->volumes())
    {
        const auto sourceBytes=sizeof(RecordSource)+volume.path.native().size()*sizeof(std::filesystem::path::value_type)+
                               volume.schemaIds.size()*64;
        if (sourceBytes>valueLimits.maxBytes-bytes) throw std::runtime_error("snapshot volume metadata exceeds budget");
        bytes+=sourceBytes;
        result->sources.push_back({volume.path,volume.highWater,volume.idBase,volume.schemaIds});
    }
    result->memoryBytes=bytes+sizeof(RecordSnapshot)+activeSchemas_.size()*64;
    if (result->memoryBytes>valueLimits.maxBytes) throw std::runtime_error("snapshot metadata exceeds budget");
    std::size_t retained=0;
    for (const auto& [id,entry]:snapshots_) retained+=entry->memoryBytes;
    while (result->memoryBytes>valueLimits.maxBytes-retained) {
        const auto unused=std::find_if(snapshots_.begin(),snapshots_.end(),[](const auto& item){return item.second.use_count()==1;});
        if (unused==snapshots_.end()) throw std::runtime_error("pinned snapshot metadata exceeds budget");
        retained-=unused->second->memoryBytes;snapshots_.erase(unused);
    }
    // 不把单卷自增 ID 当成多卷快照；令牌固定卷集合及各卷高水位，重载后旧令牌不可复用。
    static std::atomic<std::int64_t> nextToken{
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
    result->token=nextToken.fetch_add(1);
    snapshots_.emplace(result->token,result);
    return result;
}
Completion RecordQueryService::query(const Query& query,std::stop_token stop)
{
    auto lease=snapshot(query.snapshot);
    Merge merge(*lease,query,memoryBudget_,stop);
    for (std::size_t skipped=0;skipped<query.offset && merge.valid();++skipped) merge.advance();
    Completion result;
    result.operation="query";result.snapshot=lease->token;result.snapshotLease=lease;
    std::size_t bytes=0;
    const auto addBytes=[&](std::size_t size) {
        if (size>memoryBudget_/16-bytes) throw std::runtime_error("query page exceeds memory budget; reduce page size");
        bytes+=size;
    };
    while (merge.valid() && result.records.size()<query.limit) {
        auto [record,id]=merge.current();
        if (!result.schemas.contains(record.schemaVersion)) {
            const auto& schema=lease->schemas.at(record.schemaVersion);
            addBytes(schemaBytes(schema));result.schemas.emplace(record.schemaVersion,schema);
        }
        addBytes(data::recordMemoryBytes(record)+sizeof(id));
        result.records.push_back(std::move(record));result.rowIds.push_back(id);
        merge.advance();
    }
    checkStop(stop);
    result.more=merge.valid();result.ok=true;
    return result;
}
Completion RecordQueryService::exportRecords(const std::filesystem::path& path,ExportFormat format,
    const Query& query,ExportOptions options,std::stop_token stop)
{
    auto lease=snapshot(query.snapshot);
    Merge merge(*lease,query,memoryBudget_,stop);
    std::map<std::uint64_t,data::Schema> schemas;
    for (const auto& [id,schema]:lease->schemas)
        if (query.dataset.empty() || schema.dataset==query.dataset) schemas.emplace(id,schema);
    Completion result;
    result.operation="export";result.path=path;result.snapshot=lease->token;result.snapshotLease=lease;
    result.processed=exportRecordFile(path,format,schemas,[&]() -> std::optional<data::Record> {
        if (!merge.valid()) return std::nullopt;
        auto [record,id]=merge.current();merge.advance();return record;
    },stop,options);
    result.ok=true;return result;
}
void RecordQueryService::expireUnpinned()
{
    std::lock_guard lock(mutex_);
    std::erase_if(snapshots_,[](const auto& entry){return entry.second.use_count()==1;});
}
} // namespace protoscope::storage
