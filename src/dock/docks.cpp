#include "protoscope/dock/docks.hpp"

#include <chrono>

namespace protoscope::dock {

namespace {
std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}
} // namespace

void DockStore::clearReceiveRows() {
    receive_.rows.clear();
}

void DockStore::appendReceiveRow(ReceiveRow row) {
    receive_.rows.push_back(std::move(row));
}

void DockStore::appendLogRow(ReceiveRow row) {
    log_.rows.push_back(std::move(row));
}

void DockStore::appendScriptRow(ReceiveRow row) {
    script_.rows.push_back(std::move(row));
}

void DockStore::clearLogRows() {
    log_.rows.clear();
}

void DockStore::clearScriptRows() {
    script_.rows.clear();
}

void DockStore::appendLuaEvent(const scripting::ScriptEvent& event) {
    appendScriptRow(
        ReceiveRow{
            .timestampMs = event.timestampMs,
            .direction = "EVENT",
            .endpoint = "script",
            .bytes = {},
            .message = event.name + ": " + event.payload,
        });
}

void DockStore::appendRawReceive(const transport::ConnectionContext& ctx, const std::string& text) {
    receive_.rows.push_back(ReceiveRow{
        .timestampMs = nowMs(),
        .direction = "RX",
        .endpoint = ctx.endpoint,
        .bytes = std::vector<std::uint8_t>(text.begin(), text.end()),
        .message = {},
    });
}

void DockStore::appendRawSend(const transport::ConnectionContext& ctx, const std::string& text) {
    receive_.rows.push_back(ReceiveRow{
        .timestampMs = nowMs(),
        .direction = "TX",
        .endpoint = ctx.endpoint,
        .bytes = std::vector<std::uint8_t>(text.begin(), text.end()),
        .message = {},
    });
}

CommDockState& DockStore::commState() {
    return comm_;
}

ReceiveDockState& DockStore::receiveState() {
    return receive_;
}

LogDockState& DockStore::logState() {
    return log_;
}

ScriptDockState& DockStore::scriptState() {
    return script_;
}

SendDockState& DockStore::sendState() {
    return send_;
}

LuaDockState& DockStore::luaState() {
    return lua_;
}

plot::WaveDockState& DockStore::waveState() {
    return wave_;
}

ConfigDockState& DockStore::configState() {
    return config_;
}

const CommDockState& DockStore::commState() const {
    return comm_;
}

const ReceiveDockState& DockStore::receiveState() const {
    return receive_;
}

const LogDockState& DockStore::logState() const {
    return log_;
}

const ScriptDockState& DockStore::scriptState() const {
    return script_;
}

const SendDockState& DockStore::sendState() const {
    return send_;
}

const LuaDockState& DockStore::luaState() const {
    return lua_;
}

const plot::WaveDockState& DockStore::waveState() const {
    return wave_;
}

const ConfigDockState& DockStore::configState() const {
    return config_;
}

void DockStore::markDirty(const std::string& statusMessage) {
    config_.dirty = true;
    config_.statusMessage = statusMessage;
}

void DockStore::clearDirty(const std::string& statusMessage) {
    config_.dirty = false;
    config_.statusMessage = statusMessage;
}

void DockStore::setPendingExternalReload(std::uint64_t timestampMs, std::string message) {
    config_.pendingExternalReload = true;
    config_.pendingExternalReloadTimestampMs = timestampMs;
    config_.externalReloadMessage = std::move(message);
}

void DockStore::clearPendingExternalReload() {
    config_.pendingExternalReload = false;
    config_.pendingExternalReloadTimestampMs = 0;
    config_.externalReloadMessage.clear();
}

void DockStore::setConflict(std::string message) {
    config_.conflict.detected = true;
    config_.conflict.message = std::move(message);
}

void DockStore::clearConflict() {
    config_.conflict.detected = false;
    config_.conflict.message.clear();
}

} // namespace protoscope::dock
