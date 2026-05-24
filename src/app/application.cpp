#include "protoscope/app/application.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <chrono>
#include <type_traits>

namespace protoscope::app {

namespace {

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace

Application::Application() = default;

bool Application::initialize() {
    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    runtimeConfig_ = loaded.config;
    applyConfig(loaded.config);

    auto& configState = dockStore_.configState();
    configState.loadedFromPath = loaded.resolvedPath.generic_string();
    configState.fileTimestampMs = configStore_.snapshot(loaded.resolvedPath).timestampMs;
    if (loaded.loadedFromDisk) {
        dockStore_.clearDirty("已从 YAML 加载配置");
    } else {
        dockStore_.markDirty("未找到配置文件，已使用默认配置");
    }

    syncDockState();
    return true;
}

bool Application::applyConfig(const config::AppConfig& config) {
    runtimeConfig_ = config;
    configStore_.applyToDock(config, dockStore_);
    return reloadProtocolDirectory(dockStore_.luaState().protocolDir);
}

config::AppConfig Application::captureConfig() const {
    auto captured = configStore_.captureFromDock(dockStore_);
    captured.gui = runtimeConfig_.gui;
    captured.app.language = runtimeConfig_.app.language;
    return captured;
}

bool Application::reloadProtocolDirectory(const std::string& protocolDir) {
    const auto resolvedDir = configStore_.normalizeProtocolDir(protocolDir);
    auto& lua = dockStore_.luaState();
    lua.protocolDir = resolvedDir.generic_string();
    lua.protocolName = configStore_.protocolName(resolvedDir);
    lua.scriptPath = configStore_.mainLuaPath(resolvedDir).generic_string();
    lua.loaded = scriptHost_.loadProtocolDirectory(lua.protocolDir);
    lua.controls = scriptHost_.controlsSnapshot();
    lua.controlStates = scriptHost_.controlStatesSnapshot();
    lua.lastError = lua.loaded ? std::string() : std::string("协议脚本加载失败");

    const bool changed = flushScriptLogs();
    syncDockState();
    return changed;
}

bool Application::pumpOnce() {
    bool changed = false;
    changed = handleTransportEvents() || changed;
    scriptHost_.tick(nowMs());
    changed = flushScriptOutputs() || changed;
    changed = flushScriptLogs() || changed;
    syncDockState();
    return changed;
}

void Application::shutdown() {
    closeTransport();
}

dock::DockStore& Application::docks() {
    return dockStore_;
}

const dock::DockStore& Application::docks() const {
    return dockStore_;
}

void Application::openTransport() {
    const auto kind = dockStore_.commState().kind;
    transport_ = createTransport(kind);
    if (!transport_) {
        dockStore_.commState().lastError = "创建 transport 失败";
        return;
    }

    transport::TransportConfig config;
    switch (kind) {
    case transport::TransportKind::TcpClient:
        config = dockStore_.commState().tcpClient;
        break;
    case transport::TransportKind::TcpServer:
        config = dockStore_.commState().tcpServer;
        break;
    case transport::TransportKind::Serial:
        config = dockStore_.commState().serial;
        break;
    }

    const bool opened = transport_->open(config);
    if (!opened) {
        dockStore_.commState().lastError = "打开连接失败";
    } else {
        dockStore_.commState().lastError.clear();
        dockStore_.commState().reconnectRequired = false;
    }

    syncDockState();
}

void Application::closeTransport() {
    if (transport_) {
        transport_->close();
    }
    activeConnection_.reset();
    syncDockState();
}

bool Application::sendManualPayload(const std::string& payload, bool hexMode) {
    if (!transport_ || transport_->state() != transport::TransportState::Open) {
        dockStore_.commState().lastError = "连接未打开，无法发送";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (hexMode) {
        const auto parsed = protocol_utils::hexToBytes(payload);
        if (!parsed.has_value()) {
            dockStore_.commState().lastError = "HEX 文本解析失败";
            return false;
        }
        bytes = *parsed;
    } else {
        bytes.assign(payload.begin(), payload.end());
    }

    if (!transport_->send(bytes)) {
        dockStore_.commState().lastError = "发送失败";
        return false;
    }

    if (activeConnection_.has_value()) {
        dockStore_.appendRawSend(*activeConnection_, hexMode ? payload : protocol_utils::bytesToHex(bytes, true));
    }
    return true;
}

void Application::triggerAction(const std::string& actionName) {
    if (!activeConnection_.has_value()) {
        dockStore_.commState().lastError = "连接未打开，无法触发动作";
        return;
    }

    scriptHost_.invokeAction(*activeConnection_, actionName);
    flushScriptOutputs();
    flushScriptLogs();
    syncDockState();
}

void Application::updateControlValue(const std::string& id, const scripting::ControlValue& value) {
    if (!activeConnection_.has_value()) {
        dockStore_.commState().lastError = "连接未打开，无法更新控件";
        return;
    }

    scriptHost_.onControl(*activeConnection_, id, value);
    flushScriptOutputs();
    flushScriptLogs();
    syncDockState();
}

void Application::markCommConfigEdited(bool reconnectRequired) {
    dockStore_.commState().reconnectRequired = reconnectRequired;
    dockStore_.markDirty(reconnectRequired ? "通讯参数已修改，需重连后生效" : "通讯参数已修改");
}

void Application::markProtocolEdited() {
    runtimeConfig_.protocol.selectedDir = dockStore_.luaState().protocolDir;
    dockStore_.markDirty("协议目录已修改，待保存");
}

std::optional<std::uint64_t> Application::nextWakeupAtMs() const {
    return scriptHost_.nextWakeupAtMs();
}

std::unique_ptr<transport::ITransport> Application::createTransport(transport::TransportKind kind) const {
    switch (kind) {
    case transport::TransportKind::TcpClient:
        return std::make_unique<transport::TcpClientTransport>();
    case transport::TransportKind::TcpServer:
        return std::make_unique<transport::TcpServerTransport>();
    case transport::TransportKind::Serial:
        return std::make_unique<transport::SerialTransport>();
    }
    return nullptr;
}

void Application::syncDockState() {
    auto& comm = dockStore_.commState();
    if (transport_) {
        comm.state = transport_->state();
        comm.txCount = transport_->txCount();
        comm.rxCount = transport_->rxCount();
    } else {
        comm.state = transport::TransportState::Closed;
        comm.txCount = 0;
        comm.rxCount = 0;
    }

    auto& lua = dockStore_.luaState();
    lua.controls = scriptHost_.controlsSnapshot();
    lua.controlStates = scriptHost_.controlStatesSnapshot();
}

bool Application::handleTransportEvents() {
    if (!transport_) {
        return false;
    }

    bool changed = false;
    const auto events = transport_->takeEvents();
    for (const auto& event : events) {
        std::visit(
            [this, &changed](const auto& evt) {
                using T = std::decay_t<decltype(evt)>;
                if constexpr (std::is_same_v<T, transport::TransportOpenEvent>) {
                    activeConnection_ = evt.context;
                    scriptHost_.onTransportOpen(evt);
                    dockStore_.appendReceiveRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "SYS",
                        .endpoint = evt.context.endpoint,
                        .text = "连接已打开",
                    });
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportCloseEvent>) {
                    scriptHost_.onTransportClose(evt);
                    dockStore_.appendReceiveRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "SYS",
                        .endpoint = evt.context.endpoint,
                        .text = evt.reason.empty() ? "连接已关闭" : evt.reason,
                    });
                    activeConnection_.reset();
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportErrorEvent>) {
                    scriptHost_.onTransportError(evt);
                    dockStore_.commState().lastError = evt.message;
                    dockStore_.appendReceiveRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "ERR",
                        .endpoint = evt.context.endpoint,
                        .text = evt.message,
                    });
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportBytesEvent>) {
                    if (!evt.bytes.empty()) {
                        dockStore_.appendRawReceive(evt.context, protocol_utils::bytesToHex(evt.bytes, true));
                    }
                    scriptHost_.onTransportBytes(evt);
                    changed = true;
                }
            },
            event);
    }
    return changed;
}

bool Application::flushScriptOutputs() {
    bool changed = false;
    auto pendingSend = scriptHost_.drainSendQueue();
    for (const auto& bytes : pendingSend) {
        if (!transport_ || transport_->state() != transport::TransportState::Open) {
            continue;
        }
        if (transport_->send(bytes) && activeConnection_.has_value()) {
            dockStore_.appendRawSend(*activeConnection_, protocol_utils::bytesToHex(bytes, true));
            changed = true;
        }
    }

    auto scriptEvents = scriptHost_.drainEvents();
    for (const auto& event : scriptEvents) {
        dockStore_.appendLuaEvent(event);
        changed = true;
    }
    return changed;
}

bool Application::flushScriptLogs() {
    bool changed = false;
    auto scriptLogs = scriptHost_.drainLogs();
    for (const auto& log : scriptLogs) {
        dockStore_.appendReceiveRow(
            dock::ReceiveRow{.timestampMs = log.timestampMs, .direction = "LOG", .endpoint = "script", .text = "[" + log.level + "] " + log.message});
        changed = true;
    }
    return changed;
}

} // namespace protoscope::app
