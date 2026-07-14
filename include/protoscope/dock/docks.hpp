#pragma once

#include "protoscope/plot/wave_state.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace protoscope::dock {

struct ReceiveRow {
    std::uint64_t timestampMs{0};
    std::string direction{};
    std::string endpoint{};
    std::vector<std::uint8_t> bytes{};
    std::string message{};
};

struct DockHistoryLimits {
    std::size_t transferRawRows{10000};
    std::size_t transferFrameRows{120000};
    std::size_t hostLogRows{5000};
    std::size_t scriptLogRows{5000};
    std::size_t requestTraceRows{5000};
};

enum class TransferLogDisplayMode {
    RawChunks,
    ParsedFrames,
};

enum class ReceiveRowVisualKind {
    Rx,
    Tx,
    Debug,
    Info,
    Warn,
    Error,
    Event,
    ScriptLog,
    Other,
};

ReceiveRowVisualKind classifyReceiveRow(const ReceiveRow& row);
std::string formatReceiveRowContent(const ReceiveRow& row, bool showHex);
std::string formatReceiveRowSingleLine(const ReceiveRow& row, bool showTimestamps, bool showHex);
std::string formatReceiveRowsText(std::span<const ReceiveRow> rows, bool showTimestamps, bool showHex);

enum class LogStatusFilter {
    All,
    Rx,
    Tx,
    Debug,
    Info,
    Warn,
    Error,
    Event,
    ScriptLog,
    Other,
};

struct LogFilterState {
    std::string keyword;
    LogStatusFilter status{LogStatusFilter::All};
};

bool matchesLogFilter(const ReceiveRow& row, const LogFilterState& filter, bool includeBytePreview);
std::vector<const ReceiveRow*> filteredLogRows(const std::deque<ReceiveRow>& rows,
                                               const LogFilterState& filter,
                                               bool includeBytePreview);
std::vector<const ReceiveRow*> filteredLogRows(const std::vector<ReceiveRow>& rows,
                                               const LogFilterState& filter,
                                               bool includeBytePreview);

enum class RequestTraceKind {
    Send,
    Request,
};

enum class RequestTraceState {
    Queued,
    Sent,
    Completed,
    Failed,
    Timeout,
    Rejected,
    Dropped,
    Canceled,
    GuardReset,
};

enum class RequestTraceStatusFilter {
    All,
    Active,
    Success,
    Failure,
};

struct RequestTraceFilterState {
    std::string keyword;
    RequestTraceStatusFilter status{RequestTraceStatusFilter::All};
};

struct RequestTraceRow {
    std::uint64_t timestampMs{0};
    std::uint64_t id{0};
    RequestTraceKind kind{RequestTraceKind::Send};
    RequestTraceState state{RequestTraceState::Queued};
    std::string endpoint;
    std::string tag;
    std::size_t bytes{0};
    std::uint64_t queuedMs{0};
    std::uint64_t finishedMs{0};
    std::uint64_t timeoutMs{0};
    std::uint64_t durationMs{0};
    bool guarded{false};
    std::uint32_t attempt{1};
    std::uint32_t maxAttempts{1};
    std::string guardState;
    std::string error;
};

const char* requestTraceKindLabel(RequestTraceKind kind);
const char* requestTraceStateLabel(RequestTraceState state);
bool isRequestTraceFailure(RequestTraceState state);
bool matchesRequestTraceFilter(const RequestTraceRow& row, const RequestTraceFilterState& filter);
std::vector<const RequestTraceRow*> filteredRequestTraceRows(const std::deque<RequestTraceRow>& rows,
                                                             const RequestTraceFilterState& filter);
std::string formatRequestTraceDuration(const RequestTraceRow& row);
std::string formatRequestTraceDetail(const RequestTraceRow& row);
std::string formatRequestTraceRowCsv(const RequestTraceRow& row, bool showTimestamps);
std::string formatRequestTraceRowsCsv(std::span<const RequestTraceRow> rows, bool showTimestamps);

struct CommDockState {
    transport::TransportKind kind{transport::TransportKind::TcpClient};
    transport::TcpClientConfig tcpClient{};
    transport::TcpServerConfig tcpServer{};
    transport::SerialConfig serial{};
    transport::UdpPeerConfig udpPeer{};
    transport::TransportState state{transport::TransportState::Closed};
    std::string lastError;
    std::uint64_t txCount{0};
    std::uint64_t rxCount{0};
    std::size_t pendingRxBytes{0};
    std::size_t pendingTransferFrameRows{0};
    std::size_t pendingPlotAppends{0};
    std::size_t rxInputQueueBytes{0};
    std::size_t parserPendingBytes{0};
    std::size_t postprocessPendingBatches{0};
    std::size_t luaPendingItems{0};
    std::size_t uiPendingItems{0};
    std::size_t postprocessWorkerThreads{1};
    std::string backlogWarning;
    std::size_t lastPumpEvents{0};
    std::size_t lastPumpRxBytes{0};
    std::size_t lastPumpStreamFrames{0};
    std::size_t lastPumpStreamErrors{0};
    double lastPumpTransportMs{0.0};
    double lastPumpParserMs{0.0};
    double lastPumpCallbackMs{0.0};
    double lastPumpScriptMs{0.0};
    bool adaptivePerformanceEnabled{false};
    double adaptivePerformanceMaxMultiplier{1.0};
    double adaptivePerformanceEffectiveMultiplier{1.0};
    std::string adaptivePerformanceLevel{"normal"};
    std::string adaptivePerformanceReason{"disabled"};
    bool adaptivePerformanceSystemMetricsAvailable{false};
    bool reconnectRequired{false};
    std::vector<std::string> serialPortOptions;
    std::string lastErrorSummary;
};

struct ReceiveDockState {
    bool pauseScroll{false};
    bool showHex{true};
    bool showTimestamps{true};
    TransferLogDisplayMode displayMode{TransferLogDisplayMode::RawChunks};
    LogFilterState filter{};
    std::deque<ReceiveRow> rows;
    std::deque<ReceiveRow> frameRows;
    std::uint64_t rowsVersion{0};
    std::uint64_t frameRowsVersion{0};
};

struct LogDockState {
    bool pauseScroll{false};
    bool showTimestamps{true};
    LogFilterState filter{};
    std::deque<ReceiveRow> rows;
    std::uint64_t rowsVersion{0};
};

struct ScriptDockState {
    bool pauseScroll{false};
    bool showTimestamps{true};
    LogFilterState filter{};
    std::deque<ReceiveRow> rows;
    std::uint64_t rowsVersion{0};
};

struct RequestTraceDockState {
    bool pauseScroll{false};
    bool showTimestamps{true};
    RequestTraceFilterState filter{};
    std::deque<RequestTraceRow> rows;
    std::uint64_t rowsVersion{0};
};

struct SendDockState {
    bool hexMode{true};
    std::string payload{"AA 01 00"};
    std::deque<std::string> history;
    float historyComboWidth{220.0F};
};

void trimSendHistory(SendDockState& sendState, std::size_t limit);
void rememberSendHistory(SendDockState& sendState, std::string payload, std::size_t limit);

class IDockHistoryLimiter {
public:
    virtual ~IDockHistoryLimiter() = default;

    virtual bool trimRows(std::deque<ReceiveRow>& rows, std::size_t limit) const = 0;
};

class BoundedDockHistoryLimiter final : public IDockHistoryLimiter {
public:
    bool trimRows(std::deque<ReceiveRow>& rows, std::size_t limit) const override
    {
        if (rows.size() <= limit) {
            return false;
        }

        // 核心流程：只保留最新的历史记录，并从头部逐条丢弃，避免 vector 头删搬移全部历史。
        while (rows.size() > limit) {
            rows.pop_front();
        }
        return true;
    }
};

struct LuaDockState {
    bool loaded{false};
    std::string scriptPath{"protocols/templates/default_protocol/main.lua"};
    std::string protocolDir{"protocols/templates/default_protocol"};
    std::string protocolName{"default_protocol"};
    std::string protocolRootDir{"protocols/templates"};
    std::string lastError;
    std::vector<std::string> protocolDirOptions;
    std::vector<scripting::DockSnapshot> docks;
    std::vector<scripting::ControlDescriptor> controls;
    std::vector<scripting::ControlSnapshot> controlStates;
};

struct ConfigConflictState {
    bool detected{false};
    std::string message;
};

struct ConfigDockState {
    bool dirty{false};
    bool autoSaveEnabled{false};
    std::uint64_t autoSaveIntervalMs{5000};
    bool configHotReloadEnabled{false};
    std::uint32_t fpsLimit{60};
    std::string idleRender{"dirty_only"};
    std::uint64_t fileTimestampMs{0};
    std::string loadedFromPath{"config/protoscope.yaml"};
    std::string statusMessage;
    std::string transientStatusMessage;
    std::uint64_t transientStatusExpiresAtMs{0};
    bool luaDockLayoutDebug{false};
    bool luaDockRenderCopyMode{true};
    bool pendingExternalReload{false};
    std::uint64_t pendingExternalReloadTimestampMs{0};
    std::string externalReloadMessage;
    ConfigConflictState conflict{};
};

class DockStore {
public:
    DockStore() = default;

    void clearReceiveRows();
    void appendReceiveRow(ReceiveRow row);
    void appendLuaEvent(const scripting::ScriptEvent& event);
    void appendRequestTraceRow(RequestTraceRow row);
    void appendRawReceive(const transport::ConnectionContext& ctx, const std::string& text);
    void appendRawSend(const transport::ConnectionContext& ctx, const std::string& text);

    CommDockState& commState();
    ReceiveDockState& receiveState();
    LogDockState& logState();
    ScriptDockState& scriptState();
    RequestTraceDockState& requestTraceState();
    SendDockState& sendState();
    LuaDockState& luaState();
    plot::WaveDockState& waveState();
    ConfigDockState& configState();

    const CommDockState& commState() const;
    const ReceiveDockState& receiveState() const;
    const LogDockState& logState() const;
    const ScriptDockState& scriptState() const;
    const RequestTraceDockState& requestTraceState() const;
    const SendDockState& sendState() const;
    const LuaDockState& luaState() const;
    const plot::WaveDockState& waveState() const;
    const ConfigDockState& configState() const;

    void markDirty(const std::string& statusMessage);
    void clearDirty(const std::string& statusMessage);
    void setPendingExternalReload(std::uint64_t timestampMs, std::string message);
    void clearPendingExternalReload();
    void setConflict(std::string message);
    void clearConflict();
    void appendLogRow(ReceiveRow row);
    void appendScriptRow(ReceiveRow row);
    void clearLogRows();
    void clearScriptRows();
    void clearRequestTraceRows();
    void appendTransferFrameRows(std::vector<ReceiveRow> rows);
    void clearTransferFrameRows();
    void setHistoryLimits(DockHistoryLimits limits);

private:
    CommDockState comm_{};
    ReceiveDockState receive_{};
    LogDockState log_{};
    ScriptDockState script_{};
    RequestTraceDockState requestTrace_{};
    SendDockState send_{};
    LuaDockState lua_{};
    plot::WaveDockState wave_{};
    ConfigDockState config_{};
    DockHistoryLimits historyLimits_{};
    std::unique_ptr<IDockHistoryLimiter> historyLimiter_{std::make_unique<BoundedDockHistoryLimiter>()};
};

} // namespace protoscope::dock
