#include "protoscope/scripting/script_host.hpp"

#include <chrono>
#include <filesystem>
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

ControlValue defaultValueFor(const ControlDescriptor& descriptor) {
    switch (descriptor.type) {
    case ControlType::Button:
        return false;
    case ControlType::InputText:
        return descriptor.textDefault;
    case ControlType::InputInt:
        return descriptor.intDefault;
    case ControlType::InputFloat:
        return descriptor.floatDefault;
    case ControlType::Checkbox:
        return descriptor.boolDefault;
    case ControlType::Combo:
        return descriptor.comboDefaultIndex;
    }
    return false;
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

    for (const auto& control : controls_) {
        controlValues_[control.id] = defaultValueFor(control);
    }
}

bool ScriptHost::loadScriptFile(const std::string& path) {
    scriptPath_ = path;
    std::filesystem::path fsPath(path);
    protocolDirectory_ = fsPath.parent_path().generic_string();
    scriptLoaded_ = !path.empty();

    if (!scriptLoaded_) {
        protoLog("error", "脚本路径为空，保持内建脚本模式");
        return false;
    }

    protoLog("info", "已加载脚本(内建模拟): " + path);
    return true;
}

bool ScriptHost::loadProtocolDirectory(const std::string& directory) {
    std::filesystem::path root(directory);
    protocolDirectory_ = root.generic_string();
    return loadScriptFile((root / "main.lua").generic_string());
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
    controlValues_[id] = value;
    callbackOnControl(ScriptHostContext{ctx}, id, value);
}

void ScriptHost::invokeAction(const transport::ConnectionContext& ctx, const std::string& actionName) {
    if (actionName == "read_version") {
        controlValues_["read_version"] = true;
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
        auto iter = timers_.find(name);
        if (iter != timers_.end()) {
            iter->second.active = false;
        }
        callbackOnTimer(ScriptHostContext{*activeConnection_}, name);
    }
}

std::vector<ControlDescriptor> ScriptHost::controlsSnapshot() const {
    return controls_;
}

std::vector<ControlSnapshot> ScriptHost::controlStatesSnapshot() const {
    std::vector<ControlSnapshot> result;
    result.reserve(controls_.size());
    for (const auto& control : controls_) {
        const auto iter = controlValues_.find(control.id);
        result.push_back(ControlSnapshot{
            .descriptor = control,
            .value = (iter == controlValues_.end()) ? defaultValueFor(control) : iter->second,
        });
    }
    return result;
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

std::optional<std::uint64_t> ScriptHost::nextWakeupAtMs() const {
    std::optional<std::uint64_t> result;
    for (const auto& [_, timer] : timers_) {
        if (!timer.active) {
            continue;
        }
        if (!result.has_value() || timer.dueAtMs < *result) {
            result = timer.dueAtMs;
        }
    }
    return result;
}

const std::string& ScriptHost::scriptPath() const {
    return scriptPath_;
}

const std::string& ScriptHost::protocolDirectory() const {
    return protocolDirectory_;
}

void ScriptHost::callbackOnOpen(const ScriptHostContext& ctx) {
    protoLog("info", "连接已打开: " + kindName(ctx.connection.kind));
}

void ScriptHost::callbackOnClose(const ScriptHostContext&) {
    protoLog("info", "连接已关闭");
}

void ScriptHost::callbackOnError(const ScriptHostContext&, const std::string& message) {
    protoLog("error", "连接错误: " + message);
}

void ScriptHost::callbackOnBytes(const ScriptHostContext&, const std::vector<std::uint8_t>& bytes) {
    const auto hex = protocol_utils::bytesToHex(bytes, true);
    protoEmit("rx_bytes", hex);

    if (waitingResponse_ && bytes.size() >= 2 && bytes[0] == 'O' && bytes[1] == 'K') {
        waitingResponse_ = false;
        protoCancelTimer("read_version_timeout");
        protoEmit("frame", "版本读取成功");
    }
}

void ScriptHost::callbackOnTimer(const ScriptHostContext&, const std::string& timerName) {
    if (timerName == "read_version_timeout" && waitingResponse_) {
        waitingResponse_ = false;
        protoEmit("warning", "读取版本超时");
    }
}

void ScriptHost::callbackOnControl(const ScriptHostContext&, const std::string& id, const ControlValue& value) {
    if (id == "device_id") {
        lastDeviceId_ = valueToString(value);
        return;
    }

    if (id == "read_version") {
        if (waitingResponse_) {
            protoLog("warn", "读取版本仍在等待响应，忽略重复触发");
            return;
        }

        std::vector<std::uint8_t> frame{0xAA};
        auto maybeId = protocol_utils::hexToBytes(lastDeviceId_);
        if (maybeId.has_value() && !maybeId->empty()) {
            frame.insert(frame.end(), maybeId->begin(), maybeId->end());
        } else {
            frame.push_back(0x01);
        }

        frame.push_back(0x10);
        frame.push_back(0x00);

        const auto crc = protocol_utils::crc16Modbus(frame);
        frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
        frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));

        protoSend(frame);
        protoSetTimer("read_version_timeout", 1000);
        waitingResponse_ = true;
    }
}

void ScriptHost::protoSend(const std::vector<std::uint8_t>& bytes) {
    sendQueue_.push_back(bytes);
}

void ScriptHost::protoLog(const std::string& level, const std::string& message) {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    logs_.push_back(ScriptLog{.level = level, .message = message, .timestampMs = now});
}

void ScriptHost::protoEmit(const std::string& eventName, const std::string& payload) {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    events_.push_back(ScriptEvent{.name = eventName, .payload = payload, .timestampMs = now});
}

void ScriptHost::protoSetTimer(const std::string& name, std::uint64_t intervalMs) {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    timers_[name] = TimerState{.name = name, .dueAtMs = now + intervalMs, .active = true};
}

void ScriptHost::protoCancelTimer(const std::string& name) {
    auto iter = timers_.find(name);
    if (iter != timers_.end()) {
        iter->second.active = false;
    }
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
