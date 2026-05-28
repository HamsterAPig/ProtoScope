#include "protoscope/app/application.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <chrono>
#include <cmath>
#include <limits>
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

bool sameChannelSpecs(const std::vector<scripting::PlotChannelDescriptor>& setupChannels,
                      const plot::OscilloscopeBuffer& buffer) {
    if (setupChannels.size() != buffer.channelCount()) {
        return false;
    }
    for (std::size_t i = 0; i < setupChannels.size(); ++i) {
        const auto current = buffer.channelSpec(i);
        if (!current.has_value()) {
            return false;
        }
        const auto& setup = setupChannels[i];
        if (current->label != setup.label || current->unit != setup.unit) {
            return false;
        }
    }
    return true;
}

bool nearlyEqual(double left, double right) {
    return std::abs(left - right) <= 1e-12;
}

bool sameWaveViewState(const plot::WaveViewState& view, const plot::ViewConfig& config) {
    const double defaultVisibleDuration = (std::max)(view.minVisibleTimeSpan, (std::max)(config.timeScale * 1000.0, config.timeScale));
    if (!nearlyEqual(view.visibleDuration, defaultVisibleDuration)) {
        return false;
    }
    if (!nearlyEqual(view.manualVerticalMin, config.verticalMin) || !nearlyEqual(view.manualVerticalMax, config.verticalMax)) {
        return false;
    }
    if (!nearlyEqual(view.viewMinValue, config.verticalMin) || !nearlyEqual(view.viewMaxValue, config.verticalMax)) {
        return false;
    }
    return true;
}

} // namespace

Application::Application() = default;

bool Application::initialize() {
    loggingFacade_.bindDockStore(&dockStore_);
    std::string workspaceError;
    if (!configStore_.ensureDefaultProtocolWorkspace(workspaceError)) {
        dockStore_.markDirty("初始化 protocols 工作目录失败: " + workspaceError);
        loggingFacade_.warn("config", "初始化 protocols 工作目录失败: " + workspaceError);
    } else if (!configStore_.ensureDefaultScriptWorkspace(workspaceError)) {
        dockStore_.markDirty("初始化 scripts 工作目录失败: " + workspaceError);
        loggingFacade_.warn("config", "初始化 scripts 工作目录失败: " + workspaceError);
    }

    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    runtimeConfig_ = loaded.config;
    loggingFacade_.applyConfig(loaded.config.logging);
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
    loggingFacade_.applyConfig(config.logging);
    return reloadProtocolDirectory(dockStore_.luaState().protocolDir);
}

config::AppConfig Application::captureConfig() const {
    auto captured = configStore_.captureFromDock(dockStore_);
    captured.gui = runtimeConfig_.gui;
    captured.app.language = runtimeConfig_.app.language;
    captured.logging = loggingFacade_.currentConfig();
    return captured;
}

bool Application::reloadProtocolDirectory(const std::string& protocolDir, bool forceReload) {
    auto& lua = dockStore_.luaState();
    const auto resolvedDir = configStore_.normalizeProtocolDir(lua.protocolRootDir, protocolDir);
    const auto resolvedDirText = resolvedDir.generic_string();
    const auto protocolName = configStore_.protocolName(resolvedDir);
    const auto scriptPath = configStore_.mainLuaPath(resolvedDir).generic_string();
    const bool unchanged = lua.loaded && lua.protocolDir == resolvedDirText && lua.scriptPath == scriptPath;

    // 核心流程：配置热加载只在协议目录真正变化时重载脚本，避免窗口刷新阶段重复刷加载日志。
    if (!forceReload && unchanged) {
        lua.lastError.clear();
        lua.docks = scriptHost_.dockSnapshots();
        lua.controls = scriptHost_.controlsSnapshot();
        lua.controlStates = scriptHost_.controlStatesSnapshot();
        return true;
    }

    scripting::ScriptHost probeHost;
    if (!probeHost.loadProtocolDirectory(resolvedDirText)) {
        lua.lastError = probeHost.lastError();
        loggingFacade_.error("protocol", "协议加载探测失败: " + lua.lastError);
        lua.docks = scriptHost_.dockSnapshots();
        lua.controls = scriptHost_.controlsSnapshot();
        lua.controlStates = scriptHost_.controlStatesSnapshot();
        return false;
    }

    // 核心流程：先用临时宿主探测目标协议能否完整加载，确认成功后再在当前宿主上重载，
    // 避免失败场景先清空旧运行态，也避免 Lua 回调把临时宿主地址固化进运行时对象。
    if (!scriptHost_.loadProtocolDirectory(resolvedDirText)) {
        lua.lastError = scriptHost_.lastError();
        loggingFacade_.error("protocol", "协议重载失败: " + lua.lastError);
        lua.loaded = false;
        lua.docks = scriptHost_.dockSnapshots();
        lua.controls = scriptHost_.controlsSnapshot();
        lua.controlStates = scriptHost_.controlStatesSnapshot();
        return false;
    }

    lua.protocolDir = resolvedDirText;
    lua.protocolName = protocolName;
    lua.scriptPath = scriptPath;
    lua.loaded = true;
    lua.docks = scriptHost_.dockSnapshots();
    lua.controls = scriptHost_.controlsSnapshot();
    lua.controlStates = scriptHost_.controlStatesSnapshot();
    lua.lastError.clear();
    loggingFacade_.info("protocol", "协议已加载: " + resolvedDirText);
    flushScriptLogs();
    syncDockState();
    return true;
}

bool Application::pumpOnce() {
    bool changed = false;
    changed = handleTransportEvents() || changed;
    scriptHost_.tick(nowMs());
    changed = flushScriptOutputs() || changed;
    changed = flushScriptLogs() || changed;
    changed = flushScriptPlots() || changed;
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
        loggingFacade_.error("transport", dockStore_.commState().lastError);
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
        loggingFacade_.error("transport", dockStore_.commState().lastError);
    } else {
        dockStore_.commState().lastError.clear();
        dockStore_.commState().reconnectRequired = false;
        loggingFacade_.info("transport", "连接打开请求已提交");
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
        loggingFacade_.warn("transport", dockStore_.commState().lastError);
        return false;
    }

    std::vector<std::uint8_t> bytes;
    if (hexMode) {
        if (protocol_utils::countHexDigits(payload) % 2 != 0) {
            dockStore_.commState().lastError = "HEX 文本必须按完整字节输入";
            loggingFacade_.warn("transport", dockStore_.commState().lastError);
            return false;
        }
        const auto parsed = protocol_utils::hexToBytes(payload);
        if (!parsed.has_value()) {
            dockStore_.commState().lastError = "HEX 文本解析失败";
            loggingFacade_.warn("transport", dockStore_.commState().lastError);
            return false;
        }
        bytes = *parsed;
    } else {
        bytes.assign(payload.begin(), payload.end());
    }

    if (!transport_->send(bytes)) {
        dockStore_.commState().lastError = "发送失败";
        loggingFacade_.error("transport", dockStore_.commState().lastError);
        return false;
    }
    if (activeConnection_.has_value()) {
        dockStore_.appendReceiveRow(dock::ReceiveRow{
            .timestampMs = nowMs(),
            .direction = "TX",
            .endpoint = activeConnection_->endpoint,
            .bytes = bytes,
            .message = {},
        });
    }
    dockStore_.commState().lastError.clear();
    loggingFacade_.info("transport", "手动发送成功");
    return true;
}

void Application::updateControlValue(const std::string& id, const scripting::ControlValue& value) {
    if (activeConnection_.has_value()) {
        scriptHost_.onControl(*activeConnection_, id, value);
    } else {
        // 核心流程：动态控件也可能只驱动 Lua 本地演示逻辑，未连接时仍允许回调脚本。
        transport::ConnectionContext detachedContext;
        detachedContext.endpoint = "detached";
        detachedContext.connectionId = 0;
        detachedContext.timestampMs = nowMs();
        detachedContext.readyForIo = false;
        scriptHost_.onControl(detachedContext, id, value);
    }
    flushScriptOutputs();
    flushScriptLogs();
    flushScriptPlots();
    syncDockState();
}

bool Application::restoreControlValue(const std::string& id, const scripting::ControlValue& value) {
    if (!scriptHost_.setControlValue(id, value)) {
        return false;
    }
    syncDockState();
    return true;
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
        loggingFacade_.warn("transport", dockStore_.commState().lastError);
        return false;
    }

    const auto parsed = protocol_utils::hexToBytes(send.payload);
    if (!parsed.has_value()) {
        dockStore_.commState().lastError = "HEX 文本解析失败，无法切回文本模式";
        loggingFacade_.warn("transport", dockStore_.commState().lastError);
        return false;
    }

    send.payload.assign(parsed->begin(), parsed->end());
    send.hexMode = false;
    setStatusMessage("发送框已切换到文本模式");
    return true;
}

void Application::resetWaveHistory() {
    auto& wave = dockStore_.waveState();
    wave.buffer.clear();
    wave.channelSummaries.clear();
    wave.view.initialized = false;
    wave.view.centerTime = 0.0;
    wave.view.viewMinTime = 0.0;
    wave.view.viewMaxTime = wave.view.visibleDuration;
    wave.view.viewMinValue = wave.view.manualVerticalMin;
    wave.view.viewMaxValue = wave.view.manualVerticalMax;
    wave.statusMessage = "波形历史已清空";
}

std::optional<std::uint64_t> Application::nextWakeupAtMs() const {
    return scriptHost_.nextWakeupAtMs();
}

void Application::setTransportFactoryForTest(std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> factory) {
    transportFactoryForTest_ = std::move(factory);
}

std::unique_ptr<transport::ITransport> Application::createTransport(transport::TransportKind kind) const {
    if (transportFactoryForTest_) {
        return transportFactoryForTest_(kind);
    }
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

    auto& wave = dockStore_.waveState();
    wave.channelSummaries.clear();
    const auto snapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    for (const auto& channel : snapshot.channels) {
        wave.channelSummaries.push_back(channel.label + " samples=" + std::to_string(channel.totalSamples));
    }
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
                    loggingFacade_.host(config::LogLevel::Info,
                                        "OPEN",
                                        evt.context.endpoint,
                                        stateMessage(dockStore_.commState().state),
                                        evt.context.timestampMs);
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportCloseEvent>) {
                    if (evt.context.readyForIo) {
                        scriptHost_.onTransportClose(evt);
                    }
                    loggingFacade_.host(config::LogLevel::Info, "CLOSE", evt.context.endpoint, evt.reason, evt.context.timestampMs);
                    if (activeConnection_.has_value() && activeConnection_->connectionId == evt.context.connectionId) {
                        activeConnection_.reset();
                    }
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportErrorEvent>) {
                    if (evt.context.readyForIo) {
                        scriptHost_.onTransportError(evt);
                    }
                    loggingFacade_.host(config::LogLevel::Error, "ERROR", evt.context.endpoint, evt.message, evt.context.timestampMs);
                    dockStore_.commState().lastError = evt.message;
                    changed = true;
                } else if constexpr (std::is_same_v<T, transport::TransportBytesEvent>) {
                    if (!evt.bytes.empty()) {
                        // 核心流程：只消费当前活动连接的字节事件，旧连接的迟到回包直接忽略，
                        // 避免双窗口接管场景下脚本状态与 UI 日志被过期连接污染。
                        if (activeConnection_.has_value() && evt.context.readyForIo &&
                            activeConnection_->connectionId != evt.context.connectionId) {
                            return;
                        }
                        scriptHost_.onTransportBytes(evt);
                        if (evt.context.readyForIo) {
                            activeConnection_ = evt.context;
                        }
                        dockStore_.appendReceiveRow(dock::ReceiveRow{
                            .timestampMs = evt.context.timestampMs,
                            .direction = "RX",
                            .endpoint = evt.context.endpoint,
                            .bytes = evt.bytes,
                            .message = {},
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
                .message = {},
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
        loggingFacade_.script(log.level, log.message, log.timestampMs);
        changed = true;
    }
    return changed;
}

logging::LoggingFacade& Application::logger() {
    return loggingFacade_;
}

const logging::LoggingFacade& Application::logger() const {
    return loggingFacade_;
}

bool Application::flushScriptPlots() {
    bool changed = false;
    auto& wave = dockStore_.waveState();

    for (const auto& setup : scriptHost_.drainPlotSetups()) {
        const auto previousConfig = wave.buffer.viewConfig();
        const bool configChanged = !nearlyEqual(previousConfig.timeScale, setup.view.timeScale) ||
                                   previousConfig.timeUnit != setup.view.timeUnit ||
                                   !nearlyEqual(previousConfig.verticalMin, setup.view.verticalMin) ||
                                   !nearlyEqual(previousConfig.verticalMax, setup.view.verticalMax) ||
                                   previousConfig.verticalUnit != setup.view.verticalUnit ||
                                   previousConfig.historyLimit != setup.view.historyLimit;
        const bool channelsChanged = !sameChannelSpecs(setup.channels, wave.buffer);

        if (setup.resetHistory) {
            wave.buffer.clear();
        }
        wave.buffer.setViewConfig(setup.view);
        wave.buffer.configureChannels(setup.channels.size());
        wave.defaultChannelSpecs.clear();
        wave.defaultChannelSpecs.reserve(setup.channels.size());
        const bool shouldResetOverrides = setup.resetHistory || channelsChanged;
        if (shouldResetOverrides) {
            wave.channelOverrides.clear();
        }
        wave.channelOverrides.resize(setup.channels.size());
        for (std::size_t index = 0; index < setup.channels.size(); ++index) {
            const auto defaultSpec = plot::ChannelSpec{
                .label = setup.channels[index].label,
                .unit = setup.channels[index].unit,
                .scale = setup.channels[index].scale,
                .offset = setup.channels[index].offset,
            };
            auto effectiveSpec = defaultSpec;
            if (!shouldResetOverrides && index < wave.channelOverrides.size()) {
                const auto& overrideState = wave.channelOverrides[index];
                if (overrideState.scaleOverridden) {
                    effectiveSpec.scale = overrideState.scale;
                }
                if (overrideState.offsetOverridden) {
                    effectiveSpec.offset = overrideState.offset;
                }
            }
            wave.defaultChannelSpecs.push_back(defaultSpec);
            wave.buffer.setChannelSpec(index, std::move(effectiveSpec));
        }
        const bool viewChanged = !sameWaveViewState(wave.view, setup.view);
        const bool shouldResetView = setup.resetHistory || configChanged || channelsChanged || viewChanged;
        if (shouldResetView) {
            wave.view.visibleDuration = (std::max)(wave.view.minVisibleTimeSpan,
                                                   (std::max)(setup.view.timeScale * 1000.0, setup.view.timeScale));
            wave.view.manualVerticalMin = setup.view.verticalMin;
            wave.view.manualVerticalMax = setup.view.verticalMax;
            wave.view.viewMinValue = setup.view.verticalMin;
            wave.view.viewMaxValue = setup.view.verticalMax;
            wave.view.initialized = false;
            wave.statusMessage = "Lua 已更新波形通道配置";
        }
        changed = true;
    }

    for (auto& [channelIndex, request] : scriptHost_.drainPlotAppends()) {
        if (wave.buffer.append(channelIndex, std::move(request))) {
            changed = true;
        }
    }

    return changed;
}

} // namespace protoscope::app
