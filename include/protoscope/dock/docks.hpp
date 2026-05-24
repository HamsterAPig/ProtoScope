#pragma once

#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace protoscope::dock {

struct ReceiveRow {
    std::uint64_t timestampMs{0};
    std::string direction;
    std::string endpoint;
    std::string text;
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
};

struct ReceiveDockState {
    bool pauseScroll{false};
    bool showAscii{true};
    bool showHex{true};
    std::vector<ReceiveRow> rows;
};

struct SendDockState {
    bool hexMode{true};
    std::string payload{"AA 01 00"};
    std::string actionName{"read_version"};
};

struct LuaDockState {
    bool loaded{false};
    std::string scriptPath{"scripts/default_protocol.lua"};
    std::string lastError;
    std::vector<scripting::ControlDescriptor> controls;
};

struct WaveDockState {
    std::string placeholder{"TODO: 第一版仅保留波形 Dock 占位与后续数据接口"};
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
    SendDockState& sendState();
    LuaDockState& luaState();
    WaveDockState& waveState();

    const CommDockState& commState() const;
    const ReceiveDockState& receiveState() const;
    const SendDockState& sendState() const;
    const LuaDockState& luaState() const;
    const WaveDockState& waveState() const;

private:
    CommDockState comm_{};
    ReceiveDockState receive_{};
    SendDockState send_{};
    LuaDockState lua_{};
    WaveDockState wave_{};
};

} // namespace protoscope::dock
