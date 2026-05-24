#include "protoscope/scripting/script_host.hpp"

#include <chrono>
#include <sstream>

namespace protoscope::scripting {

namespace {
std::string kindName(transport::TransportKind kind) {
    switch (kind) {
    case transport::TransportKind::TcpClient:
        return "tcp_client";
    case transport::TransportKind::TcpServer:
        return "tcp_server";
    case transport::TransportKind::Serial:
        return "serial";
    }
    return "unknown";
}
} // namespace

ScriptHost::ScriptHost() {
    controls_ = {
        ControlDescriptor{.type = ControlType::Button, .id = "read_version", .label = "读取版本"},
        ControlDescriptor{.type = ControlType::InputText, .id = "device_id", .label = "设备 ID", .textDefault = "01"},
        ControlDescriptor{.type = ControlType::Checkbox, .id = "hex_send", .label = "HEX 发送", .boolDefault = true},
        ControlDescriptor{.type = ControlType::Combo,
                          .id = "mode",
                          .label = "模式",
                          .comboOptions = {"轮询", "单次"},
                          .comboDefaultIndex = 0},
    };
}

bool ScriptHost::loadScriptFile(const std::string& path) {
    scriptPath_ = path;
    scriptLoaded_ = !path.empty();

    if (!scriptLoaded_) {
        protoLog("error", "脚本路径为空，保持内建脚本模式");
        return false;
    }

    // 核心流程：第一版先记录脚本路径并启用内建回调逻辑，后续再接入真实 Lua VM。
    protoLog("info", "已加载脚本(内建模拟): " + path);
    return true;
}

void ScriptHost::resetRuntime() {
    waitingResponse_ = false;
    timers_.clear();
    sendQueue_.clear();
    events_.clear();
    logs_.clear();
    activeConnection_.reset();
}

void ScriptHost::onTransportOpen(const transport::TransportOpenEvent& event) {
    activeConnection_ = event.context;
    callbackOnOpen(ScriptHostContext{event.context});
}

void ScriptHost::onTransportClose(const transport::TransportCloseEvent& event) {
    callbackOnClose(ScriptHostContext{event.context});
    activeConnection_.reset();
    waitingResponse_ = false;
}

void ScriptHost::onTransportError(const transport::TransportErrorEvent& event) {
    callbackOnError(ScriptHostContext{event.context}, event.message);
}

void ScriptHost::onTransportBytes(const transport::TransportBytesEvent& event) {
    callbackOnBytes(ScriptHostContext{event.context}, event.bytes);
}

void ScriptHost::onControl(const transport::ConnectionContext& ctx, const std::string& id, const ControlValue& value) {
    callbackOnControl(ScriptHostContext{ctx}, id, value);
}

void ScriptHost::invokeAction(const transport::ConnectionContext& ctx, const std::string& actionName) {
    if (actionName == "read_version") {
        callbackOnControl(ScriptHostContext{ctx}, "read_version", true);
        return;
    }

    protoLog("warn", "未知 action: " + actionName);
}

void ScriptHost::tick(std::uint64_t currentMs) {
    if (!activeConnection_.has_value()) {
        return;
    }

    std::vector<std::string> dueNames;
    for (const auto& [name, timer] : timers_) {
        if (timer.active && currentMs >= timer.dueAtMs) {
            dueNames.push_back(name);
        }
    }

    for (const auto& name : dueNames) {
        callbackOnTimer(ScriptHostContext{*activeConnection_}, name);
        auto iter = timers_.find(name);
        if (iter != timers_.end()) {
            iter->second.active = false;
        }
    }
}

std::vector<ControlDescriptor> ScriptHost::controlsSnapshot() const {
    return controls_;
}

std::vector<ScriptEvent> ScriptHost::drainEvents() {
    std::vector<ScriptEvent> out;
    out.swap(events_);
    return out;
}

std::vector<ScriptLog> ScriptHost::drainLogs() {
    std::vector<ScriptLog> out;
    out.swap(logs_);
    return out;
}

std::vector<std::vector<std::uint8_t>> ScriptHost::drainSendQueue() {
    std::vector<std::vector<std::uint8_t>> out;
    out.swap(sendQueue_);
    return out;
}

void ScriptHost::callbackOnOpen(const ScriptHostContext& ctx) {
    protoLog("info", "连接已打开: " + kindName(ctx.connection.kind) + " endpoint=" + ctx.connection.endpoint);
}

void ScriptHost::callbackOnClose(const ScriptHostContext& ctx) {
    protoLog("info", "连接已关闭: id=" + std::to_string(ctx.connection.connectionId));
}

void ScriptHost::callbackOnError(const ScriptHostContext& ctx, const std::string& message) {
    protoLog("error", "连接错误(" + kindName(ctx.connection.kind) + "): " + message);
}

void ScriptHost::callbackOnBytes(const ScriptHostContext& ctx, const std::vector<std::uint8_t>& bytes) {
    const auto hex = protocol_utils::bytesToHex(bytes, true);
    protoEmit("raw_frame", "id=" + std::to_string(ctx.connection.connectionId) + " hex=" + hex);

    if (waitingResponse_ && bytes.size() >= 2 && bytes[0] == 'O' && bytes[1] == 'K') {
        waitingResponse_ = false;
        protoCancelTimer("read_version_timeout");
        protoEmit("frame", "{\"device_id\":\"" + lastDeviceId_ + "\",\"version\":\"v1.0.0\"}");
        protoLog("info", "解析到版本响应，流程完成");
    }
}

void ScriptHost::callbackOnTimer(const ScriptHostContext&, const std::string& timerName) {
    if (timerName == "read_version_timeout" && waitingResponse_) {
        waitingResponse_ = false;
        protoEmit("warning", "{\"message\":\"读取版本超时\"}");
        protoLog("warn", "read_version 超时");
    }
}

void ScriptHost::callbackOnControl(const ScriptHostContext&, const std::string& id, const ControlValue& value) {
    if (id == "device_id") {
        lastDeviceId_ = valueToString(value);
        protoLog("info", "设备 ID 更新为 " + lastDeviceId_);
        return;
    }

    if (id == "read_version") {
        if (waitingResponse_) {
            protoLog("warn", "已有请求在等待响应，拒绝重复触发");
            return;
        }

        waitingResponse_ = true;
        std::vector<std::uint8_t> frame{0xAA, 0x01, 0x00};
        auto maybeId = protocol_utils::hexToBytes(lastDeviceId_);
        if (maybeId.has_value() && !maybeId->empty()) {
            frame[1] = maybeId->front();
        }

        const auto crc = protocol_utils::crc16Modbus(frame);
        frame.push_back(static_cast<std::uint8_t>(crc & 0x00FFU));
        frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0x00FFU));

        protoSend(frame);
        protoSetTimer("read_version_timeout", 1000);
        protoLog("info", "已发送 read_version 请求");
        return;
    }

    protoLog("warn", "收到未处理控件事件: " + id + " value=" + valueToString(value));
}

void ScriptHost::protoSend(const std::vector<std::uint8_t>& bytes) {
    sendQueue_.push_back(bytes);
}

void ScriptHost::protoLog(const std::string& level, const std::string& message) {
    logs_.push_back(ScriptLog{.level = level, .message = message, .timestampMs = nowMs()});
}

void ScriptHost::protoEmit(const std::string& eventName, const std::string& payload) {
    events_.push_back(ScriptEvent{.name = eventName, .payload = payload, .timestampMs = nowMs()});
}

void ScriptHost::protoSetTimer(const std::string& name, std::uint64_t intervalMs) {
    timers_[name] = TimerState{.name = name, .dueAtMs = nowMs() + intervalMs, .active = true};
}

void ScriptHost::protoCancelTimer(const std::string& name) {
    auto iter = timers_.find(name);
    if (iter != timers_.end()) {
        iter->second.active = false;
    }
}

std::uint64_t ScriptHost::nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string ScriptHost::valueToString(const ControlValue& value) {
    return std::visit(
        [](const auto& val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, bool>) {
                return val ? std::string("true") : std::string("false");
            } else if constexpr (std::is_same_v<T, int>) {
                return std::to_string(val);
            } else if constexpr (std::is_same_v<T, float>) {
                std::ostringstream oss;
                oss << val;
                return oss.str();
            } else {
                return val;
            }
        },
        value);
}

} // namespace protoscope::scripting
