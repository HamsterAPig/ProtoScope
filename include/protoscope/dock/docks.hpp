#pragma once

#include "protoscope/plot/wave_state.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::dock {

struct ReceiveRow {
    std::uint64_t timestampMs{0};
    std::string direction;
    std::string endpoint;
    std::vector<std::uint8_t> bytes;
    std::string message;
};

struct CommDockState {
    transport::TransportKind kind{transport::TransportKind::TcpClient};
    transport::TcpClientConfig tcpClient{};
    transport::TcpServerConfig tcpServer{};
    transport::SerialConfig serial{};
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
    std::vector<ReceiveRow> rows;
};

struct LogDockState {
    bool pauseScroll{false};
    bool showTimestamps{true};
    std::vector<ReceiveRow> rows;
};

struct ScriptDockState {
    bool pauseScroll{false};
    bool showTimestamps{true};
    std::vector<ReceiveRow> rows;
};

struct SendDockState {
    bool hexMode{true};
    std::string payload{"AA 01 00"};
};

struct LuaDockState {
    bool loaded{false};
    std::string scriptPath{"protocols/default_protocol/main.lua"};
    std::string protocolDir{"protocols/default_protocol"};
    std::string protocolName{"default_protocol"};
    std::string protocolRootDir{"protocols"};
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

private:
    CommDockState comm_{};
    ReceiveDockState receive_{};
    LogDockState log_{};
    ScriptDockState script_{};
    SendDockState send_{};
    LuaDockState lua_{};
    plot::WaveDockState wave_{};
    ConfigDockState config_{};
};

} // namespace protoscope::dock
