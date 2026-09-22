#include "volume_catalog.hpp"
#include "sqlite_database.hpp"

#include <algorithm>
#include <fstream>

namespace protoscope::storage {
namespace {
constexpr int catalogApplicationId=0x50534958;
constexpr int volumeApplicationId=0x50534442;
constexpr data::ValueLimits valueLimits{32U*1024U*1024U,16,131072};

std::string metadata(sqlite::Database& db,const std::string& key)
{
    sqlite::Statement statement(db,"SELECT value FROM metadata WHERE key=?");
    statement.text(1,key);
    if (!statement.row()) throw std::runtime_error("required volume metadata missing");
    return statement.text(0);
}
void checkIdentity(const std::string& identity)
{
    if (identity.size()!=32 || std::any_of(identity.begin(),identity.end(),[](char c) {
        return !(c>='0' && c<='9') && !(c>='a' && c<='f');
    })) throw std::runtime_error("invalid volume identity");
}
void checkOwner(const std::filesystem::path& directory,const std::string& identity)
{
    checkIdentity(identity);
    if (std::filesystem::weakly_canonical(directory)!=directory || std::filesystem::is_symlink(directory))
        throw std::runtime_error("volume directory resolves outside its indexed path");
    for (const auto& entry:std::filesystem::directory_iterator(directory)) {
        const auto name=entry.path().filename().string();
        if (std::filesystem::is_symlink(entry.symlink_status()) || !entry.is_regular_file() ||
            (name!="owner" && name!="records.sqlite"))
            throw std::runtime_error("unrecognized or linked file in sealed volume");
    }
    std::ifstream owner(directory/"owner",std::ios::binary);
    std::string marker(128,'\0');
    owner.read(marker.data(),static_cast<std::streamsize>(marker.size()));
    marker.resize(static_cast<std::size_t>(owner.gcount()));
    if (marker!="ProtoScope import stage v1\n"+identity+"\n")
        throw std::runtime_error("volume ownership marker mismatch");
}
void checkVolume(sqlite::Database& db,const std::string& protocol,const std::string& identity,
                 std::uint64_t count,bool full=true)
{
    sqlite::Statement app(db,"PRAGMA application_id");
    if (!app.row() || app.integer(0)!=volumeApplicationId) throw std::runtime_error("foreign volume database");
    sqlite::Statement version(db,"PRAGMA user_version");
    if (!version.row() || version.integer(0)!=1) throw std::runtime_error("unsupported volume version");
    if (metadata(db,"protocol")!=protocol || metadata(db,"volume_id")!=identity ||
        metadata(db,"state")!="sealed" || metadata(db,"record_count")!=std::to_string(count))
        throw std::runtime_error("volume metadata mismatch");
    if (!full) return;
    sqlite::Statement integrity(db,"PRAGMA quick_check");
    if (!integrity.row() || integrity.text(0)!="ok") throw std::runtime_error("damaged volume database");
    sqlite::Statement records(db,"SELECT count(*) FROM records");
    if (!records.row() || records.integer(0)!=static_cast<std::int64_t>(count))
        throw std::runtime_error("volume record count mismatch");
}
}
struct VolumeCatalog::Impl {
    std::filesystem::path root;
    std::string protocol;
    mutable std::mutex mutex;
    std::map<std::uint64_t,std::shared_ptr<int>> pins;
    Impl(std::filesystem::path path,std::string key):protocol(std::move(key))
    {
        if (protocol.empty() || protocol.size()>4096) throw std::invalid_argument("invalid catalog protocol");
        std::filesystem::create_directories(path);
        root=std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        const auto index=root/"index.sqlite";
        if (std::filesystem::is_symlink(index) || std::filesystem::weakly_canonical(index)!=index)
            throw std::runtime_error("catalog must not follow file links");
        sqlite::Database db(index);
        std::int64_t appId=0;
        {sqlite::Statement app(db,"PRAGMA application_id");app.row();appId=app.integer(0);}
        if (appId==0) {
            {
                sqlite::Statement tables(db,"SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'");
                if (!tables.row() || tables.integer(0)!=0) throw std::runtime_error("unidentified catalog database");
            }
            // 新目录索引在独立事务中建立，不修改既有 records.sqlite 或 UI YAML。
            db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; BEGIN IMMEDIATE;"
                "PRAGMA application_id=0x50534958; PRAGMA user_version=1;"
                "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                "CREATE TABLE schemas(id INTEGER PRIMARY KEY AUTOINCREMENT,definition BLOB NOT NULL UNIQUE);"
                "CREATE TABLE volumes(id INTEGER PRIMARY KEY AUTOINCREMENT,identity TEXT NOT NULL UNIQUE,"
                "records INTEGER NOT NULL,from_us INTEGER,to_us INTEGER,sealed_us INTEGER NOT NULL,id_base INTEGER NOT NULL);"
                "CREATE TABLE volume_schemas(volume_id INTEGER NOT NULL,local_id INTEGER NOT NULL,"
                "global_id INTEGER NOT NULL,PRIMARY KEY(volume_id,local_id));");
            sqlite::Statement owner(db,"INSERT INTO metadata VALUES('protocol',?)");
            owner.text(1,protocol);owner.row();
            db.exec("INSERT INTO metadata VALUES('import_next','9223372036854775807'); COMMIT");
        } else {
            if (appId!=catalogApplicationId) throw std::runtime_error("foreign catalog database");
            sqlite::Statement version(db,"PRAGMA user_version");
            if (!version.row() || version.integer(0)!=1 || metadata(db,"protocol")!=protocol)
                throw std::runtime_error("catalog protocol or version mismatch");
            sqlite::Statement integrity(db,"PRAGMA quick_check");
            if (!integrity.row() || integrity.text(0)!="ok") throw std::runtime_error("damaged catalog database");
        }
        for (const auto& volume:volumes(db)) {
            checkOwner(volume.path.parent_path(),volume.identity);
            sqlite::Database file(volume.path,true);
            checkVolume(file,protocol,volume.identity,volume.records);
        }
    }
    std::vector<CatalogVolume> volumes(sqlite::Database& db)
    {
        std::vector<CatalogVolume> result;
        sqlite::Statement rows(db,"SELECT id,identity,records,from_us,to_us,sealed_us,id_base FROM volumes ORDER BY id");
        while (rows.row()) {
            CatalogVolume volume;
            volume.id=static_cast<std::uint64_t>(rows.integer(0));
            volume.identity=rows.text(1);checkIdentity(volume.identity);
            volume.path=root/("vol-"+volume.identity)/"records.sqlite";
            volume.records=static_cast<std::uint64_t>(rows.integer(2));
            if (volume.records) {volume.fromUs=rows.integer(3);volume.toUs=rows.integer(4);}
            volume.sealedAtUs=rows.integer(5);
            volume.idBase=rows.integer(6);
            sqlite::Statement schemas(db,"SELECT local_id,global_id FROM volume_schemas WHERE volume_id=?");
            schemas.integer(1,static_cast<std::int64_t>(volume.id));
            while (schemas.row()) volume.schemaIds.emplace(static_cast<std::uint64_t>(schemas.integer(0)),
                                                           static_cast<std::uint64_t>(schemas.integer(1)));
            result.push_back(std::move(volume));
        }
        return result;
    }
};
VolumeCatalog::VolumeCatalog(std::filesystem::path root,std::string protocol)
    :impl_(std::make_unique<Impl>(std::move(root),std::move(protocol))) {}
VolumeCatalog::~VolumeCatalog()=default;

CatalogVolume VolumeCatalog::adopt(StagedRecordImport& staged,std::int64_t sealedAtUs)
{
    std::lock_guard lock(impl_->mutex);
    const auto info=staged.info();
    checkIdentity(info.identity);
    if (info.path!=impl_->root/".staging"/info.identity/"records.sqlite")
        throw std::runtime_error("import artifact is outside this catalog staging root");
    checkOwner(info.path.parent_path(),info.identity);
    std::map<std::uint64_t,data::Bytes> definitions;
    {
        sqlite::Database volume(info.path,true);
        checkVolume(volume,impl_->protocol,info.identity,info.records);
        sqlite::Statement schemas(volume,"SELECT id,definition FROM schemas");
        while (schemas.row()) definitions.emplace(static_cast<std::uint64_t>(schemas.integer(0)),schemas.blob(1));
    }
    sqlite::Database db(impl_->root/"index.sqlite");
    db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    const auto next=std::stoll(metadata(db,"import_next"));
    if (next<0 || info.records>static_cast<std::uint64_t>(next))
        throw std::runtime_error("import record ID space exhausted");
    const auto base=next-static_cast<std::int64_t>(info.records);
    if (std::filesystem::exists(impl_->root/"records.sqlite")) {
        sqlite::Database active(impl_->root/"records.sqlite",true);
        sqlite::Statement high(active,"SELECT coalesce(max(id),0) FROM records");high.row();
        if (base<=high.integer(0)) throw std::runtime_error("import record IDs collide with active records");
    }
    // 导入使用不重用的高位区间，实时记录继续沿用原有递增 ID；两者不因模式版本相同而冲突。
    sqlite::Statement cursor(db,"UPDATE metadata SET value=? WHERE key='import_next'");
    cursor.text(1,std::to_string(base));cursor.row();
    sqlite::Statement insert(db,"INSERT INTO volumes(identity,records,from_us,to_us,sealed_us,id_base) VALUES(?,?,?,?,?,?)");
    insert.text(1,info.identity);insert.integer(2,static_cast<std::int64_t>(info.records));
    if (info.fromUs) insert.integer(3,*info.fromUs);
    if (info.toUs) insert.integer(4,*info.toUs);
    insert.integer(5,sealedAtUs);insert.integer(6,base);insert.row();
    const auto id=static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db.get()));
    const auto destination=impl_->root/("vol-"+info.identity);
    CatalogVolume registered{id,destination/"records.sqlite",info.identity,info.records,
                             info.fromUs,info.toUs,sealedAtUs,base,{}};
    for (const auto& [local,definition]:definitions) {
        data::schemaFromValue(data::decodeValue(definition,valueLimits));
        sqlite::Statement schema(db,"INSERT OR IGNORE INTO schemas(definition) VALUES(?)");
        schema.blob(1,definition);schema.row();
        sqlite::Statement global(db,"SELECT id FROM schemas WHERE definition=?");
        global.blob(1,definition);
        if (!global.row()) throw std::runtime_error("cannot register imported schema");
        sqlite::Statement mapping(db,"INSERT INTO volume_schemas VALUES(?,?,?)");
        mapping.integer(1,static_cast<std::int64_t>(id));mapping.integer(2,static_cast<std::int64_t>(local));
        mapping.integer(3,global.integer(0));mapping.row();
        registered.schemaIds.emplace(local,static_cast<std::uint64_t>(global.integer(0)));
    }
    // 先在同一根内移动完整卷，再提交索引；索引提交失败时暂存对象仍拥有新路径并负责清理。
    std::filesystem::rename(info.path.parent_path(),destination);
    staged.relocate(destination);
    db.exec("COMMIT");
    staged.release();
    return registered;
}
std::shared_ptr<const PinnedVolumes> VolumeCatalog::pinAll()
{
    std::lock_guard lock(impl_->mutex);
    sqlite::Database db(impl_->root/"index.sqlite",true);
    auto result=std::make_shared<PinnedVolumes>();
    result->volumes_=impl_->volumes(db);
    for (const auto& volume:result->volumes_) {
        checkOwner(volume.path.parent_path(),volume.identity);
        sqlite::Database file(volume.path,true);
        checkVolume(file,impl_->protocol,volume.identity,volume.records,false);
        auto& pin=impl_->pins[volume.id];
        if (!pin) pin=std::make_shared<int>(0);
        result->pins_.push_back(pin);
    }
    return result;
}
bool VolumeCatalog::isPinned(std::uint64_t id) const
{
    std::lock_guard lock(impl_->mutex);
    const auto pin=impl_->pins.find(id);
    return pin!=impl_->pins.end() && pin->second.use_count()>1;
}
std::map<std::uint64_t,data::Schema> VolumeCatalog::schemas() const
{
    std::lock_guard lock(impl_->mutex);
    sqlite::Database db(impl_->root/"index.sqlite",true);
    sqlite::Statement rows(db,"SELECT id,definition FROM schemas");
    std::map<std::uint64_t,data::Schema> result;
    while (rows.row())
        result.emplace(static_cast<std::uint64_t>(rows.integer(0)),data::schemaFromValue(data::decodeValue(rows.blob(1),valueLimits)));
    return result;
}
std::map<std::uint64_t,std::uint64_t> VolumeCatalog::registerSchemas(const std::map<std::uint64_t,data::Schema>& schemas)
{
    std::lock_guard lock(impl_->mutex);
    sqlite::Database db(impl_->root/"index.sqlite");
    db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    std::map<std::uint64_t,std::uint64_t> result;
    for (const auto& [local,schema]:schemas) {
        const auto bytes=data::encodeValue(data::schemaValue(schema),valueLimits);
        sqlite::Statement insert(db,"INSERT OR IGNORE INTO schemas(definition) VALUES(?)");
        insert.blob(1,bytes);insert.row();
        sqlite::Statement global(db,"SELECT id FROM schemas WHERE definition=?");
        global.blob(1,bytes);
        if (!global.row()) throw std::runtime_error("cannot register active schema");
        result.emplace(local,static_cast<std::uint64_t>(global.integer(0)));
    }
    db.exec("COMMIT");
    return result;
}
} // namespace protoscope::storage
