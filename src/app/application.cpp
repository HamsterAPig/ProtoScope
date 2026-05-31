#include "protoscope/app/application.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <type_traits>

namespace protoscope::app {

namespace {

constexpr std::size_t kRawCaptureReplayChunkBytes = 1024;

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
    captured.gui.elfSymbolCombo = runtimeConfig_.gui.elfSymbolCombo;
    captured.gui.sendHistoryLimit = runtimeConfig_.gui.sendHistoryLimit;
    captured.gui.luaDockLayoutDebug = runtimeConfig_.gui.luaDockLayoutDebug;
    captured.app.language = runtimeConfig_.app.language;
    captured.scripting = runtimeConfig_.scripting;
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

    cancelAllTxRequests("协议已重新加载");

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
    rebuildTransferFrameRows();
    loggingFacade_.info("protocol", "协议已加载: " + resolvedDirText);
    flushScriptLogs();
    syncDockState();
    return true;
}

bool Application::pumpOnce() {
    bool changed = false;
    changed = handleTransportEvents() || changed;
    scriptHost_.tick(nowMs());
    changed = processRequestTimeouts() || changed;
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
    cancelAllTxRequests("连接重新打开");
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
    if (transport_) {
        transport_->close();
    }
    activeConnection_.reset();
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
    wave.rawCapture.payload.insert(wave.rawCapture.payload.end(), event.bytes.begin(), event.bytes.end());

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
    if (batch.frames.empty()) {
        // RX 半包先留在 parser 缓冲中等待后续字节；TX 无匹配时按用户输入的原始 chunk 展示。
        if (sourceRow.direction == "TX" || !batch.errors.empty()) {
            dockStore_.appendTransferFrameRows({sourceRow});
        }
        return;
    }
    std::vector<dock::ReceiveRow> frameRows;
    frameRows.reserve(batch.frames.size());
    for (const auto& frame : batch.frames) {
        frameRows.push_back(makeTransferFrameRow(sourceRow, frame));
    }
    dockStore_.appendTransferFrameRows(std::move(frameRows));
}

void Application::rebuildTransferFrameRows() {
    const auto rows = dockStore_.receiveState().rows;
    dockStore_.clearTransferFrameRows();
    resetTransferFrameParser();
    for (const auto& row : rows) {
        appendTransferFrameRows(row);
    }
}

void Application::applyHistoryLimits(const config::GuiLogHistoryConfig& config) {
    dockStore_.setHistoryLimits(dock::DockHistoryLimits{
        .transferRawRows = config.transferRawLimit,
        .transferFrameRows = config.transferFrameLimit,
        .hostLogRows = config.hostLimit,
        .scriptLogRows = config.scriptLimit,
    });
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
    return plot::writeRawCaptureFile(path, capture, error);
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

    if (!capture.payload.empty()) {
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
            // 核心流程：导入回放按小块喂给 Lua stream parser，避免一次性写满环形缓冲区后只剩尾部数据。
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
                    if (activeConnection_.has_value() && activeConnection_->connectionId == evt.context.connectionId) {
                        activeConnection_.reset();
                    }
                    cancelAllTxRequests(evt.reason.empty() ? "连接已关闭" : evt.reason);
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
                        appendLiveRawCapture(evt);
                        scriptHost_.onTransportBytes(evt);
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
                    const std::optional<std::string> error = evt.error.empty() ? std::nullopt
                                                                               : std::optional<std::string>{evt.error};
                    if (state == scripting::TxEventState::Sent && activeWrite.request.kind == scripting::TxRequestKind::Request) {
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
                        activeWrite.sentAtMs = evt.finishedAtMs;
                        activeWrite.waitDeadlineMs = evt.finishedAtMs + activeWrite.request.timeoutMs;
                        activeHalfDuplexRequest_ = activeWrite;
                        scriptHost_.setRequestAwaitingCompletion(true);
                        changed = true;
                        return;
                    }

                    finishTxRequest(activeWrite.request, state, error, evt.finishedAtMs);
                    changed = true;
                    changed = driveTxScheduler() || changed;
                }
            },
            event);
    }
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
        finishTxRequest(activeRequest,
                        scripting::TxEventState::Completed,
                        result.message.empty() ? std::nullopt : std::optional<std::string>{result.message},
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

    for (auto& [channelIndex, request] : scriptHost_.drainPlotAppends()) {
        if (wave.buffer.append(channelIndex, std::move(request))) {
            changed = true;
        }
    }

    return changed;
}

} // namespace protoscope::app
