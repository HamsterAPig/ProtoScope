#include "../src/storage/record_import.hpp"
#include "../src/storage/sqlite_database.hpp"
#include "protoscope/data/psrec.hpp"
#include "protoscope/storage/store.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>

namespace {
using namespace protoscope;
using tests::require;
const data::Schema schema{"samples",{{"n",data::FieldType::Int64,false}}};
void sourceFile(const std::filesystem::path& path,bool csv=false)
{
    std::ofstream file(path,std::ios::binary);
    const auto append=[](auto& writer) {
        for (std::int64_t i=0;i<1500;++i)
            writer.append({"source-protocol","samples","device",i%3,{},9,{{i}}});
        writer.finish();
    };
    if (csv) {data::RecordCsvWriter writer(file,{{9,schema}});append(writer);}
    else {data::PsrecWriter writer(file,{{9,schema}});append(writer);}
}
void verify(const storage::ImportedVolumeInfo& info)
{
    require(info.records==1500 && info.fromUs==0 && info.toUs==2,"staged count and repeated timestamp range");
    storage::sqlite::Database db(info.path,true);
    storage::sqlite::Statement state(db,"SELECT value FROM metadata WHERE key='state'");
    require(state.row() && state.text(0)=="sealed","only fully validated import sealed");
    storage::sqlite::Statement records(db,"SELECT payload FROM records ORDER BY id");
    for (std::int64_t i=0;i<1500;++i) {
        require(records.row(),"all staged records present");
        const auto record=data::recordFromValue(data::decodeValue(records.blob(0)));
        require(record.protocol=="source-protocol" && record.schemaVersion==9 &&
                std::get<std::int64_t>(record.values[0].value)==i,"preserve source provenance and schema");
    }
    require(!records.row(),"no duplicate staged records");
}
void formatsAndIsolation()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-stage-import"));
    storage::Store store(directory.path()/"store","target-protocol",{schema});
    for (const bool csv:{false,true}) {
        const auto source=directory.path()/(csv ? "source.csv":"source.psrec");
        sourceFile(source,csv);
        std::filesystem::path stagePath;
        {
            auto staged=storage::stageRecordImport(directory.path()/"store"/"records","target-protocol",source,
                csv ? storage::ImportFormat::Csv:storage::ImportFormat::Psrec);
            verify(staged.info());stagePath=staged.info().path;
            store.query({});store.waitIdle();
            const auto results=store.poll();
            require(results.size()==1 && results[0].records.empty(),"unregistered staging not visible to queries");
            require(store.status().received==0 && store.status().committed==0,"import staging does not publish live records");
        }
        require(!std::filesystem::exists(stagePath),"unregistered artifact automatically cleaned");
    }
    const auto plain=directory.path()/"plain.csv";
    {std::ofstream file(plain);file<<"received_at_us,n\n10,42\n";}
    data::CsvImportMapping mapping;mapping.schema=schema;mapping.protocol="source-protocol";
    auto staged=storage::stageRecordImport(directory.path()/"store"/"records","target-protocol",plain,
        storage::ImportFormat::MappedCsv,mapping);
    require(staged.info().records==1 && staged.info().fromUs==10,"plain CSV staged through declared mapping");
}
void failureAndOwnership()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-stage-failure"));
    const auto root=directory.path()/"records",source=directory.path()/"source.psrec";
    sourceFile(source);
    const auto rejects=[&](auto&& run) {
        bool failed=false;
        try {run();} catch(const std::exception&) {failed=true;}
        require(failed,"invalid import must fail");
        if (std::filesystem::exists(root/".staging"))
            require(std::filesystem::is_empty(root/".staging"),"failed import leaves no registered or partial artifact");
    };
    std::stop_source stop;
    rejects([&]{storage::stageRecordImport(root,"protocol",source,storage::ImportFormat::Psrec,{}, {},
        stop.get_token(),[&](std::uint64_t count){if(count==1001)stop.request_stop();});});
    rejects([&]{storage::stageRecordImport(root,"protocol",source,storage::ImportFormat::Psrec,{}, {1024*1024,1});});
    rejects([&]{storage::stageRecordImport(root,"protocol",source,storage::ImportFormat::Psrec,{}, {1,1024*1024});});
    {std::fstream file(source,std::ios::in|std::ios::out|std::ios::binary);file.seekp(-1,std::ios::end);file.put('X');}
    rejects([&]{storage::stageRecordImport(root,"protocol",source,storage::ImportFormat::Psrec);});
    sourceFile(source);
    std::filesystem::path retained;
    {
        auto staged=storage::stageRecordImport(root,"protocol",source,storage::ImportFormat::Psrec);
        retained=staged.info().path;
        std::ofstream foreign(retained.parent_path()/"owner");foreign<<"foreign";
    }
    require(std::filesystem::exists(retained),"identity mismatch prevents destructive cleanup");
}

storage::Completion complete(storage::Store& store,std::uint64_t task)
{
    store.waitIdle();
    for (auto& event:store.poll()) if (event.task==task) return event;
    throw std::runtime_error("missing asynchronous import completion");
}
void asynchronousImport()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-async-import"));
    const auto root=directory.path()/"store",source=directory.path()/"source.psrec";
    sourceFile(source);
    {
        storage::Store store(root,"target-protocol",{schema});
        const auto empty=complete(store,store.query({}));
        require(empty.ok && empty.records.empty(),"initial empty snapshot");
        auto imported=complete(store,store.importRecords(source,storage::ImportFormat::Psrec));
        require(imported.ok && imported.operation=="import" && imported.processed==1500 &&
                imported.path==source && imported.records.empty(),"asynchronous import reports committed volume");
        storage::Query query;query.snapshot=empty.snapshot;
        require(complete(store,store.query(query)).records.empty(),"import cannot change frozen query");
        query.snapshot.reset();query.limit=1000;query.sort=data::FieldSort{"n",false};
        const auto first=complete(store,store.query(query));
        require(first.ok && first.records.size()==1000 && first.more &&
                first.records[0].protocol=="source-protocol","registered history preserves provenance");
        query.snapshot=first.snapshot;query.offset=1000;
        const auto second=complete(store,store.query(query));
        require(second.ok && second.records.size()==500 && !second.more &&
                std::get<std::int64_t>(second.records.back().values[0].value)==1499,"import pagination complete");
        const auto canceled=store.importRecords(source,storage::ImportFormat::Psrec);store.cancel(canceled);
        require(!complete(store,canceled).ok,"canceled task never registers partial history");
        const auto quota=complete(store,store.importRecords(source,storage::ImportFormat::Psrec,{}, {1,1024*1024}));
        require(!quota.ok && quota.processed==0,"source limit failure");
        const auto capacity=complete(store,store.importRecords(source,storage::ImportFormat::Psrec,{}, {1024*1024,1}));
        require(!capacity.ok && capacity.processed==0,"staging limit failure");
        {std::fstream file(source,std::ios::in|std::ios::out|std::ios::binary);file.seekp(-1,std::ios::end);file.put('X');}
        require(!complete(store,store.importRecords(source,storage::ImportFormat::Psrec)).ok,"bad final CRC cannot register");
        // 超过队列上限数量的顺序失败任务必须正常释放配额，不阻塞后续查询。
        for (int i=0;i<18;++i)
            require(!complete(store,store.importRecords(directory.path()/"absent",storage::ImportFormat::Csv)).ok,
                    "failed import returns task slot");
        require(!store.status().recording && store.status().received==0 && store.status().committed==0 &&
                !store.status().faulted,"imports leave live recording state and counters unchanged");
        require(std::filesystem::is_empty(root/"records"/".staging"),"failed import staging cleaned");
    }
    storage::Store reopened(root,"target-protocol",{schema});
    storage::Query query;query.offset=1000;query.limit=1000;
    const auto history=complete(reopened,reopened.query(query));
    require(history.ok && history.records.size()==500 && !history.more,"only successful volume survives restart");
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"formats_isolation",formatsAndIsolation},{"failure_ownership",failureAndOwnership},
        {"asynchronous_import",asynchronousImport}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"[FAIL] "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
