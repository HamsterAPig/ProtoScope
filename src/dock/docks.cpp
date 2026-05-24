#include "protoscope/dock/docks.hpp"

#include <chrono>

namespace protoscope::dock {

namespace {
std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}
} // namespace

void DockStore::clearReceiveRows() {
    receive_.rows.clear();
}

void DockStore::appendReceiveRow(ReceiveRow row) {
    receive_.rows.push_back(std::move(row));
}

void DockStore::appendLuaEvent(const scripting::ScriptEvent& event) {
    receive_.rows.push_back(
        ReceiveRow{.timestampMs = event.timestampMs, .direction = "Lua", .endpoint = "script", .text = event.name + ": " + event.payload});
}

void DockStore::appendRawReceive(const transport::ConnectionContext& ctx, const std::string& text) {
    receive_.rows.push_back(ReceiveRow{
        .timestampMs = nowMs(),
        .direction = "RX",
        .endpoint = ctx.endpoint,
        .text = text,
    });
}

void DockStore::appendRawSend(const transport::ConnectionContext& ctx, const std::string& text) {
    receive_.rows.push_back(ReceiveRow{
        .timestampMs = nowMs(),
        .direction = "TX",
        .endpoint = ctx.endpoint,
        .text = text,
    });
}

CommDockState& DockStore::commState() {
    return comm_;
}

ReceiveDockState& DockStore::receiveState() {
    return receive_;
}

SendDockState& DockStore::sendState() {
    return send_;
}

LuaDockState& DockStore::luaState() {
    return lua_;
}

WaveDockState& DockStore::waveState() {
    return wave_;
}

const CommDockState& DockStore::commState() const {
    return comm_;
}

const ReceiveDockState& DockStore::receiveState() const {
    return receive_;
}

const SendDockState& DockStore::sendState() const {
    return send_;
}

const LuaDockState& DockStore::luaState() const {
    return lua_;
}

const WaveDockState& DockStore::waveState() const {
    return wave_;
}

} // namespace protoscope::dock
