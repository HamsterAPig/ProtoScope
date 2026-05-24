#include "protoscope/app/application.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <chrono>

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
    auto& lua = dockStore_.luaState();
    lua.loaded = scriptHost_.loadScriptFile(lua.scriptPath);
    lua.controls = scriptHost_.controlsSnapshot();

    auto logs = scriptHost_.drainLogs();
    for (const auto& log : logs) {
        dockStore_.appendReceiveRow(
            dock::ReceiveRow{.timestampMs = log.timestampMs, .direction = "LOG", .endpoint = "script", .text = "[" + log.level + "] " + log.message});
    }

    syncDockState();
    return true;
}

void Application::pumpOnce() {
    handleTransportEvents();
    scriptHost_.tick(nowMs());
    flushScriptOutputs();
    syncDockState();
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

    bool opened = false;
    const auto& comm = dockStore_.commState();
    switch (kind) {
    case transport::TransportKind::TcpClient:
        opened = transport_->open(transport::TransportConfig{comm.tcpClient});
        break;
    case transport::TransportKind::TcpServer:
        opened = transport_->open(transport::TransportConfig{comm.tcpServer});
        break;
    case transport::TransportKind::Serial:
        opened = transport_->open(transport::TransportConfig{comm.serial});
        break;
    }

    if (!opened) {
        dockStore_.commState().lastError = "打开失败，请检查配置";
    }

    handleTransportEvents();
    flushScriptOutputs();
    syncDockState();
}

void Application::closeTransport() {
    if (transport_) {
        transport_->close();
        handleTransportEvents();
        flushScriptOutputs();
    }
    transport_.reset();
    activeConnection_.reset();
    syncDockState();
}

bool Application::sendManualPayload(const std::string& payload, bool hexMode) {
    if (!transport_ || transport_->state() != transport::TransportState::Open) {
        dockStore_.commState().lastError = "连接未打开，不能发送";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (hexMode) {
        const auto parsed = protocol_utils::hexToBytes(payload);
        if (!parsed.has_value()) {
            dockStore_.commState().lastError = "HEX 内容非法";
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
        dockStore_.appendRawSend(*activeConnection_, protocol_utils::bytesToHex(bytes, true));
    }

    syncDockState();
    return true;
}

void Application::triggerAction(const std::string& actionName) {
    if (!activeConnection_.has_value()) {
        dockStore_.commState().lastError = "没有活动连接，无法触发 action";
        return;
    }

    scriptHost_.invokeAction(*activeConnection_, actionName);
    flushScriptOutputs();
    syncDockState();
}

void Application::updateControlValue(const std::string& id, const scripting::ControlValue& value) {
    if (!activeConnection_.has_value()) {
        dockStore_.commState().lastError = "没有活动连接，控件事件不会触发脚本";
        return;
    }

    scriptHost_.onControl(*activeConnection_, id, value);
    flushScriptOutputs();
    syncDockState();
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
    return {};
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
}

void Application::handleTransportEvents() {
    if (!transport_) {
        return;
    }

    auto events = transport_->takeEvents();
    for (const auto& event : events) {
        std::visit(
            [this](const auto& evt) {
                using T = std::decay_t<decltype(evt)>;
                if constexpr (std::is_same_v<T, transport::TransportOpenEvent>) {
                    activeConnection_ = evt.context;
                    dockStore_.commState().lastError.clear();
                    scriptHost_.onTransportOpen(evt);
                    dockStore_.appendReceiveRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "SYS",
                        .endpoint = evt.context.endpoint,
                        .text = "连接已打开",
                    });
                } else if constexpr (std::is_same_v<T, transport::TransportCloseEvent>) {
                    scriptHost_.onTransportClose(evt);
                    dockStore_.appendReceiveRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "SYS",
                        .endpoint = evt.context.endpoint,
                        .text = "连接已关闭: " + evt.reason,
                    });
                    activeConnection_.reset();
                } else if constexpr (std::is_same_v<T, transport::TransportErrorEvent>) {
                    dockStore_.commState().lastError = evt.message;
                    scriptHost_.onTransportError(evt);
                    dockStore_.appendReceiveRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "ERR",
                        .endpoint = evt.context.endpoint,
                        .text = evt.message,
                    });
                } else if constexpr (std::is_same_v<T, transport::TransportBytesEvent>) {
                    if (!evt.bytes.empty()) {
                        dockStore_.appendRawReceive(evt.context, protocol_utils::bytesToHex(evt.bytes, true));
                    }
                    scriptHost_.onTransportBytes(evt);
                }
            },
            event);
    }
}

void Application::flushScriptOutputs() {
    auto pendingSend = scriptHost_.drainSendQueue();
    for (const auto& bytes : pendingSend) {
        if (!transport_) {
            continue;
        }
        if (transport_->send(bytes) && activeConnection_.has_value()) {
            dockStore_.appendRawSend(*activeConnection_, protocol_utils::bytesToHex(bytes, true));
        }
    }

    auto scriptEvents = scriptHost_.drainEvents();
    for (const auto& event : scriptEvents) {
        dockStore_.appendLuaEvent(event);
    }

    auto scriptLogs = scriptHost_.drainLogs();
    for (const auto& log : scriptLogs) {
        dockStore_.appendReceiveRow(
            dock::ReceiveRow{.timestampMs = log.timestampMs, .direction = "LOG", .endpoint = "script", .text = "[" + log.level + "] " + log.message});
    }
}

} // namespace protoscope::app
