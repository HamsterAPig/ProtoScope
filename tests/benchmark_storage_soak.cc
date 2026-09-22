#include "protoscope/storage/store.hpp"
#include "protoscope/data/psrec.hpp"
#include "../src/storage/sqlite_database.hpp"
#include "test_helpers.hpp"

#include <charconv>
#include <fstream>
#include <iostream>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace {
std::uint64_t residentBytes()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters)))
        throw std::runtime_error("cannot sample process working set");
    return counters.WorkingSetSize;
#elif defined(__linux__)
    std::ifstream input("/proc/self/statm");
    std::uint64_t total=0,resident=0;
    if (!(input>>total>>resident)) throw std::runtime_error("cannot sample process resident memory");
    return resident*static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
#else
    return 0;
#endif
}
}
int main(int argc,char** argv)
{
    using namespace protoscope;
    using tests::require;
    int seconds=60;
    if (argc>2) return 2;
    if (argc==2) {
        const std::string_view text(argv[1]);
        const auto parsed=std::from_chars(text.data(),text.data()+text.size(),seconds);
        if (parsed.ec!=std::errc{} || parsed.ptr!=text.data()+text.size() || seconds<10 || seconds>86400) return 2;
    }
    try {
        tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-storage-soak"));
        storage::Config config;
        config.maxVolumeBytes=256U*1024U;
        config.recordMaxBytes=8U*1024U*1024U;
        config.maintenanceInterval=std::chrono::milliseconds(100);
        storage::Store store(directory.path(),"soak",{
            {"sample",{{"sequence",data::FieldType::Int64,false},{"payload",data::FieldType::String,false}}}},config);
        store.start();store.waitIdle();store.poll();
        require(store.status().recording,"soak recording start failed");
        std::map<std::uint64_t,std::uint64_t> previousVolumes;
        std::uint64_t cleaned=0,cleanedVolumes=0,sealedVolumes=0,queried=0,exported=0;
        const auto observe=[&] {
            std::map<std::uint64_t,std::uint64_t> current;
            {
                // 观测索引只持有 SQLite 短读事务，不占用全部数据卷改变被测清理行为。
                storage::sqlite::Database db(directory.path()/"records"/"index.sqlite",true);
                storage::sqlite::Statement volumes(db,"SELECT id,records FROM volumes");
                while (volumes.row()) {
                    const auto id=static_cast<std::uint64_t>(volumes.integer(0));
                    current.emplace(id,static_cast<std::uint64_t>(volumes.integer(1)));
                    if (!previousVolumes.contains(id)) ++sealedVolumes;
                }
            }
            for (const auto& [id,rows]:previousVolumes) if (!current.contains(id)) {
                cleaned+=rows;++cleanedVolumes;
            }
            previousVolumes=std::move(current);
        };
        // 只跟踪当前卷与少量未完成任务，测试工具自身不能随运行时间累积历史。
        std::map<std::uint64_t,std::pair<std::int64_t,std::int64_t>> expected;
        const auto poll=[&] {
            for (const auto& event:store.poll()) {
                require(event.ok,event.error.c_str());
                const auto found=expected.find(event.task);
                if (found==expected.end()) {
                    require(event.operation=="stop","unexpected soak completion");
                    continue;
                }
                auto sequence=found->second.first;
                const auto end=found->second.second;
                const auto check=[&](const data::Record& record) {
                    require(std::get<std::int64_t>(record.values.at(0).value)==sequence++,
                            "soak query/export lost or reordered records");
                };
                if (event.operation=="query") {
                    for (const auto& record:event.records) check(record);
                    require(!event.more,"bounded soak query unexpectedly paginated");
                    queried+=event.records.size();
                } else {
                    require(event.operation=="export","unexpected soak read operation");
                    std::ifstream input(event.path,std::ios::binary);
                    data::PsrecReader reader(input);
                    while (const auto record=reader.next()) check(*record);
                    require(reader.count()==event.processed,"export completion disagrees with verified file");
                    exported+=reader.count();
                }
                require(sequence==end,"soak query/export record count mismatch");
                expected.erase(found);
            }
        };
        const auto begin=std::chrono::steady_clock::now();
        std::uint64_t baseMemory=0,peakMemory=0;
        std::int64_t sent=0;
        for (int tick=0;tick<seconds*10;++tick) {
            std::this_thread::sleep_until(begin+std::chrono::milliseconds(tick*100));
            std::vector<data::Record> records;
            records.reserve(100);
            for (int i=0;i<100;++i,++sent)
                records.push_back({"soak","sample","device",sent*1000,{},0,{{sent},{std::string(128,'x')}}});
            std::string error;
            const bool accepted=store.publish(std::move(records),error);
            if (!accepted) throw std::runtime_error(error+"; storage="+store.status().error);
            const auto status=store.status();
            require(!status.faulted,status.error.c_str());
            poll();observe();
            if (tick%50==49 && expected.empty() && status.lastCommittedId>=200) {
                const auto end=static_cast<std::int64_t>(status.lastCommittedId);
                storage::Query query;
                query.fromUs=(end-200)*1000;query.toUs=(end-1)*1000;
                expected.emplace(store.query(query),std::pair{end-200,end});
                expected.emplace(store.exportRecords(directory.path()/"latest.psrec",storage::ExportFormat::Psrec,query),
                                 std::pair{end-200,end});
            }
            if (tick>=50 && tick%10==0) {
                const auto memory=residentBytes();
                if (!baseMemory) baseMemory=memory;
                peakMemory=std::max(peakMemory,memory);
                require(peakMemory-baseMemory<=128U*1024U*1024U,"resident growth exceeded soak budget");
            }
            if (tick%100==99)
                std::cout<<"elapsed="<<(tick+1)/10<<" committed="<<status.committed<<" cleaned="<<cleaned
                         <<" sealed="<<sealedVolumes<<" rss_peak="<<peakMemory<<std::endl;
        }
        store.stop();store.waitIdle();poll();observe();
        const auto status=store.status();
        require(expected.empty() && status.failed==0 && status.committed==static_cast<std::uint64_t>(sent) &&
                status.received==status.committed && status.queued==status.committed,"soak publish counters disagree");
        require(sealedVolumes>0 && (seconds<30 || cleanedVolumes>0),"soak did not exercise rotation/cleanup");
        storage::Query query;
        query.limit=1000;
        std::uint64_t retained=0;
        bool more=false;
        do {
            store.query(query);store.waitIdle();
            auto events=store.poll();
            require(events.size()==1 && events[0].ok,"final retained-history query failed");
            for (const auto& record:events[0].records)
                require(std::get<std::int64_t>(record.values.at(0).value)==static_cast<std::int64_t>(cleaned+retained++),
                        "retention removed unaccounted or non-oldest records");
            more=events[0].more;query.snapshot=events[0].snapshot;query.offset=retained;
        } while (more);
        require(cleaned+retained==status.committed,"committed != cleaned + retained");
        require(queried && exported,"soak did not complete concurrent query/export");
        std::cout<<"rate=1000/s seconds="<<seconds<<" committed="<<status.committed
                 <<" retained="<<retained<<" cleaned="<<cleaned<<" cleaned_volumes="<<cleanedVolumes
                 <<" queried="<<queried<<" exported="<<exported<<" failed="<<status.failed
                 <<" rss_base="<<baseMemory<<" rss_peak="<<peakMemory
                 <<" rss_growth="<<(peakMemory-baseMemory)<<'\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr<<error.what()<<'\n';return 1;
    }
}
