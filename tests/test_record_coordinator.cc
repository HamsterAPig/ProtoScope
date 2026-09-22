#include "../src/storage/record_volume_coordinator.hpp"
#include "test_helpers.hpp"

#include <iostream>

namespace {
using namespace protoscope;
using tests::require;
const data::Schema schema{"samples",{{"n",data::FieldType::Int64,false}}};
data::Record row(const storage::RecordVolumeCoordinator& volumes,std::int64_t n)
{
    return {"protocol","samples","device",100,{},volumes.schemaIds().at("samples"),{{n}}};
}
void rotationAndSnapshots()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-coordinator"));
    storage::VolumeCatalog catalog(directory.path(),"protocol");
    storage::RecordSession session(directory.path(),"protocol");
    storage::RecordVolumeCoordinator volumes(directory.path(),"protocol",{schema},catalog,session,1);
    storage::RecordQueryService queries(volumes.info().path,catalog,32U*1024U*1024U);
    volumes.append({row(volumes,1),row(volumes,2)},1,UINT64_MAX,&queries);
    auto old=queries.query({.limit=1},{});
    const auto firstPath=volumes.info().path;
    volumes.append({row(volumes,3)},2,1,&queries);
    require(volumes.info().path!=firstPath && volumes.info().lastId==3,"capacity rotation preserves global IDs");
    const auto page=queries.query({.offset=1,.limit=1,.snapshot=old.snapshot},{});
    require(page.rowIds==std::vector<std::uint64_t>{2} && !page.more,"old snapshot excludes post-rotation rows");
    volumes.append({row(volumes,4)},86400000000LL,UINT64_MAX,&queries);
    const auto fresh=queries.query({},{});
    require(fresh.rowIds==std::vector<std::uint64_t>{1,2,3,4},"duplicate timestamps survive cross-day multi-volume query");
    require(catalog.pinAll()->volumes().size()==2,"capacity and UTC day each seal one volume");
    volumes.synchronize();
    require(session.state().lastCommittedId==4,"session reconciles committed data");
}
void pendingRecovery()
{
    for (int stage=0;stage<3;++stage) {
        tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-coordinator-recovery"));
        storage::VolumeCatalog catalog(directory.path(),"protocol");
        storage::RecordSession session(directory.path(),"protocol");
        std::filesystem::path source;
        std::uint64_t schemaId=0;
        {
            storage::RecordVolumeCoordinator volumes(directory.path(),"protocol",{schema},catalog,session,1);
            volumes.append({row(volumes,42)},1,UINT64_MAX);
            source=volumes.info().path;schemaId=volumes.schemaIds().at("samples");
            // 模拟记录提交后会话位置尚未保存，重开必须从活动卷补齐。
        }
        {
            storage::RecordVolumeCoordinator recovered(directory.path(),"protocol",{schema},catalog,session,2);
            require(session.state().lastCommittedId==1,"recover committed volume ahead of session index");
        }
        const auto target=storage::RecordVolume::newIdentity();
        session.prepareRotation(target,2,3);
        if (stage>=1) {
            auto prepared=storage::RecordVolume::create(directory.path(),"protocol",{{schemaId,schema}},2,3,target);
        }
        if (stage>=2) {
            auto previous=storage::RecordVolume::reopen(source,"protocol");
            previous->seal(3);catalog.adoptRecording(previous->info(),3);
        }
        storage::RecordVolumeCoordinator recovered(directory.path(),"protocol",{schema},catalog,session,4);
        require(recovered.info().identity==target && session.state().pendingIdentity.empty(),"pending rotation completes idempotently");
        recovered.append({row(recovered,43)},4,UINT64_MAX);
        storage::RecordQueryService queries(recovered.info().path,catalog,32U*1024U*1024U);
        require(queries.query({},{}).rowIds==std::vector<std::uint64_t>{1,2},"recovery neither duplicates nor loses source rows");
    }
}
void restartRecording()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-coordinator-restart"));
    storage::VolumeCatalog catalog(directory.path(),"protocol");
    storage::RecordSession session(directory.path(),"protocol");
    std::filesystem::path previous;
    {
        storage::RecordVolumeCoordinator volumes(directory.path(),"protocol",{schema},catalog,session,1);
        previous=volumes.info().path;
        volumes.append({row(volumes,1)},1,UINT64_MAX);
    }
    storage::RecordVolumeCoordinator resumed(directory.path(),"protocol",{schema},catalog,session,2,true);
    require(resumed.info().path!=previous && resumed.info().lastId==1 && resumed.info().records==0,
            "recording restart opens a new volume without losing committed position");
}
void storeRotationAndHandoff()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-store-rotation"));
    storage::Config config;
    config.maxVolumeBytes=1;
    storage::Store store(directory.path(),"protocol",{schema},config);
    store.start();store.waitIdle();store.poll();
    const auto publish=[&](storage::Store& target,std::int64_t n) {
        std::string error;
        require(target.publish({{"protocol","samples","device",100,{},0,{{n}}}},error),"store accepts record");
        target.waitIdle();
        require(!target.status().faulted,"store commits record without rotation fault");
    };
    publish(store,1);
    store.query({});store.waitIdle();
    auto snapshot=store.poll().at(0);
    publish(store,2);
    require(store.status().committed==2 && store.status().lastCommittedId==2,"store counts both committed volumes");
    store.suspend();
    {
        storage::Store replacement(directory.path(),"protocol",{schema},config);
        require(replacement.status().recovered && replacement.status().lastCommittedId==2,
                "replacement restores recording intent and position");
        publish(replacement,3);
        replacement.stop();replacement.waitIdle();replacement.poll();
    }
    store.resume();
    store.query({.snapshot=snapshot.snapshot});store.waitIdle();
    auto old=store.poll();
    require(old.size()==1 && old[0].ok && old[0].rowIds==std::vector<std::uint64_t>{1},
            "suspended store keeps original fixed snapshot after replacement rotates");
    store.query({});store.waitIdle();
    auto fresh=store.poll();
    require(fresh.size()==1 && fresh[0].ok && fresh[0].rowIds==std::vector<std::uint64_t>{1,2,3},
            "resumed store refresh sees all committed volumes");
    require(!store.status().recording,"resume honors stop committed by replacement");
}
void schemaChangeAndFailedSwitch()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-coordinator-schema"));
    storage::VolumeCatalog catalog(directory.path(),"protocol");
    storage::RecordSession session(directory.path(),"protocol");
    std::filesystem::path source;
    {
        storage::RecordVolumeCoordinator volumes(directory.path(),"protocol",{schema},catalog,session,1);
        volumes.append({row(volumes,1)},1,UINT64_MAX);
        source=volumes.info().path;
    }
    storage::RecordQueryService queries(source,catalog,32U*1024U*1024U);
    auto old=queries.query({},{});
    auto changed=schema;
    changed.fields.push_back({"ready",data::FieldType::Bool,false});
    storage::RecordVolumeCoordinator volumes(directory.path(),"protocol",{changed},catalog,session,2);
    require(volumes.info().path!=source,"schema change opens a new versioned volume");
    // 旧查询服务尚未切换时，旧源已经登记封存，新快照也不能重复纳入同一个文件。
    require(queries.query({},{}).rowIds==std::vector<std::uint64_t>{1},"partially switched source is not duplicated");
    queries.switchActive(volumes.info().path,[](std::shared_ptr<int>){});
    volumes.append({{"protocol","samples","device",100,{},volumes.schemaIds().at("samples"),
                     {{std::int64_t{2}},{true}}}},2,UINT64_MAX,&queries);
    const auto result=queries.query({},{});
    require(result.records.size()==2 && result.records[0].schemaVersion!=result.records[1].schemaVersion,
            "old and new schemas remain independently readable");
    require(queries.query({.snapshot=old.snapshot},{}).records.size()==1,"old schema snapshot remains frozen");
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"rotation_snapshots",rotationAndSnapshots},{"pending_recovery",pendingRecovery},
        {"restart_recording",restartRecording},{"store_rotation_handoff",storeRotationAndHandoff},
        {"schema_change_partial_switch",schemaChangeAndFailedSwitch}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& error) {++failed;std::cerr<<"[FAIL] "<<name<<": "<<error.what()<<'\n';}
    }
    return failed ? 1:0;
}
