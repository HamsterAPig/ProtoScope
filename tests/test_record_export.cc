#include "protoscope/storage/store.hpp"
#include "protoscope/data/psrec.hpp"
#include "protoscope/data/record_csv.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>

namespace {
using namespace protoscope;
using tests::require;
const data::Schema schema{"samples",{{"n",data::FieldType::Int64,false}}};
data::Record row(std::int64_t number)
{
    return {"protocol","samples","device",123,{},0,{{number}}};
}
storage::Completion completion(storage::Store& store,std::uint64_t task)
{
    store.waitIdle();
    for (auto& event:store.poll()) if (event.task==task) return event;
    throw std::runtime_error("missing export completion");
}
struct Fixture {
    tests::ScopedTempPath directory{tests::makeUniqueTempDir("protoscope-record-export")};
    storage::Store store{directory.path(),"protocol",{schema}};
    Fixture() {
        require(completion(store,store.start()).ok,"start recording");
        std::vector<data::Record> rows;
        for (int i=1;i<=2000;++i) rows.push_back(row(i));
        std::string error;require(store.publish(std::move(rows),error),"publish fixtures");
        store.waitIdle();
    }
};
void formatsAndSnapshot()
{
    Fixture f;
    auto initial=completion(f.store,f.store.query({.limit=1}));
    require(initial.ok && initial.snapshot,"query cutoff");
    std::string error;require(f.store.publish({row(3000)},error),"later record");
    f.store.waitIdle();
    storage::Query query;
    query.dataset="samples";query.snapshot=initial.snapshot;
    query.conditions={{"n",data::CompareOp::Greater,{std::int64_t{500}}}};
    query.sort=data::FieldSort{"n",true};query.limit=1;query.offset=99;
    for (const auto format:{storage::ExportFormat::Psrec,storage::ExportFormat::Csv}) {
        const auto path=f.directory.path()/std::filesystem::u8path(
            format==storage::ExportFormat::Psrec ? "测量.psrec":"测量.csv");
        const auto result=completion(f.store,f.store.exportRecords(path,format,query));
        require(result.ok && result.operation=="export" && result.processed==1500 &&
                result.snapshot==initial.snapshot && result.records.empty(),"streamed full filtered snapshot export");
        std::ifstream file(path,std::ios::binary);
        auto verify=[&](auto& reader) {
            for (std::int64_t i=2000;i>500;--i) {
                const auto record=reader.next();
                require(record && std::get<std::int64_t>(record->values[0].value)==i,"export ordering before pagination");
            }
            require(!reader.next(),"export footer count");
        };
        if (format==storage::ExportFormat::Psrec) {data::PsrecReader reader(file);verify(reader);}
        else {data::RecordCsvReader reader(file);verify(reader);}
    }
    require(f.store.status().committed==2001 && !f.store.status().faulted,"export independent of recording counters");
}
void cancellationAndPaths()
{
    Fixture f;
    const auto path=f.directory.path()/"cancel.psrec";
    {std::ofstream file(path);file<<"original";}
    const auto task=f.store.exportRecords(path,storage::ExportFormat::Psrec);
    f.store.cancel(task);
    const auto result=completion(f.store,task);
    require(!result.ok && !result.error.empty(),"queued export canceled");
    {std::ifstream input(path);std::string text;input>>text;require(text=="original","cancel protects original output");}
    const auto fail=completion(f.store,f.store.exportRecords(f.directory.path()/"absent"/"out.csv",storage::ExportFormat::Csv));
    require(!fail.ok && !f.store.status().faulted,"output failure does not stop device recording");
    const auto quota=completion(f.store,f.store.exportRecords(path,storage::ExportFormat::Csv,{}, {64,true}));
    require(!quota.ok && !quota.error.empty(),"export enforces file write quota");
    const auto noOverwrite=completion(f.store,f.store.exportRecords(path,storage::ExportFormat::Psrec,{}, {UINT64_MAX,false}));
    require(!noOverwrite.ok,"overwrite must be explicit when disabled");
    {std::ifstream input(path);std::string text;input>>text;require(text=="original","quota and overwrite failures preserve output");}
    for (const auto& target:{f.directory.path()/"records"/"records.sqlite",
                            f.directory.path()/"records"/"export.psrec",
                            f.directory.path()/"kv"/"values.sqlite"}) {
        bool rejected=false;
        try {f.store.exportRecords(target,storage::ExportFormat::Psrec);} catch(const std::exception&) {rejected=true;}
        require(rejected,"export cannot overwrite storage directories");
    }
    for (const auto& entry:std::filesystem::directory_iterator(f.directory.path()))
        require(entry.path().filename().string().find(".tmp-")==std::string::npos,"no abandoned export temporary file");
}
void boundedLiveExport()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-live-export"));
    storage::Store store(directory.path(),"protocol",{schema});
    auto record=row(INT64_MAX);record.schemaVersion=1;
    const auto path=directory.path()/"live.psrec";
    auto event=completion(store,store.exportRows(path,storage::ExportFormat::Psrec,{record},{{1,schema}}));
    require(event.ok && event.processed==1 && store.status().committed==0,"live export does not require or alter recording");
    {std::ifstream input(path,std::ios::binary);data::PsrecReader reader(input);
     require(std::get<std::int64_t>(reader.next()->values[0].value)==INT64_MAX && !reader.next(),"live export preserves typed int64");}
    const auto task=store.exportRows(path,storage::ExportFormat::Csv,std::vector<data::Record>(1000,record),{{1,schema}});
    store.cancel(task);
    event=completion(store,task);
    require(!event.ok && !event.error.empty(),"live export cancellation reports failure");
    {std::ifstream input(path,std::ios::binary);data::PsrecReader reader(input);
     require(reader.next().has_value() && !reader.next(),"cancel leaves prior export untouched");}
    for (int attempt=0;attempt<2;++attempt) {
        event=completion(store,store.exportRows(path,storage::ExportFormat::Csv,{record},{{1,schema}},{1,true}));
        require(!event.ok,"failed live exports release task memory budget");
    }
    bool rejected=false;
    try {store.exportRows(path,storage::ExportFormat::Csv,std::vector<data::Record>(1001,record),{{1,schema}});}
    catch(const std::exception&) {rejected=true;}
    require(rejected,"live export bounded to 1000 rows");
    storage::Config limits;limits.queueBytes=128;
    storage::Store limited(directory.path()/"limited","protocol",{schema},limits);
    rejected=false;
    try {limited.exportRows(directory.path()/"limit.csv",storage::ExportFormat::Csv,{record},{{1,schema}});}
    catch(const std::exception&) {rejected=true;}
    require(rejected,"live export obeys aggregate memory budget");
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"formats_snapshot",formatsAndSnapshot},{"cancellation_paths",cancellationAndPaths},
        {"bounded_live_export",boundedLiveExport}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"[FAIL] "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
