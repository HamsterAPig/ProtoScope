#include "../src/storage/volume_catalog.hpp"
#include "protoscope/storage/store.hpp"
#include "protoscope/data/psrec.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>
#include <set>

namespace {
using namespace protoscope;
using tests::require;
const data::Schema schema{"samples",{{"n",data::FieldType::Int64,false}}};
storage::Completion complete(storage::Store& store,std::uint64_t task)
{
    store.waitIdle();
    for (auto& event:store.poll()) if (event.task==task) return event;
    throw std::runtime_error("missing cross-volume completion");
}
void addVolume(storage::VolumeCatalog& catalog,const std::filesystem::path& root,
               const data::Schema& definition,const std::vector<data::Value>& values)
{
    const auto source=root.parent_path()/"source.psrec";
    {
        std::ofstream file(source,std::ios::binary);
        data::PsrecWriter writer(file,{{9,definition}});
        for (const auto& value:values) writer.append({"archive","samples","device",100,{},9,{value}});
        writer.finish();
    }
    auto staged=storage::stageRecordImport(root,"protocol",source,storage::ImportFormat::Psrec);
    catalog.adopt(staged,100);
}
void crossVolumeSnapshot()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-cross-volume"));
    const auto root=directory.path()/"records";
    std::optional<std::int64_t> oldToken;
    {
        storage::Store store(directory.path(),"protocol",{schema});
        require(complete(store,store.start()).ok,"start active recording");
        std::string error;
        require(store.publish({{"protocol","samples","device",100,{},0,{{std::int64_t{2}}}},
                               {"protocol","samples","device",100,{},0,{{std::int64_t{4}}}}},error),"active rows");
        store.waitIdle();
        storage::VolumeCatalog catalog(root,"protocol");
        addVolume(catalog,root,schema,{{std::int64_t{1}},{std::int64_t{3}},{std::int64_t{5}}});
        storage::Query query;query.sort=data::FieldSort{"n",false};query.limit=2;
        const auto first=complete(store,store.query(query));
        require(first.ok && first.records.size()==2 && first.more && first.snapshotLease,"first merged page");
        require(std::get<std::int64_t>(first.records[0].values[0].value)==1 &&
                std::get<std::int64_t>(first.records[1].values[0].value)==2,"global sort before page limit");
        oldToken=first.snapshot;
        require(store.publish({{"protocol","samples","device",100,{},0,{{std::int64_t{6}}}}},error),"new active row");
        store.waitIdle();
        addVolume(catalog,root,schema,{{std::int64_t{0}},{std::int64_t{7}}});
        query.snapshot=first.snapshot;query.limit=100;
        const auto frozen=complete(store,store.query(query));
        require(frozen.ok && frozen.records.size()==5,"old snapshot excludes new volume and active append");
        for (std::size_t i=0;i<5;++i)
            require(std::get<std::int64_t>(frozen.records[i].values[0].value)==static_cast<std::int64_t>(i+1),"fixed cross-volume ordering");
        query.snapshot.reset();
        const auto refreshed=complete(store,store.query(query));
        require(refreshed.ok && refreshed.records.size()==8,"refresh includes all sources");
        std::set<std::uint64_t> ids(refreshed.rowIds.begin(),refreshed.rowIds.end());
        require(ids.size()==8,"stable global IDs do not collide across volumes");
        for (std::size_t i=0;i<8;++i)
            require(std::get<std::int64_t>(refreshed.records[i].values[0].value)==static_cast<std::int64_t>(i),"fresh global order");
        const data::Schema changed{"samples",{{"n",data::FieldType::String,false}}};
        addVolume(catalog,root,changed,{{std::string("eight")}});
        auto mixed=complete(store,store.query(query));
        require(mixed.ok && mixed.records.size()==9 && mixed.schemas.size()==2,"old schema versions remain readable");
        require(std::get<std::string>(mixed.records.back().values[0].value)=="eight","typed ordering across schema versions");
        query.snapshot=first.snapshot;
        const auto path=directory.path()/"frozen.psrec";
        const auto exported=complete(store,store.exportRecords(path,storage::ExportFormat::Psrec,query));
        require(exported.ok && exported.processed==5,"export shares cross-volume frozen snapshot");
        std::ifstream file(path,std::ios::binary);data::PsrecReader reader(file);
        for (int i=1;i<=5;++i) {
            const auto record=reader.next();
            require(record && std::get<std::int64_t>(record->values[0].value)==i,"exported merge order");
        }
        require(!reader.next(),"complete merged archive");
    }
    storage::Store reopened(directory.path(),"protocol",{schema});
    storage::Query old;old.snapshot=oldToken;
    require(!complete(reopened,reopened.query(old)).ok,"old runtime snapshot cannot silently become new data");
    const auto restored=complete(reopened,reopened.query({}));
    require(restored.ok && restored.records.size()==9,"reopened query restores registered imported volumes");
}
}
int main()
{
    try {crossVolumeSnapshot();std::cout<<"[PASS] cross_volume_snapshot\n";return 0;}
    catch(const std::exception& error){std::cerr<<"[FAIL] "<<error.what()<<'\n';return 1;}
}
