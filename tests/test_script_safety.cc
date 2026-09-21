#include "protoscope/scripting/script_host.hpp"
#include "protoscope/scripting/script_runtime_worker.hpp"
#include "protoscope/config/config.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>
#include <limits>
#include <thread>

namespace {
using namespace protoscope::scripting;
using protoscope::tests::require;

struct Fixture {
    protoscope::tests::ScopedTempPath directory{
        protoscope::tests::makeUniqueTempDir("protoscope-script-safety")};
    ScriptHost host;

    void load(const char* source)
    {
        {
            std::ofstream out(directory.path() / "main.lua");
            out << source;
        }
        const bool loaded = host.loadProtocolDirectory(directory.path().generic_string());
        require(loaded, host.lastError().c_str());
    }

    void open() { host.onTransportOpen({context()}); }

    static protoscope::transport::ConnectionContext context()
    {
        protoscope::transport::ConnectionContext ctx;
        ctx.connectionId = 1;
        ctx.readyForIo = true;
        return ctx;
    }
};

void timerCancellation()
{
    Fixture f;
    f.load(R"(
        proto.set_timer("a", 0)
        proto.set_timer("b", 0)
        function on_timer(ctx, name)
            proto.emit("timer", name)
            proto.cancel_timer("a")
            proto.cancel_timer("b")
        end
    )");
    f.host.tick(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    require(f.host.drainEvents().size() == 1, "同轮取消的定时器不得再次回调");
}

void timerReplacement()
{
    Fixture f;
    f.load(R"(
        proto.set_timer("a", 0)
        proto.set_timer("b", 0)
        function on_timer(ctx, name)
            proto.emit("timer", name)
            proto.set_timer("a", 0)
            proto.set_timer("b", 0)
        end
    )");
    f.host.tick(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));
    require(f.host.drainEvents().size() == 1, "同轮重新设置的定时器必须留到下一轮");
}

void appendLimit()
{
    Fixture f;
    { std::ofstream out(f.directory.path() / "data.bin", std::ios::binary); out << "abc"; }
    FileIoConfig config;
    config.maxWriteFileSizeBytes = 4;
    f.host.setFileIoConfig(config);
    f.load(R"(
        function on_open(ctx)
            local h = assert(proto.fs.open("data.bin", {mode="append"}))
            assert(proto.fs.write(h, {100}))
            local ok, err = proto.fs.write(h, {101})
            assert(not ok and err)
            assert(proto.fs.close(h))
            h = assert(proto.fs.open("data.bin", {mode="append"}))
            assert(not proto.fs.write(h, {102}))
            assert(proto.fs.close(h))
            proto.emit("done", "")
        end
    )");
    f.open();
    require(f.host.drainEvents().size() == 1, "追加限额必须包含原文件和重新打开前的写入");
    require(std::filesystem::file_size(f.directory.path() / "data.bin") == 4, "拒绝的写入不得改变文件");
}

void sharedAppendLimit()
{
    Fixture f;
    FileIoConfig config;
    config.maxWriteFileSizeBytes = 2;
    f.host.setFileIoConfig(config);
    f.load(R"(
        function on_open(ctx)
            local a = assert(proto.fs.open("shared.bin", {mode="append"}))
            local b = assert(proto.fs.open("shared.bin", {mode="append"}))
            assert(proto.fs.write(a, {1}))
            assert(proto.fs.write(b, {2}))
            assert(not proto.fs.write(a, {3}))
            assert(proto.fs.close(a))
            assert(proto.fs.close(b))
            proto.emit("done", "")
        end
    )");
    f.open();
    require(f.host.drainEvents().size() == 1, "多个追加句柄不能分别计算文件限额");
    require(std::filesystem::file_size(f.directory.path() / "shared.bin") == 2, "共享追加文件不得超过限额");
}

void closeAfterEof()
{
    Fixture f;
    { std::ofstream out(f.directory.path() / "data.bin"); out << "a"; }
    f.load(R"(
        function on_open(ctx)
            local h = assert(proto.fs.open("data.bin"))
            assert(proto.fs.read(h))
            local data, err = proto.fs.read(h)
            assert(data == nil and err == "eof")
            assert(proto.fs.close(h))
            assert(not proto.fs.close(h))
            proto.emit("done", "")
        end
    )");
    f.open();
    require(f.host.drainEvents().size() == 1, "读到 EOF 后关闭仍应成功，重复关闭应失败");
}

void fileFailure()
{
    for (const auto state : {TxEventState::Failed, TxEventState::Rejected, TxEventState::Dropped,
                             TxEventState::Canceled, TxEventState::Timeout}) {
        Fixture f;
        { std::ofstream out(f.directory.path() / "data.bin"); out << "abcd"; }
        FileIoConfig config;
        config.sendFile.maxInflightChunks = 1;
        config.maxOpenFiles = 1;
        f.host.setFileIoConfig(config);
        f.load(R"(
            function on_open(ctx)
                assert(proto.fs.send_file("data.bin", {chunk_size=1}))
            end
            function on_control(ctx, id, value)
                local h = assert(proto.fs.open("data.bin"))
                assert(proto.fs.close(h))
                proto.emit("released", "")
            end
            function ui() return {{id="dock", title="test", controls={
                {type="button", id="check", label="check"}
            }}} end
        )");
        f.open();
        const auto requests = f.host.drainTxRequests();
        require(requests.size() == 1, "文件发送只应产生一个在途块");
        TxEvent event;
        event.id = requests[0].id;
        event.fileJobId = requests[0].fileJobId;
        event.state = state;
        f.host.onTxEvent(Fixture::context(), event);
        require(f.host.drainTxRequests().empty(), "失败后不得继续发送文件后续块");
        f.host.onControl(Fixture::context(), "check", true);
        require(f.host.drainEvents().size() == 1, "失败必须释放文件句柄");
    }
}

void requestCompletion()
{
    Fixture f;
    { std::ofstream out(f.directory.path() / "data.bin"); out << "abc"; }
    FileIoConfig config;
    config.sendFile.maxInflightChunks = 1;
    f.host.setFileIoConfig(config);
    f.load(R"(
        function on_open(ctx)
            assert(proto.fs.send_file("data.bin", {kind="request", chunk_size=1}))
        end
    )");
    f.open();
    const auto first = f.host.drainTxRequests();
    require(first.size() == 1, "请求模式首块应入队");
    TxEvent event;
    event.id = first[0].id;
    event.kind = TxRequestKind::Request;
    event.fileJobId = first[0].fileJobId;
    event.state = TxEventState::Sent;
    f.host.onTxEvent(Fixture::context(), event);
    require(f.host.drainTxRequests().empty(), "请求块必须等响应完成再推进");
    event.state = TxEventState::Completed;
    f.host.onTxEvent(Fixture::context(), event);
    require(f.host.drainTxRequests().size() == 1, "响应完成应推进下一块");
    f.host.onTxEvent(Fixture::context(), event);
    require(f.host.drainTxRequests().empty(), "重复完成事件不得释放其他在途块");
}

void reloadCompatibility()
{
    Fixture f;
    f.load(R"(function ui() return {{id="dock", title="test", controls={{type="input_int", id="value", label="value", default=2}}}} end)");
    require(f.host.setControlValue("value", 9), "旧输入值应可设置");
    f.load(R"(function ui() return {{id="dock", title="test", controls={{type="input_int", id="value", label="value", default=3}}}} end)");
    require(std::get<int>(f.host.controlStatesSnapshot()[0].value) == 9, "兼容输入应保留");
    f.load(R"(function ui() return {{id="dock", title="test", controls={{type="input_text", id="value", label="value", default="new"}}}} end)");
    require(std::holds_alternative<std::string>(f.host.controlStatesSnapshot()[0].value), "同 ID 类型变更必须重置");
    f.load(R"(function ui() return {{id="dock", title="test", controls={{type="combo", id="value", label="value", options={"a","b"}, default=1}}}} end)");
    require(f.host.setControlValue("value", 1), "选项应可设置");
    f.load(R"(function ui() return {{id="dock", title="test", controls={{type="combo", id="value", label="value", options={"x"}, default=1}}}} end)");
    require(std::get<int>(f.host.controlStatesSnapshot()[0].value) == 0, "选项失效应回退默认值");
}

void executionBudgets()
{
    Fixture f;
    f.host.setExecutionConfig({.loadTimeoutMs=20, .callbackTimeoutMs=20});
    f.load(R"(
        function on_open(ctx)
            proto.set_timer("pending", 0)
            proto.send({1})
            while true do end
        end
        function on_timer(ctx, name) proto.emit("unexpected", "") end
        function on_close(ctx) proto.emit("unexpected", "") end
    )");
    const auto start = std::chrono::steady_clock::now();
    f.open();
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "无限循环必须被预算中断");
    require(f.host.executionFaulted(), "超时后必须进入业务故障状态");
    require(f.host.lastError().find("budget") != std::string::npos, "超时原因必须对外可见");
    require(!f.host.nextWakeupAtMs(), "故障脚本不得继续唤醒定时器");
    require(f.host.drainTxRequests().empty(), "故障回调的未发送输出不得交给设备");
    f.host.onTransportClose({Fixture::context(), "test"});
    require(f.host.drainEvents().empty(), "故障后必须停止其他业务回调");
    f.load(R"(function on_open(ctx) proto.emit("recovered", "") end)");
    f.open();
    require(!f.host.executionFaulted() && f.host.drainEvents().size() == 1, "成功重载必须恢复业务");
}

void loadingBudget()
{
    for (const auto* source : {
        "while true do end",
        "function ui() while true do end end",
        "pcall(function() while true do end end)",
    }) {
        Fixture f;
        f.host.setExecutionConfig({.loadTimeoutMs=20, .callbackTimeoutMs=20});
        f.load(R"(function on_open(ctx) proto.emit("old", "") end)");
        { std::ofstream out(f.directory.path() / "main.lua"); out << source; }
        const auto start = std::chrono::steady_clock::now();
        require(!f.host.loadProtocolDirectory(f.directory.path().generic_string()), "加载预算覆盖顶层及声明函数");
        require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "加载无限循环必须中断");
        f.open();
        require(f.host.drainEvents().size() == 1, "加载失败不得破坏旧运行时");
    }
}

void workerStop()
{
    Fixture f;
    f.load(R"(
        function on_open(ctx)
            local h = assert(proto.fs.open("started.bin", {mode="write"}))
            assert(proto.fs.write(h, {1}))
            assert(proto.fs.close(h))
            while true do end
        end
    )");
    ScriptRuntimeWorker worker;
    ScriptRuntimeWorkerConfig config;
    config.execution.callbackTimeoutMs = 10000;
    worker.configure(config);
    require(worker.loadProtocolDirectory(f.directory.path().generic_string()).ok, "worker 应加载测试协议");
    worker.postTransportOpen({Fixture::context()});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!std::filesystem::exists(f.directory.path() / "started.bin") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool started = std::filesystem::exists(f.directory.path() / "started.bin");
    const auto start = std::chrono::steady_clock::now();
    worker.stop();
    require(started, "停止前必须确认 Lua 回调已开始执行");
    require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2), "stop 不得等待十秒回调预算");
}

void executionConfig()
{
    protoscope::config::ConfigStore store;
    const auto loaded = store.loadText(
        "scripting:\n  execution:\n    load_timeout_ms: 25\n    callback_timeout_ms: 30\n");
    require(loaded.error.empty(), "执行预算配置应可读取");
    require(loaded.config.scripting.execution.loadTimeoutMs == 25 &&
            loaded.config.scripting.execution.callbackTimeoutMs == 30, "必须读取配置的加载与回调预算");
    std::string yaml, error;
    require(store.saveText(loaded.config, yaml, error), "执行预算应可保存");
    const auto reloaded = store.loadText(yaml);
    require(reloaded.config.scripting.execution.loadTimeoutMs == 25 &&
            reloaded.config.scripting.execution.callbackTimeoutMs == 30, "保存重载必须保持执行预算");
    const auto clamped = store.loadText(
        "scripting:\n  execution:\n    load_timeout_ms: 0\n    callback_timeout_ms: 99999999\n");
    require(clamped.config.scripting.execution.loadTimeoutMs == 1 &&
            clamped.config.scripting.execution.callbackTimeoutMs == 3600000, "执行预算必须限定有效范围");
}
} // namespace

int main()
{
    int failed = 0;
    const std::pair<const char*, void(*)()> tests[] = {
        {"timer_cancel", timerCancellation}, {"timer_replace", timerReplacement},
        {"append_limit", appendLimit}, {"close_eof", closeAfterEof},
        {"shared_append_limit", sharedAppendLimit},
        {"file_failure", fileFailure}, {"request_completion", requestCompletion},
        {"reload_compatibility", reloadCompatibility},
        {"execution_budget", executionBudgets}, {"loading_budget", loadingBudget},
        {"worker_stop", workerStop},
        {"execution_config", executionConfig},
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
