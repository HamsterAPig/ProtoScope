#pragma once

#include "protoscope/config/config.hpp"
#include "protoscope/dock/docks.hpp"
#include "protoscope/logging/logging.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/plugin/elf_static_view_bridge.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace protoscope::app {

class Application {
public:
    Application();

    bool initialize();
    bool applyConfig(const config::AppConfig& config);
    config::AppConfig captureConfig() const;
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
    void resetWaveHistory();
    bool loadElfStaticAddressFile(const std::filesystem::path& path, std::string& error);
    [[nodiscard]] std::vector<scripting::ElfSymbolValue> queryElfStaticAddresses(const std::string& queryText,
                                                                                 std::size_t limit) const;
    void rebuildTransferFrameRows();
    logging::LoggingFacade& logger();
    const logging::LoggingFacade& logger() const;
    std::vector<scripting::DialogRequest> drainDialogRequests();
    void respondDialog(const scripting::DialogEvent& event);
    std::vector<scripting::FileDialogRequest> drainFileDialogRequests();
    void respondFileDialog(const scripting::FileDialogEvent& event);

    std::optional<std::uint64_t> nextWakeupAtMs() const;
    void setTransportFactoryForTest(std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> factory);

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

    std::unique_ptr<transport::ITransport> createTransport(transport::TransportKind kind) const;
    transport::TransportConfig currentTransportConfig(transport::TransportKind kind) const;
    void syncDockState();
    bool handleTransportEvents();
    bool flushScriptOutputs();
    bool flushScriptLogs();
    bool flushScriptPlots();
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
    void enqueueDialogRequest(const scripting::DialogRequest& request);
    void appendTransferRow(dock::ReceiveRow row);
    void resetTransferFrameParser();
    void appendTransferFrameRows(const dock::ReceiveRow& sourceRow);
    [[nodiscard]] dock::ReceiveRow makeTransferFrameRow(const dock::ReceiveRow& sourceRow,
                                                        const scripting::StreamParsedFrame& frame) const;
    [[nodiscard]] std::optional<TransferFrameParserState> makeTransferFrameParserState() const;

    dock::DockStore dockStore_;
    config::ConfigStore configStore_{};
    config::AppConfig runtimeConfig_{};
    logging::LoggingFacade loggingFacade_{};
    scripting::ScriptHost scriptHost_;
    plugin::ElfStaticViewBridge elfStaticView_;
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
    std::unordered_map<std::string, std::uint64_t> dialogDedupeKeys_;
    std::optional<TransferFrameParserState> transferFrameParser_;
};

} // namespace protoscope::app
