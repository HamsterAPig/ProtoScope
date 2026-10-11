#include "protoscope/scripting/pipeline_threading.hpp"
#include "protoscope/scripting/script_runtime_worker.hpp"
#include "protoscope/transport/transport.hpp"

#include "test_registry.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path makeWorkerProtocolDir(const char* name, const std::string& script)
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("protoscope-worker-") + name + "-" + std::to_string(ticks));
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "main.lua");
    out << script;
    return dir;
}

struct ScopedTempPath {
    explicit ScopedTempPath(std::filesystem::path path) : path_(std::move(path)) {}

    ~ScopedTempPath()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

protoscope::transport::ConnectionContext workerContext(std::uint64_t timestampMs = 1)
{
    return protoscope::transport::ConnectionContext{
        .kind = protoscope::transport::TransportKind::TcpClient,
        .endpoint = "worker-test",
        .connectionId = 42,
        .timestampMs = timestampMs,
        .readyForIo = true,
    };
}

protoscope::transport::TransportBytesEvent bytesEvent(std::vector<std::uint8_t> bytes, std::uint64_t timestampMs = 1)
{
    return protoscope::transport::TransportBytesEvent{
        .context = workerContext(timestampMs),
        .bytes = std::move(bytes),
    };
}

bool hasEvent(const std::vector<protoscope::scripting::ScriptRuntimeOutputBatch>& batches,
              const std::string& eventName,
              const std::string& token)
{
    for (const auto& batch : batches) {
        for (const auto& event : batch.events) {
            if (event.name == eventName && event.payload.find(token) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool hasEventWithTokens(const std::vector<protoscope::scripting::ScriptRuntimeOutputBatch>& batches,
                        const std::string& eventName,
                        const std::string& firstToken,
                        const std::string& secondToken)
{
    for (const auto& batch : batches) {
        for (const auto& event : batch.events) {
            if (event.name == eventName && event.payload.find(firstToken) != std::string::npos &&
                event.payload.find(secondToken) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool hasLog(const std::vector<protoscope::scripting::ScriptRuntimeOutputBatch>& batches, const std::string& token)
{
    for (const auto& batch : batches) {
        for (const auto& log : batch.logs) {
            if (log.message.find(token) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool hasOscilloscopeRunningUpdate(const std::vector<protoscope::scripting::ScriptRuntimeOutputBatch>& batches,
                                  bool running)
{
    for (const auto& batch : batches) {
        for (const auto& update : batch.oscilloscopeRunningUpdates) {
            if (update.running == running) {
                return true;
            }
        }
    }
    return false;
}

std::string workerProbeScript()
{
    return R"lua(
function controls()
  return {}
end

local calls = 0

function on_bytes(ctx, bytes)
  calls = calls + 1
  if calls == 1 and bytes[1] == 1 then
    local started = os.clock()
    while os.clock() - started < 1.0 do
    end
  end
  proto.emit("worker_bytes", {
    first = bytes[1],
    size = #bytes,
  })
end
)lua";
}

std::string workerBatchProbeScript()
{
    return R"lua(
function controls()
  return {}
end

local calls = 0

function on_bytes(ctx, bytes)
  calls = calls + 1
  if calls == 1 and bytes[1] == 1 then
    local started = os.clock()
    while os.clock() - started < 1.0 do
    end
  end
  proto.emit("worker_bytes", "first=" .. tostring(bytes[1]) .. ",size=" .. tostring(#bytes))
end
)lua";
}

} // namespace

void test_script_runtime_worker_disabled_mode_waits_for_rx_idle()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir("sync", workerProbeScript()));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure(protoscope::scripting::ScriptRuntimeWorkerConfig{
        .enabled = false,
        .rxQueueLimitBytes = 64U * 1024U,
        .outputQueueLimit = 128U,
        .batchBytes = 1024U,
    });
    const auto loaded = worker.loadProtocolDirectory(protocolDir.path().generic_string());
    require(loaded.ok, "worker 同步模式测试协议应可加载");
    (void) worker.drainOutputs();
    const auto syncEvent = bytesEvent({0x42, 0x43, 0x44});
    require(worker.tryPostTransportBytes(syncEvent, syncEvent.bytes.size(), false) == 0U,
            "非阻塞GUI专用入口不得更改disabled worker同步模式契约");

    worker.postTransportBytes(syncEvent);
    const auto outputs = worker.drainOutputs();

    require(worker.snapshot().pendingWorkerRxBytes == 0U, "禁用异步 worker 时 postTransportBytes 应等待 RX 空闲");
    require(hasEvent(outputs, "worker_bytes", "first=66"), "禁用异步 worker 时应立即产出脚本事件");
}

void test_script_runtime_worker_rejects_stale_rx_without_subtracting_other_pending_bytes()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir("stale-rx-counter", R"lua(
function controls() return {} end
function on_bytes(ctx, bytes)
  proto.emit("worker_bytes", tostring(bytes[1]))
end
)lua"));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure({.enabled = true, .rxQueueLimitBytes = 64U, .outputQueueLimit = 1U,
                      .backpressureEnabled = false});
    require(worker.loadProtocolDirectory(protocolDir.path().generic_string()).ok,
            "旧连接RX计数测试协议应可加载");
    (void)worker.drainOutputs();

    const auto observe = [](auto&& predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return predicate();
    };
    worker.postTransportBytes(bytesEvent({0x01}), false);
    require(observe([&] { return worker.inputIdle() && worker.snapshot().outputQueueSize == 1U; }),
            "首个RX应建立活动连接并占满输出发布槽");

    worker.postTransportBytes(bytesEvent({0x02}), false);
    // 出队先设置 executing，随后才扣 pending；必须等扣减完成才能验证后续四字节积压。
    require(observe([&] {
                const auto snapshot = worker.snapshot();
                return snapshot.executingRxBytes == 1U && snapshot.pendingWorkerRxBytes == 0U &&
                       !worker.inputIdle();
            }),
            "第二个RX应受控阻塞在输出发布窗口");

    auto stale = bytesEvent({0xA1, 0xA2});
    stale.context.connectionId = 41U;
    worker.postTransportBytes(std::move(stale), false);
    worker.postTransportBytes(bytesEvent({0x03}), false);
    worker.postTransportBytes(bytesEvent({0x04}), false);
    require(worker.pendingRxBytes() == 4U,
            "旧连接RX与其他已接受RX必须同时计入待处理字节数");

    (void)worker.drainOutputs();
    require(observe([&] {
                return worker.executingRxBytes() == 1U && worker.pendingRxBytes() == 1U &&
                       !worker.inputIdle();
            }),
            "拒绝旧代次后，后续RX仍须在输出发布窗口保留剩余待处理计数");
    require(worker.pendingRxBytes() == 1U,
            "拒绝旧连接RX不得重复扣减其他已接受输入计数");
    require(worker.executingRxBytes() == 1U,
            "RX输出发布窗口必须持续计入executingRxBytes");

    const auto idleDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!worker.inputIdle() || worker.pendingRxBytes() != 0U || worker.executingRxBytes() != 0U) &&
           std::chrono::steady_clock::now() < idleDeadline) {
        (void)worker.drainOutputs();
        std::this_thread::yield();
    }
    require(worker.inputIdle() && worker.pendingRxBytes() == 0U && worker.executingRxBytes() == 0U,
            "正常处理后worker应恢复空闲并归零RX计数");
    worker.stop();
    require(worker.pendingRxBytes() == 0U && worker.executingRxBytes() == 0U,
            "停止/取消后pending与executing RX计数必须归零");
}

void test_script_runtime_worker_rx_limit_keeps_all_queued_bytes()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir("rx-limit", workerProbeScript()));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure(protoscope::scripting::ScriptRuntimeWorkerConfig{
        // 此用例故意忙等一秒制造队列积压，执行预算需覆盖探针耗时。
        .execution = {.callbackTimeoutMs = 5000},
        .enabled = true,
        .rxQueueLimitBytes = 6U,
        .outputQueueLimit = 16U,
        .batchBytes = 1U,
        .backpressureEnabled = false,
    });
    const auto loaded = worker.loadProtocolDirectory(protocolDir.path().generic_string());
    require(loaded.ok, "worker 限流测试协议应可加载");
    (void) worker.drainOutputs();

    worker.postTransportBytes(bytesEvent({0x01}, 1));
    bool firstEventInWorker = false;
    for (int i = 0; i < 200; ++i) {
        const auto snapshot = worker.snapshot();
        if (snapshot.pendingWorkerRxBytes == 0U && snapshot.inputQueueSize == 0U) {
            firstEventInWorker = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(firstEventInWorker, "限流测试应先等待首个 RX 进入 worker 执行中");

    worker.postTransportBytes(bytesEvent({0xA1, 0x00, 0x00}, 2));
    worker.postTransportBytes(bytesEvent({0xB2, 0x00, 0x00}, 3));
    worker.postTransportBytes(bytesEvent({0xC3, 0x00, 0x00}, 4));
    worker.waitIdle();
    const auto outputs = worker.drainOutputs();

    require(outputs.size() <= 16U, "worker 输出队列不得超过配置硬上界");
    require(hasEvent(outputs, "worker_bytes", "first=1"), "阻塞中的首个 RX 事件不应被限流清理");
    require(hasEvent(outputs, "worker_bytes", "first=161"), "RX 队列超过阈值后仍应保留最旧待解析字节");
    require(hasEvent(outputs, "worker_bytes", "first=178"), "RX 队列超过阈值后仍应保留较新的待解析字节");
    require(hasEvent(outputs, "worker_bytes", "first=195"), "RX 队列超过阈值后仍应保留最新待解析字节");
}

void test_lua_contract_backpressure_try_post_rejects_without_consuming_and_recovers()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir("try-post-backpressure", R"lua(
function controls() return {} end
local calls = 0
function on_bytes(ctx, bytes)
  calls = calls + 1
  if calls == 1 and bytes[1] == 1 then
    local started = os.clock()
    while os.clock() - started < 1.0 do end
  end
  proto.emit("worker_bytes", {
    first = bytes[1],
    size = #bytes,
  })
end
)lua"));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure({.execution = {.callbackTimeoutMs = 5000}, .enabled = true, .rxQueueLimitBytes = 4U,
                      .outputQueueLimit = 8U, .batchBytes = 1U, .backpressureEnabled = true,
                      .backpressureHighWatermark = 0.5, .backpressureLowWatermark = 0.25});
    require(worker.loadProtocolDirectory(protocolDir.path().generic_string()).ok,
            "非阻塞投递测试协议应可加载");
    (void)worker.drainOutputs();

    auto event = bytesEvent({0x01, 0x22, 0x33, 0x34, 0x55});
    const auto firstAccepted = worker.tryPostTransportBytes(event, 8U, false);
    require(firstAccepted == 2U, "高水位预算4、比例0.5时仅应原子接收2字节");
    event.bytes.erase(event.bytes.begin(), event.bytes.begin() + static_cast<std::ptrdiff_t>(firstAccepted));
    const auto callbackDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((worker.pendingRxBytes() != 0U || worker.inputIdle()) &&
           std::chrono::steady_clock::now() < callbackDeadline) {
        std::this_thread::yield();
    }
    require(worker.pendingRxBytes() == 0U && !worker.inputIdle(),
            "首个RX回调必须处于脚本的受控阻塞段");
    const auto queuedAtHighWater = worker.tryPostTransportBytes(event, 8U, false);
    require(queuedAtHighWater == 2U, "低水位恢复后应原子接收至高水位，且仅接收有限前缀");
    event.bytes.erase(event.bytes.begin(), event.bytes.begin() + static_cast<std::ptrdiff_t>(queuedAtHighWater));
    require(worker.tryPostTransportBytes(event, 8U, false) == 0U,
            "重新达到高水位时必须再次拒绝输入并保留调用方尾部");
    require(event.bytes == std::vector<std::uint8_t>{0x55}, "高水位拒绝后尾字节仍须由调用方持有");

    std::vector<protoscope::scripting::ScriptRuntimeOutputBatch> outputs;
    const auto finishDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while ((!worker.inputIdle() || worker.pendingRxBytes() != 0U) &&
           std::chrono::steady_clock::now() < finishDeadline) {
        auto drained = worker.drainOutputs();
        outputs.insert(outputs.end(), std::make_move_iterator(drained.begin()), std::make_move_iterator(drained.end()));
        std::this_thread::yield();
    }
    auto preRetryDrained = worker.drainOutputs();
    outputs.insert(outputs.end(), std::make_move_iterator(preRetryDrained.begin()),
                   std::make_move_iterator(preRetryDrained.end()));
    require(worker.inputIdle() && worker.pendingRxBytes() == 0U,
            "worker应在有限时间内处理高水位前已接受输入并恢复到低水位");
    auto lowWaterOutputs = worker.drainOutputs();
    outputs.insert(outputs.end(), std::make_move_iterator(lowWaterOutputs.begin()),
                    std::make_move_iterator(lowWaterOutputs.end()));
    const auto acceptedAfterLowWater = worker.tryPostTransportBytes(event, 8U, false);
    require(acceptedAfterLowWater == 1U, "低水位恢复后应继续接收调用方保留的尾字节");
    event.bytes.erase(event.bytes.begin(), event.bytes.begin() + static_cast<std::ptrdiff_t>(acceptedAfterLowWater));
    require(event.bytes.empty(), "低水位恢复后已接受的输入不得残留");
    const auto deliveryDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((!worker.inputIdle() || worker.pendingRxBytes() != 0U || worker.snapshot().outputQueueSize > 0U) &&
           std::chrono::steady_clock::now() < deliveryDeadline) {
        auto drained = worker.drainOutputs();
        outputs.insert(outputs.end(), std::make_move_iterator(drained.begin()), std::make_move_iterator(drained.end()));
        std::this_thread::yield();
    }
    auto drained = worker.drainOutputs();
    outputs.insert(outputs.end(), std::make_move_iterator(drained.begin()), std::make_move_iterator(drained.end()));
    require(worker.inputIdle() && worker.pendingRxBytes() == 0U, "worker 应在有限时间内处理全部已接收输入");
    std::string delivered;
    std::size_t eventCount = 0U;
    std::vector<std::string> deliveredPayloads;
    for (const auto& batch : outputs) {
        for (const auto& output : batch.events) {
            if (output.name == "worker_bytes") {
                delivered += output.payload;
                delivered.push_back(';');
                deliveredPayloads.push_back(output.payload);
                ++eventCount;
            }
        }
    }
    const auto firstPosition = delivered.find("first=1");
    const auto secondPosition = delivered.find("first=51");
    const auto thirdPosition = delivered.find("first=85");
    const auto deliveredCount = deliveredPayloads.size();
    require(eventCount == 3U && deliveredCount == 3U && firstPosition != std::string::npos &&
                secondPosition != std::string::npos && thirdPosition != std::string::npos &&
                firstPosition < secondPosition && secondPosition < thirdPosition,
            (std::string("交付事件数=") + std::to_string(eventCount) + ", payloads=" + delivered +
             ", lastError=" + worker.snapshot().lastError).c_str());
}

void test_lua_contract_backpressure_reload_and_stop_recover_from_output_saturation()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir("reload-stop-backpressure", workerProbeScript()));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure({.enabled = true, .rxQueueLimitBytes = 4U, .outputQueueLimit = 1U, .batchBytes = 1U,
                      .backpressureEnabled = true, .backpressureHighWatermark = 0.5,
                      .backpressureLowWatermark = 0.25});
    require(worker.loadProtocolDirectory(protocolDir.path().generic_string()).ok,
            "重载/停止背压测试协议应可加载");
    (void)worker.drainOutputs();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const auto observe = [&](auto&& predicate) {
        while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return predicate();
    };

    worker.postTransportBytes(bytesEvent({0x31}, 1), false);
    require(observe([&] { return worker.inputIdle() && worker.snapshot().outputQueueSize == 1U; }),
            "输出硬上界应先被第一批占满");
    worker.postTransportBytes(bytesEvent({0x32}, 2), false);
    require(observe([&] { return worker.pendingRxBytes() == 0U && !worker.inputIdle(); }),
            "第二批应确定阻塞在输出发布");

    auto reload = worker.loadProtocolDirectoryAsync(protocolDir.path().generic_string());
    while (reload.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
           std::chrono::steady_clock::now() < deadline) {
        (void)worker.drainOutputs();
        std::this_thread::yield();
    }
    require(reload.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready,
            "持续drain输出后排队的协议重载必须完成");
    require(reload.get().ok, "输出饱和恢复后新协议应成功加载");

    worker.postTransportBytes(bytesEvent({0x33}, 3), false);
    worker.stop();
    require(worker.tryPostTransportBytes(bytesEvent({0x34}, 4), 1U, false) == 0U,
            "stop完成后非阻塞RX必须拒绝输入并保留调用方所有权");
}

void test_lua_contract_backpressure_stop_cancels_blocked_output_waiter()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir("stop-output-backpressure", workerProbeScript()));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure({.enabled = true, .rxQueueLimitBytes = 4U, .outputQueueLimit = 1U, .batchBytes = 1U,
                      .backpressureEnabled = true, .backpressureHighWatermark = 0.5,
                      .backpressureLowWatermark = 0.25});
    require(worker.loadProtocolDirectory(protocolDir.path().generic_string()).ok,
            "停止背压测试协议应可加载");
    (void)worker.drainOutputs();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    const auto observe = [&](auto&& predicate) {
        while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
        return predicate();
    };
    worker.postTransportBytes(bytesEvent({0x41}, 1), false);
    require(observe([&] { return worker.inputIdle() && worker.snapshot().outputQueueSize == 1U; }),
            "首批输出应达到硬上限");
    worker.postTransportBytes(bytesEvent({0x42}, 2), false);
    require(observe([&] { return worker.pendingRxBytes() == 0U && !worker.inputIdle(); }),
            "第二批应阻塞在满输出发布");
    worker.postTransportBytes(bytesEvent({0x43}, 3), false);
    const auto startedAt = std::chrono::steady_clock::now();
    worker.stop();
    require(std::chrono::steady_clock::now() - startedAt < std::chrono::seconds(2),
            "stop必须唤醒并join等待输出空间的worker");
    require(worker.tryPostTransportBytes(bytesEvent({0x44}, 4), 1U, false) == 0U,
            "停止后非阻塞投递应拒绝且不接收调用方输入");
}

void test_script_runtime_worker_output_limit_drain_wakes_blocked_publisher()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir("output-limit", R"lua(
function on_bytes(ctx, bytes)
  proto.emit("bounded_output", tostring(bytes[1]))
end
)lua"));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure({.enabled = true, .outputQueueLimit = 1U, .backpressureEnabled = false});
    require(worker.loadProtocolDirectory(protocolDir.path().generic_string()).ok,
            "输出硬上界协议应可加载");
    (void) worker.drainOutputs();
    worker.postTransportBytes(bytesEvent({1}, 1), false);
    worker.postTransportBytes(bytesEvent({2}, 2), false);
    for (int i = 0; i < 200 && worker.snapshot().outputQueueSize == 0U; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    require(worker.snapshot().outputQueueSize == 1U, "输出队列应达到但不超过硬上界 1");
    const auto first = worker.drainOutputs();
    require(first.size() == 1U && hasEvent(first, "bounded_output", "1"),
            "首次 drain 应得到第一批输出");
    worker.waitIdle();
    const auto second = worker.drainOutputs();
    require(second.size() == 1U && hasEvent(second, "bounded_output", "2"),
            "批量 drain 后必须唤醒被阻塞发布者且不丢第二批输出");
    require(worker.snapshot().outputQueueSize == 0U, "最终输出队列应归零");
}

void test_script_runtime_worker_batch_bytes_merges_adjacent_rx_events()
{
    const protoscope::scripting::ScriptRuntimeWorkerConfig defaultConfig;
    require(defaultConfig.batchBytes == 8192U, "ScriptRuntimeWorkerConfig 默认 batch_bytes 应为 8192");

    const ScopedTempPath protocolDir(makeWorkerProtocolDir("batch-bytes", workerBatchProbeScript()));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure(protoscope::scripting::ScriptRuntimeWorkerConfig{
        .execution = {.callbackTimeoutMs = 5000},
        .enabled = true,
        .rxQueueLimitBytes = 64U * 1024U,
        .outputQueueLimit = 128U,
        .batchBytes = 3U,
        .backpressureEnabled = false,
    });
    const auto loaded = worker.loadProtocolDirectory(protocolDir.path().generic_string());
    require(loaded.ok, "worker 分块测试协议应可加载");
    (void) worker.drainOutputs();

    // 核心流程：首个 RX 事件刚好填满 batch，后续相邻事件必须另起一批并继续按 batch_bytes 合并。
    worker.postTransportBytes(bytesEvent({0x01, 0x02, 0x03}, 1));
    worker.postTransportBytes(bytesEvent({0x10}, 2));
    worker.postTransportBytes(bytesEvent({0x11}, 3));
    worker.postTransportBytes(bytesEvent({0x12}, 4));
    worker.postTransportBytes(bytesEvent({0x13}, 5));
    worker.waitIdle();
    const auto outputs = worker.drainOutputs();

    require(hasEventWithTokens(outputs, "worker_bytes", "first=16", "size=3"),
            "相邻 RX 事件应合并到不超过 batch_bytes 的块");
    require(hasEventWithTokens(outputs, "worker_bytes", "first=19", "size=1"),
            "超过 batch_bytes 的后续 RX 应保留为新块");
}

void test_script_runtime_worker_can_disable_adjacent_rx_merge()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir("no-batch-bytes", workerBatchProbeScript()));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure(protoscope::scripting::ScriptRuntimeWorkerConfig{
        .execution = {.callbackTimeoutMs = 5000},
        .enabled = true,
        .rxQueueLimitBytes = 64U * 1024U,
        .outputQueueLimit = 128U,
        .batchBytes = 8U,
        .backpressureEnabled = false,
    });
    const auto loaded = worker.loadProtocolDirectory(protocolDir.path().generic_string());
    require(loaded.ok, "worker 禁止合并测试协议应可加载");
    (void) worker.drainOutputs();

    worker.postTransportBytes(bytesEvent({0x01, 0x02, 0x03}, 1), false);
    worker.postTransportBytes(bytesEvent({0x04, 0x05}, 2), false);
    worker.waitIdle();
    const auto outputs = worker.drainOutputs();

    require(hasEventWithTokens(outputs, "worker_bytes", "first=1", "size=3"),
            "禁止相邻 RX 合并时首个导入块应保持原大小");
    require(hasEventWithTokens(outputs, "worker_bytes", "first=4", "size=2"),
            "禁止相邻 RX 合并时后续导入块应保持原大小");
}

void test_script_runtime_worker_oscilloscope_toggle_sync_returns_lua_result()
{
    const ScopedTempPath protocolDir(makeWorkerProtocolDir(
        "oscilloscope-toggle",
        R"lua(
function on_oscilloscope_toggle(ctx, current_running, target_running)
  return target_running
end
)lua"));
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure(protoscope::scripting::ScriptRuntimeWorkerConfig{
        .enabled = true,
        .rxQueueLimitBytes = 64U * 1024U,
        .outputQueueLimit = 128U,
        .batchBytes = 1024U,
        .backpressureEnabled = false,
    });
    const auto loaded = worker.loadProtocolDirectory(protocolDir.path().generic_string());
    require(loaded.ok, "worker 示波器切换测试协议应可加载");
    (void) worker.drainOutputs();

    require(worker.requestOscilloscopeToggle(workerContext(), false, true),
            "worker 同步请求应传回 Lua true");
    const auto acceptedOutputs = worker.drainOutputs();
    require(hasOscilloscopeRunningUpdate(acceptedOutputs, true),
            "worker 应把 Lua true 的默认 target 状态发布到输出队列");
    require(!worker.requestOscilloscopeToggle(workerContext(), true, false),
            "worker 同步请求应传回 Lua false");
    const auto rejectedOutputs = worker.drainOutputs();
    require(!hasOscilloscopeRunningUpdate(rejectedOutputs, false),
            "Lua 返回 false 时 worker 不应默认发布 target 状态");
}

void test_pipeline_worker_threads_resolve_from_hardware_limit()
{
    using protoscope::scripting::resolvePipelineWorkerThreads;

    require(resolvePipelineWorkerThreads(std::nullopt, 8U) == 7U, "缺省线程数应使用硬件并发减一");
    require(resolvePipelineWorkerThreads(3U, 8U) == 3U, "显式线程数小于机器上限时应原样使用");
    require(resolvePipelineWorkerThreads(32U, 8U) == 7U, "显式线程数超过机器上限时应裁剪");
    require(resolvePipelineWorkerThreads(std::nullopt, 0U) == 1U, "硬件并发返回 0 时应按 1 处理");
    require(resolvePipelineWorkerThreads(0U, 8U) == 1U, "显式 0 应按最小 1 个后处理线程处理");
}
