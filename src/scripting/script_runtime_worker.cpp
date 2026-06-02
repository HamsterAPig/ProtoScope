#include "protoscope/scripting/script_runtime_worker.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <future>
#include <mutex>
#include <thread>
#include <utility>
#include <variant>

namespace protoscope::scripting {

namespace {

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
                                   std::size_t postprocessWorkerThreads) {
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

bool hasOutputs(const ScriptRuntimeOutputBatch& batch) {
    return !batch.events.empty() || !batch.logs.empty() || !batch.txRequests.empty() || !batch.plotSetups.empty() ||
           !batch.plotAppends.empty() || !batch.requestDoneResults.empty() || !batch.statusUpdates.empty() ||
           !batch.streamRuntimeProfiles.empty() || !batch.dialogRequests.empty() || !batch.fileDialogRequests.empty() ||
           batch.transportStats.has_value();
}

ScriptRuntimeOutputBatch drainHostOutputs(ScriptHost& host, bool includeTransportStats) {
    ScriptRuntimeOutputBatch batch{
        .events = host.drainEvents(),
        .logs = host.drainLogs(),
        .txRequests = host.drainTxRequests(),
        .plotSetups = host.drainPlotSetups(),
        .plotAppends = host.drainPlotAppends(),
        .requestDoneResults = host.drainRequestDoneResults(),
        .statusUpdates = host.drainStatusUpdates(),
        .streamRuntimeProfiles = host.drainStreamRuntimeProfileEvents(),
        .dialogRequests = host.drainDialogRequests(),
        .fileDialogRequests = host.drainFileDialogRequests(),
        .transportStats = includeTransportStats ? std::optional<ScriptHostTransportStats>{host.lastTransportStats()} : std::nullopt,
    };
    return batch;
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
    bool stopping{false};
    std::atomic_bool failed{false};
    std::thread thread;

    Impl() {
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

    ~Impl() {
        stop();
    }

    void stop() {
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
        }
        if (thread.joinable()) {
            thread.join();
        }
    }

    void pushCommand(WorkerCommand command) {
        {
            std::lock_guard lock(mutex);
            if (stopping) {
                return;
            }
            commands.push_back(std::move(command));
        }
        signalCommandAvailable();
    }

    void pushBytes(transport::TransportBytesEvent event) {
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
            dropQueuedRxUntilWithinLimitLocked(event.bytes.size());
            pendingRxBytes += event.bytes.size();
            syncMode = !config.enabled;
            if (!commands.empty()) {
                auto* previous = std::get_if<BytesCommand>(&commands.back());
                if (previous != nullptr &&
                    previous->event.context.connectionId == event.context.connectionId &&
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
        }
    }

    void signalCommandAvailable() {
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

    template <typename T>
    T runSync(WorkerCommand command, std::shared_ptr<std::promise<T>> promise) {
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

    void waitIdle() {
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

    std::vector<ScriptRuntimeOutputBatch> drainOutputs() {
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

    ScriptRuntimeSnapshot currentSnapshot() const {
        std::lock_guard lock(mutex);
        return snapshot;
    }

    void publishOutputs(ScriptRuntimeOutputBatch batch) {
        if (!hasOutputs(batch)) {
            return;
        }
        std::lock_guard lock(mutex);
        outputs.push_back(std::move(batch));
        while (outputs.size() > config.outputQueueLimit && !outputs.empty()) {
            outputs.pop_front();
        }
        snapshot.outputQueueSize = outputs.size();
    }

    void publishSnapshot(const ScriptHost& host) {
        std::lock_guard lock(mutex);
        snapshot = makeSnapshot(host, pendingRxBytes, commands.size(), outputs.size(), config.postprocessWorkerThreads);
    }

    void subtractPendingRxBytes(std::size_t bytes) {
        std::lock_guard lock(mutex);
        pendingRxBytes -= (std::min)(pendingRxBytes, bytes);
    }

    void clearQueuedRxLocked() {
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

    void pushRxLimitWarningLocked(std::size_t droppedBytes) {
        ScriptRuntimeOutputBatch batch;
        batch.logs.push_back(ScriptLog{
            .level = "warn",
            .message = "脚本 worker RX 队列超过限制，已丢弃最旧待解析字节: " + std::to_string(droppedBytes) + " 字节",
            .timestampMs = 0,
        });
        outputs.push_back(std::move(batch));
        while (outputs.size() > config.outputQueueLimit && !outputs.empty()) {
            outputs.pop_front();
        }
        snapshot.outputQueueSize = outputs.size();
    }

    void dropQueuedRxUntilWithinLimitLocked(std::size_t incomingBytes) {
        if (config.rxQueueLimitBytes == 0U) {
            return;
        }
        bool warned = false;
        while (pendingRxBytes + incomingBytes > config.rxQueueLimitBytes) {
            auto oldestBytes = std::find_if(commands.begin(), commands.end(), [](const WorkerCommand& command) {
                return std::holds_alternative<BytesCommand>(command);
            });
            if (oldestBytes == commands.end()) {
                break;
            }
            const auto droppedBytes = std::get<BytesCommand>(*oldestBytes).event.bytes.size();
            pendingRxBytes -= (std::min)(pendingRxBytes, droppedBytes);
            commands.erase(oldestBytes);
            // 核心流程：限流只淘汰已排队的 RX 字节，不触碰配置、重载、控制和关闭等命令。
            if (!warned) {
                pushRxLimitWarningLocked(droppedBytes);
                warned = true;
            }
        }
    }

    void run() {
        ScriptHost host;
        std::optional<std::uint64_t> activeConnectionId;
        publishSnapshot(host);

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
                        break;
                    }
                    shouldWait = true;
                } else {
                    command = std::move(commands.front());
                    commands.pop_front();
                    hasCommand = true;
                }
            }
            if (!hasCommand) {
#ifndef __MINGW32__
                std::unique_lock waitLock(waitMutex);
                commandAvailable.wait(waitLock, [this]() {
                    return wakeRequested;
                });
                wakeRequested = false;
#else
                if (shouldWait) {
                    wakeGeneration.wait(observedWake, std::memory_order_acquire);
                }
#endif
                continue;
            }

            bool includeTransportStats = false;
            std::shared_ptr<std::promise<void>> idlePromise;

            std::visit(
                [&](auto& item) {
                    using T = std::decay_t<decltype(item)>;
                    if constexpr (std::is_same_v<T, ConfigureCommand>) {
                        std::lock_guard lock(mutex);
                        config = item.config;
                    } else if constexpr (std::is_same_v<T, SetFileIoConfigCommand>) {
                        host.setFileIoConfig(std::move(item.config));
                    } else if constexpr (std::is_same_v<T, ReloadProtocolCommand>) {
                        {
                            std::lock_guard lock(mutex);
                            clearQueuedRxLocked();
                        }
                        host.clearPendingRealtimeOutputs();
                        const bool ok = host.loadProtocolDirectory(item.directory);
                        activeConnectionId.reset();
                        ScriptRuntimeSnapshot resultSnapshot;
                        {
                            std::lock_guard lock(mutex);
                            resultSnapshot =
                                makeSnapshot(host, pendingRxBytes, commands.size(), outputs.size(), config.postprocessWorkerThreads);
                        }
                        item.result->set_value(ScriptRuntimeLoadResult{
                            .ok = ok,
                            .lastError = host.lastError(),
                            .snapshot = resultSnapshot,
                        });
                    } else if constexpr (std::is_same_v<T, SetControlValueCommand>) {
                        item.result->set_value(host.setControlValue(item.id, item.value));
                    } else if constexpr (std::is_same_v<T, ClearRealtimeOutputsCommand>) {
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
                        item.result->set_value(counts);
                    } else if constexpr (std::is_same_v<T, ApplyStreamRuntimeProfileCommand>) {
                        std::string error;
                        const bool ok = host.applyStreamRuntimeProfileEvent(item.event, error);
                        item.result->set_value(std::pair<bool, std::string>{ok, std::move(error)});
                    } else if constexpr (std::is_same_v<T, OpenCommand>) {
                        if (item.event.context.readyForIo) {
                            activeConnectionId = item.event.context.connectionId;
                        }
                        host.onTransportOpen(item.event);
                    } else if constexpr (std::is_same_v<T, CloseCommand>) {
                        host.onTransportClose(item.event);
                        if (activeConnectionId.has_value() && *activeConnectionId == item.event.context.connectionId) {
                            activeConnectionId.reset();
                        }
                        std::lock_guard lock(mutex);
                        clearQueuedRxLocked();
                    } else if constexpr (std::is_same_v<T, ErrorCommand>) {
                        host.onTransportError(item.event);
                    } else if constexpr (std::is_same_v<T, BytesCommand>) {
                        subtractPendingRxBytes(item.event.bytes.size());
                        if (!activeConnectionId.has_value() || !item.event.context.readyForIo ||
                            *activeConnectionId == item.event.context.connectionId) {
                            host.onTransportBytes(item.event);
                            if (item.event.context.readyForIo) {
                                activeConnectionId = item.event.context.connectionId;
                            }
                            includeTransportStats = true;
                        }
                    } else if constexpr (std::is_same_v<T, ControlCommand>) {
                        host.onControl(item.context, item.id, item.value);
                    } else if constexpr (std::is_same_v<T, TickCommand>) {
                        host.tick(item.currentMs);
                    } else if constexpr (std::is_same_v<T, TxEventCommand>) {
                        host.onTxEvent(item.context, item.event);
                    } else if constexpr (std::is_same_v<T, DialogEventCommand>) {
                        host.onDialogEvent(item.context, item.event);
                    } else if constexpr (std::is_same_v<T, FileDialogEventCommand>) {
                        host.onFileDialogEvent(item.context, item.event);
                    } else if constexpr (std::is_same_v<T, RequestAwaitingCompletionCommand>) {
                        host.setRequestAwaitingCompletion(item.active);
                    } else if constexpr (std::is_same_v<T, IdleCommand>) {
                        idlePromise = item.result;
                    }
                },
                command);

            publishOutputs(drainHostOutputs(host, includeTransportStats));
            publishSnapshot(host);
            if (idlePromise) {
                idlePromise->set_value();
            }
        }
    }
};

ScriptRuntimeWorker::ScriptRuntimeWorker()
    : impl_(std::make_unique<Impl>()) {}

ScriptRuntimeWorker::~ScriptRuntimeWorker() = default;

void ScriptRuntimeWorker::configure(ScriptRuntimeWorkerConfig config) {
    impl_->pushCommand(ConfigureCommand{.config = config});
}

void ScriptRuntimeWorker::setFileIoConfig(FileIoConfig config) {
    impl_->pushCommand(SetFileIoConfigCommand{.config = std::move(config)});
}

ScriptRuntimeLoadResult ScriptRuntimeWorker::loadProtocolDirectory(const std::string& directory) {
    auto promise = std::make_shared<std::promise<ScriptRuntimeLoadResult>>();
    return impl_->runSync<ScriptRuntimeLoadResult>(ReloadProtocolCommand{.directory = directory, .result = promise}, promise);
}

bool ScriptRuntimeWorker::setControlValue(const std::string& id, const ControlValue& value) {
    auto promise = std::make_shared<std::promise<bool>>();
    return impl_->runSync<bool>(SetControlValueCommand{.id = id, .value = value, .result = promise}, promise);
}

RealtimeOutputDiscardCounts ScriptRuntimeWorker::clearPendingRealtimeOutputs() {
    auto promise = std::make_shared<std::promise<RealtimeOutputDiscardCounts>>();
    return impl_->runSync<RealtimeOutputDiscardCounts>(ClearRealtimeOutputsCommand{.result = promise}, promise);
}

void ScriptRuntimeWorker::postTransportOpen(transport::TransportOpenEvent event) {
    impl_->pushCommand(OpenCommand{.event = std::move(event)});
}

void ScriptRuntimeWorker::postTransportClose(transport::TransportCloseEvent event) {
    impl_->pushCommand(CloseCommand{.event = std::move(event)});
}

void ScriptRuntimeWorker::postTransportError(transport::TransportErrorEvent event) {
    impl_->pushCommand(ErrorCommand{.event = std::move(event)});
}

void ScriptRuntimeWorker::postTransportBytes(transport::TransportBytesEvent event) {
    impl_->pushBytes(std::move(event));
}

void ScriptRuntimeWorker::postControl(transport::ConnectionContext context, std::string id, ControlValue value) {
    impl_->pushCommand(ControlCommand{.context = std::move(context), .id = std::move(id), .value = std::move(value)});
}

void ScriptRuntimeWorker::postTick(std::uint64_t currentMs) {
    impl_->pushCommand(TickCommand{.currentMs = currentMs});
}

void ScriptRuntimeWorker::postTxEvent(transport::ConnectionContext context, TxEvent event) {
    impl_->pushCommand(TxEventCommand{.context = std::move(context), .event = std::move(event)});
}

void ScriptRuntimeWorker::postDialogEvent(transport::ConnectionContext context, DialogEvent event) {
    impl_->pushCommand(DialogEventCommand{.context = std::move(context), .event = std::move(event)});
}

void ScriptRuntimeWorker::postFileDialogEvent(transport::ConnectionContext context, FileDialogEvent event) {
    impl_->pushCommand(FileDialogEventCommand{.context = std::move(context), .event = std::move(event)});
}

bool ScriptRuntimeWorker::applyStreamRuntimeProfileEvent(StreamRuntimeProfileEvent event, std::string& error) {
    auto promise = std::make_shared<std::promise<std::pair<bool, std::string>>>();
    const auto result = impl_->runSync<std::pair<bool, std::string>>(
        ApplyStreamRuntimeProfileCommand{.event = std::move(event), .result = promise},
        promise);
    error = result.second;
    return result.first;
}

void ScriptRuntimeWorker::postRequestAwaitingCompletion(bool active) {
    impl_->pushCommand(RequestAwaitingCompletionCommand{.active = active});
}

void ScriptRuntimeWorker::waitIdle() {
    impl_->waitIdle();
}

std::vector<ScriptRuntimeOutputBatch> ScriptRuntimeWorker::drainOutputs() {
    return impl_->drainOutputs();
}

ScriptRuntimeSnapshot ScriptRuntimeWorker::snapshot() const {
    return impl_->currentSnapshot();
}

void ScriptRuntimeWorker::stop() {
    impl_->stop();
}

} // namespace protoscope::scripting
