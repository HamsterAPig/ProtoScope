#include "protoscope/dock/docks.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <string_view>

namespace protoscope::dock {

namespace {
std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string formatTimestampText(std::uint64_t timestampMs) {
    const auto timePoint = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
    const auto secondsPoint = std::chrono::time_point_cast<std::chrono::seconds>(timePoint);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(timePoint - secondsPoint).count();
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(timePoint);

    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &timeValue);
#else
    localtime_r(&timeValue, &localTm);
#endif

    char buffer[64]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04d-%02d-%02d %02d:%02d:%02d:%03d",
                  localTm.tm_year + 1900,
                  localTm.tm_mon + 1,
                  localTm.tm_mday,
                  localTm.tm_hour,
                  localTm.tm_min,
                  localTm.tm_sec,
                  static_cast<int>(millis));
    return buffer;
}

std::string bytesToAsciiPreview(const std::vector<std::uint8_t>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto byte : bytes) {
        const char ch = static_cast<char>(byte);
        text.push_back(std::isprint(static_cast<unsigned char>(ch)) ? ch : '.');
    }
    return text;
}

std::string flattenSingleLineText(std::string_view text) {
    std::string flattened;
    flattened.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '\r':
        case '\n':
        case '\t':
            flattened.push_back(' ');
            break;
        default:
            flattened.push_back(ch);
            break;
        }
    }
    return flattened;
}

std::string uppercaseAscii(std::string text) {
    for (auto& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}
} // namespace

ReceiveRowVisualKind classifyReceiveRow(const ReceiveRow& row) {
    const auto direction = uppercaseAscii(row.direction);
    if (direction == "RX") {
        return ReceiveRowVisualKind::Rx;
    }
    if (direction == "TX") {
        return ReceiveRowVisualKind::Tx;
    }
    if (direction == "DEBUG") {
        return ReceiveRowVisualKind::Debug;
    }
    if (direction == "INFO") {
        return ReceiveRowVisualKind::Info;
    }
    if (direction == "WARN" || direction == "WARNING") {
        return ReceiveRowVisualKind::Warn;
    }
    if (direction == "ERROR") {
        return ReceiveRowVisualKind::Error;
    }
    if (direction == "EVENT") {
        return ReceiveRowVisualKind::Event;
    }
    if (direction == "LOG") {
        return ReceiveRowVisualKind::ScriptLog;
    }
    return ReceiveRowVisualKind::Other;
}

std::string formatReceiveRowContent(const ReceiveRow& row, bool showHex) {
    if (!row.message.empty()) {
        return flattenSingleLineText(row.message);
    }
    if (row.bytes.empty()) {
        return {};
    }
    return showHex ? protocol_utils::bytesToHex(row.bytes, true) : bytesToAsciiPreview(row.bytes);
}

std::string formatReceiveRowSingleLine(const ReceiveRow& row, bool showTimestamps, bool showHex) {
    std::string line;
    if (showTimestamps) {
        line.append("[");
        line.append(formatTimestampText(row.timestampMs));
        line.append("] ");
    }
    if (!row.direction.empty()) {
        line.append(row.direction);
    }
    if (!row.endpoint.empty()) {
        if (!line.empty() && line.back() != ' ') {
            line.push_back(' ');
        }
        line.append(row.endpoint);
    }

    const auto content = formatReceiveRowContent(row, showHex);
    if (!content.empty()) {
        if (!line.empty()) {
            line.append(" | ");
        }
        line.append(content);
    }
    return line;
}

std::string formatReceiveRowsText(std::span<const ReceiveRow> rows, bool showTimestamps, bool showHex) {
    std::string text;
    for (const auto& row : rows) {
        text.append(formatReceiveRowSingleLine(row, showTimestamps, showHex));
        text.push_back('\n');
    }
    return text;
}

void trimSendHistory(SendDockState& sendState, std::size_t limit) {
    if (limit == 0U) {
        sendState.history.clear();
        return;
    }
    while (sendState.history.size() > limit) {
        sendState.history.pop_back();
    }
}

void rememberSendHistory(SendDockState& sendState, std::string payload, std::size_t limit) {
    if (limit == 0U || payload.empty()) {
        trimSendHistory(sendState, limit);
        return;
    }

    const auto duplicate = std::find(sendState.history.begin(), sendState.history.end(), payload);
    if (duplicate != sendState.history.end()) {
        sendState.history.erase(duplicate);
    }
    sendState.history.push_front(std::move(payload));
    trimSendHistory(sendState, limit);
}

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
