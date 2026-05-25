#include "protoscope/app/application.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <chrono>
#include <type_traits>

namespace protoscope::app {

namespace {

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

const char* stateMessage(transport::TransportState state) {
    switch (state) {
    case transport::TransportState::Closed:
        return "closed";
    case transport::TransportState::Opening:
        return "opening";
    case transport::TransportState::Open:
        return "open";
    case transport::TransportState::Error:
        return "error";
    }
    return "unknown";
}

} // namespace

Application::Application() = default;

bool Application::initialize() {
    std::string workspaceError;
    if (!configStore_.ensureDefaultScriptWorkspace(workspaceError)) {
        dockStore_.markDirty("初始化 scripts 工作目录失败: " + workspaceError);
    }

    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    runtimeConfig_ = loaded.config;
    applyConfig(loaded.config);

    auto& configState = dockStore_.configState();
    configState.loadedFromPath = loaded.resolvedPath.generic_string();
    configState.fileTimestampMs = configStore_.snapshot(loaded.resolvedPath).timestampMs;
    if (loaded.loadedFromDisk) {
        dockStore_.clearDirty("已从 YAML 加载配置");
    } else if (!loaded.error.empty()) {
        dockStore_.markDirty(loaded.error);
    } else if (workspaceError.empty()) {
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

bool Application::reloadProtocolDirectory(const std::string& protocolDir, bool forceReload) {
    auto& lua = dockStore_.luaState();
    const auto resolvedDir = configStore_.normalizeProtocolDir(lua.protocolRootDir, protocolDir);
    const auto resolvedDirText = resolvedDir.generic_string();
    const auto scriptPath = configStore_.mainLuaPath(resolvedDir).generic_string();
    const bool unchanged = lua.loaded && lua.protocolDir == resolvedDirText && lua.scriptPath == scriptPath;

    lua.protocolDir = resolvedDirText;
    lua.protocolName = configStore_.protocolName(resolvedDir);
    lua.scriptPath = scriptPath;

    // 核心流程：配置热加载只在协议目录真正变化时重载脚本，避免窗口刷新阶段重复刷加载日志。
    if (!forceReload && unchanged) {
        lua.lastError.clear();
        lua.docks = scriptHost_.dockSnapshots();
        lua.controls = scriptHost_.controlsSnapshot();
        lua.controlStates = scriptHost_.controlStatesSnapshot();
        return true;
    }

    lua.loaded = scriptHost_.loadProtocolDirectory(lua.protocolDir);
    lua.docks = scriptHost_.dockSnapshots();
    lua.controls = scriptHost_.controlsSnapshot();
    lua.controlStates = scriptHost_.controlStatesSnapshot();
    lua.lastError = lua.loaded ? std::string() : scriptHost_.lastError();
    flushScriptLogs();
    syncDockState();
    return lua.loaded;
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
        if (protocol_utils::countHexDigits(payload) % 2 != 0) {
            dockStore_.commState().lastError = "HEX 文本必须按完整字节输入";
            return false;
        }
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
        dockStore_.appendReceiveRow(dock::ReceiveRow{
            .timestampMs = nowMs(),
            .direction = "TX",
            .endpoint = activeConnection_->endpoint,
            .bytes = bytes,
        });
    }
    dockStore_.commState().lastError.clear();
    dockStore_.appendLogRow(dock::ReceiveRow{
        .timestampMs = nowMs(),
        .direction = "INFO",
        .endpoint = "transport",
        .message = "手动发送成功",
    });
    return true;
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
    dockStore_.markDirty(reconnectRequired ? "通讯配置已修改，需重新连接" : "通讯配置已修改");
}

void Application::markProtocolEdited() {
    dockStore_.markDirty("协议配置已修改");
}

void Application::setStatusMessage(std::string message, bool markDirty) {
    if (markDirty) {
        dockStore_.markDirty(message);
        return;
    }
    dockStore_.configState().statusMessage = std::move(message);
}

bool Application::setSendHexMode(bool enabled) {
    auto& send = dockStore_.sendState();
    if (send.hexMode == enabled) {
        return true;
    }

    if (enabled) {
        const std::vector<std::uint8_t> bytes(send.payload.begin(), send.payload.end());
        send.payload = protocol_utils::bytesToHex(bytes, true);
        send.hexMode = true;
        setStatusMessage("发送框已切换到 HEX 模式");
        return true;
    }

    if (protocol_utils::countHexDigits(send.payload) % 2 != 0) {
        dockStore_.commState().lastError = "HEX 文本必须按完整字节输入，无法切回文本模式";
        return false;
    }

    const auto parsed = protocol_utils::hexToBytes(send.payload);
    if (!parsed.has_value()) {
        dockStore_.commState().lastError = "HEX 文本解析失败，无法切回文本模式";
        return false;
    }

    send.payload.assign(parsed->begin(), parsed->end());
    send.hexMode = false;
    setStatusMessage("发送框已切换到文本模式");
    return true;
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
    }

    auto& lua = dockStore_.luaState();
    lua.docks = scriptHost_.dockSnapshots();
    lua.controls = scriptHost_.controlsSnapshot();
    lua.controlStates = scriptHost_.controlStatesSnapshot();
}

bool Application::handleTransportEvents() {
    if (!transport_) {
        return false;
    }

    bool changed = false;
    auto events = transport_->takeEvents();
    for (const auto& event : events) {
        std::visit(
            [this, &changed](const auto& evt) {
                using T = std::decay_t<decltype(evt)>;
                if constexpr (std::is_same_v<T, transport::TransportOpenEvent>) {
                    if (evt.context.readyForIo) {
                        activeConnection_ = evt.context;
                        scriptHost_.onTransportOpen(evt);
                    }
                    dockStore_.appendLogRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "OPEN",
                        .endpoint = evt.context.endpoint,
                        .message = stateMessage(dockStore_.commState().state),
                    });
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportCloseEvent>) {
                    if (evt.context.readyForIo) {
                        scriptHost_.onTransportClose(evt);
                    }
                    dockStore_.appendLogRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "CLOSE",
                        .endpoint = evt.context.endpoint,
                        .message = evt.reason,
                    });
                    if (activeConnection_.has_value() && activeConnection_->connectionId == evt.context.connectionId) {
                        activeConnection_.reset();
                    }
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportErrorEvent>) {
                    if (evt.context.readyForIo) {
                        scriptHost_.onTransportError(evt);
                    }
                    dockStore_.appendLogRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "ERROR",
                        .endpoint = evt.context.endpoint,
                        .message = evt.message,
                    });
                    dockStore_.commState().lastError = evt.message;
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportBytesEvent>) {
                    if (!evt.bytes.empty()) {
                        scriptHost_.onTransportBytes(evt);
                        if (evt.context.readyForIo) {
                            activeConnection_ = evt.context;
                        }
                        dockStore_.appendReceiveRow(dock::ReceiveRow{
                            .timestampMs = evt.context.timestampMs,
                            .direction = "RX",
                            .endpoint = evt.context.endpoint,
                            .bytes = evt.bytes,
                        });
                        changed = true;
                    }
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
            dockStore_.commState().lastError = "脚本产生了待发数据，但连接未打开";
            continue;
        }
        // 核心流程：脚本动作发送改成异步投递到 transport I/O 线程，避免 GUI 线程卡在同步写。
        if (transport_->enqueueSend(bytes) && activeConnection_.has_value()) {
            dockStore_.appendReceiveRow(dock::ReceiveRow{
                .timestampMs = nowMs(),
                .direction = "TX",
                .endpoint = activeConnection_->endpoint,
                .bytes = bytes,
            });
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
        dockStore_.appendScriptRow(
            dock::ReceiveRow{.timestampMs = log.timestampMs, .direction = "LOG", .endpoint = "script", .message = "[" + log.level + "] " + log.message});
        changed = true;
    }
    return changed;
}

} // namespace protoscope::app
