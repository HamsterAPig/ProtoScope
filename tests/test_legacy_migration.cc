#include "../src/storage/legacy_record_migration.hpp"
#include "../src/storage/record_query.hpp"
#include "../src/storage/sqlite_database.hpp"
#include "protoscope/storage/store.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>

namespace {
using namespace protoscope;
using tests::require;
const data::Schema schema{"samples",{{"n",data::FieldType::Int64,false}}};
void fixture(const std::filesystem::path& root)
{
    std::filesystem::create_directories(root/"records");
    storage::sqlite::Database db(root/"records"/"records.sqlite");
    db.exec("PRAGMA application_id=1347634242; PRAGMA user_version=1;"
        "CREATE TABLE metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
        "INSERT INTO metadata VALUES('protocol','protocol'),('recording','1');"
        "CREATE TABLE schemas(id INTEGER PRIMARY KEY,dataset TEXT NOT NULL,definition BLOB NOT NULL);"
        "CREATE TABLE records(id INTEGER PRIMARY KEY AUTOINCREMENT,dataset TEXT NOT NULL,"
        "device TEXT NOT NULL,received_us INTEGER NOT NULL,schema_id INTEGER NOT NULL,payload BLOB NOT NULL);");
    storage::sqlite::Statement definition(db,"INSERT INTO schemas VALUES(1,'samples',?)");
    definition.blob(1,data::encodeValue(data::schemaValue(schema)));definition.row();
    for (const std::int64_t n:{std::int64_t{42},INT64_MAX}) {
        data::Record record{"protocol","samples","device",123,{},1,{{n}}};
        storage::sqlite::Statement insert(db,"INSERT INTO records(dataset,device,received_us,schema_id,payload) "
                                            "VALUES('samples','device',123,1,?)");
        insert.blob(1,data::encodeValue(data::recordValue(record)));insert.row();
    }
}
template<class F> void rejects(F&& run)
{
    bool failed=false;try {run();} catch(const std::exception&) {failed=true;}
    require(failed,"unsafe or interrupted legacy migration rejected");
}
void phaseRecovery()
{
    for (const auto phase:{storage::LegacyMigrationPhase::IntentCommitted,storage::LegacyMigrationPhase::FilePrepared,
                          storage::LegacyMigrationPhase::FileMoved,storage::LegacyMigrationPhase::Indexed}) {
        tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-legacy-migration"));
        fixture(directory.path());
        const auto root=directory.path()/"records";
        {
            storage::VolumeCatalog catalog(root,"protocol");
            auto writer=catalog.claimWriter();
            storage::RecordSession session(root,"protocol");
            rejects([&]{storage::migrateLegacyRecords(root,"protocol",catalog,session,100,[&](auto current) {
                if (current==phase) throw std::runtime_error("simulated interruption");
            });});
            require(storage::hasLegacyMigration(root),"interruption retains durable migration marker");
        }
        storage::VolumeCatalog catalog(root,"protocol");
        auto writer=catalog.claimWriter();
        storage::RecordSession session(root,"protocol");
        const auto migrated=storage::migrateLegacyRecords(root,"protocol",catalog,session,101);
        require(migrated && migrated->records==2 && migrated->highWater==2 &&
                !std::filesystem::exists(root/"records.sqlite"),"resume preserves committed rows and removes legacy pathname by rename");
        require(session.state().recording && session.state().lastCommittedId==2,"migration preserves unstopped intent and position");
        const auto again=storage::migrateLegacyRecords(root,"protocol",catalog,session,102);
        require(again && again->identity==migrated->identity && catalog.pinAll()->volumes().size()==1,
                "completed migration is idempotent");
        auto active=storage::RecordVolume::create(root,"protocol",{{1,schema}},3,102);
        storage::RecordQueryService query(active->info().path,catalog,32U*1024U*1024U);
        const auto result=query.query({},{});
        require(result.records.size()==2 && result.rowIds==std::vector<std::uint64_t>{1,2} &&
                std::get<std::int64_t>(result.records.back().values[0].value)==INT64_MAX,
                "migrated history keeps IDs and int64 payload unchanged");
    }
}
void rejectForeignAndCollision()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-legacy-migration-identity"));
    fixture(directory.path());
    const auto root=directory.path()/"records";
    storage::VolumeCatalog catalog(root,"protocol");
    auto writer=catalog.claimWriter();
    storage::RecordSession session(root,"protocol");
    rejects([&]{storage::migrateLegacyRecords(root,"foreign",catalog,session,100);});
    require(std::filesystem::exists(root/"records.sqlite"),"wrong protocol leaves original file intact");
    rejects([&]{storage::migrateLegacyRecords(root,"protocol",catalog,session,100,[&](auto phase) {
        if (phase!=storage::LegacyMigrationPhase::IntentCommitted) return;
        storage::sqlite::Database db(root/"index.sqlite",true);
        storage::sqlite::Statement id(db,"SELECT value FROM metadata WHERE key='legacy_migration'");id.row();
        const auto target=root/("vol-"+id.text(0));
        std::filesystem::create_directories(target);
        std::ofstream foreign(target/"records.sqlite");foreign<<"foreign";
    });});
    require(std::filesystem::exists(root/"records.sqlite"),"occupied destination cannot overwrite original or target");
    storage::sqlite::Database db(root/"index.sqlite",true);
    storage::sqlite::Statement id(db,"SELECT value FROM metadata WHERE key='legacy_migration'");id.row();
    require(std::filesystem::file_size(root/("vol-"+id.text(0))/"records.sqlite")==7,"foreign target unchanged");
}
void storeMigratesOnLoad()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-legacy-store"));
    fixture(directory.path());
    storage::Store store(directory.path(),"protocol",{schema});
    require(store.status().recording && store.status().lastCommittedId==2,"store resumes legacy recording");
    store.query({});store.waitIdle();
    auto results=store.poll();
    require(results.size()==1 && results[0].ok && results[0].rowIds==std::vector<std::uint64_t>{1,2},
            "store exposes migrated history");
    require(!std::filesystem::exists(directory.path()/"records"/"records.sqlite"),"store no longer writes legacy path");
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"phase_recovery",phaseRecovery},{"identity_collision",rejectForeignAndCollision},
        {"store_migrates_on_load",storeMigratesOnLoad}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& error){++failed;std::cerr<<"[FAIL] "<<name<<": "<<error.what()<<'\n';}
    }
    return failed ? 1:0;
}
