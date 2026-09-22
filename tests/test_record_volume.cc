#include "../src/storage/record_volume.hpp"
#include "../src/storage/sqlite_database.hpp"
#include "../src/storage/volume_catalog.hpp"
#include "../src/storage/record_query.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <fstream>
#include <iostream>

namespace {
using namespace protoscope;
using tests::require;
const data::Schema schema{"samples",{{"n",data::FieldType::Int64,false}}};
data::Record row(std::int64_t n) {return {"protocol","samples","device",100,{},7,{{n}}};}
template<class F> void rejects(F&& run)
{
    bool failed=false;try {run();} catch(const std::exception&) {failed=true;}
    require(failed,"invalid record volume operation rejected");
}
void stablePathAndRotation()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-record-volume"));
    auto volume=storage::RecordVolume::create(directory.path(),"protocol",{{7,schema}},1000,1);
    require(!volume->rotationDue(86400000000LL,1),"empty volume does not rotate repeatedly");
    volume->append({row(1)});
    const auto path=volume->info().path;
    require(volume->info().lastId==1000 && volume->info().records==1,"global ID range starts at assigned position");
    require(volume->rotationDue(86400000000LL),"UTC day transition rotates occupied volume");
    require(!volume->rotationDue(0) && !volume->rotationDue(1),"clock regression does not repeatedly rotate");
    require(volume->rotationDue(1,1) && volume->diskBytes()>0,"capacity includes active WAL");
    storage::sqlite::Database reader(path,true);
    reader.exec("BEGIN");
    {storage::sqlite::Statement first(reader,"SELECT count(*) FROM records");first.row();require(first.integer(0)==1,"reader snapshot established");}
    volume->append({row(2)});
    const auto began=std::chrono::steady_clock::now();
    volume->seal(2);
    require(std::chrono::steady_clock::now()-began<std::chrono::seconds(2),"sealing does not wait for read transaction");
    require(volume->info().path==path && std::filesystem::exists(path) && volume->info().sealed,
            "seal preserves source path while readers remain active");
    {storage::sqlite::Statement first(reader,"SELECT count(*) FROM records");first.row();require(first.integer(0)==1,"old SQLite read snapshot unchanged");}
    auto sealed=storage::RecordVolume::reopen(path,"protocol");
    require(sealed->info().records==2 && sealed->info().lastId==1001,"new reader sees committed WAL after seal");
    require(!volume->rotationDue(86400000000LL,1),"sealed volume cannot rotate twice");
    rejects([&]{volume->append({row(3)});});
    reader.exec("COMMIT");
    volume.reset();sealed.reset();
    const auto reopened=storage::RecordVolume::reopen(path,"protocol");
    require(reopened->info().sealed && reopened->info().records==2,"sealed state survives connection close");
}
void atomicAppendAndRecovery()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-record-volume-recovery"));
    auto volume=storage::RecordVolume::create(directory.path(),"protocol",{{7,schema}},1,123);
    volume->append({row(1)});
    auto invalid=row(2);invalid.values[0]={std::string("not an integer")};
    rejects([&]{volume->append({row(2),invalid});});
    require(volume->info().records==1,"invalid batch cannot partly advance counts");
    rejects([&]{volume->append({row(2)},1);});
    require(volume->info().lastId==1,"ID exhaustion does not consume position");
    const auto path=volume->info().path;
    volume.reset();
    volume=storage::RecordVolume::reopen(path,"protocol");
    require(!volume->info().sealed && volume->info().records==1 && volume->info().openedAtUs==123,
            "active committed metadata survives reopen");
    volume->append({row(2)});
    require(volume->info().lastId==2,"recovered writer continues assigned ID range");
    rejects([&]{storage::RecordVolume::reopen(path,"other-protocol");});
    auto edge=storage::RecordVolume::create(directory.path(),"protocol",{{7,schema}},INT64_MAX,1);
    edge->append({row(1)});
    require(edge->info().lastId==INT64_MAX,"maximum int64 record ID preserved");
    rejects([&]{edge->append({row(2)});});
}
void ownershipAndCorruption()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-record-volume-owner"));
    auto volume=storage::RecordVolume::create(directory.path(),"protocol",{{7,schema}},1,1);
    const auto path=volume->info().path;
    volume.reset();
    {storage::sqlite::Database db(path);db.exec("UPDATE metadata SET value='-1' WHERE key='record_count'");}
    rejects([&]{storage::RecordVolume::reopen(path,"protocol");});
    {std::ofstream owner(path.parent_path()/"owner");owner<<"foreign";}
    rejects([&]{storage::RecordVolume::reopen(path,"protocol");});
    require(std::filesystem::exists(path),"invalid owner never causes automatic deletion");
}
void catalogHandoff()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-record-volume-catalog"));
    const auto root=directory.path()/"records";
    storage::Store store(directory.path(),"protocol",{schema});
    storage::VolumeCatalog catalog(root,"protocol");
    auto volume=storage::RecordVolume::create(root,"protocol",{{7,schema}},1000,1);
    volume->append({row(1),row(2)});
    auto activePin=std::make_shared<int>(0);
    volume->seal(2);
    const auto registered=catalog.adoptRecording(volume->info(),2,activePin);
    require(registered.path==volume->info().path && registered.highWater==1001 &&
            registered.records==2 && catalog.isPinned(registered.id),"seal transfers active lease without moving path");
    const auto retained=catalog.retain({UINT64_MAX,std::chrono::microseconds(0)},3);
    require(retained.removedVolumes==0 && retained.expiredPinned,"active query lease protects newly sealed volume");
    const auto task=store.query({});store.waitIdle();
    auto results=store.poll();
    require(results.size()==1 && results[0].task==task && results[0].ok && results[0].records.size()==2 &&
            results[0].rowIds[0]==1000 && results[0].rowIds[1]==1001,
            "cross-volume query uses max row ID rather than record count");
    rejects([&]{catalog.adoptRecording(volume->info(),2);});
    auto overlap=storage::RecordVolume::create(root,"protocol",{{7,schema}},1001,2);
    overlap->append({row(3)});overlap->seal(3);
    rejects([&]{catalog.adoptRecording(overlap->info(),3);});
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"stable_path_rotation",stablePathAndRotation},{"atomic_recovery",atomicAppendAndRecovery},
        {"ownership_corruption",ownershipAndCorruption},{"catalog_handoff",catalogHandoff}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& error){++failed;std::cerr<<"[FAIL] "<<name<<": "<<error.what()<<'\n';}
    }
    return failed ? 1:0;
}
