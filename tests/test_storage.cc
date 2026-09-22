#include "protoscope/storage/store.hpp"
#include "test_helpers.hpp"

#include <bit>
#include <iostream>
#include <fstream>
#include <limits>

namespace {
using namespace protoscope;
using tests::require;

template<class F>
void rejects(F run)
{
    bool rejected = false;
    try { run(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "非法数据必须被拒绝");
}

data::Schema schema()
{
    return {"samples", {{"count", data::FieldType::Int64, false},
                        {"value", data::FieldType::Double, true},
                        {"payload", data::FieldType::Bytes, true}}};
}

data::Record record(std::int64_t count = 1)
{
    return {"protocol", "samples", "device", 123, {}, 0,
        {{count}, {}, {data::Bytes{0, 255}}}};
}

void model()
{
    const data::Value input{data::Value::Object{
        {"integer", {std::numeric_limits<std::int64_t>::max()}},
        {"minimum", {std::numeric_limits<std::int64_t>::min()}},
        {"text", {std::string("a\0b", 3)}}, {"empty", {}},
        {"bytes", {data::Bytes{0, 128, 255}}},
        {"array", {data::Value::Array{{true}, {-0.0}, {std::string{}}}}}
    }};
    const auto encoded = data::encodeValue(input);
    const auto decoded = data::decodeValue(encoded);
    require(decoded == input, "类型化值必须精确往返");
    const auto& values = std::get<data::Value::Array>(
        std::get<data::Value::Object>(decoded.value).at("array").value);
    require(std::bit_cast<std::uint64_t>(std::get<double>(values[1].value)) ==
            std::bit_cast<std::uint64_t>(-0.0), "负零必须保持位型");
    for (std::size_t size = 0; size < encoded.size(); ++size) {
        rejects([&] { data::decodeValue(std::span(encoded).first(size)); });
    }
    auto trailing = encoded;
    trailing.push_back(0);
    rejects([&] { data::decodeValue(trailing); });
    rejects([&] { data::encodeValue(input, {8, 16}); });
    rejects([&] { data::encodeValue(input, {1024, 0}); });
    rejects([&] { data::encodeValue({std::numeric_limits<double>::quiet_NaN()}); });
    auto definition = schema();
    require(data::schemaFromValue(data::schemaValue(definition)) == definition, "模式必须精确往返");
    auto sample = record();
    data::validateRecord(definition, sample);
    require(data::recordFromValue(data::recordValue(sample)) == sample, "记录必须精确往返");
    sample.values[0] = {1.0};
    rejects([&] { data::validateRecord(definition, sample); });
    definition.fields.push_back(definition.fields[0]);
    rejects([&] { data::validateSchema(definition); });
}

storage::Completion completion(storage::Store& store, std::uint64_t id)
{
    store.waitIdle();
    const auto results = store.poll();
    for (const auto& result : results) if (result.task == id) return result;
    throw std::runtime_error("任务必须返回结果");
}

void kvPersistence()
{
    const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-kv"));
    const data::Value value{data::Value::Array{{std::int64_t{123}}, {std::string{"abc"}}, {true}}};
    {
        storage::Store store(directory.path(), "protocol", {schema()});
        require(!store.get("device/config"), "缺失的键返回空");
        const auto id = store.set("device/config", value);
        require(completion(store, id).ok, "KV 写入必须提交成功");
        require(store.get("device/config") == value, "提交后缓存才包含新值");
    }
    {
        storage::Store store(directory.path(), "protocol", {schema()});
        require(store.get("device/config") == value, "重启必须载入已提交 KV");
        require(completion(store, store.erase("device/config")).ok, "删除任务必须成功");
        require(!store.get("device/config"), "删除提交后缓存同步");
        require(completion(store, store.flush()).ok, "flush 必须报告成功");
    }
    rejects([&] { storage::Store other(directory.path(), "other-protocol", {schema()}); });
}

void recordSnapshots()
{
    const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-record"));
    std::optional<std::int64_t> snapshot;
    {
        storage::Store store(directory.path(), "protocol", {schema()});
        require(completion(store, store.start()).ok, "记录启动必须成功");
        std::string error;
        require(store.publish({record(1), record(2)}, error), "数据批次必须成功入队");
        require(completion(store, store.flush()).ok, "数据批次必须提交");
        auto result = completion(store, store.query({.limit=1}));
        require(result.ok && result.records.size() == 1 && result.more, "历史查询必须分页");
        require(std::get<std::int64_t>(result.records[0].values[0].value) == 1, "重复时间戳按记录 ID 稳定排序");
        snapshot = result.snapshot;
        require(store.publish({record(3)}, error), "追加数据必须成功");
        store.waitIdle();
        result = completion(store, store.query({.offset=1, .limit=1, .snapshot=snapshot}));
        require(result.ok && result.records.size() == 1 && !result.more, "旧快照不得纳入新记录");
        require(std::get<std::int64_t>(result.records[0].values[0].value) == 2, "分页不能重复或跳过相同时间戳");
        const auto state = store.status();
        require(state.queued == 3 && state.committed == 3 && state.failed == 0, "入队与提交计数必须准确");
    }
    {
        storage::Store store(directory.path(), "protocol", {schema()});
        require(store.status().recording && store.status().recovered, "未主动停止的任务必须恢复记录开关");
        const auto result = completion(store, store.query({}));
        require(result.ok && result.records.size() == 3, "重启后已提交数据必须可查询");
        require(completion(store, store.stop()).ok, "主动停止必须提交");
    }
    {
        storage::Store store(directory.path(), "protocol", {schema()});
        require(!store.status().recording, "主动停止后重启不能自动记录");
    }
}

void fieldQueries()
{
    const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-field-query"));
    storage::Store store(directory.path(),"protocol",{schema()});
    require(completion(store,store.start()).ok,"field query recording");
    std::string error;
    require(store.publish({record(4),record(2),record(3),record(2),
                           record(std::numeric_limits<std::int64_t>::max())},error),"field query rows");
    store.waitIdle();
    storage::Query query;
    query.dataset="samples";query.limit=2;
    query.conditions={{"count",data::CompareOp::GreaterEqual,{std::int64_t{2}}}};
    query.sort=data::FieldSort{"count",true};
    auto first=completion(store,store.query(query));
    require(first.ok && first.more && first.records.size()==2,"filtered sorted page");
    require(std::get<std::int64_t>(first.records[0].values[0].value)==std::numeric_limits<std::int64_t>::max() &&
            std::get<std::int64_t>(first.records[1].values[0].value)==4,"global sort preserves int64");
    query.snapshot=first.snapshot;query.offset=2;
    require(store.publish({record(10)},error),"append after fixed query");
    store.waitIdle();
    auto next=completion(store,store.query(query));
    require(next.ok && next.more && std::get<std::int64_t>(next.records[0].values[0].value)==3,
            "filtered pages use fixed high water");
    query.offset=4;
    auto last=completion(store,store.query(query));
    require(last.ok && !last.more && last.records.size()==1 &&
            std::get<std::int64_t>(last.records[0].values[0].value)==2,"duplicate sort keys remain stable");
    query={};query.conditions={{"value",data::CompareOp::IsNull,{}}};
    require(completion(store,store.query(query)).records.size()==6,"explicit null query");
    query.conditions={{"absent",data::CompareOp::IsNull,{}}};
    require(completion(store,store.query(query)).records.empty(),"missing field is not explicit null");
    query.conditions={{"count",data::CompareOp::Equal,{2.0}}};
    require(completion(store,store.query(query)).records.empty(),"no implicit comparison conversion");
    query.conditions={{"payload",data::CompareOp::Equal,{data::Bytes{0,255}}}};
    require(completion(store,store.query(query)).records.size()==6,"typed byte filter");
    query.conditions={{"count",data::CompareOp::Equal,{}}};
    rejects([&]{store.query(query);});
    query.conditions.assign(17,{"count",data::CompareOp::Equal,{std::int64_t{2}}});
    rejects([&]{store.query(query);});
    query.conditions={{"count'); DROP TABLE records;--",data::CompareOp::Equal,{std::int64_t{2}}}};
    require(completion(store,store.query(query)).records.empty(),"field names are not SQL");
    require(completion(store,store.query({})).records.size()==6,"query must not alter record data");
}

void limits()
{
    const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-storage-limits"));
    storage::Config config;
    config.kvValueBytes = 32;
    config.kvTotalBytes = 32;
    storage::Store store(directory.path(), "protocol", {schema()}, config);
    rejects([&] { store.set("key", {std::string(100, 'x')}); });
    require(completion(store, store.set("first", {std::string(12, 'x')})).ok, "首个 KV 应成功");
    const auto result = completion(store, store.set("second", {std::string(12, 'x')}));
    require(!result.ok && !store.get("second"), "总量超限不能污染缓存");
    require(store.get("first").has_value(), "失败不能回滚先前已提交键");
    rejects([&] { store.query({.limit=1001}); });
}

void schemaVersions()
{
    const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-schema-versions"));
    {
        storage::Store store(directory.path(), "protocol", {schema()});
        require(completion(store, store.start()).ok, "初始记录应启动");
        std::string error;
        require(store.publish({record()}, error), "初始模式记录应发布");
    }
    auto changed = schema();
    changed.fields[0].type = data::FieldType::String;
    {
        storage::Store store(directory.path(), "protocol", {changed});
        auto next = record();
        next.values[0] = {std::string{"changed"}};
        std::string error;
        require(store.publish({next}, error), "新模式记录应发布");
        store.waitIdle();
        const auto result = completion(store, store.query({}));
        require(result.ok && result.records.size() == 2 && result.schemas.size() == 2, "旧模式记录和元数据必须保持可读");
        require(result.records[0].schemaVersion != result.records[1].schemaVersion, "模式变化必须生成新版本");
        require(std::holds_alternative<std::int64_t>(result.records[0].values[0].value), "旧值不能按新模式重解释");
    }
}

void queueFault()
{
    const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-queue-fault"));
    storage::Config config;
    config.queueBytes = 512;
    storage::Store store(directory.path(), "protocol", {schema()}, config);
    require(completion(store, store.start()).ok, "小队列应允许启动");
    auto large = record();
    large.values[2] = {data::Bytes(1024, 1)};
    std::string error;
    require(!store.publish({large}, error), "超容量记录必须明确拒绝");
    const auto state = store.status();
    require(state.faulted && !state.recording && state.failed == 1 && state.committed == 0,
            "队列满必须进入记录故障并保留正确计数");
    require(completion(store, store.set("config", {true})).ok, "记录故障不应阻断独立 KV 任务");
}

void cancellationAndBackpressure()
{
    const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-query-cancel"));
    storage::Store store(directory.path(), "protocol", {schema()});
    require(completion(store, store.start()).ok, "记录应启动");
    std::vector<data::Record> records;
    for (int i = 0; i < 1000; ++i) records.push_back(record(i));
    std::string error;
    require(store.publish(std::move(records), error), "千条批次应入队");
    store.waitIdle();
    require(store.status().committed == 1000, "批量事务必须提交全部记录");
    for (int i = 0; i < 16; ++i) {
        const auto task = store.query({.limit=1000});
        store.cancel(task);
    }
    rejects([&] { store.query({}); });
    store.waitIdle();
    const auto results = store.poll();
    require(results.size() == 16, "取消也必须返回任务结果，未消费结果必须受限");
    for (const auto& result : results) {
        require(result.ok ? result.records.size() == 1000 : result.records.empty(),
                "取消不能暴露部分查询页");
    }
    require(completion(store, store.query({})).ok, "消费结果后应释放查询额度");
}

void damagedDatabase()
{
    const tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-damaged-db"));
    std::filesystem::create_directories(directory.path() / "records");
    {
        std::ofstream out(directory.path() / "records" / "records.sqlite", std::ios::binary);
        out << "not a sqlite database";
    }
    rejects([&] { storage::Store store(directory.path(), "protocol", {schema()}); });
    require(std::filesystem::file_size(directory.path() / "records" / "records.sqlite") == 21,
            "数据库损坏时不得重建并覆盖原文件");
}
}

int main()
{
    int failed = 0;
    const std::pair<const char*, void(*)()> tests[] = {
        {"model", model}, {"kv_persistence", kvPersistence},
        {"record_snapshots", recordSnapshots}, {"limits", limits},
        {"schema_versions", schemaVersions}, {"queue_fault", queueFault},
        {"cancel_backpressure", cancellationAndBackpressure},
        {"damaged_database", damagedDatabase},
        {"field_queries",fieldQueries},
    };
    for (const auto& [name, run] : tests) {
        try { run(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
        }
    }
    return failed == 0 ? 0 : 1;
}
