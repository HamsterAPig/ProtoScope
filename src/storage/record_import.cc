#include "record_import.hpp"
#include "sqlite_database.hpp"
#include "protoscope/data/psrec.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace protoscope::storage {
namespace {
constexpr data::ValueLimits valueLimits{32U*1024U*1024U,16,131072};
void checkStop(std::stop_token stop)
{
    if (stop.stop_requested()) throw std::runtime_error("record import canceled");
}
std::string identity()
{
    std::random_device random;
    std::ostringstream text;
    text<<std::hex<<std::setfill('0');
    for (int i=0;i<4;++i) text<<std::setw(8)<<static_cast<std::uint32_t>(random());
    return text.str();
}
void metadata(sqlite::Database& db,const std::string& key,const std::string& value)
{
    sqlite::Statement statement(db,"INSERT OR REPLACE INTO metadata(key,value) VALUES(?,?)");
    statement.text(1,key);statement.text(2,value);statement.row();
}
void checkCapacity(const std::filesystem::path& path,std::uint64_t limit)
{
    std::uint64_t total=0;
    for (const auto suffix:{"","-wal","-shm"}) {
        auto file=path;file+=suffix;
        std::error_code error;
        const auto exists=std::filesystem::exists(file,error);
        if (error) throw std::runtime_error("cannot inspect import disk usage");
        if (!exists) continue;
        const auto bytes=std::filesystem::file_size(file,error);
        if (error || bytes>limit-total) throw std::runtime_error("record import staging capacity exceeded");
        total+=bytes;
    }
}
}

struct StagedRecordImport::Impl {
    ImportedVolumeInfo info;
    std::filesystem::path directory;
    bool released{false};
    bool owned{false};
    std::string marker() const {return "ProtoScope import stage v1\n"+info.identity+"\n";}
    ~Impl()
    {
        if (released || !owned || directory.empty()) return;
        // 不递归删除，不接管其他目录，不沿链接清理；未知文件留给显式恢复检查。
        try {
            if (std::filesystem::is_symlink(directory) || std::filesystem::weakly_canonical(directory)!=directory) return;
            const auto owner=directory/"owner";
            if (std::filesystem::is_symlink(owner)) return;
            std::ifstream input(owner,std::ios::binary);
            std::array<char,128> bytes{};
            input.read(bytes.data(),bytes.size());
            if (std::string(bytes.data(),static_cast<std::size_t>(input.gcount()))!=marker()) return;
            input.close();
            for (const auto suffix:{"","-wal","-shm"}) {
                auto file=info.path;file+=suffix;
                if (std::filesystem::is_symlink(file)) return;
            }
            std::error_code ignored;
            for (const auto suffix:{"","-wal","-shm"}) {auto file=info.path;file+=suffix;std::filesystem::remove(file,ignored);}
            std::filesystem::remove(owner,ignored);
            std::filesystem::remove(directory,ignored);
        } catch (...) {}
    }
};
StagedRecordImport::StagedRecordImport(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
StagedRecordImport::~StagedRecordImport()=default;
StagedRecordImport::StagedRecordImport(StagedRecordImport&&) noexcept=default;
StagedRecordImport& StagedRecordImport::operator=(StagedRecordImport&&) noexcept=default;
const ImportedVolumeInfo& StagedRecordImport::info() const {return impl_->info;}
void StagedRecordImport::release() {impl_->released=true;}
void StagedRecordImport::relocate(const std::filesystem::path& directory)
{
    impl_->directory=directory;impl_->info.path=directory/"records.sqlite";
}

StagedRecordImport stageRecordImport(const std::filesystem::path& recordsRoot,const std::string& protocol,
    const std::filesystem::path& source,ImportFormat format,
    const std::optional<data::CsvImportMapping>& mapping,ImportLimits limits,std::stop_token stop,
    const std::function<void(std::uint64_t)>& progress)
{
    checkStop(stop);
    if (protocol.empty() || protocol.size()>4096 || !limits.sourceBytes || !limits.stagedBytes)
        throw std::invalid_argument("invalid import configuration");
    if (!std::filesystem::is_regular_file(source) || std::filesystem::file_size(source)>limits.sourceBytes)
        throw std::invalid_argument("import source exceeds limit or is not a file");
    std::ifstream input(source,std::ios::binary);
    if (!input) throw std::runtime_error("cannot open record import");
    const auto checkSource=[&] {
        const auto position=input.rdbuf()->pubseekoff(0,std::ios::cur,std::ios::in);
        if (position<0 || static_cast<std::uint64_t>(position)>limits.sourceBytes)
            throw std::runtime_error("record import source exceeds byte limit");
    };
    std::unique_ptr<data::PsrecReader> psrec;
    std::unique_ptr<data::RecordCsvReader> csv;
    std::unique_ptr<data::MappedCsvReader> plain;
    std::map<std::uint64_t,data::Schema> schemas;
    if (format==ImportFormat::Psrec) {psrec=std::make_unique<data::PsrecReader>(input,stop);schemas=psrec->schemas();}
    else if (format==ImportFormat::Csv) {csv=std::make_unique<data::RecordCsvReader>(input,stop);schemas=csv->schemas();}
    else if (format==ImportFormat::MappedCsv && mapping) {
        plain=std::make_unique<data::MappedCsvReader>(input,*mapping,stop);schemas.emplace(1,mapping->schema);
    } else throw std::invalid_argument("unsupported import format or missing mapping");
    checkSource();
    const auto next=[&] {return psrec ? psrec->next():csv ? csv->next():plain->next();};
    auto impl=std::make_unique<StagedRecordImport::Impl>();
    const auto root=std::filesystem::weakly_canonical(std::filesystem::absolute(recordsRoot));
    std::filesystem::create_directories(root/".staging");
    const auto parent=std::filesystem::weakly_canonical(root/".staging");
    if (parent!=root/".staging" || std::filesystem::is_symlink(root/".staging"))
        throw std::runtime_error("import staging directory must not follow links");
    bool created=false;
    for (int attempt=0;attempt<8 && !created;++attempt) {
        impl->info.identity=identity();
        impl->directory=parent/impl->info.identity;
        created=std::filesystem::create_directory(impl->directory);
    }
    if (!created) {impl->directory.clear();throw std::runtime_error("cannot reserve import staging directory");}
    impl->owned=true;
    impl->info.path=impl->directory/"records.sqlite";
    {
        std::ofstream owner(impl->directory/"owner",std::ios::binary);
        owner<<impl->marker();owner.flush();owner.close();
        if (!owner) throw std::runtime_error("cannot write import ownership marker");
    }
    {
        sqlite::Database db(impl->info.path);
        sqlite3_progress_handler(db.get(),1000,[](void* token) {
            return static_cast<std::stop_token*>(token)->stop_requested() ? 1:0;
        },&stop);
        db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA application_id=1347634242; PRAGMA user_version=1;"
            "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "CREATE TABLE schemas(id INTEGER PRIMARY KEY,dataset TEXT NOT NULL,definition BLOB NOT NULL);"
            "CREATE TABLE records(id INTEGER PRIMARY KEY,dataset TEXT NOT NULL,device TEXT NOT NULL,"
            "received_us INTEGER NOT NULL,schema_id INTEGER NOT NULL,payload BLOB NOT NULL);"
            "CREATE INDEX records_time ON records(received_us,id);"
            "CREATE INDEX records_dataset_time ON records(dataset,received_us,id);");
        metadata(db,"protocol",protocol);metadata(db,"volume_id",impl->info.identity);
        metadata(db,"state","staging");metadata(db,"origin","import");
        db.exec("BEGIN IMMEDIATE");
        for (const auto& [id,schema]:schemas) {
            sqlite::Statement insert(db,"INSERT INTO schemas(id,dataset,definition) VALUES(?,?,?)");
            insert.integer(1,static_cast<std::int64_t>(id));insert.text(2,schema.dataset);
            insert.blob(3,data::encodeValue(data::schemaValue(schema),valueLimits));insert.row();
        }
        db.exec("COMMIT");
        checkCapacity(impl->info.path,limits.stagedBytes);
        db.exec("BEGIN IMMEDIATE");
        std::size_t batchRows=0,batchBytes=0;
        auto batchStart=std::chrono::steady_clock::now();
        while (auto record=next()) {
            checkStop(stop);
            checkSource();
            if (impl->info.records>=static_cast<std::uint64_t>(INT64_MAX)) throw std::overflow_error("import row count overflow");
            const auto payload=data::encodeValue(data::recordValue(*record),valueLimits);
            sqlite::Statement insert(db,"INSERT INTO records VALUES(?,?,?,?,?,?)");
            insert.integer(1,static_cast<std::int64_t>(impl->info.records+1));
            insert.text(2,record->dataset);insert.text(3,record->device);insert.integer(4,record->receivedAtUs);
            insert.integer(5,static_cast<std::int64_t>(record->schemaVersion));insert.blob(6,payload);insert.row();
            ++impl->info.records;++batchRows;batchBytes+=payload.size();
            if (!impl->info.fromUs || record->receivedAtUs<*impl->info.fromUs) impl->info.fromUs=record->receivedAtUs;
            if (!impl->info.toUs || record->receivedAtUs>*impl->info.toUs) impl->info.toUs=record->receivedAtUs;
            if (batchRows>=1000 || batchBytes>=32U*1024U*1024U ||
                std::chrono::steady_clock::now()-batchStart>=std::chrono::milliseconds(100)) {
                db.exec("COMMIT");db.exec("BEGIN IMMEDIATE");
                batchRows=0;batchBytes=0;batchStart=std::chrono::steady_clock::now();
            }
            checkCapacity(impl->info.path,limits.stagedBytes);
            if (progress) progress(impl->info.records);
        }
        checkStop(stop);
        checkSource();
        // 直到文件结束校验通过才写 sealed；此前即使已批量提交也不在查询目录索引中。
        metadata(db,"state","sealed");metadata(db,"record_count",std::to_string(impl->info.records));
        db.exec("COMMIT");
        db.exec("PRAGMA wal_checkpoint(TRUNCATE); PRAGMA journal_mode=DELETE;");
        checkCapacity(impl->info.path,limits.stagedBytes);
        checkStop(stop);
    }
    return StagedRecordImport(std::move(impl));
}
} // namespace protoscope::storage
