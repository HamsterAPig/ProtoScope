#include "protoscope/scripting/script_runtime_worker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace protoscope::scripting {

namespace {

    constexpr std::size_t kMiB = 1024U * 1024U;
    constexpr std::size_t kDefaultWorkerMemoryBudgetBytes = 256U * kMiB;

    struct ConfigureCommand {
        ScriptRuntimeWorkerConfig config;
    };

    struct SetFileIoConfigCommand {
        FileIoConfig config;
    };

    struct ReloadProtocolCommand {
        std::string directory;
        std::shared_ptr<std::promise<ScriptRuntimeLoadResult>> result;
    };

    struct SetControlValueCommand {
        std::string id;
        ControlValue value;
        std::shared_ptr<std::promise<bool>> result;
    };

    struct OscilloscopeToggleCommand {
        transport::ConnectionContext context;
        bool currentRunning{false};
        bool targetRunning{false};
        std::shared_ptr<std::promise<bool>> result;
    };

    struct ClearRealtimeOutputsCommand {
        std::shared_ptr<std::promise<RealtimeOutputDiscardCounts>> result;
    };

    struct ApplyStreamRuntimeProfileCommand {
        StreamRuntimeProfileEvent event;
        std::shared_ptr<std::promise<std::pair<bool, std::string>>> result;
    };

    struct OpenCommand {
        transport::TransportOpenEvent event;
    };

    struct CloseCommand {
        transport::TransportCloseEvent event;
    };

    struct ErrorCommand {
        transport::TransportErrorEvent event;
    };

    struct BytesCommand {
        transport::TransportBytesEvent event;
    };

    struct ControlCommand {
        transport::ConnectionContext context;
        std::string id;
        ControlValue value;
    };

    struct TickCommand {
        std::uint64_t currentMs{0};
    };

    struct TxEventCommand {
        transport::ConnectionContext context;
        TxEvent event;
    };

    struct DialogEventCommand {
        transport::ConnectionContext context;
        DialogEvent event;
    };

    struct FileDialogEventCommand {
        transport::ConnectionContext context;
        FileDialogEvent event;
    };

    struct RequestAwaitingCompletionCommand {
        bool active{false};
    };

    struct IdleCommand {
        std::shared_ptr<std::promise<void>> result;
    };

    using WorkerCommand = std::variant<ConfigureCommand,
                                       SetFileIoConfigCommand,
                                       ReloadProtocolCommand,
                                       SetControlValueCommand,
                                       OscilloscopeToggleCommand,
                                       ClearRealtimeOutputsCommand,
                                       ApplyStreamRuntimeProfileCommand,
                                       OpenCommand,
                                       CloseCommand,
                                       ErrorCommand,
                                       BytesCommand,
                                       ControlCommand,
                                       TickCommand,
                                       TxEventCommand,
                                       DialogEventCommand,
                                       FileDialogEventCommand,
                                       RequestAwaitingCompletionCommand,
                                       IdleCommand>;

    ScriptRuntimeSnapshot makeSnapshot(const ScriptHost& host,
                                       std::size_t pendingRxBytes,
                                       std::size_t inputQueueSize,
                                       std::size_t outputQueueSize,
                                       std::size_t postprocessWorkerThreads)
    {
        return ScriptRuntimeSnapshot{
            .controls = host.controlsSnapshot(),
            .controlStates = host.controlStatesSnapshot(),
            .docks = host.dockSnapshots(),
            .streamBuffer = host.streamBufferDefinition(),
            .streamFrames = host.streamFrameDefinitions(),
            .nextWakeupAtMs = host.nextWakeupAtMs(),
            .scriptPath = host.scriptPath(),
            .protocolDirectory = host.protocolDirectory(),
            .lastError = host.lastError(),
            .pendingPlotAppends = host.pendingPlotAppendCount(),
            .pendingWorkerRxBytes = pendingRxBytes,
            .inputQueueSize = inputQueueSize,
            .outputQueueSize = outputQueueSize,
            .postprocessWorkerThreads = postprocessWorkerThreads,
            .lastTransportStats = host.lastTransportStats(),
        };
    }

    bool hasOutputs(const ScriptRuntimeOutputBatch& batch)
    {
        return !batch.events.empty() || !batch.logs.empty() || !batch.txRequests.empty() ||
               !batch.requestGuardResets.empty() || !batch.plotSetups.empty() || !batch.plotAppends.empty() ||
               !batch.requestDoneResults.empty() || !batch.statusUpdates.empty() ||
               !batch.oscilloscopeRunningUpdates.empty() || !batch.streamRuntimeProfiles.empty() ||
               !batch.dialogRequests.empty() ||
               !batch.fileDialogRequests.empty() || batch.transportStats.has_value();
    }

    ScriptRuntimeOutputBatch drainHostOutputs(ScriptHost& host, bool includeTransportStats)
    {
        ScriptRuntimeOutputBatch batch{
            .events = host.drainEvents(),
            .logs = host.drainLogs(),
            .txRequests = host.drainTxRequests(),
            .requestGuardResets = host.drainRequestGuardResets(),
            .plotSetups = host.drainPlotSetups(),
            .plotAppends = host.drainPlotAppends(),
            .requestDoneResults = host.drainRequestDoneResults(),
            .statusUpdates = host.drainStatusUpdates(),
            .oscilloscopeRunningUpdates = host.drainOscilloscopeRunningUpdates(),
            .streamRuntimeProfiles = host.drainStreamRuntimeProfileEvents(),
            .dialogRequests = host.drainDialogRequests(),
            .fileDialogRequests = host.drainFileDialogRequests(),
            .transportStats = includeTransportStats ? std::optional<ScriptHostTransportStats>{host.lastTransportStats()}
                                                    : std::nullopt,
        };
        return batch;
    }

    std::size_t availableSystemMemoryBytes()
    {
#ifdef _WIN32
        MEMORYSTATUSEX status{};
        status.dwLength = sizeof(status);
        if (GlobalMemoryStatusEx(&status) != 0) {
            const auto clamped = (std::min<unsigned long long>) (status.ullAvailPhys,
                                                                 static_cast<unsigned long long>(
                                                                     (std::numeric_limits<std::size_t>::max)()));
            return static_cast<std::size_t>(clamped);
        }
#endif
        return 0U;
    }

    std::size_t resolveMemoryBudgetBytes(const ScriptRuntimeWorkerConfig& config)
    {
        if (config.memoryBudgetBytes > 0U) {
            return config.memoryBudgetBytes;
        }
        if (config.memoryBudgetAvailableRatio > 0.0) {
            const auto availableBytes = availableSystemMemoryBytes();
            if (availableBytes > 0U) {
                return static_cast<std::size_t>(static_cast<double>(availableBytes) *
                                                config.memoryBudgetAvailableRatio);
            }
        }
        return kDefaultWorkerMemoryBudgetBytes;
    }

    std::size_t rxQueueBudgetBytes(const ScriptRuntimeWorkerConfig& config)
    {
        const auto memoryBudgetBytes = resolveMemoryBudgetBytes(config);
        if (config.rxQueueLimitBytes == 0U) {
            return memoryBudgetBytes;
        }
        if (memoryBudgetBytes == 0U) {
            return config.rxQueueLimitBytes;
        }
        return (std::min)(config.rxQueueLimitBytes, memoryBudgetBytes);
    }

    std::size_t watermarkBytes(std::size_t budgetBytes, double ratio)
    {
        if (budgetBytes == 0U || ratio <= 0.0) {
            return 0U;
        }
        if (ratio >= 1.0) {
            return budgetBytes;
        }
        return static_cast<std::size_t>(static_cast<double>(budgetBytes) * ratio);
    }

} // namespace

struct ScriptRuntimeWorker::Impl {
    mutable std::mutex mutex;
#ifndef __MINGW32__
    std::mutex waitMutex;
    std::condition_variable commandAvailable;
    bool wakeRequested{false};
#else
    std::atomic_uint64_t wakeGeneration{0};
#endif
    std::deque<WorkerCommand> commands;
    std::deque<ScriptRuntimeOutputBatch> outputs;
    ScriptRuntimeWorkerConfig config{};
    ScriptRuntimeSnapshot snapshot{};
    std::size_t pendingRxBytes{0};
    bool rxBackpressureWarningActive{false};
    bool outputQueueWarningActive{false};
    bool stopping{false};
    std::atomic_bool failed{false};
    std::thread thread;
    std::condition_variable rxBackpressureChanged;

    Impl()
    {
        thread = std::thread([this]() {
            try {
                run();
            } catch (const std::exception& ex) {
                failed = true;
                std::cerr << "[ScriptRuntimeWorker] worker 线程异常: " << ex.what() << '\n' << std::flush;
            } catch (...) {
                failed = true;
                std::cerr << "[ScriptRuntimeWorker] worker 线程异常: 未知异常\n" << std::flush;
            }
        });
    }

    ~Impl() { stop(); }

    void stop()
    {
        bool shouldNotify = false;
        {
            std::lock_guard lock(mutex);
            if (!stopping) {
                stopping = true;
                shouldNotify = true;
            }
        }
        if (shouldNotify) {
            signalCommandAvailable();
            rxBackpressureChanged.notify_all();
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    void pushCommand(WorkerCommand command)
    {
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            commands.push_back(std::move(command));
        }
        signalCommandAvailable();
    }

    void pushBytes(transport::TransportBytesEvent event)
    {
        if (event.bytes.empty()) {
            return;
        }
        bool syncMode = false;
        bool merged = false;
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            pendingRxBytes += event.bytes.size();
            syncMode = !config.enabled;
            if (!commands.empty()) {
                auto* previous = std::get_if<BytesCommand>(&commands.back());
                if (previous != nullptr && previous->event.context.connectionId == event.context.connectionId &&
                    previous->event.context.readyForIo == event.context.readyForIo &&
                    previous->event.context.endpoint == event.context.endpoint &&
                    previous->event.bytes.size() + event.bytes.size() <= config.batchBytes) {
                    previous->event.bytes.insert(previous->event.bytes.end(), event.bytes.begin(), event.bytes.end());
                    previous->event.context.timestampMs = event.context.timestampMs;
                    merged = true;
                }
            }
            if (!merged) {
                commands.emplace_back(BytesCommand{.event = std::move(event)});
            }
        }
        signalCommandAvailable();
        if (syncMode) {
            waitIdle();
        } else if (config.backpressureEnabled) {
            waitUntilRxQueueBelowLowWatermark();
        }
    }

    void signalCommandAvailable()
    {
#ifndef __MINGW32__
        {
            std::lock_guard waitLock(waitMutex);
            wakeRequested = true;
        }
        commandAvailable.notify_all();
#else
        wakeGeneration.fetch_add(1, std::memory_order_release);
        wakeGeneration.notify_one();
#endif
    }

    template <typename T> T runSync(WorkerCommand command, std::shared_ptr<std::promise<T>> promise)
    {
        auto future = promise->get_future();
        pushCommand(std::move(command));
        const auto startedAt = std::chrono::steady_clock::now();
        while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            if (failed) {
                throw std::runtime_error("ScriptRuntimeWorker 线程已异常退出");
            }
            if (std::chrono::steady_clock::now() - startedAt > std::chrono::seconds(30)) {
                throw std::runtime_error("ScriptRuntimeWorker 同步命令等待超时");
            }
        }
        return future.get();
    }

    void waitIdle()
    {
        auto promise = std::make_shared<std::promise<void>>();
        auto future = promise->get_future();
        pushCommand(IdleCommand{.result = promise});
        const auto startedAt = std::chrono::steady_clock::now();
        while (future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            if (failed) {
                throw std::runtime_error("ScriptRuntimeWorker 线程已异常退出");
            }
            if (std::chrono::steady_clock::now() - startedAt > std::chrono::seconds(30)) {
                throw std::runtime_error("ScriptRuntimeWorker 空闲等待超时");
            }
        }
    }

    std::size_t pendingRxBytesCount() const
    {
        std::lock_guard lock(mutex);
        return pendingRxBytes;
    }

    std::vector<ScriptRuntimeOutputBatch> drainOutputs()
    {
        std::vector<ScriptRuntimeOutputBatch> drained;
        std::lock_guard lock(mutex);
        drained.reserve(outputs.size());
        while (!outputs.empty()) {
            drained.push_back(std::move(outputs.front()));
            outputs.pop_front();
        }
        snapshot.outputQueueSize = outputs.size();
        return drained;
    }

    std::optional<ScriptRuntimeOutputBatch> drainOneOutput()
    {
        std::lock_guard lock(mutex);
        if (outputs.empty()) {
            return std::nullopt;
        }
        auto batch = std::move(outputs.front());
        outputs.pop_front();
        outputQueueWarningActive = config.outputQueueLimit > 0U && outputs.size() > config.outputQueueLimit;
        snapshot.outputQueueSize = outputs.size();
        return batch;
    }

    ScriptRuntimeSnapshot currentSnapshot() const
    {
        std::lock_guard lock(mutex);
        return snapshot;
    }

    void publishOutputs(ScriptRuntimeOutputBatch batch)
    {
        if (!hasOutputs(batch)) {
            return;
        }
        std::lock_guard lock(mutex);
        outputs.push_back(std::move(batch));
        pushOutputQueueWarningIfNeededLocked();
        snapshot.outputQueueSize = outputs.size();
    }

    void publishSnapshot(const ScriptHost& host)
    {
        std::lock_guard lock(mutex);
        snapshot = makeSnapshot(host, pendingRxBytes, commands.size(), outputs.size(), config.postprocessWorkerThreads);
    }

    void subtractPendingRxBytes(std::size_t bytes)
    {
        std::lock_guard lock(mutex);
        const auto before = pendingRxBytes;
        pendingRxBytes -= (std::min)(pendingRxBytes, bytes);
        if (pendingRxBytes < before) {
            rxBackpressureChanged.notify_all();
        }
    }

    void clearQueuedRxLocked()
    {
        std::deque<WorkerCommand> kept;
        while (!commands.empty()) {
            auto command = std::move(commands.front());
            commands.pop_front();
            if (auto* bytes = std::get_if<BytesCommand>(&command); bytes != nullptr) {
                pendingRxBytes -= (std::min)(pendingRxBytes, bytes->event.bytes.size());
                continue;
            }
            kept.push_back(std::move(command));
        }
        commands = std::move(kept);
    }

    void pushRxBackpressureWarningLocked(std::size_t pendingBytes, std::size_t highWaterBytes)
    {
        ScriptRuntimeOutputBatch batch;
        batch.logs.push_back(ScriptLog{
            .level = "warn",
            .message = "脚本 worker RX 队列达到高水位，已暂停投递等待后台解析: pending=" +
                       std::to_string(pendingBytes) + " 字节, high_water=" + std::to_string(highWaterBytes) + " 字节",
            .timestampMs = 0,
        });
        outputs.push_back(std::move(batch));
        pushOutputQueueWarningIfNeededLocked();
        snapshot.outputQueueSize = outputs.size();
    }

    void pushOutputQueueWarningIfNeededLocked()
    {
        if (config.outputQueueLimit == 0U || outputs.size() <= config.outputQueueLimit || outputQueueWarningActive) {
            return;
        }
        outputQueueWarningActive = true;
        ScriptRuntimeOutputBatch batch;
        batch.logs.push_back(ScriptLog{
            .level = "warn",
            .message = "脚本 worker 输出队列超过告警阈值，UI 将继续分帧 drain: " + std::to_string(outputs.size()) +
                       "/" + std::to_string(config.outputQueueLimit),
            .timestampMs = 0,
        });
        outputs.push_back(std::move(batch));
    }

    void waitUntilRxQueueBelowLowWatermark()
    {
        std::unique_lock lock(mutex);
        const auto budgetBytes = rxQueueBudgetBytes(config);
        const auto highWaterBytes = watermarkBytes(budgetBytes, config.backpressureHighWatermark);
        if (budgetBytes == 0U || highWaterBytes == 0U || pendingRxBytes <= highWaterBytes) {
            return;
        }
        if (!rxBackpressureWarningActive) {
            rxBackpressureWarningActive = true;
            // 核心流程：预算耗尽时暂停投递并显式告警，不再静默丢弃已排队 RX。
            pushRxBackpressureWarningLocked(pendingRxBytes, highWaterBytes);
        }
        const auto lowWaterBytes =
            (std::min)(highWaterBytes, watermarkBytes(budgetBytes, config.backpressureLowWatermark));
        rxBackpressureChanged.wait(lock,
                                   [this, lowWaterBytes]() { return stopping || pendingRxBytes <= lowWaterBytes; });
        if (pendingRxBytes <= lowWaterBytes) {
            rxBackpressureWarningActive = false;
        }
    }

    struct CommandExecutionResult {
        bool includeTransportStats{false};
        std::shared_ptr<std::promise<void>> idlePromise;
    };

    std::optional<WorkerCommand> popCommandOrWait()
    {
        for (;;) {
#ifdef __MINGW32__
            const auto observedWake = wakeGeneration.load(std::memory_order_acquire);
#endif
            WorkerCommand command;
            bool hasCommand = false;
            bool shouldWait = false;
            {
                std::lock_guard lock(mutex);
                if (commands.empty()) {
                    if (stopping) {
                        return std::nullopt;
                    }
                    shouldWait = true;
                } else {
                    command = std::move(commands.front());
                    commands.pop_front();
                    hasCommand = true;
                }
            }
            if (hasCommand) {
                return command;
            }

#ifndef __MINGW32__
            std::unique_lock waitLock(waitMutex);
            commandAvailable.wait(waitLock, [this]() { return wakeRequested; });
            wakeRequested = false;
#else
            if (shouldWait) {
                wakeGeneration.wait(observedWake, std::memory_order_acquire);
            }
#endif
        }
    }

    ScriptRuntimeLoadResult reloadHostProtocol(ScriptHost& host,
                                               std::optional<std::uint64_t>& activeConnectionId,
                                               const std::string& directory)
    {
        {
            std::lock_guard lock(mutex);
            clearQueuedRxLocked();
        }
        host.clearPendingRealtimeOutputs();
        const bool ok = host.loadProtocolDirectory(directory);
        activeConnectionId.reset();
        ScriptRuntimeSnapshot resultSnapshot;
        {
            std::lock_guard lock(mutex);
            resultSnapshot =
                makeSnapshot(host, pendingRxBytes, commands.size(), outputs.size(), config.postprocessWorkerThreads);
        }
        return ScriptRuntimeLoadResult{
            .ok = ok,
            .lastError = host.lastError(),
            .snapshot = resultSnapshot,
        };
    }

    RealtimeOutputDiscardCounts clearHostRealtimeOutputs(ScriptHost& host)
    {
        auto counts = host.clearPendingRealtimeOutputs();
        {
            std::lock_guard lock(mutex);
            for (const auto& batch : outputs) {
                counts.logs += batch.logs.size();
                counts.events += batch.events.size();
                counts.plotAppends += batch.plotAppends.size();
            }
            outputs.clear();
        }
        return counts;
    }

    bool handleHostBytesCommand(ScriptHost& host,
                                std::optional<std::uint64_t>& activeConnectionId,
                                const BytesCommand& command)
    {
        const auto& event = command.event;
        subtractPendingRxBytes(event.bytes.size());
        if (activeConnectionId.has_value() && event.context.readyForIo &&
            *activeConnectionId != event.context.connectionId) {
            return false;
        }

        host.onTransportBytes(event);
        if (event.context.readyForIo) {
            activeConnectionId = event.context.connectionId;
        }
        return true;
    }

    CommandExecutionResult executeCommandItem(ScriptHost&, std::optional<std::uint64_t>&, ConfigureCommand& command)
    {
        std::lock_guard lock(mutex);
        config = command.config;
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>&,
                                              SetFileIoConfigCommand& command)
    {
        host.setFileIoConfig(std::move(command.config));
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>& activeConnectionId,
                                              ReloadProtocolCommand& command)
    {
        command.result->set_value(reloadHostProtocol(host, activeConnectionId, command.directory));
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>&,
                                              SetControlValueCommand& command)
    {
        command.result->set_value(host.setControlValue(command.id, command.value));
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>&,
                                              OscilloscopeToggleCommand& command)
    {
        command.result->set_value(host.requestOscilloscopeToggle(
            command.context, command.currentRunning, command.targetRunning));
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>&,
                                              ClearRealtimeOutputsCommand& command)
    {
        command.result->set_value(clearHostRealtimeOutputs(host));
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>&,
                                              ApplyStreamRuntimeProfileCommand& command)
    {
        std::string error;
        const bool ok = host.applyStreamRuntimeProfileEvent(command.event, error);
        command.result->set_value(std::pair<bool, std::string>{ok, std::move(error)});
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>& activeConnectionId,
                                              OpenCommand& command)
    {
        if (command.event.context.readyForIo) {
            activeConnectionId = command.event.context.connectionId;
        }
        host.onTransportOpen(command.event);
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>& activeConnectionId,
                                              CloseCommand& command)
    {
        host.onTransportClose(command.event);
        if (activeConnectionId.has_value() && *activeConnectionId == command.event.context.connectionId) {
            activeConnectionId.reset();
        }
        std::lock_guard lock(mutex);
        clearQueuedRxLocked();
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host, std::optional<std::uint64_t>&, ErrorCommand& command)
    {
        host.onTransportError(command.event);
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>& activeConnectionId,
                                              BytesCommand& command)
    {
        CommandExecutionResult result;
        result.includeTransportStats = handleHostBytesCommand(host, activeConnectionId, command);
        return result;
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host, std::optional<std::uint64_t>&, ControlCommand& command)
    {
        host.onControl(command.context, command.id, command.value);
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host, std::optional<std::uint64_t>&, TickCommand& command)
    {
        host.tick(command.currentMs);
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host, std::optional<std::uint64_t>&, TxEventCommand& command)
    {
        host.onTxEvent(command.context, command.event);
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>&,
                                              DialogEventCommand& command)
    {
        host.onDialogEvent(command.context, command.event);
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>&,
                                              FileDialogEventCommand& command)
    {
        host.onFileDialogEvent(command.context, command.event);
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost& host,
                                              std::optional<std::uint64_t>&,
                                              RequestAwaitingCompletionCommand& command)
    {
        host.setRequestAwaitingCompletion(command.active);
        return {};
    }

    CommandExecutionResult executeCommandItem(ScriptHost&, std::optional<std::uint64_t>&, IdleCommand& command)
    {
        CommandExecutionResult result;
        result.idlePromise = command.result;
        return result;
    }

    CommandExecutionResult executeCommand(ScriptHost& host,
                                          std::optional<std::uint64_t>& activeConnectionId,
                                          WorkerCommand& command)
    {
        CommandExecutionResult result;

        // 核心流程：命令副作用按命令类型下沉到具名 helper，run 只负责取命令和发布执行后的状态。
        std::visit([&](auto& item) { result = executeCommandItem(host, activeConnectionId, item); }, command);

        return result;
    }

    void run()
    {
        ScriptHost host;
        std::optional<std::uint64_t> activeConnectionId;
        publishSnapshot(host);

        for (;;) {
            auto command = popCommandOrWait();
            if (!command.has_value()) {
                break;
            }

            const auto execution = executeCommand(host, activeConnectionId, *command);
            publishOutputs(drainHostOutputs(host, execution.includeTransportStats));
            publishSnapshot(host);
            if (execution.idlePromise) {
                execution.idlePromise->set_value();
            }
        }
    }
};

ScriptRuntimeWorker::ScriptRuntimeWorker() : impl_(std::make_unique<Impl>()) {}

ScriptRuntimeWorker::~ScriptRuntimeWorker() = default;

void ScriptRuntimeWorker::configure(ScriptRuntimeWorkerConfig config)
{
    impl_->pushCommand(ConfigureCommand{.config = config});
}

void ScriptRuntimeWorker::setFileIoConfig(FileIoConfig config)
{
    impl_->pushCommand(SetFileIoConfigCommand{.config = std::move(config)});
}

ScriptRuntimeLoadResult ScriptRuntimeWorker::loadProtocolDirectory(const std::string& directory)
{
    auto promise = std::make_shared<std::promise<ScriptRuntimeLoadResult>>();
    return impl_->runSync<ScriptRuntimeLoadResult>(ReloadProtocolCommand{.directory = directory, .result = promise},
                                                   promise);
}

bool ScriptRuntimeWorker::setControlValue(const std::string& id, const ControlValue& value)
{
    auto promise = std::make_shared<std::promise<bool>>();
    return impl_->runSync<bool>(SetControlValueCommand{.id = id, .value = value, .result = promise}, promise);
}

bool ScriptRuntimeWorker::requestOscilloscopeToggle(transport::ConnectionContext context,
                                                    bool currentRunning,
                                                    bool targetRunning)
{
    auto promise = std::make_shared<std::promise<bool>>();
    return impl_->runSync<bool>(OscilloscopeToggleCommand{
                                    .context = std::move(context),
                                    .currentRunning = currentRunning,
                                    .targetRunning = targetRunning,
                                    .result = promise,
                                },
                                promise);
}

RealtimeOutputDiscardCounts ScriptRuntimeWorker::clearPendingRealtimeOutputs()
{
    auto promise = std::make_shared<std::promise<RealtimeOutputDiscardCounts>>();
    return impl_->runSync<RealtimeOutputDiscardCounts>(ClearRealtimeOutputsCommand{.result = promise}, promise);
}

void ScriptRuntimeWorker::postTransportOpen(transport::TransportOpenEvent event)
{
    impl_->pushCommand(OpenCommand{.event = std::move(event)});
}

void ScriptRuntimeWorker::postTransportClose(transport::TransportCloseEvent event)
{
    impl_->pushCommand(CloseCommand{.event = std::move(event)});
}

void ScriptRuntimeWorker::postTransportError(transport::TransportErrorEvent event)
{
    impl_->pushCommand(ErrorCommand{.event = std::move(event)});
}

void ScriptRuntimeWorker::postTransportBytes(transport::TransportBytesEvent event)
{
    impl_->pushBytes(std::move(event));
}

void ScriptRuntimeWorker::postControl(transport::ConnectionContext context, std::string id, ControlValue value)
{
    impl_->pushCommand(ControlCommand{.context = std::move(context), .id = std::move(id), .value = std::move(value)});
}

void ScriptRuntimeWorker::postTick(std::uint64_t currentMs)
{
    impl_->pushCommand(TickCommand{.currentMs = currentMs});
}

void ScriptRuntimeWorker::postTxEvent(transport::ConnectionContext context, TxEvent event)
{
    impl_->pushCommand(TxEventCommand{.context = std::move(context), .event = std::move(event)});
}

void ScriptRuntimeWorker::postDialogEvent(transport::ConnectionContext context, DialogEvent event)
{
    impl_->pushCommand(DialogEventCommand{.context = std::move(context), .event = std::move(event)});
}

void ScriptRuntimeWorker::postFileDialogEvent(transport::ConnectionContext context, FileDialogEvent event)
{
    impl_->pushCommand(FileDialogEventCommand{.context = std::move(context), .event = std::move(event)});
}

bool ScriptRuntimeWorker::applyStreamRuntimeProfileEvent(StreamRuntimeProfileEvent event, std::string& error)
{
    auto promise = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    const auto result = impl_->runSync<std::pair<bool, std::string>>(
        ApplyStreamRuntimeProfileCommand{.event = std::move(event), .result = promise}, promise);
    error = result.second;
    return result.first;
}

void ScriptRuntimeWorker::postRequestAwaitingCompletion(bool active)
{
    impl_->pushCommand(RequestAwaitingCompletionCommand{.active = active});
}

void ScriptRuntimeWorker::waitIdle()
{
    impl_->waitIdle();
}

std::size_t ScriptRuntimeWorker::pendingRxBytes() const
{
    return impl_->pendingRxBytesCount();
}

std::vector<ScriptRuntimeOutputBatch> ScriptRuntimeWorker::drainOutputs()
{
    return impl_->drainOutputs();
}

std::optional<ScriptRuntimeOutputBatch> ScriptRuntimeWorker::drainOneOutput()
{
    return impl_->drainOneOutput();
}

ScriptRuntimeSnapshot ScriptRuntimeWorker::snapshot() const
{
    return impl_->currentSnapshot();
}

void ScriptRuntimeWorker::stop()
{
    impl_->stop();
}

} // namespace protoscope::scripting
