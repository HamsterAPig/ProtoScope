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
std::vector<const ReceiveRow*> filteredLogRows(const std::vector<ReceiveRow>& rows, const LogFilterState& filter, bool includeBytePreview);

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
    bool reconnectRequired{false};
    std::vector<std::string> serialPortOptions;
};

struct ReceiveDockState {
    bool pauseScroll{false};
    bool showHex{true};
    bool showTimestamps{true};
    TransferLogDisplayMode displayMode{TransferLogDisplayMode::RawChunks};
    LogFilterState filter{};
    std::vector<ReceiveRow> rows;
    std::vector<ReceiveRow> frameRows;
    std::uint64_t rowsVersion{0};
    std::uint64_t frameRowsVersion{0};
};

struct LogDockState {
    bool pauseScroll{false};
    bool showTimestamps{true};
    LogFilterState filter{};
    std::vector<ReceiveRow> rows;
    std::uint64_t rowsVersion{0};
};

struct ScriptDockState {
    bool pauseScroll{false};
    bool showTimestamps{true};
    LogFilterState filter{};
    std::vector<ReceiveRow> rows;
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

    virtual bool trimRows(std::vector<ReceiveRow>& rows, std::size_t limit) const = 0;
};

class BoundedDockHistoryLimiter final : public IDockHistoryLimiter {
public:
    bool trimRows(std::vector<ReceiveRow>& rows, std::size_t limit) const override {
        if (rows.size() <= limit) {
            return false;
        }

        // 核心流程：只保留最新的历史记录，避免 Dock 列表无限膨胀。
        rows.erase(rows.begin(),
                   rows.begin() + static_cast<std::vector<ReceiveRow>::difference_type>(rows.size() - limit));
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
    bool luaDockLayoutDebug{false};
    bool pendingExternalReload{false};
    std::uint64_t pendingExternalReloadTimestampMs{0};
    std::string externalReloadMessage;
    ConfigConflictState conflict{};
};

class DockStore {
public:
    void clearReceiveRows();
    void appendReceiveRow(ReceiveRow row);
    void appendLuaEvent(const scripting::ScriptEvent& event);
    void appendRawReceive(const transport::ConnectionContext& ctx, const std::string& text);
    void appendRawSend(const transport::ConnectionContext& ctx, const std::string& text);

    CommDockState& commState();
    ReceiveDockState& receiveState();
    LogDockState& logState();
    ScriptDockState& scriptState();
    SendDockState& sendState();
    LuaDockState& luaState();
    plot::WaveDockState& waveState();
    ConfigDockState& configState();

    const CommDockState& commState() const;
    const ReceiveDockState& receiveState() const;
    const LogDockState& logState() const;
    const ScriptDockState& scriptState() const;
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
    void appendTransferFrameRows(std::vector<ReceiveRow> rows);
    void clearTransferFrameRows();
    void setHistoryLimits(DockHistoryLimits limits);

private:
    std::unique_ptr<IDockHistoryLimiter> historyLimiter_{std::make_unique<BoundedDockHistoryLimiter>()};
    CommDockState comm_{};
    ReceiveDockState receive_{};
    LogDockState log_{};
    ScriptDockState script_{};
    SendDockState send_{};
    LuaDockState lua_{};
    plot::WaveDockState wave_{};
    ConfigDockState config_{};
    DockHistoryLimits historyLimits_{};
};

} // namespace protoscope::dock
