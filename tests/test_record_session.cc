#include "../src/storage/record_session.hpp"
#include "../src/storage/record_volume.hpp"
#include "../src/storage/volume_catalog.hpp"
#include "../src/storage/sqlite_database.hpp"
#include "test_helpers.hpp"
#include <iostream>

namespace {
using namespace protoscope;
using tests::require;
template<class F> void rejects(F&& run)
{
    bool failed=false;try {run();} catch(const std::exception&) {failed=true;}
    require(failed,"invalid record session transition rejected");
}
void recordingIntent()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-record-session"));
    storage::VolumeCatalog catalog(directory.path(),"protocol");
    auto writer=catalog.claimWriter();
    {
        storage::RecordSession session(directory.path(),"protocol");
        require(session.state().cleanExit && !session.state().recording,"new session defaults stopped and clean");
        session.beginRun();session.start();
        session.committed(INT64_MAX,123);
        session.fault("disk full",124,129);
        const auto state=session.state();
        require(state.recording && state.faulted && state.lastCommittedId==INT64_MAX &&
                state.interruptedFromUs==124 && state.interruptedToUs==129,"fault preserves desired recording intent and committed boundary");
        // 不标记 cleanExit，模拟上次进程异常终止。
    }
    {
        storage::RecordSession recovered(directory.path(),"protocol");
        require(!recovered.state().cleanExit && recovered.state().recording &&
                recovered.state().lastCommittedId==INT64_MAX,"restart restores committed intent and position");
        recovered.beginRun();
        require(recovered.state().abnormalRuns==1 && recovered.state().run==2,"abnormal exit marker remains durable");
        recovered.stop();recovered.cleanExit();
    }
    storage::RecordSession stopped(directory.path(),"protocol");
    require(stopped.state().cleanExit && !stopped.state().recording && stopped.state().abnormalRuns==1,
            "explicit stop prevents automatic recording after restart without erasing abnormal history");
    stopped.beginRun();
    require(stopped.state().abnormalRuns==1,"clean restart does not add false crash marker");
    stopped.start();
    require(stopped.state().session==2 && !stopped.state().faulted,"new explicit session clears fault status");
    rejects([&]{stopped.committed(0,0);});
    require(stopped.state().lastCommittedId==INT64_MAX,"invalid transition cannot change committed cache");
    rejects([&]{storage::RecordSession other(directory.path(),"other");});
}
void rotationIntent()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-session-rotation"));
    storage::VolumeCatalog catalog(directory.path(),"protocol");
    auto writer=catalog.claimWriter();
    const auto identity=storage::RecordVolume::newIdentity();
    {
        storage::RecordSession session(directory.path(),"protocol");session.beginRun();session.start();
        session.prepareRotation(identity,1,123);
        require(session.state().activeIdentity.empty() && session.state().pendingIdentity==identity,
                "pending target cannot become query source before pointer commit");
        rejects([&]{session.prepareRotation(storage::RecordVolume::newIdentity(),1,124);});
        rejects([&]{session.commitRotation("wrong");});
        rejects([&]{session.committed(1,123);});
    }
    storage::RecordSession recovering(directory.path(),"protocol");
    require(recovering.state().pendingIdentity==identity && recovering.state().pendingFirstId==1,
            "crash before file preparation retains reserved identity");
    const data::Schema schema{"samples",{{"n",data::FieldType::Int64,false}}};
    auto volume=storage::RecordVolume::create(directory.path(),"protocol",{{1,schema}},1,123,identity);
    rejects([&]{storage::RecordVolume::create(directory.path(),"protocol",{{1,schema}},1,123,identity);});
    recovering.commitRotation(identity);
    require(recovering.state().activeIdentity==identity && recovering.state().pendingIdentity.empty(),
            "pointer commit atomically clears pending identity");
    volume->append({{"protocol","samples","device",123,{},1,{{std::int64_t{42}}}}});
    recovering.committed(volume->info().lastId,123);
    const auto next=storage::RecordVolume::newIdentity();
    recovering.prepareRotation(next,2,124);
    storage::RecordSession afterCrash(directory.path(),"protocol");
    require(afterCrash.state().activeIdentity==identity && afterCrash.state().pendingIdentity==next &&
            afterCrash.state().lastCommittedId==1,"crash keeps old active pointer alongside intended replacement");
    afterCrash.cancelRotation(next);
    require(afterCrash.state().activeIdentity==identity && afterCrash.state().pendingIdentity.empty(),
            "canceling uncommitted replacement leaves old source intact");
}
void damagedState()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-session-damage"));
    storage::VolumeCatalog catalog(directory.path(),"protocol");
    auto writer=catalog.claimWriter();
    storage::RecordSession session(directory.path(),"protocol");session.beginRun();
    rejects([&]{session.fault("bad range",2,1);});
    rejects([&]{session.prepareRotation("../outside",1,1);});
    require(!session.state().faulted && session.state().pendingIdentity.empty(),"validation precedes durable mutation");
    {storage::sqlite::Database db(directory.path()/"index.sqlite");
     db.exec("UPDATE metadata SET value=X'00' WHERE key='record_session'");}
    rejects([&]{storage::RecordSession corrupted(directory.path(),"protocol");});
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"recording_intent",recordingIntent},{"rotation_intent",rotationIntent},{"damaged_state",damagedState}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& error){++failed;std::cerr<<"[FAIL] "<<name<<": "<<error.what()<<'\n';}
    }
    return failed ? 1:0;
}
