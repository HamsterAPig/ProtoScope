#include "legacy_record_migration.hpp"
#include "sqlite_database.hpp"

#include <fstream>

namespace protoscope::storage {
namespace {
std::string read(sqlite::Database& db,const char* key)
{
    sqlite::Statement row(db,"SELECT value FROM metadata WHERE key=?");row.text(1,key);
    return row.row() ? row.text(0):std::string{};
}
void write(sqlite::Database& db,const char* key,const std::string& value)
{
    sqlite::Statement row(db,"INSERT INTO metadata(key,value) VALUES(?,?) "
                             "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
    row.text(1,key);row.text(2,value);row.row();
}
void checkPath(const std::filesystem::path& path)
{
    if (std::filesystem::is_symlink(path) || std::filesystem::weakly_canonical(path)!=path)
        throw std::runtime_error("legacy migration must not follow links");
}
void checkIdentity(const std::string& id)
{
    if (id.size()!=32 || id.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::runtime_error("invalid legacy migration identity");
}
struct LegacySummary {
    std::int64_t first{1},last{0},count{0};
    std::optional<std::int64_t> lastTime;
    bool recording{false};
};
LegacySummary inspect(sqlite::Database& db,const std::string& protocol)
{
    sqlite::Statement app(db,"PRAGMA application_id");app.row();
    sqlite::Statement version(db,"PRAGMA user_version");version.row();
    if (app.integer(0)!=0x50534442 || version.integer(0)!=1 || read(db,"protocol")!=protocol)
        throw std::runtime_error("legacy record database identity mismatch");
    sqlite::Statement check(db,"PRAGMA quick_check");
    if (!check.row() || check.text(0)!="ok") throw std::runtime_error("legacy record database damaged");
    LegacySummary result;
    sqlite::Statement bounds(db,"SELECT count(*),min(id),max(id) FROM records");bounds.row();
    result.count=bounds.integer(0);
    if (result.count) {
        result.first=bounds.integer(1);result.last=bounds.integer(2);
        if (result.first<1 || result.last<result.first || result.last-result.first+1!=result.count)
            throw std::runtime_error("legacy record ID range is not contiguous");
        sqlite::Statement last(db,"SELECT received_us FROM records ORDER BY id DESC LIMIT 1");last.row();
        result.lastTime=last.integer(0);
    }
    result.recording=read(db,"recording")=="1";
    return result;
}
void owner(const std::filesystem::path& directory,const std::string& id)
{
    checkPath(directory);
    if (!std::filesystem::exists(directory)) std::filesystem::create_directory(directory);
    for (const auto& entry:std::filesystem::directory_iterator(directory)) {
        checkPath(entry.path());
        if (!entry.is_regular_file() || entry.path().filename()!="owner")
            throw std::runtime_error("migration destination contains unexpected file");
    }
    const auto path=directory/"owner";
    const auto marker="ProtoScope record volume v1\n"+id+"\n";
    if (std::filesystem::exists(path)) {
        std::ifstream file(path,std::ios::binary);
        std::string bytes(128,'\0');file.read(bytes.data(),static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(file.gcount()));
        if (bytes!=marker) throw std::runtime_error("migration destination owner mismatch");
    } else {
        std::ofstream file(path,std::ios::binary);
        file<<marker;file.flush();file.close();
        if (!file) throw std::runtime_error("cannot write migration owner");
    }
}
}
bool hasLegacyMigration(const std::filesystem::path& root)
{
    const auto index=std::filesystem::weakly_canonical(std::filesystem::absolute(root))/"index.sqlite";
    checkPath(index);
    sqlite::Database db(index,true);
    return !read(db,"legacy_migration").empty() || !read(db,"legacy_migrated").empty();
}
std::optional<CatalogVolume> migrateLegacyRecords(const std::filesystem::path& recordsRoot,
    const std::string& protocol,VolumeCatalog& catalog,RecordSession& session,std::int64_t nowUs,
    const std::function<void(LegacyMigrationPhase)>& checkpoint)
{
    if (nowUs<0) throw std::invalid_argument("invalid migration time");
    const auto root=std::filesystem::weakly_canonical(std::filesystem::absolute(recordsRoot));
    const auto source=root/"records.sqlite",index=root/"index.sqlite";
    checkPath(index);checkPath(source);
    for (const auto* suffix:{"-wal","-shm","-journal"}) {
        auto path=source;path+=suffix;checkPath(path);
    }
    std::string id,completed;
    {
        sqlite::Database db(index,true);
        if (read(db,"protocol")!=protocol) throw std::runtime_error("migration catalog protocol mismatch");
        id=read(db,"legacy_migration");completed=read(db,"legacy_migrated");
    }
    if (!id.empty() && !completed.empty() && id!=completed)
        throw std::runtime_error("conflicting legacy migration identities");
    if (id.empty() && !completed.empty()) {
        checkIdentity(completed);
        if (std::filesystem::exists(source)) throw std::runtime_error("legacy source reappeared after migration");
        auto volumes=catalog.pinAll();
        for (const auto& volume:volumes->volumes()) if (volume.identity==completed) return volume;
        return {}; // 已完成迁移的旧卷可能随后因保留策略被清理，不能再次接管同名源。
    }
    if (id.empty()) {
        if (!std::filesystem::exists(source)) return {};
        {
            sqlite::Database db(source,true);inspect(db,protocol);
        }
        id=RecordVolume::newIdentity();
        sqlite::Database db(index);db.exec("PRAGMA synchronous=FULL");
        write(db,"legacy_migration",id);
        if (checkpoint) checkpoint(LegacyMigrationPhase::IntentCommitted);
    }
    checkIdentity(id);
    const auto target=root/("vol-"+id)/"records.sqlite";
    checkPath(target.parent_path());checkPath(target);
    if (std::filesystem::exists(source)) {
        if (std::filesystem::exists(target)) throw std::runtime_error("both legacy source and migration target exist");
        for (const auto* suffix:{"-wal","-shm","-journal"}) {
            auto path=source;path+=suffix;checkPath(path);
        }
        owner(target.parent_path(),id);
        {
            sqlite::Database db(source);
            const auto summary=inspect(db,protocol);
            const auto oldIdentity=read(db,"volume_id");
            if (!oldIdentity.empty() && oldIdentity!=id) throw std::runtime_error("legacy database belongs to another volume");
            if (summary.count && session.state().lastCommittedId>summary.last)
                throw std::runtime_error("legacy database is behind durable session position");
            if (summary.lastTime) session.committed(summary.last,*summary.lastTime);
            if (session.state().run==0 && summary.recording) session.start();
            db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
            try {
                write(db,"volume_id",id);write(db,"origin","recording");write(db,"state","sealed");
                write(db,"first_id",std::to_string(summary.first));write(db,"last_id",std::to_string(summary.last));
                write(db,"record_count",std::to_string(summary.count));write(db,"opened_us",std::to_string(nowUs));
                write(db,"sealed_us",std::to_string(nowUs));db.exec("COMMIT");
            } catch (...) {db.exec("ROLLBACK");throw;}
            // 迁移仅在单写者启动阶段执行。检查点/模式切换失败时保留原路径和恢复意图，绝不强移 WAL。
            sqlite::check(sqlite3_wal_checkpoint_v2(db.get(),nullptr,SQLITE_CHECKPOINT_TRUNCATE,nullptr,nullptr),db.get());
            db.exec("PRAGMA journal_mode=DELETE");
        }
        if (checkpoint) checkpoint(LegacyMigrationPhase::FilePrepared);
        std::filesystem::rename(source,target);
        if (checkpoint) checkpoint(LegacyMigrationPhase::FileMoved);
    }
    if (!std::filesystem::exists(target)) throw std::runtime_error("legacy migration lost both source and target");
    auto volume=RecordVolume::reopen(target,protocol);
    if (!volume->info().sealed) throw std::runtime_error("legacy migration target is not sealed");
    const auto info=volume->info();volume.reset();
    std::optional<CatalogVolume> registered;
    {
        auto volumes=catalog.pinAll();
        for (const auto& item:volumes->volumes()) if (item.identity==id) registered=item;
    }
    if (!registered) registered=catalog.adoptRecording(info,nowUs);
    if (checkpoint) checkpoint(LegacyMigrationPhase::Indexed);
    {
        sqlite::Database db(index);db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
        try {
            write(db,"legacy_migrated",id);
            db.exec("DELETE FROM metadata WHERE key='legacy_migration'; COMMIT");
        } catch (...) {db.exec("ROLLBACK");throw;}
    }
    return registered;
}
} // namespace protoscope::storage
