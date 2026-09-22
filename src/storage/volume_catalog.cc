#include "volume_catalog.hpp"
#include "sqlite_database.hpp"

#include <algorithm>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace protoscope::storage {
namespace {
constexpr int catalogApplicationId=0x50534958;
constexpr int volumeApplicationId=0x50534442;
constexpr data::ValueLimits valueLimits{32U*1024U*1024U,16,131072};

struct CatalogRuntime {
    std::mutex mutex;
    std::map<std::uint64_t,std::shared_ptr<int>> pins;
    bool writerOwned{false};
    std::map<std::string,std::weak_ptr<int>> activePins;
#ifdef _WIN32
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    int handle{-1};
#endif
    explicit CatalogRuntime(const std::filesystem::path& root)
    {
        const auto path=root/".owner.lock";
        if (std::filesystem::is_symlink(path) || std::filesystem::weakly_canonical(path)!=path)
            throw std::runtime_error("record directory lock must not follow links");
#ifdef _WIN32
        handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if (handle==INVALID_HANDLE_VALUE) throw std::runtime_error("record directory is in use or lock is unavailable");
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
            CloseHandle(handle);handle=INVALID_HANDLE_VALUE;
            throw std::runtime_error("invalid record directory lock file");
        }
#else
        handle=::open(path.c_str(),O_RDWR|O_CREAT|O_NOFOLLOW,0600);
        if (handle<0) throw std::runtime_error("record directory lock unavailable");
        if (flock(handle,LOCK_EX|LOCK_NB)!=0) {
            ::close(handle);handle=-1;
            throw std::runtime_error("record directory is in use");
        }
#endif
    }
    ~CatalogRuntime()
    {
#ifdef _WIN32
        if (handle!=INVALID_HANDLE_VALUE) CloseHandle(handle);
#else
        if (handle>=0) ::close(handle);
#endif
    }
};
struct WriterClaim {
    std::shared_ptr<CatalogRuntime> runtime;
    explicit WriterClaim(std::shared_ptr<CatalogRuntime> owner):runtime(std::move(owner)) {}
    ~WriterClaim()
    {
        std::lock_guard lock(runtime->mutex);
        runtime->writerOwned=false;
    }
};
std::shared_ptr<CatalogRuntime> runtimeFor(const std::filesystem::path& root)
{
    static std::mutex mutex;
    static std::map<std::filesystem::path,std::weak_ptr<CatalogRuntime>> directories;
    std::lock_guard lock(mutex);
    std::erase_if(directories,[](const auto& entry){return entry.second.expired();});
    if (auto found=directories[root].lock()) return found;
    auto result=std::make_shared<CatalogRuntime>(root);
    directories[root]=result;
    return result;
}

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
            (name!="owner" && name!="records.sqlite" && name!="records.sqlite-wal" && name!="records.sqlite-shm"))
            throw std::runtime_error("unrecognized or linked file in sealed volume");
    }
    std::ifstream owner(directory/"owner",std::ios::binary);
    std::string marker(128,'\0');
    owner.read(marker.data(),static_cast<std::streamsize>(marker.size()));
    marker.resize(static_cast<std::size_t>(owner.gcount()));
    if (marker!="ProtoScope import stage v1\n"+identity+"\n" &&
        marker!="ProtoScope record volume v1\n"+identity+"\n")
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
std::uint64_t directoryBytes(const std::filesystem::path& root,std::stop_token stop={})
{
    std::uint64_t result=0;
    std::size_t entries=0;
    // 普通迭代不跟随链接，canonical 复验同时拒绝 Windows 目录联接。
    std::error_code ec;
    std::filesystem::recursive_directory_iterator current(root,ec),end;
    if (ec) throw std::filesystem::filesystem_error("scan record directory",root,ec);
    while (current!=end) {
        const auto entry=*current;
        if (stop.stop_requested()) throw std::runtime_error("record retention canceled");
        if (++entries>1000000) throw std::runtime_error("record directory entry budget exceeded");
        const auto status=entry.symlink_status(ec);
        if (ec && ec!=std::errc::no_such_file_or_directory)
            throw std::filesystem::filesystem_error("inspect record capacity entry",entry.path(),ec);
        ec.clear();
        if (std::filesystem::is_symlink(status) || std::filesystem::weakly_canonical(entry.path())!=entry.path())
            throw std::runtime_error("record capacity scan encountered a linked path");
        if (std::filesystem::is_regular_file(status)) {
            const auto bytes=entry.file_size(ec);
            if (ec && ec!=std::errc::no_such_file_or_directory)
                throw std::filesystem::filesystem_error("measure record capacity entry",entry.path(),ec);
            if (!ec) {
                if (bytes>UINT64_MAX-result) throw std::overflow_error("record capacity overflow");
                result+=bytes;
            }
        }
        ec.clear();
        current.increment(ec);
        if (ec) {
            // 暂存取消或 WAL 关闭可能与扫描并发；消失路径由下一轮重新统计，其他错误必须报告。
            if (ec==std::errc::no_such_file_or_directory) break;
            throw std::filesystem::filesystem_error("advance record capacity scan",root,ec);
        }
    }
    return result;
}
}
struct VolumeCatalog::Impl {
    std::filesystem::path root;
    std::string protocol;
    std::shared_ptr<CatalogRuntime> runtime;
    Impl(std::filesystem::path path,std::string key):protocol(std::move(key))
    {
        if (protocol.empty() || protocol.size()>4096) throw std::invalid_argument("invalid catalog protocol");
        std::filesystem::create_directories(path);
        root=std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        runtime=runtimeFor(root);
        std::lock_guard lock(runtime->mutex);
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
        resumeRetirements(db);
        for (const auto& volume:volumes(db)) {
            checkOwner(volume.path.parent_path(),volume.identity);
            sqlite::Database file(volume.path,true);
            checkVolume(file,protocol,volume.identity,volume.records);
        }
    }
    std::vector<CatalogVolume> volumes(sqlite::Database& db)
    {
        std::vector<CatalogVolume> result;
        std::size_t bytes=0;
        const auto account=[&](std::size_t size) {
            if (size>valueLimits.maxBytes-bytes) throw std::runtime_error("catalog volume metadata exceeds budget");
            bytes+=size;
        };
        sqlite::Statement rows(db,"SELECT id,identity,records,from_us,to_us,sealed_us,id_base FROM volumes "
            "WHERE NOT EXISTS(SELECT 1 FROM metadata WHERE key='retiring:'||volumes.id) ORDER BY id");
        while (rows.row()) {
            CatalogVolume volume;
            volume.id=static_cast<std::uint64_t>(rows.integer(0));
            volume.identity=rows.text(1);checkIdentity(volume.identity);
            volume.path=root/("vol-"+volume.identity)/"records.sqlite";
            account(sizeof(CatalogVolume)+volume.path.native().size()*sizeof(std::filesystem::path::value_type)+32);
            volume.records=static_cast<std::uint64_t>(rows.integer(2));
            if (volume.records) {volume.fromUs=rows.integer(3);volume.toUs=rows.integer(4);}
            volume.sealedAtUs=rows.integer(5);
            volume.idBase=rows.integer(6);
            sqlite::Statement schemas(db,"SELECT local_id,global_id FROM volume_schemas WHERE volume_id=?");
            schemas.integer(1,static_cast<std::int64_t>(volume.id));
            while (schemas.row()) {
                account(64);
                volume.schemaIds.emplace(static_cast<std::uint64_t>(schemas.integer(0)),
                                          static_cast<std::uint64_t>(schemas.integer(1)));
            }
            result.push_back(std::move(volume));
        }
        return result;
    }
    void finishRetirement(sqlite::Database& db,std::uint64_t id,const std::string& identity,std::uint64_t count)
    {
        checkIdentity(identity);
        const auto directory=root/("vol-"+identity);
        if (std::filesystem::weakly_canonical(directory)!=directory || std::filesystem::is_symlink(directory))
            throw std::runtime_error("retiring volume resolves outside its owned path");
        if (std::filesystem::exists(directory)) {
            // 持久化清理意图之后允许恢复已删除数据库/标记的中间态，但绝不接管未知文件。
            for (const auto& entry:std::filesystem::directory_iterator(directory)) {
                const auto name=entry.path().filename().string();
                if (entry.is_symlink() || !entry.is_regular_file() ||
                    std::filesystem::weakly_canonical(entry.path())!=entry.path() ||
                    (name!="owner" && name!="records.sqlite" && name!="records.sqlite-wal" && name!="records.sqlite-shm"))
                    throw std::runtime_error("unrecognized file in retiring volume");
            }
            const auto database=directory/"records.sqlite",owner=directory/"owner";
            if (std::filesystem::exists(database) || std::filesystem::exists(owner))
                checkOwner(directory,identity);
            if (std::filesystem::exists(database)) {
                {sqlite::Database file(database,true);checkVolume(file,protocol,identity,count);}
                if (!std::filesystem::remove(database)) throw std::runtime_error("cannot remove retired volume database");
            }
            for (const auto* suffix:{"-wal","-shm"}) {
                auto sidecar=database;sidecar+=suffix;
                if (std::filesystem::exists(sidecar) && !std::filesystem::remove(sidecar))
                    throw std::runtime_error("cannot remove retired volume sidecar");
            }
            if (std::filesystem::exists(owner) && !std::filesystem::remove(owner))
                throw std::runtime_error("cannot remove retired volume marker");
            if (!std::filesystem::remove(directory)) throw std::runtime_error("cannot remove retired volume directory");
        }
        db.exec("BEGIN IMMEDIATE");
        try {
            sqlite::Statement mappings(db,"DELETE FROM volume_schemas WHERE volume_id=?");
            mappings.integer(1,static_cast<std::int64_t>(id));mappings.row();
            sqlite::Statement volume(db,"DELETE FROM volumes WHERE id=? AND identity=?");
            volume.integer(1,static_cast<std::int64_t>(id));volume.text(2,identity);volume.row();
            sqlite::Statement intent(db,"DELETE FROM metadata WHERE key=?");
            intent.text(1,"retiring:"+std::to_string(id));intent.row();
            db.exec("COMMIT");
        } catch (...) {db.exec("ROLLBACK");throw;}
        runtime->pins.erase(id);
    }
    void resumeRetirements(sqlite::Database& db)
    {
        struct Intent {std::uint64_t id,count;std::string identity;};
        std::vector<Intent> pending;
        {
            sqlite::Statement rows(db,"SELECT v.id,v.records,v.identity,m.value FROM volumes v "
                "JOIN metadata m ON m.key='retiring:'||v.id");
            while (rows.row()) {
                if (rows.text(2)!=rows.text(3)) throw std::runtime_error("retirement identity mismatch");
                pending.push_back({static_cast<std::uint64_t>(rows.integer(0)),
                    static_cast<std::uint64_t>(rows.integer(1)),rows.text(2)});
                if (pending.size()>131072) throw std::runtime_error("retirement metadata budget exceeded");
            }
        }
        for (const auto& intent:pending) finishRetirement(db,intent.id,intent.identity,intent.count);
    }
};
VolumeCatalog::VolumeCatalog(std::filesystem::path root,std::string protocol)
    :impl_(std::make_unique<Impl>(std::move(root),std::move(protocol))) {}
VolumeCatalog::~VolumeCatalog()=default;

std::shared_ptr<void> VolumeCatalog::claimWriter()
{
    std::lock_guard lock(impl_->runtime->mutex);
    if (impl_->runtime->writerOwned) throw std::runtime_error("record directory already has a Store writer");
    auto lease=std::make_shared<WriterClaim>(impl_->runtime);
    impl_->runtime->writerOwned=true;
    return lease;
}

CatalogVolume VolumeCatalog::adopt(StagedRecordImport& staged,std::int64_t sealedAtUs,std::stop_token stop)
{
    std::lock_guard lock(impl_->runtime->mutex);
    const auto checkStop=[&] {
        if (stop.stop_requested()) throw std::runtime_error("record import canceled");
    };
    checkStop();
    const auto info=staged.info();
    checkIdentity(info.identity);
    if (info.path!=impl_->root/".staging"/info.identity/"records.sqlite")
        throw std::runtime_error("import artifact is outside this catalog staging root");
    checkOwner(info.path.parent_path(),info.identity);
    std::map<std::uint64_t,data::Bytes> definitions;
    {
        sqlite::Database volume(info.path,true);
        sqlite3_progress_handler(volume.get(),1000,[](void* token) {
            return static_cast<std::stop_token*>(token)->stop_requested() ? 1:0;
        },&stop);
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
    {
        sqlite::Statement high(db,"SELECT value FROM metadata WHERE key='record_high'");
        if (high.row() && base<std::stoll(high.text(0)))
            throw std::runtime_error("import record IDs collide with sealed recordings");
    }
    {
        sqlite::Statement high(db,"SELECT value FROM metadata WHERE key='record_allocated'");
        if (high.row() && base<std::stoll(high.text(0)))
            throw std::runtime_error("import record IDs collide with reserved live range");
    }
    if (std::filesystem::exists(impl_->root/"records.sqlite")) {
        sqlite::Database active(impl_->root/"records.sqlite",true);
        sqlite::Statement high(active,"SELECT coalesce(max(id),0) FROM records");high.row();
        if (base<high.integer(0)) throw std::runtime_error("import record IDs collide with active records");
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
    registered.highWater=static_cast<std::int64_t>(info.records);
    for (const auto& [local,definition]:definitions) {
        checkStop();
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
    checkStop();
    std::filesystem::rename(info.path.parent_path(),destination);
    staged.relocate(destination);
    checkStop();
    db.exec("COMMIT");
    staged.release();
    return registered;
}
std::shared_ptr<const PinnedVolumes> VolumeCatalog::pinAll()
{
    std::lock_guard lock(impl_->runtime->mutex);
    sqlite::Database db(impl_->root/"index.sqlite",true);
    auto result=std::make_shared<PinnedVolumes>();
    result->runtimeLease_=impl_->runtime;
    result->volumes_=impl_->volumes(db);
    for (auto& volume:result->volumes_) {
        checkOwner(volume.path.parent_path(),volume.identity);
        sqlite::Database file(volume.path,true);
        checkVolume(file,impl_->protocol,volume.identity,volume.records,false);
        sqlite::Statement high(file,"SELECT coalesce(max(id),0) FROM records");high.row();
        volume.highWater=high.integer(0);
        auto& pin=impl_->runtime->pins[volume.id];
        if (!pin) pin=std::make_shared<int>(0);
        result->pins_.push_back(pin);
    }
    return result;
}
CatalogVolume VolumeCatalog::adoptRecording(const RecordVolumeInfo& info,std::int64_t sealedAtUs,
    std::shared_ptr<int> activePin)
{
    std::lock_guard lock(impl_->runtime->mutex);
    checkIdentity(info.identity);
    if (!info.sealed || sealedAtUs<0 || info.path!=impl_->root/("vol-"+info.identity)/"records.sqlite")
        throw std::runtime_error("recording volume must be sealed at its permanent owned path");
    checkOwner(info.path.parent_path(),info.identity);
    std::map<std::uint64_t,data::Bytes> definitions;
    std::int64_t first=0;
    {
        sqlite::Database file(info.path,true);
        checkVolume(file,impl_->protocol,info.identity,info.records);
        if (metadata(file,"origin")!="recording") throw std::runtime_error("not a recording volume");
        sqlite::Statement bounds(file,"SELECT min(id),coalesce(max(id),0) FROM records");bounds.row();
        first=bounds.integer(0);
        if (info.records && (first<1 || bounds.integer(1)!=info.lastId ||
            static_cast<std::uint64_t>(info.lastId-first)+1!=info.records))
            throw std::runtime_error("recording volume ID range mismatch");
        sqlite::Statement schemas(file,"SELECT id,definition FROM schemas");
        std::size_t bytes=0;
        while (schemas.row()) {
            auto definition=schemas.blob(1);
            if (schemas.integer(0)<1 || definition.size()>valueLimits.maxBytes-bytes)
                throw std::runtime_error("recording volume schema budget exceeded");
            bytes+=definition.size();
            definitions.emplace(static_cast<std::uint64_t>(schemas.integer(0)),std::move(definition));
        }
    }
    sqlite::Database db(impl_->root/"index.sqlite");
    db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    std::int64_t previous=0;
    {sqlite::Statement high(db,"SELECT value FROM metadata WHERE key='record_high'");
     if (high.row()) previous=std::stoll(high.text(0));}
    if (std::filesystem::exists(impl_->root/"records.sqlite")) {
        sqlite::Database legacy(impl_->root/"records.sqlite",true);
        sqlite::Statement high(legacy,"SELECT coalesce(max(id),0) FROM records");high.row();
        previous=std::max(previous,high.integer(0));
    }
    const auto importNext=std::stoll(metadata(db,"import_next"));
    if (info.records && (first<=previous || info.lastId>importNext))
        throw std::runtime_error("recording volume collides with existing record ID range");
    sqlite::Statement insert(db,"INSERT INTO volumes(identity,records,from_us,to_us,sealed_us,id_base) VALUES(?,?,?,?,?,0)");
    insert.text(1,info.identity);insert.integer(2,static_cast<std::int64_t>(info.records));
    if (info.fromUs) insert.integer(3,*info.fromUs);
    if (info.toUs) insert.integer(4,*info.toUs);
    insert.integer(5,sealedAtUs);insert.row();
    const auto id=static_cast<std::uint64_t>(sqlite3_last_insert_rowid(db.get()));
    CatalogVolume registered{id,info.path,info.identity,info.records,info.fromUs,info.toUs,sealedAtUs,0,{},info.lastId};
    for (const auto& [local,definition]:definitions) {
        data::schemaFromValue(data::decodeValue(definition,valueLimits));
        sqlite::Statement schema(db,"INSERT OR IGNORE INTO schemas(definition) VALUES(?)");
        schema.blob(1,definition);schema.row();
        sqlite::Statement global(db,"SELECT id FROM schemas WHERE definition=?");global.blob(1,definition);
        if (!global.row()) throw std::runtime_error("cannot register recording schema");
        sqlite::Statement mapping(db,"INSERT INTO volume_schemas VALUES(?,?,?)");
        mapping.integer(1,static_cast<std::int64_t>(id));mapping.integer(2,static_cast<std::int64_t>(local));
        mapping.integer(3,global.integer(0));mapping.row();
        registered.schemaIds.emplace(local,static_cast<std::uint64_t>(global.integer(0)));
    }
    sqlite::Statement high(db,"INSERT INTO metadata(key,value) VALUES('record_high',?) "
                             "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    high.text(1,std::to_string(std::max(previous,info.lastId)));high.row();
    // 封存不移动源文件；把活动快照的占用引用转交给目录，清理不能趁切换丢失引用保护。
    if (!activePin) {
        const auto previousPin=impl_->runtime->activePins.find(info.identity);
        if (previousPin!=impl_->runtime->activePins.end()) activePin=previousPin->second.lock();
    }
    if (!activePin) activePin=std::make_shared<int>(0);
    impl_->runtime->pins.emplace(id,std::move(activePin));
    try {db.exec("COMMIT");}
    catch (...) {impl_->runtime->pins.erase(id);throw;}
    return registered;
}
bool VolumeCatalog::isPinned(std::uint64_t id) const
{
    std::lock_guard lock(impl_->runtime->mutex);
    const auto pin=impl_->runtime->pins.find(id);
    return pin!=impl_->runtime->pins.end() && pin->second.use_count()>1;
}
std::map<std::uint64_t,data::Schema> VolumeCatalog::schemas() const
{
    std::lock_guard lock(impl_->runtime->mutex);
    sqlite::Database db(impl_->root/"index.sqlite",true);
    sqlite::Statement rows(db,"SELECT id,definition FROM schemas");
    std::map<std::uint64_t,data::Schema> result;
    std::size_t bytes=0;
    while (rows.row()) {
        auto schema=data::schemaFromValue(data::decodeValue(rows.blob(1),valueLimits));
        std::size_t size=sizeof(schema)+schema.dataset.capacity()+schema.fields.capacity()*sizeof(data::Field);
        for (const auto& field:schema.fields) size+=field.name.capacity();
        if (size>valueLimits.maxBytes-bytes) throw std::runtime_error("catalog schema metadata exceeds budget");
        bytes+=size;
        result.emplace(static_cast<std::uint64_t>(rows.integer(0)),std::move(schema));
    }
    return result;
}
std::map<std::uint64_t,std::uint64_t> VolumeCatalog::registerSchemas(const std::map<std::uint64_t,data::Schema>& schemas)
{
    std::lock_guard lock(impl_->runtime->mutex);
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
std::uint64_t VolumeCatalog::diskBytes() const
{
    std::lock_guard lock(impl_->runtime->mutex);
    return directoryBytes(impl_->root);
}
RetentionResult VolumeCatalog::retain(RetentionPolicy policy,std::int64_t nowUs,std::stop_token stop)
{
    if (!policy.maxBytes || policy.maxAge.count()<0 || nowUs<0)
        throw std::invalid_argument("invalid retention policy or time");
    std::lock_guard lock(impl_->runtime->mutex);
    sqlite::Database db(impl_->root/"index.sqlite");
    db.exec("PRAGMA synchronous=FULL");
    impl_->resumeRetirements(db);
    auto volumes=impl_->volumes(db);
    std::sort(volumes.begin(),volumes.end(),[](const auto& a,const auto& b) {
        return a.sealedAtUs!=b.sealedAtUs ? a.sealedAtUs<b.sealedAtUs:a.id<b.id;
    });
    RetentionResult result;
    result.bytes=directoryBytes(impl_->root,stop);
    for (const auto& volume:volumes) {
        if (stop.stop_requested()) throw std::runtime_error("record retention canceled");
        const bool expired=volume.sealedAtUs>=0 && volume.sealedAtUs<=nowUs &&
            nowUs-volume.sealedAtUs>=policy.maxAge.count();
        if (!expired && result.bytes<=policy.maxBytes) continue;
        const auto pin=impl_->runtime->pins.find(volume.id);
        if (pin!=impl_->runtime->pins.end() && pin->second.use_count()>1) {
            result.expiredPinned=result.expiredPinned || expired;
            continue;
        }
        checkOwner(volume.path.parent_path(),volume.identity);
        {
            sqlite::Database file(volume.path,true);
            sqlite3_progress_handler(file.get(),1000,[](void* token) {
                return static_cast<std::stop_token*>(token)->stop_requested() ? 1:0;
            },&stop);
            checkVolume(file,impl_->protocol,volume.identity,volume.records);
        }
        if (stop.stop_requested()) throw std::runtime_error("record retention canceled");
        // 先提交清理意图，再删精确自有文件。崩溃后按索引身份恢复，禁止递归删除。
        {
            sqlite::Statement intent(db,"INSERT INTO metadata(key,value) VALUES(?,?)");
            intent.text(1,"retiring:"+std::to_string(volume.id));intent.text(2,volume.identity);intent.row();
        }
        impl_->finishRetirement(db,volume.id,volume.identity,volume.records);
        ++result.removedVolumes;
        result.bytes=directoryBytes(impl_->root,stop);
    }
    result.capacityExceeded=result.bytes>policy.maxBytes;
    return result;
}
std::shared_ptr<int> VolumeCatalog::trackActive(const std::filesystem::path& path,const std::shared_ptr<int>& pin)
{
    if (path==impl_->root/"records.sqlite") return pin;
    const auto directory=path.parent_path().filename().string();
    const auto id=directory.starts_with("vol-") ? directory.substr(4):std::string{};
    checkIdentity(id);
    if (!pin || path!=impl_->root/("vol-"+id)/"records.sqlite")
        throw std::runtime_error("active query source is outside record directory");
    std::lock_guard lock(impl_->runtime->mutex);
    std::erase_if(impl_->runtime->activePins,[](const auto& item){return item.second.expired();});
    if (auto existing=impl_->runtime->activePins[id].lock()) return existing;
    impl_->runtime->activePins[id]=pin;
    return pin;
}
void VolumeCatalog::reserveLiveThrough(std::int64_t id)
{
    if (id<0) throw std::invalid_argument("invalid live record ID reservation");
    std::lock_guard lock(impl_->runtime->mutex);
    sqlite::Database db(impl_->root/"index.sqlite");db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    const auto available=std::stoll(metadata(db,"import_next"));
    if (id>available) throw std::runtime_error("live record ID space exhausted by imported ranges");
    std::int64_t previous=0;
    {sqlite::Statement high(db,"SELECT value FROM metadata WHERE key='record_allocated'");
     if (high.row()) previous=std::stoll(high.text(0));}
    sqlite::Statement reserve(db,"INSERT INTO metadata(key,value) VALUES('record_allocated',?) "
                                "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    reserve.text(1,std::to_string(std::max(previous,id)));reserve.row();db.exec("COMMIT");
}
} // namespace protoscope::storage
