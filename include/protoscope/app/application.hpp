#pragma once

#include "protoscope/config/config.hpp"
#include "protoscope/dock/docks.hpp"
#include "protoscope/logging/logging.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/plugin/elf_static_view_bridge.hpp"
#include "protoscope/scripting/script_runtime_worker.hpp"
#include "protoscope/transport/transport.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace protoscope::app {

class Application {
public:
    Application();

    bool initialize();
    bool applyConfig(const config::AppConfig& config);
    config::AppConfig captureConfig() const;
    [[nodiscard]] const config::AppConfig& runtimeConfig() const;
    bool reloadProtocolDirectory(const std::string& protocolDir, bool forceReload = false);
    bool pumpOnce();
    void shutdown();

    dock::DockStore& docks();
    const dock::DockStore& docks() const;

    void openTransport();
    void closeTransport();
    bool sendManualPayload(const std::string& payload, bool hexMode);
    void updateControlValue(const std::string& id, const scripting::ControlValue& value);
    bool restoreControlValue(const std::string& id, const scripting::ControlValue& value);
    void markCommConfigEdited(bool reconnectRequired);
    void markProtocolEdited();
    void setStatusMessage(std::string message, bool markDirty = false);
    void setLogLevel(config::LogLevel level);
    bool setSendHexMode(bool enabled);
    bool exportWaveRawCapture(const std::filesystem::path& path, std::string& error) const;
    bool importWaveRawCapture(const plot::RawCaptureFileData& capture, std::string& error);
    bool startRawCaptureRecording(const std::filesystem::path& path, std::string& error);
    bool stopRawCaptureRecording(std::string& error);
    [[nodiscard]] bool isRawCaptureRecording() const;
    [[nodiscard]] const std::filesystem::path& rawCaptureRecordingPath() const;
    [[nodiscard]] std::uint64_t rawCaptureRecordingBytes() const;
    void resetWaveHistory();
    bool loadElfStaticAddressFile(const std::filesystem::path& path, std::string& error);
    void clearElfStaticAddressFile();
    [[nodiscard]] std::uint64_t elfStaticAddressRevision() const;
    [[nodiscard]] std::vector<scripting::ElfSymbolValue> queryElfStaticAddresses(const std::string& queryText,
                                                                                 std::size_t limit) const;
    void refreshSelectedElfSymbolControls();
    void rebuildTransferFrameRows();
    void activateParsedTransferLogView();
    logging::LoggingFacade& logger();
    const logging::LoggingFacade& logger() const;
    std::vector<scripting::DialogRequest> drainDialogRequests();
    void respondDialog(const scripting::DialogEvent& event);
    std::vector<scripting::FileDialogRequest> drainFileDialogRequests();
    void respondFileDialog(const scripting::FileDialogEvent& event);

    std::optional<std::uint64_t> nextWakeupAtMs() const;
    void setTransportFactoryForTest(
        std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> factory);

private:
    struct ActiveTxRequest {
        scripting::TxRequest request;
        std::uint64_t sentAtMs{0};
        std::uint64_t waitDeadlineMs{0};
    };

    struct TransferFrameParserState {
        scripting::FrameStreamParser rx;
        scripting::FrameStreamParser tx;
    };

    struct PendingRxBytes {
        transport::ConnectionContext context{};
        std::vector<std::uint8_t> bytes{};
        std::size_t offset{0};
    };

    struct StreamBufferAlertState {
        std::uint64_t connectionId{0};
        bool popupMuted{false};
        bool popupOpen{false};
    };

    struct RealtimeBacklogDiscardCounts {
        std::size_t transportEvents{0};
        std::size_t rxBytes{0};
        std::size_t transferFrameRows{0};
        std::size_t plotAppends{0};
        std::size_t scriptLogs{0};
        std::size_t scriptEvents{0};
    };

    std::unique_ptr<transport::ITransport> createTransport(transport::TransportKind kind) const;
    transport::TransportConfig currentTransportConfig(transport::TransportKind kind) const;
    void syncDockState();
    bool applyScriptOutputBatch(const scripting::ScriptRuntimeOutputBatch& batch);
    bool handleTransportEvents();
    bool processTransportEvent(const transport::TransportEvent& event);
    bool processPendingRxBytes(std::size_t maxBytes);
    void enqueuePendingRxBytes(transport::TransportBytesEvent event);
    void detachPendingRealtimeBacklogFromConnection();
    RealtimeBacklogDiscardCounts clearPendingRealtimeBacklog();
    void logRealtimeBacklogDiscard(const RealtimeBacklogDiscardCounts& counts);
    [[nodiscard]] bool responsiveBacklogMode() const;
    [[nodiscard]] std::size_t rxBytesPerPump() const;
    [[nodiscard]] std::size_t transferFrameRowsPerPump() const;
    [[nodiscard]] std::size_t plotAppendsPerPump() const;
    [[nodiscard]] std::size_t pendingRxByteCount() const;
    [[nodiscard]] bool hasPendingRequestDrainWork() const;
    bool drainRequestTimeoutBacklog();
    bool flushScriptOutputs();
    bool flushScriptOutputsUnbounded();
    bool flushScriptLogs();
    bool flushScriptPlots();
    bool flushPendingTransferFrameRows(std::size_t maxRows);
    bool flushScriptStatusAndDialogs();
    bool processScriptRequestCompletions();
    bool processRequestTimeouts();
    bool driveTxScheduler();
    bool enqueueTxRequest(scripting::TxRequest request);
    void finishTxRequest(const scripting::TxRequest& request,
                         scripting::TxEventState state,
                         std::optional<std::string> error,
                         std::uint64_t finishedAtMs);
    void cancelAllTxRequests(const std::string& reason);
    void notifyTxOverflow(const std::string& message);
    void handleStreamBufferAlert(const transport::ConnectionContext& context,
                                 const scripting::StreamParseBatch& batch,
                                 const scripting::StreamBufferDefinition& bufferDefinition);
    void resetStreamBufferAlertState(std::uint64_t connectionId = 0);
    void enqueueDialogRequest(const scripting::DialogRequest& request);
    void appendTransferRow(dock::ReceiveRow row);
    void appendLiveRawCapture(const transport::TransportBytesEvent& event);
    void appendRawCaptureRecording(const transport::TransportBytesEvent& event);
    void appendRawCaptureEvent(const plot::RawCaptureEvent& event);
    bool applyPlotSetup(const plot::RawCapturePlotSetupEventData& setup);
    void recordPlotSetupSnapshot(const plot::RawCapturePlotSetupEventData& setup, std::uint64_t timestampMs);
    void resetTransferFrameParser();
    void resetTransferFrameDisplayState();
    void appendTransferFrameRows(const dock::ReceiveRow& sourceRow);
    void enqueueTransferFrameRows(std::vector<dock::ReceiveRow> rows);
    void trimPendingTransferFrameRowsToLimit();
    void applyHistoryLimits(const config::GuiLogHistoryConfig& config);
    [[nodiscard]] dock::ReceiveRow makeTransferFrameRow(const dock::ReceiveRow& sourceRow,
                                                        const scripting::StreamParsedFrame& frame) const;
    [[nodiscard]] std::optional<TransferFrameParserState> makeTransferFrameParserState() const;

    dock::DockStore dockStore_;
    config::ConfigStore configStore_{};
    config::AppConfig runtimeConfig_{};
    logging::LoggingFacade loggingFacade_{};
    scripting::ScriptRuntimeWorker scriptWorker_;
    plugin::ElfStaticViewBridge elfStaticView_;
    std::uint64_t elfStaticAddressRevision_{0};
    std::unique_ptr<transport::ITransport> transport_;
    std::optional<transport::ConnectionContext> activeConnection_;
    std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> transportFactoryForTest_;
    std::deque<scripting::TxRequest> pendingTxQueue_;
    std::optional<ActiveTxRequest> activeWrite_;
    std::optional<ActiveTxRequest> activeHalfDuplexRequest_;
    std::deque<scripting::DialogRequest> pendingDialogs_;
    std::unordered_map<std::uint64_t, scripting::DialogRequest> openDialogs_;
    std::deque<scripting::FileDialogRequest> pendingFileDialogs_;
    std::unordered_map<std::uint64_t, scripting::FileDialogRequest> openFileDialogs_;
    std::deque<std::pair<std::size_t, plot::WaveAppendRequest>> pendingScriptPlotAppends_;
    std::unordered_map<std::string, std::uint64_t> dialogDedupeKeys_;
    StreamBufferAlertState streamBufferAlertState_{};
    std::optional<TransferFrameParserState> transferFrameParser_;
    plot::RawCaptureStreamWriter rawCaptureRecording_;
    std::deque<transport::TransportEvent> pendingTransportEvents_;
    std::deque<PendingRxBytes> pendingRxByteChunks_;
    std::deque<dock::ReceiveRow> pendingTransferFrameRows_;
    std::optional<std::uint64_t> cachedWaveSummaryRevision_;
    bool suppressRawCaptureProfileEvents_{false};
    bool suppressRawCapturePlotSetupEvents_{false};
};

} // namespace protoscope::app
