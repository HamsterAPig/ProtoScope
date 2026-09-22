#include "record_volume.hpp"
#include "sqlite_database.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <random>

namespace protoscope::storage {
namespace {
constexpr data::ValueLimits limits{32U*1024U*1024U,16,131072};
std::string identity()
{
    std::random_device random;
    constexpr char digits[]="0123456789abcdef";
    std::string result(32,'0');
    for (auto& value:result) value=digits[random()&15];
    return result;
}
void put(sqlite::Database& db,const char* key,const std::string& value)
{
    sqlite::Statement row(db,"INSERT INTO metadata(key,value) VALUES(?,?) "
                             "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    row.text(1,key);row.text(2,value);row.row();
}
std::string get(sqlite::Database& db,const char* key)
{
    sqlite::Statement row(db,"SELECT value FROM metadata WHERE key=?");row.text(1,key);
    if (!row.row()) throw std::runtime_error("record volume metadata missing");
    return row.text(0);
}
std::int64_t number(sqlite::Database& db,const char* key)
{
    const auto text=get(db,key);
    std::int64_t value=0;
    const auto parsed=std::from_chars(text.data(),text.data()+text.size(),value);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size())
        throw std::runtime_error("invalid record volume metadata number");
    return value;
}
std::string marker(const std::string& id) {return "ProtoScope record volume v1\n"+id+"\n";}
void owner(const std::filesystem::path& path,const std::string& id)
{
    const auto directory=path.parent_path();
    if (id.size()!=32 || id.find_first_not_of("0123456789abcdef")!=std::string::npos ||
        directory.filename()!=("vol-"+id) || path.filename()!="records.sqlite" ||
        std::filesystem::weakly_canonical(directory)!=directory || std::filesystem::is_symlink(directory))
        throw std::runtime_error("invalid record volume owned path");
    for (const auto& file:std::filesystem::directory_iterator(directory)) {
        const auto name=file.path().filename().string();
        if (file.is_symlink() || !file.is_regular_file() ||
            std::filesystem::weakly_canonical(file.path())!=file.path() ||
            (name!="owner" && name!="records.sqlite" && name!="records.sqlite-wal" && name!="records.sqlite-shm"))
            throw std::runtime_error("unrecognized record volume file");
    }
    std::ifstream file(directory/"owner",std::ios::binary);
    std::string text(128,'\0');file.read(text.data(),static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (text!=marker(id)) throw std::runtime_error("record volume ownership mismatch");
}
}
struct RecordVolume::Impl {
    RecordVolumeInfo info;
    std::string protocol;
    std::map<std::uint64_t,data::Schema> schemas;
    std::unique_ptr<sqlite::Database> db;
};
RecordVolume::RecordVolume(std::unique_ptr<Impl> impl):impl_(std::move(impl)) {}
RecordVolume::~RecordVolume()=default;
const RecordVolumeInfo& RecordVolume::info() const {return impl_->info;}
std::string RecordVolume::newIdentity() {return identity();}

std::unique_ptr<RecordVolume> RecordVolume::create(const std::filesystem::path& root,const std::string& protocol,
    const std::map<std::uint64_t,data::Schema>& schemas,std::int64_t firstId,std::int64_t openedAtUs,
    std::string reservedIdentity)
{
    if (protocol.empty() || protocol.size()>4096 || firstId<1 || openedAtUs<0)
        throw std::invalid_argument("invalid new record volume parameters");
    if (!reservedIdentity.empty() && (reservedIdentity.size()!=32 ||
        reservedIdentity.find_first_not_of("0123456789abcdef")!=std::string::npos))
        throw std::invalid_argument("invalid reserved record volume identity");
    std::size_t schemaBytes=0;
    for (const auto& [id,schema]:schemas) {
        if (!id || id>INT64_MAX) throw std::invalid_argument("invalid record schema ID");
        const auto size=data::encodeValue(data::schemaValue(schema),limits).size();
        if (size>limits.maxBytes-schemaBytes) throw std::runtime_error("record schema budget exceeded");
        schemaBytes+=size;
    }
    auto impl=std::make_unique<Impl>();impl->protocol=protocol;impl->schemas=schemas;
    const auto parent=std::filesystem::weakly_canonical(std::filesystem::absolute(root));
    std::filesystem::create_directories(parent);
    bool created=false;
    for (int attempt=0;attempt<(reservedIdentity.empty() ? 8:1) && !created;++attempt) {
        impl->info.identity=reservedIdentity.empty() ? identity():reservedIdentity;
        impl->info.path=parent/("vol-"+impl->info.identity)/"records.sqlite";
        created=std::filesystem::create_directory(impl->info.path.parent_path());
    }
    if (!created) throw std::runtime_error("cannot reserve record volume directory");
    {
        std::ofstream file(impl->info.path.parent_path()/"owner",std::ios::binary);
        file<<marker(impl->info.identity);file.flush();file.close();
        if (!file) throw std::runtime_error("cannot write record volume owner");
    }
    impl->db=std::make_unique<sqlite::Database>(impl->info.path);
    auto& db=*impl->db;
    db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; BEGIN IMMEDIATE;"
        "PRAGMA application_id=1347634242; PRAGMA user_version=1;"
        "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "CREATE TABLE schemas(id INTEGER PRIMARY KEY,dataset TEXT NOT NULL,definition BLOB NOT NULL);"
        "CREATE TABLE records(id INTEGER PRIMARY KEY,dataset TEXT NOT NULL,device TEXT NOT NULL,"
        "received_us INTEGER NOT NULL,schema_id INTEGER NOT NULL,payload BLOB NOT NULL);"
        "CREATE INDEX records_time ON records(received_us,id);"
        "CREATE INDEX records_dataset_time ON records(dataset,received_us,id);");
    put(db,"protocol",protocol);put(db,"volume_id",impl->info.identity);
    put(db,"origin","recording");put(db,"state","active");put(db,"record_count","0");
    put(db,"last_id",std::to_string(firstId-1));put(db,"first_id",std::to_string(firstId));
    put(db,"opened_us",std::to_string(openedAtUs));
    for (const auto& [id,schema]:schemas) {
        sqlite::Statement row(db,"INSERT INTO schemas VALUES(?,?,?)");
        row.integer(1,static_cast<std::int64_t>(id));row.text(2,schema.dataset);
        row.blob(3,data::encodeValue(data::schemaValue(schema),limits));row.row();
    }
    db.exec("COMMIT");
    impl->info.lastId=firstId-1;impl->info.openedAtUs=openedAtUs;
    return std::unique_ptr<RecordVolume>(new RecordVolume(std::move(impl)));
}

std::unique_ptr<RecordVolume> RecordVolume::reopen(const std::filesystem::path& path,const std::string& protocol)
{
    const auto absolute=std::filesystem::absolute(path).lexically_normal();
    const auto directory=absolute.parent_path().filename().string();
    const auto id=directory.starts_with("vol-") ? directory.substr(4):std::string{};
    owner(absolute,id);
    if (!std::filesystem::is_regular_file(absolute)) throw std::runtime_error("record volume database missing");
    auto impl=std::make_unique<Impl>();impl->protocol=protocol;
    impl->info.path=absolute;impl->info.identity=id;
    impl->db=std::make_unique<sqlite::Database>(absolute);
    auto& db=*impl->db;
    {
        sqlite::Statement app(db,"PRAGMA application_id");app.row();
        sqlite::Statement version(db,"PRAGMA user_version");version.row();
        if (app.integer(0)!=0x50534442 || version.integer(0)!=1 ||
            get(db,"protocol")!=protocol || get(db,"volume_id")!=id || get(db,"origin")!="recording")
            throw std::runtime_error("record volume identity mismatch");
        sqlite::Statement check(db,"PRAGMA quick_check");
        if (!check.row() || check.text(0)!="ok") throw std::runtime_error("record volume integrity failure");
    }
    const auto state=get(db,"state");
    if (state!="active" && state!="sealed") throw std::runtime_error("invalid record volume state");
    impl->info.sealed=state=="sealed";
    impl->info.lastId=number(db,"last_id");impl->info.openedAtUs=number(db,"opened_us");
    const auto count=number(db,"record_count"),firstId=number(db,"first_id");
    if (count<0 || firstId<1 || impl->info.lastId<firstId-1 || impl->info.openedAtUs<0 ||
        impl->info.lastId-(firstId-1)!=count)
        throw std::runtime_error("inconsistent record volume counters");
    impl->info.records=static_cast<std::uint64_t>(count);
    {
        sqlite::Statement rows(db,"SELECT count(*),min(id),max(id),min(received_us),max(received_us) FROM records");
        rows.row();
        if (rows.integer(0)!=count || (count && (rows.integer(1)!=firstId || rows.integer(2)!=impl->info.lastId)))
            throw std::runtime_error("record volume index count mismatch");
        if (count) {impl->info.fromUs=rows.integer(3);impl->info.toUs=rows.integer(4);}
    }
    sqlite::Statement schemas(db,"SELECT id,definition FROM schemas");
    std::size_t bytes=0;
    while (schemas.row()) {
        const auto encoded=schemas.blob(1);
        if (schemas.integer(0)<1 || encoded.size()>limits.maxBytes-bytes)
            throw std::runtime_error("record volume schema budget exceeded");
        bytes+=encoded.size();
        impl->schemas.emplace(static_cast<std::uint64_t>(schemas.integer(0)),
            data::schemaFromValue(data::decodeValue(encoded,limits)));
    }
    db.exec("PRAGMA synchronous=FULL");
    return std::unique_ptr<RecordVolume>(new RecordVolume(std::move(impl)));
}

void RecordVolume::append(const std::vector<data::Record>& records,std::int64_t idLimit)
{
    auto& info=impl_->info;
    if (info.sealed) throw std::runtime_error("cannot append sealed record volume");
    auto& db=*impl_->db;
    if (idLimit<info.lastId || records.size()>static_cast<std::uint64_t>(idLimit-info.lastId))
        throw std::overflow_error("record ID range exhausted");
    for (const auto& record:records) {
        if (record.protocol!=impl_->protocol) throw std::invalid_argument("record volume protocol mismatch");
        data::validateRecord(impl_->schemas.at(record.schemaVersion),record);
    }
    auto last=info.lastId;auto from=info.fromUs,to=info.toUs;
    db.exec("BEGIN IMMEDIATE");
    try {
        for (const auto& record:records) {
            sqlite::Statement row(db,"INSERT INTO records VALUES(?,?,?,?,?,?)");
            row.integer(1,++last);row.text(2,record.dataset);row.text(3,record.device);
            row.integer(4,record.receivedAtUs);row.integer(5,static_cast<std::int64_t>(record.schemaVersion));
            row.blob(6,data::encodeValue(data::recordValue(record),limits));row.row();
            if (!from || record.receivedAtUs<*from) from=record.receivedAtUs;
            if (!to || record.receivedAtUs>*to) to=record.receivedAtUs;
        }
        put(db,"last_id",std::to_string(last));
        put(db,"record_count",std::to_string(info.records+records.size()));
        db.exec("COMMIT");
    } catch (...) {db.exec("ROLLBACK");throw;}
    info.records+=records.size();info.lastId=last;info.fromUs=from;info.toUs=to;
}

void RecordVolume::seal(std::int64_t sealedAtUs)
{
    if (sealedAtUs<0) throw std::invalid_argument("invalid seal time");
    if (impl_->info.sealed) return;
    auto& db=*impl_->db;
    db.exec("BEGIN IMMEDIATE");
    try {
        put(db,"state","sealed");put(db,"sealed_us",std::to_string(sealedAtUs));db.exec("COMMIT");
    } catch (...) {db.exec("ROLLBACK");throw;}
    impl_->info.sealed=true;
    // PASSIVE 不等待长查询释放读事务；保留 WAL 格式和固定路径，后续查询仍能恢复已提交页。
    const auto code=sqlite3_wal_checkpoint_v2(db.get(),nullptr,SQLITE_CHECKPOINT_PASSIVE,nullptr,nullptr);
    if (code!=SQLITE_BUSY) sqlite::check(code,db.get());
    impl_->db.reset();
}
std::uint64_t RecordVolume::diskBytes() const
{
    std::uint64_t bytes=0;
    for (const auto* suffix:{"","-wal","-shm"}) {
        auto path=impl_->info.path;path+=suffix;
        std::error_code ec;
        const auto size=std::filesystem::file_size(path,ec);
        if (ec==std::errc::no_such_file_or_directory) continue;
        if (ec) throw std::filesystem::filesystem_error("record volume size",path,ec);
        if (size>UINT64_MAX-bytes) throw std::overflow_error("record volume size overflow");
        bytes+=size;
    }
    return bytes;
}
bool RecordVolume::rotationDue(std::int64_t nowUs,std::uint64_t maxBytes) const
{
    if (nowUs<0 || !maxBytes) throw std::invalid_argument("invalid rotation policy");
    if (impl_->info.sealed || !impl_->info.records) return false;
    constexpr std::int64_t day=24LL*60*60*1000000;
    return nowUs/day>impl_->info.openedAtUs/day || diskBytes()>=maxBytes;
}
} // namespace protoscope::storage
