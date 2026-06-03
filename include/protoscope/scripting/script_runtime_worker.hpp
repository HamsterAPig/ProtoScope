#pragma once

#include "protoscope/scripting/script_host.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::scripting {

struct ScriptRuntimeWorkerConfig {
    bool enabled{true};
    std::size_t postprocessWorkerThreads{1U};
    std::size_t rxQueueLimitBytes{64U * 1024U * 1024U};
    std::size_t outputQueueLimit{65536U};
    std::size_t batchBytes{256U * 1024U};
    bool backpressureEnabled{true};
    double backpressureHighWatermark{0.5};
    double backpressureLowWatermark{0.3};
};

struct ScriptRuntimeSnapshot {
    std::vector<ControlDescriptor> controls;
    std::vector<ControlSnapshot> controlStates;
    std::vector<DockSnapshot> docks;
    std::optional<StreamBufferDefinition> streamBuffer;
    std::vector<StreamFrameDefinition> streamFrames;
    std::optional<std::uint64_t> nextWakeupAtMs;
    std::string scriptPath;
    std::string protocolDirectory;
    std::string lastError;
    std::size_t pendingPlotAppends{0};
    std::size_t pendingWorkerRxBytes{0};
    std::size_t inputQueueSize{0};
    std::size_t outputQueueSize{0};
    std::size_t postprocessWorkerThreads{1U};
    ScriptHostTransportStats lastTransportStats{};
};

struct ScriptRuntimeOutputBatch {
    std::vector<ScriptEvent> events;
    std::vector<ScriptLog> logs;
    std::vector<TxRequest> txRequests;
    std::vector<PlotSetup> plotSetups;
    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> plotAppends;
    std::vector<RequestDoneResult> requestDoneResults;
    std::vector<StatusUpdate> statusUpdates;
    std::vector<StreamRuntimeProfileEvent> streamRuntimeProfiles;
    std::vector<DialogRequest> dialogRequests;
    std::vector<FileDialogRequest> fileDialogRequests;
    std::optional<ScriptHostTransportStats> transportStats;
};

struct ScriptRuntimeLoadResult {
    bool ok{false};
    std::string lastError;
    ScriptRuntimeSnapshot snapshot;
};

class ScriptRuntimeWorker {
public:
    ScriptRuntimeWorker();
    ~ScriptRuntimeWorker();

    ScriptRuntimeWorker(const ScriptRuntimeWorker&) = delete;
    ScriptRuntimeWorker& operator=(const ScriptRuntimeWorker&) = delete;

    void configure(ScriptRuntimeWorkerConfig config);
    void setFileIoConfig(FileIoConfig config);
    [[nodiscard]] ScriptRuntimeLoadResult loadProtocolDirectory(const std::string& directory);
    [[nodiscard]] bool setControlValue(const std::string& id, const ControlValue& value);
    [[nodiscard]] RealtimeOutputDiscardCounts clearPendingRealtimeOutputs();

    void postTransportOpen(transport::TransportOpenEvent event);
    void postTransportClose(transport::TransportCloseEvent event);
    void postTransportError(transport::TransportErrorEvent event);
    void postTransportBytes(transport::TransportBytesEvent event);
    void postControl(transport::ConnectionContext context, std::string id, ControlValue value);
    void postTick(std::uint64_t currentMs);
    void postTxEvent(transport::ConnectionContext context, TxEvent event);
    void postDialogEvent(transport::ConnectionContext context, DialogEvent event);
    void postFileDialogEvent(transport::ConnectionContext context, FileDialogEvent event);
    [[nodiscard]] bool applyStreamRuntimeProfileEvent(StreamRuntimeProfileEvent event, std::string& error);
    void postRequestAwaitingCompletion(bool active);

    void waitIdle();
    [[nodiscard]] std::vector<ScriptRuntimeOutputBatch> drainOutputs();
    [[nodiscard]] std::optional<ScriptRuntimeOutputBatch> drainOneOutput();
    [[nodiscard]] ScriptRuntimeSnapshot snapshot() const;
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace protoscope::scripting
