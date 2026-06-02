#include "protoscope/app/application.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <type_traits>
#include <unordered_map>

namespace protoscope::app {

namespace {

constexpr std::size_t kRawCaptureReplayChunkBytes = 1024;
constexpr std::size_t kTransportEventsPerPump = 256;
constexpr auto kTransportEventBudget = std::chrono::milliseconds(4);

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point start,
                           std::chrono::steady_clock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
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

bool sameColor(const std::optional<std::array<float, 4>>& left, const std::optional<std::array<float, 4>>& right) {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    if (!left.has_value()) {
        return true;
    }
    for (std::size_t index = 0; index < left->size(); ++index) {
        if (std::abs((*left)[index] - (*right)[index]) > 1e-6F) {
            return false;
        }
    }
    return true;
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
        if (const auto& setup = setupChannels[i]; current->label != setup.label || current->unit != setup.unit || !sameColor(current->color, setup.color)) {
            return false;
        }
    }
    return true;
}

bool sameChannelIdentity(const std::vector<scripting::PlotChannelDescriptor>& setupChannels,
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

std::string transferFrameFieldValueText(const scripting::StreamFieldValue& value) {
    return std::visit(
        [](const auto& stored) -> std::string {
            using ValueType = std::decay_t<decltype(stored)>;
            std::ostringstream builder;
            if constexpr (std::is_same_v<ValueType, std::vector<std::uint8_t>>) {
                builder << protocol_utils::bytesToHex(stored, true);
            } else if constexpr (std::is_same_v<ValueType, std::vector<std::int64_t>>
                                 || std::is_same_v<ValueType, std::vector<double>>) {
                builder << "[";
                for (std::size_t index = 0; index < stored.size(); ++index) {
                    if (index > 0) {
                        builder << ", ";
                    }
                    builder << stored[index];
                }
                builder << "]";
            } else if constexpr (std::is_same_v<ValueType, double>) {
                builder << std::setprecision(6) << stored;
            } else {
                builder << stored;
            }
            return builder.str();
        },
        value.value);
}

std::string transferFrameMessage(const scripting::StreamParsedFrame& frame) {
    std::ostringstream builder;
    builder << "frame";
    if (!frame.name.empty()) {
        builder << " " << frame.name;
    }
    builder << " len=" << frame.raw.size();
    if (!frame.fields.empty()) {
        builder << " fields={";
        bool first = true;
        for (const auto& [name, value] : frame.fields) {
            if (!first) {
                builder << ", ";
            }
            first = false;
            builder << name << "=" << transferFrameFieldValueText(value);
        }
        builder << "}";
    }
    return builder.str();
}

transport::TransportTxKind toTransportTxKind(scripting::TxRequestKind kind) {
    switch (kind) {
    case scripting::TxRequestKind::Send:
        return transport::TransportTxKind::Send;
    case scripting::TxRequestKind::Request:
        return transport::TransportTxKind::Request;
    }
    return transport::TransportTxKind::Send;
}

scripting::TxEventState toScriptTxState(transport::TransportTxState state) {
    switch (state) {
    case transport::TransportTxState::Sent:
        return scripting::TxEventState::Sent;
    case transport::TransportTxState::Timeout:
        return scripting::TxEventState::Timeout;
    case transport::TransportTxState::Rejected:
        return scripting::TxEventState::Rejected;
    case transport::TransportTxState::Dropped:
        return scripting::TxEventState::Dropped;
    case transport::TransportTxState::Canceled:
        return scripting::TxEventState::Canceled;
    }
    return scripting::TxEventState::Rejected;
}

bool nearlyEqual(double left, double right) {
    return std::abs(left - right) <= 1e-12;
}

std::string formatFrequencyInput(double valueHz) {
    if (!(std::isfinite(valueHz)) || valueHz <= 0.0) {
        return {};
    }
    std::ostringstream out;
    out << valueHz;
    return out.str();
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
    resetStreamBufferAlertState();
    scriptHost_.setFileIoConfig(config.scripting.fileIo);
    applyHistoryLimits(config.gui.logHistory);
    configStore_.applyToDock(config, dockStore_);
    loggingFacade_.applyConfig(config.logging);
    return reloadProtocolDirectory(dockStore_.luaState().protocolDir);
}

void Application::setLogLevel(const config::LogLevel level) {
    runtimeConfig_.logging.level = level;
    auto logging = loggingFacade_.currentConfig();
    logging.level = level;
    // 核心流程：菜单切换只刷新日志门限，不走 applyConfig，避免无关协议重载。
    loggingFacade_.applyConfig(logging);
}

config::AppConfig Application::captureConfig() const {
    auto captured = configStore_.captureFromDock(dockStore_);
    captured.gui.window = runtimeConfig_.gui.window;
    captured.gui.logHistory = runtimeConfig_.gui.logHistory;
    captured.gui.rawCapture = runtimeConfig_.gui.rawCapture;
    captured.gui.realtimeBacklog = runtimeConfig_.gui.realtimeBacklog;
    captured.gui.elfSymbolCombo = runtimeConfig_.gui.elfSymbolCombo;
    captured.gui.showAppHeader = runtimeConfig_.gui.showAppHeader;
    captured.gui.sendHistoryLimit = runtimeConfig_.gui.sendHistoryLimit;
    captured.gui.luaDockLayoutDebug = runtimeConfig_.gui.luaDockLayoutDebug;
    captured.gui.replayRawHistoryOnSchemaSwitch = runtimeConfig_.gui.replayRawHistoryOnSchemaSwitch;
    captured.app.language = runtimeConfig_.app.language;
    captured.receive = runtimeConfig_.receive;
    captured.scripting = runtimeConfig_.scripting;
    captured.logging = loggingFacade_.currentConfig();
    return captured;
}

const config::AppConfig& Application::runtimeConfig() const {
    return runtimeConfig_;
}

bool Application::reloadProtocolDirectory(const std::string& protocolDir, bool forceReload) {
    auto& lua = dockStore_.luaState();
    try {
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
        probeHost.setFileIoConfig(runtimeConfig_.scripting.fileIo);
        if (!probeHost.loadProtocolDirectory(resolvedDirText)) {
            lua.lastError = probeHost.lastError();
            loggingFacade_.error("protocol", "协议加载探测失败: " + lua.lastError);
            lua.docks = scriptHost_.dockSnapshots();
            lua.controls = scriptHost_.controlsSnapshot();
            lua.controlStates = scriptHost_.controlStatesSnapshot();
            return false;
        }

        try {
            cancelAllTxRequests("协议已重新加载");
        } catch (const std::exception& ex) {
            loggingFacade_.warn("protocol", std::string("协议重载前取消旧请求失败: ") + ex.what());
        } catch (...) {
            loggingFacade_.warn("protocol", "协议重载前取消旧请求失败: 未知异常");
        }
        try {
            // 核心流程：取消旧 request 可能触发旧脚本 on_tx；替换宿主前丢弃旧输出，
            // 避免旧回调追加的新请求、状态或弹窗污染新协议运行态。
            scriptHost_.drainTxRequests();
            scriptHost_.drainRequestDoneResults();
            scriptHost_.drainStatusUpdates();
            scriptHost_.drainDialogRequests();
            scriptHost_.drainFileDialogRequests();
            scriptHost_.drainEvents();
            scriptHost_.drainLogs();
            scriptHost_.clearPendingRealtimeOutputs();
        } catch (const std::exception& ex) {
            loggingFacade_.warn("protocol", std::string("协议重载前清理旧脚本输出失败: ") + ex.what());
        } catch (...) {
            loggingFacade_.warn("protocol", "协议重载前清理旧脚本输出失败: 未知异常");
        }

        scriptHost_.setFileIoConfig(runtimeConfig_.scripting.fileIo);
        if (!scriptHost_.loadProtocolDirectory(resolvedDirText)) {
            lua.lastError = scriptHost_.lastError();
            loggingFacade_.error("protocol", "协议加载失败: " + lua.lastError);
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
        rebuildTransferFrameRows();
        loggingFacade_.info("protocol", "协议已加载: " + resolvedDirText);
        flushScriptLogs();
        syncDockState();
        return true;
    } catch (const std::exception& ex) {
        lua.lastError = std::string("协议重载异常: ") + ex.what();
    } catch (...) {
        lua.lastError = "协议重载异常: 未知异常";
    }

    loggingFacade_.error("protocol", lua.lastError);
    lua.docks = scriptHost_.dockSnapshots();
    lua.controls = scriptHost_.controlsSnapshot();
    lua.controlStates = scriptHost_.controlStatesSnapshot();
    return false;
}

bool Application::pumpOnce() {
    bool changed = false;
    changed = handleTransportEvents() || changed;
    scriptHost_.tick(nowMs());
    changed = processRequestTimeouts() || changed;
    changed = flushScriptOutputs() || changed;
    changed = flushScriptLogs() || changed;
    changed = flushScriptPlots() || changed;
    changed = flushPendingTransferFrameRows(transferFrameRowsPerPump()) || changed;
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
    cancelAllTxRequests("连接重新打开");
    pendingTransportEvents_.clear();
    pendingRxByteChunks_.clear();
    const auto kind = dockStore_.commState().kind;
    transport_ = createTransport(kind);
    if (!transport_) {
        dockStore_.commState().lastError = "创建 transport 失败";
        loggingFacade_.error("transport", dockStore_.commState().lastError);
        return;
    }

    const bool opened = transport_->open(currentTransportConfig(kind));
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
    cancelAllTxRequests("连接已关闭");
    if (activeConnection_.has_value()) {
        resetStreamBufferAlertState(activeConnection_->connectionId);
    } else {
        resetStreamBufferAlertState();
    }
    std::string recordingError;
    if (rawCaptureRecording_.isOpen() && !stopRawCaptureRecording(recordingError)) {
        loggingFacade_.error("raw_capture", "停止完整原始数据录制失败: " + recordingError);
    }
    if (transport_) {
        transport_->close();
    }
    activeConnection_.reset();
    if (responsiveBacklogMode() && runtimeConfig_.gui.realtimeBacklog.discardBacklogOnDisconnect) {
        const auto counts = clearPendingRealtimeBacklog();
        logRealtimeBacklogDiscard(counts);
    } else {
        detachPendingRealtimeBacklogFromConnection();
    }
    transport_.reset();
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
        appendTransferRow(dock::ReceiveRow{
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

void Application::appendTransferRow(dock::ReceiveRow row) {
    // 核心流程：先用当前行生成逐帧增量，再把原始行移入历史，避免高速收包时额外复制整包字节。
    appendTransferFrameRows(row);
    dockStore_.appendReceiveRow(std::move(row));
}

void Application::appendLiveRawCapture(const transport::TransportBytesEvent& event) {
    auto& wave = dockStore_.waveState();
    const auto& lua = dockStore_.luaState();
    if (!wave.rawCapture.payload.empty()
        && (wave.rawCapture.protocolDir != lua.protocolDir || wave.rawCapture.protocolName != lua.protocolName)) {
        wave.rawCapture = {};
    }
    if (wave.rawCapture.payload.empty()) {
        wave.rawCapture.capturedAtMs = event.context.timestampMs;
    }
    wave.rawCapture.protocolName = lua.protocolName;
    wave.rawCapture.protocolDir = lua.protocolDir;
    wave.rawCapture.sampleFrequencyHz = wave.view.sampleFrequencyHz;
    appendRawCaptureEvent(plot::RawCaptureEvent{
        .type = plot::RawCaptureEventType::RxBytes,
        .timestampMs = event.context.timestampMs,
        .bytes = event.bytes,
        .profile = {},
    });

    const auto limit = runtimeConfig_.gui.rawCapture.liveLimitBytes;
    if (wave.rawCapture.payload.size() <= limit) {
        return;
    }

    // 核心流程：实时接收只保存最近一段原始字节，完整历史应由显式录制或外部文件承载。
    wave.rawCapture.truncated = true;
    if (limit == 0U) {
        wave.rawCapture.payload.clear();
        return;
    }
    const auto removeCount = wave.rawCapture.payload.size() - limit;
    wave.rawCapture.payload.erase(
        wave.rawCapture.payload.begin(),
        wave.rawCapture.payload.begin() + static_cast<std::vector<std::uint8_t>::difference_type>(removeCount));
}

void Application::appendRawCaptureEvent(const plot::RawCaptureEvent& event) {
    auto& rawCapture = dockStore_.waveState().rawCapture;
    rawCapture.events.push_back(event);
    if (event.type == plot::RawCaptureEventType::RxBytes) {
        rawCapture.payload.insert(rawCapture.payload.end(), event.bytes.begin(), event.bytes.end());
    }
}

void Application::appendRawCaptureRecording(const transport::TransportBytesEvent& event) {
    if (!rawCaptureRecording_.isOpen() || event.bytes.empty()) {
        return;
    }

    std::string error;
    if (rawCaptureRecording_.appendEvent(plot::RawCaptureEvent{
            .type = plot::RawCaptureEventType::RxBytes,
            .timestampMs = event.context.timestampMs,
            .bytes = event.bytes,
            .profile = {},
        },
        error)) {
        return;
    }

    const auto path = rawCaptureRecording_.path();
    std::string closeError;
    static_cast<void>(rawCaptureRecording_.close(closeError));
    const auto message = "完整原始数据录制失败: " + error + " (" + path.generic_string() + ")";
    setStatusMessage(message, true);
    loggingFacade_.error("raw_capture", message);
}

std::optional<Application::TransferFrameParserState> Application::makeTransferFrameParserState() const {
    const auto bufferDefinition = scriptHost_.streamBufferDefinition();
    auto frameDefinitions = scriptHost_.streamFrameDefinitions();
    if (!bufferDefinition.has_value() || frameDefinitions.empty()) {
        return std::nullopt;
    }
    auto rxFrames = frameDefinitions;
    auto txFrames = std::move(frameDefinitions);
    return TransferFrameParserState{
        .rx = scripting::FrameStreamParser(*bufferDefinition, std::move(rxFrames)),
        .tx = scripting::FrameStreamParser(*bufferDefinition, std::move(txFrames)),
    };
}

void Application::resetTransferFrameParser() {
    // 核心流程：收发记录视图使用独立 parser，不复用 Lua 回调 parser，避免 UI 展示影响协议运行态。
    transferFrameParser_ = makeTransferFrameParserState();
}

void Application::resetTransferFrameDisplayState() {
    dockStore_.clearTransferFrameRows();
    pendingTransferFrameRows_.clear();
    resetTransferFrameParser();
}

dock::ReceiveRow Application::makeTransferFrameRow(const dock::ReceiveRow& sourceRow,
                                                   const scripting::StreamParsedFrame& frame) const {
    return dock::ReceiveRow{
        .timestampMs = sourceRow.timestampMs,
        .direction = sourceRow.direction,
        .endpoint = sourceRow.endpoint,
        .bytes = frame.raw,
        .message = transferFrameMessage(frame),
    };
}

void Application::appendTransferFrameRows(const dock::ReceiveRow& sourceRow) {
    if (sourceRow.bytes.empty() || (sourceRow.direction != "RX" && sourceRow.direction != "TX")) {
        return;
    }
    if (!transferFrameParser_.has_value()) {
        resetTransferFrameParser();
    }
    if (!transferFrameParser_.has_value()) {
        return;
    }

    auto& parser = sourceRow.direction == "TX" ? transferFrameParser_->tx : transferFrameParser_->rx;
    const auto batch = parser.pushBytes(sourceRow.bytes);
    if (sourceRow.direction == "RX") {
        transport::ConnectionContext context;
        if (activeConnection_.has_value() && activeConnection_->endpoint == sourceRow.endpoint) {
            context = *activeConnection_;
        } else {
            context.endpoint = sourceRow.endpoint;
            context.connectionId = 0;
            context.timestampMs = sourceRow.timestampMs;
            context.readyForIo = false;
        }
        handleStreamBufferAlert(context, batch, parser.bufferDefinition());
    }
    if (batch.frames.empty()) {
        // RX 半包先留在 parser 缓冲中等待后续字节；TX 无匹配时按用户输入的原始 chunk 展示。
        if (sourceRow.direction == "TX" || !batch.errors.empty()) {
            enqueueTransferFrameRows({sourceRow});
        }
        return;
    }
    std::vector<dock::ReceiveRow> frameRows;
    frameRows.reserve(batch.frames.size());
    for (const auto& frame : batch.frames) {
        frameRows.push_back(makeTransferFrameRow(sourceRow, frame));
    }
    enqueueTransferFrameRows(std::move(frameRows));
}

void Application::rebuildTransferFrameRows() {
    const auto rows = dockStore_.receiveState().rows;
    resetTransferFrameDisplayState();
    for (const auto& row : rows) {
        appendTransferFrameRows(row);
    }
    flushPendingTransferFrameRows(std::numeric_limits<std::size_t>::max());
}

void Application::activateParsedTransferLogView() {
    // 核心流程：从 raw 切到 schema 时默认只看切换后的新流，
    // 避免拿已裁剪/未对齐的 raw 历史重放后把 parser 卡在旧半帧里。
    resetTransferFrameDisplayState();
    if (!runtimeConfig_.gui.replayRawHistoryOnSchemaSwitch) {
        return;
    }
    for (const auto& row : dockStore_.receiveState().rows) {
        appendTransferFrameRows(row);
    }
    flushPendingTransferFrameRows(std::numeric_limits<std::size_t>::max());
}

void Application::applyHistoryLimits(const config::GuiLogHistoryConfig& config) {
    dockStore_.setHistoryLimits(dock::DockHistoryLimits{
        .transferRawRows = config.transferRawLimit,
        .transferFrameRows = config.transferFrameLimit,
        .hostLogRows = config.hostLimit,
        .scriptLogRows = config.scriptLimit,
    });
    trimPendingTransferFrameRowsToLimit();
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

bool Application::exportWaveRawCapture(const std::filesystem::path& path, std::string& error) const {
    const auto& lua = dockStore_.luaState();
    const auto& wave = dockStore_.waveState();

    plot::RawCaptureFileData capture = wave.rawCapture;
    capture.protocolName = lua.protocolName.empty() ? capture.protocolName : lua.protocolName;
    capture.protocolDir = lua.protocolDir.empty() ? capture.protocolDir : lua.protocolDir;
    capture.sampleFrequencyHz = wave.view.sampleFrequencyHz;
    if (capture.capturedAtMs == 0) {
        capture.capturedAtMs = nowMs();
    }

    if (capture.protocolName.empty() || capture.protocolDir.empty()) {
        error = "当前协议元数据不完整，无法导出";
        return false;
    }
    // 核心流程：导出按当前 rawCapture 可见状态重建事件流；
    // 实时缓存若已截断，不再把内存里更早的完整历史事件一并带出。
    capture.events.clear();
    if (!capture.payload.empty()) {
        capture.events.push_back(plot::RawCaptureEvent{
            .type = plot::RawCaptureEventType::RxBytes,
            .timestampMs = capture.capturedAtMs,
            .bytes = capture.payload,
            .profile = {},
        });
    }
    return plot::writeRawCaptureFile(path, capture, error);
}

bool Application::startRawCaptureRecording(const std::filesystem::path& path, std::string& error) {
    if (rawCaptureRecording_.isOpen()) {
        error = "已有完整原始数据录制正在进行";
        return false;
    }

    const auto& luaState = dockStore_.luaState();
    const auto& wave = dockStore_.waveState();
    plot::RawCaptureFileData metadata{
        .protocolName = luaState.protocolName,
        .protocolDir = luaState.protocolDir,
        .sampleFrequencyHz = wave.view.sampleFrequencyHz,
        .capturedAtMs = nowMs(),
        .truncated = false,
        .payload = {},
        .events = {},
    };
    if (metadata.protocolName.empty() || metadata.protocolDir.empty()) {
        error = "当前协议元数据不完整，无法开始录制";
        return false;
    }

    if (!rawCaptureRecording_.open(path, metadata, error)) {
        return false;
    }
    setStatusMessage("完整原始数据录制已开始: " + path.generic_string());
    loggingFacade_.info("raw_capture", "完整原始数据录制已开始: " + path.generic_string());
    return true;
}

bool Application::stopRawCaptureRecording(std::string& error) {
    if (!rawCaptureRecording_.isOpen()) {
        return true;
    }

    const auto path = rawCaptureRecording_.path();
    const auto bytesWritten = rawCaptureRecording_.bytesWritten();
    if (!rawCaptureRecording_.close(error)) {
        return false;
    }

    setStatusMessage("完整原始数据录制已停止: " + path.generic_string() + " (" + std::to_string(bytesWritten) + " bytes)");
    loggingFacade_.info("raw_capture", "完整原始数据录制已停止: " + path.generic_string());
    return true;
}

bool Application::isRawCaptureRecording() const {
    return rawCaptureRecording_.isOpen();
}

const std::filesystem::path& Application::rawCaptureRecordingPath() const {
    return rawCaptureRecording_.path();
}

std::uint64_t Application::rawCaptureRecordingBytes() const {
    return rawCaptureRecording_.bytesWritten();
}

bool Application::importWaveRawCapture(const plot::RawCaptureFileData& capture, std::string& error) {
    auto& lua = dockStore_.luaState();
    if (!lua.loaded) {
        error = "当前协议尚未加载";
        return false;
    }
    if (capture.protocolDir.empty() || capture.protocolDir != lua.protocolDir) {
        error = "导入文件协议目录与当前工作区不一致";
        return false;
    }

    // 核心流程：导入回放必须先清空旧波形与旧原始缓冲，再走一次 on_bytes -> flushScriptPlots，
    // 避免导入样本与现场采集样本混在同一份波形/原始容器里。
    resetWaveHistory();
    auto& wave = dockStore_.waveState();
    wave.rawCapture = capture;
    wave.view.sampleFrequencyHz = capture.sampleFrequencyHz;
    wave.view.sampleFrequencyInput = formatFrequencyInput(capture.sampleFrequencyHz);
    wave.view.sampleFrequencyError.clear();
    wave.buffer.setHistoryTrimSuspended(true);

    if (!capture.events.empty()) {
        transport::ConnectionContext replayContext;
        replayContext.endpoint = "psraw-import";
        replayContext.connectionId = 0;
        replayContext.timestampMs = capture.capturedAtMs == 0 ? nowMs() : capture.capturedAtMs;
        replayContext.readyForIo = false;
        suppressRawCaptureProfileEvents_ = true;
        for (const auto& recordedEvent : capture.events) {
            replayContext.timestampMs = recordedEvent.timestampMs == 0 ? replayContext.timestampMs : recordedEvent.timestampMs;
            if (recordedEvent.type == plot::RawCaptureEventType::ProfileSet) {
                sol::state tempLua;
                tempLua.open_libraries(sol::lib::base, sol::lib::table);
                auto table = tempLua.create_table();
                table["frame"] = recordedEvent.profile.frameName;
                table["length"] = static_cast<long long>(recordedEvent.profile.length);
                if (!recordedEvent.profile.channelMap.empty()) {
                    auto mapTable = tempLua.create_table();
                    for (std::size_t index = 0; index < recordedEvent.profile.channelMap.size(); ++index) {
                        mapTable[index + 1] = static_cast<long long>(recordedEvent.profile.channelMap[index] + 1);
                    }
                    table["channel_map"] = mapTable;
                }
                std::string profileError;
                if (!scriptHost_.setStreamRuntimeProfile(sol::make_object(tempLua, table), profileError)) {
                    suppressRawCaptureProfileEvents_ = false;
                    error = "导入 profile_set 失败: " + profileError;
                    wave.buffer.setHistoryTrimSuspended(false);
                    return false;
                }
            } else if (recordedEvent.type == plot::RawCaptureEventType::ProfileClear) {
                sol::state tempLua;
                tempLua.open_libraries(sol::lib::base);
                std::string clearError;
                sol::object frameObject = recordedEvent.profile.frameName.empty()
                    ? sol::make_object(tempLua, sol::lua_nil)
                    : sol::make_object(tempLua, recordedEvent.profile.frameName);
                if (!scriptHost_.clearStreamRuntimeProfile(frameObject, clearError)) {
                    suppressRawCaptureProfileEvents_ = false;
                    error = "导入 profile_clear 失败: " + clearError;
                    wave.buffer.setHistoryTrimSuspended(false);
                    return false;
                }
            } else if (!recordedEvent.bytes.empty()) {
                std::size_t cursor = 0;
                while (cursor < recordedEvent.bytes.size()) {
                    const auto chunkSize = (std::min)(kRawCaptureReplayChunkBytes, recordedEvent.bytes.size() - cursor);
                    std::vector<std::uint8_t> chunk(recordedEvent.bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                                    recordedEvent.bytes.begin() + static_cast<std::ptrdiff_t>(cursor + chunkSize));
                    scriptHost_.onTransportBytes(transport::TransportBytesEvent{replayContext, std::move(chunk)});
                    flushScriptOutputs();
                    flushScriptLogs();
                    flushScriptPlots();
                    flushScriptStatusAndDialogs();
                    cursor += chunkSize;
                }
            }
        }
        flushScriptStatusAndDialogs();
        suppressRawCaptureProfileEvents_ = false;
    } else if (!capture.payload.empty()) {
        transport::ConnectionContext replayContext;
        replayContext.endpoint = "psraw-import";
        replayContext.connectionId = 0;
        replayContext.timestampMs = capture.capturedAtMs == 0 ? nowMs() : capture.capturedAtMs;
        replayContext.readyForIo = false;
        std::size_t cursor = 0;
        while (cursor < capture.payload.size()) {
            const auto chunkSize = (std::min)(kRawCaptureReplayChunkBytes, capture.payload.size() - cursor);
            std::vector<std::uint8_t> chunk(capture.payload.begin() + static_cast<std::ptrdiff_t>(cursor),
                                            capture.payload.begin() + static_cast<std::ptrdiff_t>(cursor + chunkSize));
            scriptHost_.onTransportBytes(transport::TransportBytesEvent{replayContext, std::move(chunk)});
            flushScriptOutputs();
            flushScriptLogs();
            flushScriptPlots();
            flushScriptStatusAndDialogs();
            cursor += chunkSize;
        }
    }

    flushScriptOutputs();
    flushScriptLogs();
    flushScriptPlots();
    flushScriptStatusAndDialogs();
    const auto importedSnapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(),
                                                       std::numeric_limits<double>::infinity());
    std::size_t importedHistoryLimit = 0;
    for (const auto& channel : importedSnapshot.channels) {
        importedHistoryLimit = (std::max)(importedHistoryLimit, channel.totalSamples);
    }
    wave.buffer.setHistoryTrimSuspended(false);
    wave.buffer.preserveHistoryLimitAtLeast(importedHistoryLimit);
    syncDockState();
    wave.statusMessage = "原始波形已导入";
    return true;
}

bool Application::loadElfStaticAddressFile(const std::filesystem::path& path, std::string& error) {
    if (!elfStaticView_.loadFile(path, error)) {
        return false;
    }
    ++elfStaticAddressRevision_;
    setStatusMessage("ELF/ElfStaticView 数据文件已加载: " + elfStaticView_.sourcePath());
    return true;
}

std::uint64_t Application::elfStaticAddressRevision() const {
    return elfStaticAddressRevision_;
}

std::vector<scripting::ElfSymbolValue> Application::queryElfStaticAddresses(const std::string& queryText,
                                                                            std::size_t limit) const {
    std::vector<scripting::ElfSymbolValue> symbols;
    for (const auto& entry : elfStaticView_.query(queryText, limit)) {
        symbols.push_back(scripting::ElfSymbolValue{
            .label = entry.label,
            .value = entry.value,
            .type = entry.type,
        });
    }
    return symbols;
}

void Application::resetWaveHistory() {
    auto& wave = dockStore_.waveState();
    wave.buffer.clear();
    wave.rawCapture = {};
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
    if (!pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty() || !pendingTransferFrameRows_.empty() ||
        scriptHost_.pendingPlotAppendCount() > 0U) {
        return nowMs();
    }
    auto nextWakeup = scriptHost_.nextWakeupAtMs();
    if (activeHalfDuplexRequest_.has_value()) {
        if (!nextWakeup.has_value() || activeHalfDuplexRequest_->waitDeadlineMs < *nextWakeup) {
            nextWakeup = activeHalfDuplexRequest_->waitDeadlineMs;
        }
    }
    return nextWakeup;
}

void Application::setTransportFactoryForTest(std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> factory) {
    transportFactoryForTest_ = std::move(factory);
}

std::unique_ptr<transport::ITransport> Application::createTransport(transport::TransportKind kind) const {
    if (transportFactoryForTest_) {
        return transportFactoryForTest_(kind);
    }
    return transport::createTransport(kind);
}

transport::TransportConfig Application::currentTransportConfig(transport::TransportKind kind) const {
    const auto& comm = dockStore_.commState();
    switch (kind) {
    case transport::TransportKind::TcpClient:
        return comm.tcpClient;
    case transport::TransportKind::TcpServer:
        return comm.tcpServer;
    case transport::TransportKind::Serial:
        return comm.serial;
    case transport::TransportKind::UdpPeer:
        return comm.udpPeer;
    }
    return comm.tcpClient;
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
    comm.pendingRxBytes = pendingRxByteCount();
    comm.pendingTransferFrameRows = pendingTransferFrameRows_.size();
    comm.pendingPlotAppends = scriptHost_.pendingPlotAppendCount();

    auto& lua = dockStore_.luaState();
    lua.docks = scriptHost_.dockSnapshots();
    lua.controls = scriptHost_.controlsSnapshot();
    lua.controlStates = scriptHost_.controlStatesSnapshot();

    auto& wave = dockStore_.waveState();
    const auto waveRevision = wave.buffer.dataRevision();
    if (!cachedWaveSummaryRevision_.has_value() || *cachedWaveSummaryRevision_ != waveRevision) {
        wave.channelSummaries.clear();
        const auto snapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
        for (const auto& channel : snapshot.channels) {
            wave.channelSummaries.push_back(channel.label + " samples=" + std::to_string(channel.totalSamples));
        }
        cachedWaveSummaryRevision_ = waveRevision;
    }
}

bool Application::handleTransportEvents() {
    bool changed = false;
    auto& comm = dockStore_.commState();
    comm.lastPumpEvents = 0;
    comm.lastPumpRxBytes = 0;
    comm.lastPumpStreamFrames = 0;
    comm.lastPumpStreamErrors = 0;
    comm.lastPumpTransportMs = 0.0;
    comm.lastPumpParserMs = 0.0;
    comm.lastPumpCallbackMs = 0.0;
    comm.lastPumpScriptMs = 0.0;
    if (transport_) {
        auto events = transport_->takeEvents();
        pendingTransportEvents_.insert(pendingTransportEvents_.end(),
                                       std::make_move_iterator(events.begin()),
                                       std::make_move_iterator(events.end()));
    }

    const auto startedAt = std::chrono::steady_clock::now();
    std::size_t processed = 0;
    std::size_t processedRxBytes = 0;
    const auto maxRxBytes = rxBytesPerPump();
    while (processed < kTransportEventsPerPump) {
        if (!pendingRxByteChunks_.empty()) {
            const auto remainingRxBudget = maxRxBytes > processedRxBytes ? maxRxBytes - processedRxBytes : 0U;
            if (remainingRxBudget == 0U) {
                break;
            }
            const auto before = pendingRxByteCount();
            changed = processPendingRxBytes(remainingRxBudget) || changed;
            const auto after = pendingRxByteCount();
            processedRxBytes += before >= after ? before - after : 0U;
            ++processed;
            if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) {
                break;
            }
            continue;
        }

        if (pendingTransportEvents_.empty()) {
            break;
        }

        auto event = std::move(pendingTransportEvents_.front());
        pendingTransportEvents_.pop_front();
        if (auto* bytes = std::get_if<transport::TransportBytesEvent>(&event); bytes != nullptr && !bytes->bytes.empty()) {
            enqueuePendingRxBytes(std::move(*bytes));
            continue;
        }

        changed = processTransportEvent(event) || changed;
        ++processed;
        if (processed >= kTransportEventsPerPump) {
            break;
        }
        if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) {
            break;
        }
    }
    comm.lastPumpEvents = processed;
    comm.lastPumpTransportMs = elapsedMilliseconds(startedAt, std::chrono::steady_clock::now());
    return changed || !pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty();
}

bool Application::processPendingRxBytes(const std::size_t maxBytes) {
    if (pendingRxByteChunks_.empty() || maxBytes == 0U) {
        return false;
    }

    auto& pending = pendingRxByteChunks_.front();
    const auto remaining = pending.bytes.size() - pending.offset;
    const auto chunkSize = (std::min)(remaining, maxBytes);
    transport::TransportBytesEvent chunk{
        .context = pending.context,
        .bytes = std::vector<std::uint8_t>(pending.bytes.begin() + static_cast<std::ptrdiff_t>(pending.offset),
                                           pending.bytes.begin() + static_cast<std::ptrdiff_t>(pending.offset + chunkSize)),
    };

    // 核心流程：大 RX 事件拆成小块喂给脚本和 UI，避免单次 pump 长时间占住主线程。
    const bool changed = processTransportEvent(chunk);
    pending.offset += chunkSize;
    if (pending.offset >= pending.bytes.size()) {
        pendingRxByteChunks_.pop_front();
    }
    return changed;
}

void Application::enqueuePendingRxBytes(transport::TransportBytesEvent event) {
    pendingRxByteChunks_.push_back(PendingRxBytes{
        .context = event.context,
        .bytes = std::move(event.bytes),
        .offset = 0,
    });
}

void Application::detachPendingRealtimeBacklogFromConnection() {
    for (auto& pending : pendingRxByteChunks_) {
        pending.context.readyForIo = false;
    }

    std::deque<PendingRxBytes> detachedRxBytes;
    while (!pendingTransportEvents_.empty()) {
        auto event = std::move(pendingTransportEvents_.front());
        pendingTransportEvents_.pop_front();
        if (auto* bytes = std::get_if<transport::TransportBytesEvent>(&event); bytes != nullptr && !bytes->bytes.empty()) {
            bytes->context.readyForIo = false;
            detachedRxBytes.push_back(PendingRxBytes{
                .context = bytes->context,
                .bytes = std::move(bytes->bytes),
                .offset = 0,
            });
        }
    }
    pendingRxByteChunks_.insert(pendingRxByteChunks_.end(),
                                std::make_move_iterator(detachedRxBytes.begin()),
                                std::make_move_iterator(detachedRxBytes.end()));
}

Application::RealtimeBacklogDiscardCounts Application::clearPendingRealtimeBacklog() {
    RealtimeBacklogDiscardCounts counts{
        .transportEvents = pendingTransportEvents_.size(),
        .rxBytes = pendingRxByteCount(),
        .transferFrameRows = pendingTransferFrameRows_.size(),
    };
    pendingTransportEvents_.clear();
    pendingRxByteChunks_.clear();
    pendingTransferFrameRows_.clear();

    const auto scriptCounts = scriptHost_.clearPendingRealtimeOutputs();
    counts.plotAppends = scriptCounts.plotAppends;
    counts.scriptLogs = scriptCounts.logs;
    counts.scriptEvents = scriptCounts.events;
    return counts;
}

void Application::logRealtimeBacklogDiscard(const RealtimeBacklogDiscardCounts& counts) {
    const auto total = counts.transportEvents + counts.rxBytes + counts.transferFrameRows + counts.plotAppends +
                       counts.scriptLogs + counts.scriptEvents;
    if (total == 0U) {
        return;
    }

    std::ostringstream message;
    message << "断开时已丢弃实时 UI backlog: transport_events=" << counts.transportEvents
            << ", rx_bytes=" << counts.rxBytes
            << ", transfer_frame_rows=" << counts.transferFrameRows
            << ", plot_appends=" << counts.plotAppends
            << ", script_logs=" << counts.scriptLogs
            << ", script_events=" << counts.scriptEvents;
    loggingFacade_.host(config::LogLevel::Warn, "BACKLOG_DROP", "realtime", message.str(), nowMs());
}

bool Application::responsiveBacklogMode() const {
    return runtimeConfig_.gui.realtimeBacklog.mode != "complete";
}

std::size_t Application::rxBytesPerPump() const {
    return (std::max<std::size_t>)(runtimeConfig_.gui.realtimeBacklog.rxChunkBytesPerPump, 1U);
}

std::size_t Application::transferFrameRowsPerPump() const {
    return (std::max<std::size_t>)(runtimeConfig_.gui.realtimeBacklog.transferFrameRowsPerPump, 1U);
}

std::size_t Application::plotAppendsPerPump() const {
    return (std::max<std::size_t>)(runtimeConfig_.gui.realtimeBacklog.plotAppendsPerPump, 1U);
}

std::size_t Application::pendingRxByteCount() const {
    std::size_t total = 0;
    for (const auto& pending : pendingRxByteChunks_) {
        total += pending.bytes.size() - pending.offset;
    }
    for (const auto& event : pendingTransportEvents_) {
        if (const auto* bytes = std::get_if<transport::TransportBytesEvent>(&event); bytes != nullptr) {
            total += bytes->bytes.size();
        }
    }
    return total;
}

bool Application::processTransportEvent(const transport::TransportEvent& event) {
    bool changed = false;
    std::visit(
        [this, &changed]<typename T0>(const T0& evt) {
            using T = std::decay_t<T0>;
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
                resetStreamBufferAlertState(evt.context.connectionId);
                if (activeConnection_.has_value() && activeConnection_->connectionId == evt.context.connectionId) {
                    activeConnection_.reset();
                }
                cancelAllTxRequests(evt.reason.empty() ? "连接已关闭" : evt.reason);
                std::string recordingError;
                if (rawCaptureRecording_.isOpen() && !stopRawCaptureRecording(recordingError)) {
                    loggingFacade_.error("raw_capture", "停止完整原始数据录制失败: " + recordingError);
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
                    appendRawCaptureRecording(evt);
                    appendLiveRawCapture(evt);
                    scriptHost_.onTransportBytes(evt);
                    const auto& stats = scriptHost_.lastTransportStats();
                    auto& comm = dockStore_.commState();
                    comm.lastPumpRxBytes += stats.bytes;
                    comm.lastPumpStreamFrames += stats.streamFrames;
                    comm.lastPumpStreamErrors += stats.streamErrors;
                    comm.lastPumpParserMs += stats.parserMs;
                    comm.lastPumpCallbackMs += stats.callbackMs;
                    comm.lastPumpScriptMs += stats.totalMs;
                    if (evt.context.readyForIo) {
                        activeConnection_ = evt.context;
                    }
                    appendTransferRow(dock::ReceiveRow{
                        .timestampMs = evt.context.timestampMs,
                        .direction = "RX",
                        .endpoint = evt.context.endpoint,
                        .bytes = evt.bytes,
                        .message = {},
                    });
                    changed = true;
                }
            } else if constexpr (std::is_same_v<T, transport::TransportTxEvent>) {
                if (!activeWrite_.has_value() || activeWrite_->request.id != evt.requestId) {
                    return;
                }

                auto activeWrite = *activeWrite_;
                activeWrite_.reset();
                const auto state = toScriptTxState(evt.state);
                const std::optional<std::string> error = evt.error.empty() ? std::nullopt : std::optional<std::string>{evt.error};
                if (state == scripting::TxEventState::Sent && activeWrite.request.kind == scripting::TxRequestKind::Request) {
                    activeWrite.sentAtMs = evt.finishedAtMs;
                    activeWrite.waitDeadlineMs = evt.finishedAtMs + activeWrite.request.timeoutMs;
                    activeHalfDuplexRequest_ = activeWrite;
                    scriptHost_.setRequestAwaitingCompletion(true);
                    // 核心流程：先进入等待 ACK 状态再回调 on_tx，允许脚本在 sent 事件中立即调用 proto.request_done。
                    scriptHost_.onTxEvent(activeWrite.request.connection,
                                          scripting::TxEvent{
                                              .id = activeWrite.request.id,
                                              .kind = activeWrite.request.kind,
                                              .state = scripting::TxEventState::Sent,
                                              .tag = activeWrite.request.tag,
                                              .bytes = evt.bytes,
                                              .queuedMs = activeWrite.request.createdAtMs,
                                              .finishedMs = evt.finishedAtMs,
                                              .error = error,
                                          });
                    appendTransferRow(dock::ReceiveRow{
                        .timestampMs = evt.finishedAtMs,
                        .direction = "TX",
                        .endpoint = activeWrite.request.connection.endpoint,
                        .bytes = activeWrite.request.payload,
                        .message = {},
                    });
                    changed = true;
                    return;
                }

                finishTxRequest(activeWrite.request, state, error, evt.finishedAtMs);
                changed = true;
                changed = driveTxScheduler() || changed;
            }
        },
        event);
    return changed;
}

bool Application::flushScriptOutputs() {
    bool changed = false;
    for (auto& request : scriptHost_.drainTxRequests()) {
        enqueueTxRequest(std::move(request));
        changed = true;
    }
    changed = processScriptRequestCompletions() || changed;
    changed = flushScriptStatusAndDialogs() || changed;
    changed = driveTxScheduler() || changed;

    for (const auto& event : scriptHost_.drainEvents()) {
        dockStore_.appendLuaEvent(event);
        changed = true;
    }
    return changed;
}

bool Application::flushScriptStatusAndDialogs() {
    bool changed = false;
    for (const auto& profileEvent : scriptHost_.drainStreamRuntimeProfileEvents()) {
        const auto timestampMs = activeConnection_.has_value() ? activeConnection_->timestampMs : nowMs();
        const plot::RawCaptureEvent event{
            .type = profileEvent.cleared ? plot::RawCaptureEventType::ProfileClear : plot::RawCaptureEventType::ProfileSet,
            .timestampMs = timestampMs,
            .bytes = {},
            .profile = plot::RawCaptureProfileEventData{
                .frameName = profileEvent.frameName,
                .length = profileEvent.length,
                .channelMap = profileEvent.channelMap,
            },
        };
        if (!suppressRawCaptureProfileEvents_) {
            appendRawCaptureEvent(event);
        }
        if (rawCaptureRecording_.isOpen()) {
            std::string error;
            if (!rawCaptureRecording_.appendEvent(event, error)) {
                loggingFacade_.error("raw_capture", "录制 stream profile 事件失败: " + error);
            }
        }
        changed = true;
    }
    for (const auto& update : scriptHost_.drainStatusUpdates()) {
        setStatusMessage(update.clear ? std::string{} : update.text, false);
        changed = true;
    }
    for (const auto& request : scriptHost_.drainDialogRequests()) {
        enqueueDialogRequest(request);
        changed = true;
    }
    for (const auto& request : scriptHost_.drainFileDialogRequests()) {
        pendingFileDialogs_.push_back(request);
        openFileDialogs_[request.id] = request;
        changed = true;
    }
    return changed;
}

bool Application::processScriptRequestCompletions() {
    bool changed = false;
    bool completionConsumed = false;
    for (const auto& result : scriptHost_.drainRequestDoneResults()) {
        if (completionConsumed) {
            loggingFacade_.warn("protocol", "同一轮收到多次 request_done，后续结果已忽略");
            changed = true;
            continue;
        }
        if (!activeHalfDuplexRequest_.has_value()) {
            loggingFacade_.warn("protocol", "收到 request_done，但当前没有活动 request");
            changed = true;
            continue;
        }
        const auto activeRequest = activeHalfDuplexRequest_->request;
        activeHalfDuplexRequest_.reset();
        scriptHost_.setRequestAwaitingCompletion(false);
        if (result.ok && !result.message.empty() && dockStore_.commState().lastError == result.message) {
            dockStore_.commState().lastError.clear();
        }
        finishTxRequest(activeRequest,
                        result.ok ? scripting::TxEventState::Completed : scripting::TxEventState::Failed,
                        (!result.ok && !result.message.empty()) ? std::optional<std::string>{result.message} : std::nullopt,
                        result.timestampMs);
        completionConsumed = true;
        changed = true;
        changed = driveTxScheduler() || changed;
    }
    return changed;
}

bool Application::processRequestTimeouts() {
    if (!activeHalfDuplexRequest_.has_value()) {
        return false;
    }
    const auto currentMs = nowMs();
    if (currentMs < activeHalfDuplexRequest_->waitDeadlineMs) {
        return false;
    }

    const auto request = activeHalfDuplexRequest_->request;
    activeHalfDuplexRequest_.reset();
    scriptHost_.setRequestAwaitingCompletion(false);
    finishTxRequest(request, scripting::TxEventState::Timeout, std::string("等待 request_done 超时"), currentMs);
    driveTxScheduler();
    return true;
}

bool Application::driveTxScheduler() {
    bool changed = false;
    while (!activeWrite_.has_value() && !activeHalfDuplexRequest_.has_value() && !pendingTxQueue_.empty()) {
        if (!transport_ || transport_->state() != transport::TransportState::Open) {
            auto request = std::move(pendingTxQueue_.front());
            pendingTxQueue_.pop_front();
            finishTxRequest(request, scripting::TxEventState::Rejected, std::string("连接未打开"), nowMs());
            changed = true;
            continue;
        }

        ActiveTxRequest active{};
        active.request = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();

        transport::TransportTxTask task{};
        task.requestId = active.request.id;
        task.kind = toTransportTxKind(active.request.kind);
        task.payload = active.request.payload;
        task.timeoutMs = active.request.timeoutMs;
        task.queuedAtMs = active.request.createdAtMs;
        if (!transport_->enqueueSend(std::move(task))) {
            finishTxRequest(active.request, scripting::TxEventState::Rejected, std::string("transport enqueueSend 失败"), nowMs());
            changed = true;
            continue;
        }

        activeWrite_ = std::move(active);
        changed = true;
    }
    return changed;
}

bool Application::enqueueTxRequest(scripting::TxRequest request) {
    request.timeoutMs = request.timeoutMs > 0
        ? request.timeoutMs
        : (request.kind == scripting::TxRequestKind::Request
               ? runtimeConfig_.protocol.tx.requestTimeoutMs
               : runtimeConfig_.protocol.tx.sendTimeoutMs);

    const std::size_t activeCount = pendingTxQueue_.size()
        + (activeWrite_.has_value() ? 1U : 0U)
        + (activeHalfDuplexRequest_.has_value() ? 1U : 0U);
    if (activeCount < runtimeConfig_.protocol.tx.maxPending) {
        pendingTxQueue_.push_back(std::move(request));
        return true;
    }

    const std::string overflowMessage = "发送队列已满";
    if (runtimeConfig_.protocol.tx.overflowPolicy == "drop_oldest_waiting" && !pendingTxQueue_.empty()) {
        auto dropped = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();
        finishTxRequest(dropped, scripting::TxEventState::Dropped, overflowMessage, nowMs());
        pendingTxQueue_.push_back(std::move(request));
        notifyTxOverflow(overflowMessage);
        return true;
    }

    if (runtimeConfig_.protocol.tx.overflowPolicy == "drop_newest") {
        finishTxRequest(request, scripting::TxEventState::Dropped, overflowMessage, nowMs());
        notifyTxOverflow(overflowMessage);
        return true;
    }

    finishTxRequest(request, scripting::TxEventState::Rejected, overflowMessage, nowMs());
    notifyTxOverflow(overflowMessage);
    return true;
}

void Application::finishTxRequest(const scripting::TxRequest& request,
                                  scripting::TxEventState state,
                                  std::optional<std::string> error,
                                  std::uint64_t finishedAtMs) {
    if (state == scripting::TxEventState::Canceled && activeHalfDuplexRequest_.has_value()
        && activeHalfDuplexRequest_->request.id == request.id) {
        scriptHost_.setRequestAwaitingCompletion(false);
    }

    if (state == scripting::TxEventState::Sent) {
        appendTransferRow(dock::ReceiveRow{
            .timestampMs = finishedAtMs,
            .direction = "TX",
            .endpoint = request.connection.endpoint,
            .bytes = request.payload,
            .message = {},
        });
    }
    if (error.has_value()) {
        dockStore_.commState().lastError = *error;
        loggingFacade_.warn("protocol", *error);
    }

    scriptHost_.onTxEvent(request.connection,
                          scripting::TxEvent{
                              .id = request.id,
                              .kind = request.kind,
                              .state = state,
                              .tag = request.tag,
                              .bytes = request.payload.size(),
                              .queuedMs = request.createdAtMs,
                              .finishedMs = finishedAtMs,
                              .fileJobId = request.fileJobId,
                              .offset = request.fileOffset,
                              .total = request.fileTotal,
                              .progress = request.fileTotal == 0
                                  ? 0.0
                                  : static_cast<double>(request.fileOffset + request.payload.size()) / static_cast<double>(request.fileTotal),
                              .error = std::move(error),
                          });
}

void Application::cancelAllTxRequests(const std::string& reason) {
    const auto finishedAtMs = nowMs();
    if (activeWrite_.has_value()) {
        finishTxRequest(activeWrite_->request, scripting::TxEventState::Canceled, reason, finishedAtMs);
        activeWrite_.reset();
    }
    if (activeHalfDuplexRequest_.has_value()) {
        scriptHost_.setRequestAwaitingCompletion(false);
        finishTxRequest(activeHalfDuplexRequest_->request, scripting::TxEventState::Canceled, reason, finishedAtMs);
        activeHalfDuplexRequest_.reset();
    }
    while (!pendingTxQueue_.empty()) {
        auto request = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();
        finishTxRequest(request, scripting::TxEventState::Canceled, reason, finishedAtMs);
    }
}

void Application::notifyTxOverflow(const std::string& message) {
    setStatusMessage(message, false);
    loggingFacade_.warn("protocol", message);
    if (runtimeConfig_.protocol.tx.overflowNotify == "popup_once") {
        const auto createdAtMs = static_cast<std::uint64_t>(nowMs());
        transport::ConnectionContext connection{};
        if (activeConnection_.has_value()) {
            connection = *activeConnection_;
        }
        const scripting::DialogRequest request{
            .id = createdAtMs,
            .kind = scripting::DialogKind::Alert,
            .connection = connection,
            .title = "发送队列已满",
            .message = message,
            .level = "warn",
            .dedupeKey = "protocol.tx.overflow",
            .createdAtMs = createdAtMs,
        };
        enqueueDialogRequest(request);
    }
}

void Application::handleStreamBufferAlert(const transport::ConnectionContext& context,
                                          const scripting::StreamParseBatch& batch,
                                          const scripting::StreamBufferDefinition& bufferDefinition) {
    if (batch.bufferCapacity == 0 || batch.overflowed) {
        return;
    }

    const double thresholdRatio = runtimeConfig_.receive.streamBuffer.nearOverflowThreshold > 0.0
        ? runtimeConfig_.receive.streamBuffer.nearOverflowThreshold
        : bufferDefinition.nearOverflowThresholdRatio;
    const bool nearOverflow = static_cast<double>(batch.bufferSize) / static_cast<double>(batch.bufferCapacity) >= thresholdRatio;
    if (!nearOverflow) {
        return;
    }

    if (streamBufferAlertState_.connectionId != context.connectionId) {
        streamBufferAlertState_.connectionId = context.connectionId;
        streamBufferAlertState_.popupMuted = false;
        streamBufferAlertState_.popupOpen = false;
    }

    std::ostringstream status;
    status << "协议解析缓冲区占用过高: " << batch.bufferSize << "/" << batch.bufferCapacity << " bytes";
    setStatusMessage(status.str(), true);

    std::ostringstream logMessage;
    logMessage << status.str()
               << ", threshold=" << static_cast<int>(thresholdRatio * 100.0)
               << "%, endpoint=" << context.endpoint;
    loggingFacade_.warn("stream_buffer", logMessage.str());

    if (!bufferDefinition.popupEnabled || !runtimeConfig_.receive.streamBuffer.popupEnabled ||
        streamBufferAlertState_.popupMuted || streamBufferAlertState_.popupOpen) {
        return;
    }

    const auto createdAtMs = static_cast<std::uint64_t>(nowMs());
    const std::string thresholdText = std::to_string(static_cast<int>(thresholdRatio * 100.0));
    const scripting::DialogRequest request{
        .id = createdAtMs,
        .kind = scripting::DialogKind::Alert,
        .connection = context,
        .title = "协议解析缓冲区占用过高",
        .message = "当前连接 " + context.endpoint + " 的协议解析缓冲区已达到 " + std::to_string(batch.bufferSize) + "/" +
            std::to_string(batch.bufferCapacity) + " bytes，告警阈值 " + thresholdText + "%。",
        .level = "warn",
        .dedupeKey = "receive.stream_buffer.near_overflow",
        .createdAtMs = createdAtMs,
    };
    enqueueDialogRequest(request);
    streamBufferAlertState_.popupOpen = true;
}

void Application::resetStreamBufferAlertState(const std::uint64_t connectionId) {
    if (connectionId != 0 && streamBufferAlertState_.connectionId != connectionId) {
        return;
    }
    streamBufferAlertState_ = StreamBufferAlertState{};
}

void Application::enqueueDialogRequest(const scripting::DialogRequest& request) {
    if (!request.dedupeKey.empty() && dialogDedupeKeys_.contains(request.dedupeKey)) {
        return;
    }
    pendingDialogs_.push_back(request);
    openDialogs_[request.id] = request;
    if (!request.dedupeKey.empty()) {
        dialogDedupeKeys_[request.dedupeKey] = request.id;
    }
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

void Application::enqueueTransferFrameRows(std::vector<dock::ReceiveRow> rows) {
    if (rows.empty()) {
        return;
    }
    for (auto& row : rows) {
        pendingTransferFrameRows_.push_back(std::move(row));
    }
    trimPendingTransferFrameRowsToLimit();
}

void Application::trimPendingTransferFrameRowsToLimit() {
    const auto limit = runtimeConfig_.gui.logHistory.transferFrameLimit;
    if (limit == 0U) {
        pendingTransferFrameRows_.clear();
        return;
    }
    const auto displayed = dockStore_.receiveState().frameRows.size();
    if (displayed + pendingTransferFrameRows_.size() <= limit) {
        return;
    }
    auto excess = displayed + pendingTransferFrameRows_.size() - limit;
    while (excess > 0 && !pendingTransferFrameRows_.empty()) {
        pendingTransferFrameRows_.pop_front();
        --excess;
    }
}

bool Application::flushPendingTransferFrameRows(std::size_t maxRows) {
    if (pendingTransferFrameRows_.empty() || maxRows == 0) {
        return false;
    }
    std::vector<dock::ReceiveRow> rows;
    rows.reserve((std::min)(maxRows, pendingTransferFrameRows_.size()));
    while (!pendingTransferFrameRows_.empty() && rows.size() < maxRows) {
        rows.push_back(std::move(pendingTransferFrameRows_.front()));
        pendingTransferFrameRows_.pop_front();
    }
    dockStore_.appendTransferFrameRows(std::move(rows));
    trimPendingTransferFrameRowsToLimit();
    return true;
}

logging::LoggingFacade& Application::logger() {
    return loggingFacade_;
}

const logging::LoggingFacade& Application::logger() const {
    return loggingFacade_;
}

std::vector<scripting::DialogRequest> Application::drainDialogRequests() {
    std::vector<scripting::DialogRequest> drained;
    drained.reserve(pendingDialogs_.size());
    while (!pendingDialogs_.empty()) {
        drained.push_back(std::move(pendingDialogs_.front()));
        pendingDialogs_.pop_front();
    }
    return drained;
}

void Application::respondDialog(const scripting::DialogEvent& event) {
    const auto iter = openDialogs_.find(event.id);
    if (iter == openDialogs_.end()) {
        return;
    }
    const auto request = iter->second;
    if (request.dedupeKey == "receive.stream_buffer.near_overflow") {
        streamBufferAlertState_.popupOpen = false;
        if (event.state == "mute_until_disconnect") {
            streamBufferAlertState_.popupMuted = true;
            streamBufferAlertState_.connectionId = request.connection.connectionId;
        }
    }
    if (!request.dedupeKey.empty()) {
        dialogDedupeKeys_.erase(request.dedupeKey);
    }
    openDialogs_.erase(iter);
    scriptHost_.onDialogEvent(request.connection, event);
}

std::vector<scripting::FileDialogRequest> Application::drainFileDialogRequests() {
    std::vector<scripting::FileDialogRequest> drained;
    drained.reserve(pendingFileDialogs_.size());
    while (!pendingFileDialogs_.empty()) {
        drained.push_back(std::move(pendingFileDialogs_.front()));
        pendingFileDialogs_.pop_front();
    }
    return drained;
}

void Application::respondFileDialog(const scripting::FileDialogEvent& event) {
    const auto iter = openFileDialogs_.find(event.id);
    if (iter == openFileDialogs_.end()) {
        return;
    }
    const auto request = iter->second;
    openFileDialogs_.erase(iter);
    scriptHost_.onFileDialogEvent(request.connection, event);
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
        const bool channelIdentityChanged = !sameChannelIdentity(setup.channels, wave.buffer);

        if (setup.resetHistory) {
            wave.buffer.clear();
        }
        auto viewConfig = setup.view;
        viewConfig.displayFormula = wave.view.displayFormula;
        wave.buffer.setViewConfig(viewConfig);
        wave.buffer.configureChannels(setup.channels.size());
        wave.defaultChannelSpecs.clear();
        wave.defaultChannelSpecs.reserve(setup.channels.size());
        const bool shouldResetOverrides = setup.resetHistory || channelIdentityChanged;
        if (shouldResetOverrides) {
            wave.channelOverrides.clear();
        }
        wave.channelOverrides.resize(setup.channels.size());
        for (std::size_t index = 0; index < setup.channels.size(); ++index) {
            const auto defaultSpec = plot::ChannelSpec{
                .label = setup.channels[index].label,
                .unit = setup.channels[index].unit,
                .ratio = setup.channels[index].ratio,
                .scale = setup.channels[index].scale,
                .offset = setup.channels[index].offset,
                .color = setup.channels[index].color,
            };
            auto effectiveSpec = defaultSpec;
            if (!shouldResetOverrides && index < wave.channelOverrides.size()) {
                const auto& overrideState = wave.channelOverrides[index];
                if (overrideState.labelOverridden) {
                    effectiveSpec.label = overrideState.label;
                }
                if (overrideState.ratioOverridden) {
                    effectiveSpec.ratio = overrideState.ratio;
                }
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

    auto appendRequests = scriptHost_.drainPlotAppends(plotAppendsPerPump());
    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> mergedRequests;
    std::unordered_map<std::string, std::size_t> mergedIndexes;
    mergedRequests.reserve(appendRequests.size());
    for (auto& [channelIndex, request] : appendRequests) {
        if (request.samples.empty()) {
            continue;
        }
        auto key = std::to_string(channelIndex);
        key.push_back('\x1F');
        key.append(request.source);
        const auto [position, inserted] = mergedIndexes.emplace(key, mergedRequests.size());
        if (inserted) {
            mergedRequests.emplace_back(channelIndex, std::move(request));
            continue;
        }
        auto& targetSamples = mergedRequests[position->second].second.samples;
        targetSamples.insert(targetSamples.end(),
                             std::make_move_iterator(request.samples.begin()),
                             std::make_move_iterator(request.samples.end()));
    }

    for (auto& [channelIndex, request] : mergedRequests) {
        if (wave.buffer.append(channelIndex, std::move(request))) {
            changed = true;
        }
    }

    return changed;
}

} // namespace protoscope::app
