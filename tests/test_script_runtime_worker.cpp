#include "test_registry.hpp"

#include "protoscope/scripting/pipeline_threading.hpp"
#include "protoscope/scripting/script_runtime_worker.hpp"
#include "protoscope/transport/transport.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path makeWorkerProtocolDir(const char* name, const std::string& script) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = std::filesystem::temp_directory_path() /
                     (std::string("protoscope-worker-") + name + "-" + std::to_string(ticks));
    std::filesystem::create_directories(dir);
    std::ofstream out(dir / "main.lua");
    out << script;
    return dir;
}

protoscope::transport::ConnectionContext workerContext(std::uint64_t timestampMs = 1) {
    return protoscope::transport::ConnectionContext{
        .kind = protoscope::transport::TransportKind::TcpClient,
        .endpoint = "worker-test",
        .connectionId = 42,
        .timestampMs = timestampMs,
        .readyForIo = true,
    };
}

protoscope::transport::TransportBytesEvent bytesEvent(std::vector<std::uint8_t> bytes, std::uint64_t timestampMs = 1) {
    return protoscope::transport::TransportBytesEvent{
        .context = workerContext(timestampMs),
        .bytes = std::move(bytes),
    };
}

bool hasEvent(const std::vector<protoscope::scripting::ScriptRuntimeOutputBatch>& batches,
              const std::string& eventName,
              const std::string& token) {
    for (const auto& batch : batches) {
        for (const auto& event : batch.events) {
            if (event.name == eventName && event.payload.find(token) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

bool hasLog(const std::vector<protoscope::scripting::ScriptRuntimeOutputBatch>& batches, const std::string& token) {
    for (const auto& batch : batches) {
        for (const auto& log : batch.logs) {
            if (log.message.find(token) != std::string::npos) {
                return true;
            }
        }
    }
    return false;
}

std::string workerProbeScript() {
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

} // namespace

void test_script_runtime_worker_disabled_mode_waits_for_rx_idle() {
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure(protoscope::scripting::ScriptRuntimeWorkerConfig{
        .enabled = false,
        .rxQueueLimitBytes = 64U * 1024U,
        .outputQueueLimit = 128U,
        .batchBytes = 1024U,
    });
    const auto protocolDir = makeWorkerProtocolDir("sync", workerProbeScript());
    const auto loaded = worker.loadProtocolDirectory(protocolDir.generic_string());
    require(loaded.ok, "worker 同步模式测试协议应可加载");
    (void)worker.drainOutputs();

    worker.postTransportBytes(bytesEvent({0x42, 0x43, 0x44}));
    const auto outputs = worker.drainOutputs();

    require(worker.snapshot().pendingWorkerRxBytes == 0U, "禁用异步 worker 时 postTransportBytes 应等待 RX 空闲");
    require(hasEvent(outputs, "worker_bytes", "first=66"), "禁用异步 worker 时应立即产出脚本事件");
}

void test_script_runtime_worker_rx_limit_drops_oldest_bytes_only() {
    protoscope::scripting::ScriptRuntimeWorker worker;
    worker.configure(protoscope::scripting::ScriptRuntimeWorkerConfig{
        .enabled = true,
        .rxQueueLimitBytes = 6U,
        .outputQueueLimit = 128U,
        .batchBytes = 1U,
    });
    const auto protocolDir = makeWorkerProtocolDir("rx-limit", workerProbeScript());
    const auto loaded = worker.loadProtocolDirectory(protocolDir.generic_string());
    require(loaded.ok, "worker 限流测试协议应可加载");
    (void)worker.drainOutputs();

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

    require(hasLog(outputs, "RX 队列超过限制"), "RX 队列超限时应通过脚本日志告警");
    require(hasEvent(outputs, "worker_bytes", "first=1"), "阻塞中的首个 RX 事件不应被限流清理");
    require(!hasEvent(outputs, "worker_bytes", "first=161"), "限流应丢弃最旧的待解析 RX 字节命令");
    require(hasEvent(outputs, "worker_bytes", "first=178"), "限流后应保留较新的待解析 RX 字节命令");
    require(hasEvent(outputs, "worker_bytes", "first=195"), "限流后应保留最新的待解析 RX 字节命令");
}

void test_pipeline_worker_threads_resolve_from_hardware_limit() {
    using protoscope::scripting::resolvePipelineWorkerThreads;

    require(resolvePipelineWorkerThreads(std::nullopt, 8U) == 7U, "缺省线程数应使用硬件并发减一");
    require(resolvePipelineWorkerThreads(3U, 8U) == 3U, "显式线程数小于机器上限时应原样使用");
    require(resolvePipelineWorkerThreads(32U, 8U) == 7U, "显式线程数超过机器上限时应裁剪");
    require(resolvePipelineWorkerThreads(std::nullopt, 0U) == 1U, "硬件并发返回 0 时应按 1 处理");
    require(resolvePipelineWorkerThreads(0U, 8U) == 1U, "显式 0 应按最小 1 个后处理线程处理");
}
