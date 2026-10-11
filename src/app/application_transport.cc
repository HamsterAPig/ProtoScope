#include "protoscope/app/application.hpp"
#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace protoscope::app {

namespace {

    constexpr std::size_t kTransportEventsPerPump = 256;
    constexpr auto kTransportEventBudget = std::chrono::milliseconds(4);
    constexpr std::uint64_t kCommPressureDebugLogIntervalMs = 2000;

    double elapsedMilliseconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
    {
        return std::chrono::duration<double, std::milli>(end - start).count();
    }

    const char* stateMessage(transport::TransportState state)
    {
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

    const char* txRequestKindName(scripting::TxRequestKind kind)
    {
        switch (kind) {
            case scripting::TxRequestKind::Send:
                return "send";
            case scripting::TxRequestKind::Request:
                return "request";
        }
        return "send";
    }

    const char* txEventStateName(scripting::TxEventState state)
    {
        switch (state) {
            case scripting::TxEventState::Sent:
                return "sent";
            case scripting::TxEventState::Completed:
                return "completed";
            case scripting::TxEventState::Failed:
                return "failed";
            case scripting::TxEventState::Timeout:
                return "timeout";
            case scripting::TxEventState::Rejected:
                return "rejected";
            case scripting::TxEventState::Dropped:
                return "dropped";
            case scripting::TxEventState::Canceled:
                return "canceled";
        }
        return "rejected";
    }

    void appendLogField(std::ostringstream& out, std::string_view name, std::string_view value)
    {
        if (value.empty()) {
            return;
        }
        out << ' ' << name << '=' << value;
    }

    void appendLogField(std::ostringstream& out, std::string_view name, std::uint64_t value)
    {
        out << ' ' << name << '=' << value;
    }

    std::string txRequestLogMessage(std::string_view state,
                                    const scripting::TxRequest& request,
                                    std::size_t queueSize,
                                    std::optional<std::string_view> error = std::nullopt,
                                    std::uint64_t timestampMs = 0)
    {
        std::ostringstream out;
        appendLogField(out, "kind", txRequestKindName(request.kind));
        appendLogField(out, "state", state);
        appendLogField(out, "endpoint", request.connection.endpoint);
        appendLogField(out, "connection_id", request.connection.connectionId);
        appendLogField(out, "request_id", request.id);
        appendLogField(out, "tag", request.tag);
        appendLogField(out, "attempt", request.attempt);
        out << " bytes=" << request.payload.size();
        out << " queue_size=" << queueSize;
        if (timestampMs != 0U) {
            const auto elapsedMs = timestampMs >= request.createdAtMs ? timestampMs - request.createdAtMs : 0U;
            out << " elapsed_ms=" << elapsedMs;
        }
        if (error.has_value() && !error->empty()) {
            appendLogField(out, "error", *error);
        }
        return out.str();
    }

    std::string transferFrameFieldValueText(const scripting::StreamFieldValue& value)
    {
        return std::visit(
            [](const auto& stored) -> std::string {
                using ValueType = std::decay_t<decltype(stored)>;
                std::ostringstream builder;
                if constexpr (std::is_same_v<ValueType, std::vector<std::uint8_t>>) {
                    builder << protocol_utils::bytesToHex(stored, true);
                } else if constexpr (std::is_same_v<ValueType, std::vector<std::int64_t>> ||
                                     std::is_same_v<ValueType, std::vector<double>>) {
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

    std::string transferFrameMessage(const scripting::StreamParsedFrame& frame)
    {
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

    transport::TransportTxKind toTransportTxKind(scripting::TxRequestKind kind)
    {
        switch (kind) {
            case scripting::TxRequestKind::Send:
                return transport::TransportTxKind::Send;
            case scripting::TxRequestKind::Request:
                return transport::TransportTxKind::Request;
        }
        return transport::TransportTxKind::Send;
    }

    scripting::TxEventState toScriptTxState(transport::TransportTxState state)
    {
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

    bool isGuardedTerminalFailure(const scripting::TxRequest& request, scripting::TxEventState state)
    {
        if (!request.guarded) {
            return false;
        }
        switch (state) {
            case scripting::TxEventState::Failed:
            case scripting::TxEventState::Rejected:
            case scripting::TxEventState::Dropped:
                return true;
            case scripting::TxEventState::Timeout:
                return request.attempt >= request.maxAttempts;
            default:
                return false;
        }
    }

    std::optional<std::string> guardStateForEvent(const scripting::TxRequest& request, scripting::TxEventState state)
    {
        if (!request.guarded) {
            return std::nullopt;
        }
        if (state == scripting::TxEventState::Timeout && request.attempt < request.maxAttempts) {
            return std::string("retrying");
        }
        if (isGuardedTerminalFailure(request, state)) {
            return std::string("halted");
        }
        return std::string("active");
    }

    dock::RequestTraceKind toRequestTraceKind(scripting::TxRequestKind kind)
    {
        switch (kind) {
            case scripting::TxRequestKind::Send:
                return dock::RequestTraceKind::Send;
            case scripting::TxRequestKind::Request:
                return dock::RequestTraceKind::Request;
        }
        return dock::RequestTraceKind::Send;
    }

    dock::RequestTraceState toRequestTraceState(scripting::TxEventState state)
    {
        switch (state) {
            case scripting::TxEventState::Sent:
                return dock::RequestTraceState::Sent;
            case scripting::TxEventState::Completed:
                return dock::RequestTraceState::Completed;
            case scripting::TxEventState::Failed:
                return dock::RequestTraceState::Failed;
            case scripting::TxEventState::Timeout:
                return dock::RequestTraceState::Timeout;
            case scripting::TxEventState::Rejected:
                return dock::RequestTraceState::Rejected;
            case scripting::TxEventState::Dropped:
                return dock::RequestTraceState::Dropped;
            case scripting::TxEventState::Canceled:
                return dock::RequestTraceState::Canceled;
        }
        return dock::RequestTraceState::Rejected;
    }

    std::string guardStateForTrace(const scripting::TxRequest& request, dock::RequestTraceState state)
    {
        if (!request.guarded) {
            return {};
        }
        if (state == dock::RequestTraceState::Queued) {
            return "queued";
        }
        const auto txState = [&]() -> std::optional<scripting::TxEventState> {
            switch (state) {
                case dock::RequestTraceState::Sent:
                    return scripting::TxEventState::Sent;
                case dock::RequestTraceState::Completed:
                    return scripting::TxEventState::Completed;
                case dock::RequestTraceState::Failed:
                    return scripting::TxEventState::Failed;
                case dock::RequestTraceState::Timeout:
                    return scripting::TxEventState::Timeout;
                case dock::RequestTraceState::Rejected:
                    return scripting::TxEventState::Rejected;
                case dock::RequestTraceState::Dropped:
                    return scripting::TxEventState::Dropped;
                case dock::RequestTraceState::Canceled:
                    return scripting::TxEventState::Canceled;
                default:
                    return std::nullopt;
            }
        }();
        if (!txState.has_value()) {
            return {};
        }
        const auto guardState = guardStateForEvent(request, *txState);
        return guardState.value_or(std::string{});
    }

    dock::RequestTraceRow makeRequestTraceRow(const scripting::TxRequest& request,
                                              dock::RequestTraceState state,
                                              const std::optional<std::string>& error,
                                              std::uint64_t timestampMs)
    {
        return dock::RequestTraceRow{
            .timestampMs = timestampMs,
            .id = request.id,
            .kind = toRequestTraceKind(request.kind),
            .state = state,
            .endpoint = request.connection.endpoint,
            .tag = request.tag,
            .bytes = request.payload.size(),
            .queuedMs = request.createdAtMs,
            .finishedMs = timestampMs,
            .timeoutMs = request.timeoutMs,
            .durationMs = timestampMs >= request.createdAtMs ? timestampMs - request.createdAtMs : 0,
            .guarded = request.guarded,
            .attempt = request.attempt,
            .maxAttempts = request.maxAttempts,
            .guardState = guardStateForTrace(request, state),
            .error = error.value_or(std::string{}),
        };
    }

} // namespace

std::string Application::payloadPreviewSuffix(const config::AppLoggingConfig& logging, const std::vector<std::uint8_t>& payload)
{
    if (!logging.payloadPreview.enabled || payload.empty()) {
        return {};
    }
    std::ostringstream out;
    const auto maxBytes = logging.payloadPreview.maxBytes == 0U ? payload.size() : logging.payloadPreview.maxBytes;
    out << " payload_hex="
        << protocol_utils::bytesToHex(
               std::vector<std::uint8_t>(
                   payload.begin(),
                   payload.begin() + static_cast<std::ptrdiff_t>((std::min)(payload.size(), maxBytes))),
               true);
    if (payload.size() > maxBytes) {
        out << "...";
    }
    return out.str();
}

std::string Application::transportContextFields(std::string_view kind, const transport::ConnectionContext& context)
{
    std::ostringstream out;
    appendLogField(out, "kind", kind);
    appendLogField(out, "endpoint", context.endpoint);
    appendLogField(out, "connection_id", context.connectionId);
    return out.str();
}

void Application::openTransport()
{
    if (dataImportActive_) {
        dockStore_.commState().lastError = "数据导入期间不能连接设备";
        return;
    }
    if (rawCaptureReplay_.loaded) {
        dockStore_.commState().lastError = "原始回放时间轴已载入，请先卸载后再打开实时连接";
        loggingFacade_.warn("transport", dockStore_.commState().lastError);
        syncDockState();
        return;
    }
    switchToLiveWaveSource();
    if (replayReceiveHistory_) {
        // 核心流程：离线回放可能停在半帧，重新连接前必须丢弃回放 parser 状态，
        // 但保留当前收发记录供用户继续查看。
        replayReceiveHistory_ = false;
        dockStore_.waveState().rawCapture = {};
        retainedRawBytes_ = 0;
        pendingTransferFrameRows_.clear();
        resetTransferFrameParser();
    }
    cancelAllTxRequests("连接重新打开");
    pendingTransportEvents_.clear();
    pendingRxByteChunks_.clear();
    const auto kind = dockStore_.commState().kind;
    activeConnection_.reset();
    transport_ = createTransport(kind);
    if (!transport_) {
        dockStore_.commState().lastError = "创建 transport 失败";
        loggingFacade_.error("transport",
                             "transport create failed kind=" + std::string(transport::transportKindId(kind)) +
                                 " error=" + dockStore_.commState().lastError);
        return;
    }

    loggingFacade_.info("transport", "transport open requested kind=" + std::string(transport::transportKindId(kind)));
    const bool opened = transport_->open(currentTransportConfig(kind));
    if (!opened) {
        dockStore_.commState().lastError = "打开连接失败";
        loggingFacade_.error("transport",
                             "transport open rejected kind=" + std::string(transport::transportKindId(kind)) +
                                 " error=" + dockStore_.commState().lastError);
    } else {
        dockStore_.commState().lastError.clear();
        dockStore_.commState().reconnectRequired = false;
        loggingFacade_.info("transport",
                            "transport open submitted kind=" + std::string(transport::transportKindId(kind)));
    }

    syncDockState();
}

void Application::closeTransport()
{
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
        loggingFacade_.info(
            "transport",
            "transport close requested kind=" + std::string(transport::transportKindId(dockStore_.commState().kind)));
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

bool Application::sendManualPayload(const std::string& payload, bool hexMode)
{
    if (dataImportActive_ || rawCaptureReplay_.loaded || replayReceiveHistory_) return false;
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

void Application::appendTransferRow(dock::ReceiveRow row)
{
    // 离线重放始终同步生成独立逐帧缓存；视图按钮只选择 rows/frameRows，不能触发 Lua 或重建语义。
    if (replayReceiveHistory_ || dockStore_.receiveState().displayMode == dock::TransferLogDisplayMode::ParsedFrames) {
        appendTransferFrameRows(row);
    }
    dockStore_.appendReceiveRow(std::move(row));
}

std::optional<Application::TransferFrameParserState> Application::makeTransferFrameParserState() const
{
    return makeTransferFrameParserState(scriptWorker_.snapshot());
}

std::optional<Application::TransferFrameParserState> Application::makeTransferFrameParserState(
    const scripting::ScriptRuntimeSnapshot& snapshot) const
{
    const auto bufferDefinition = snapshot.streamBuffer;
    auto frameDefinitions = snapshot.streamFrames;
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

void Application::resetTransferFrameParser()
{
    // 核心流程：收发记录视图使用独立 parser，不复用 Lua 回调 parser，避免 UI 展示影响协议运行态。
    transferFrameParser_ = makeTransferFrameParserState();
}

void Application::resetTransferFrameDisplayState()
{
    dockStore_.clearTransferFrameRows();
    pendingTransferFrameRows_.clear();
    resetTransferFrameParser();
}

dock::ReceiveRow Application::makeTransferFrameRow(const dock::ReceiveRow& sourceRow,
                                                   const scripting::StreamParsedFrame& frame) const
{
    return dock::ReceiveRow{
        .timestampMs = sourceRow.timestampMs,
        .direction = sourceRow.direction,
        .endpoint = sourceRow.endpoint,
        .bytes = frame.raw,
        .message = transferFrameMessage(frame),
    };
}

void Application::appendTransferFrameRows(const dock::ReceiveRow& sourceRow)
{
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

void Application::rebuildTransferFrameRows()
{
    const auto rows = dockStore_.receiveState().rows;
    resetTransferFrameDisplayState();
    for (const auto& row : rows) {
        appendTransferFrameRows(row);
    }
    flushPendingTransferFrameRows(std::numeric_limits<std::size_t>::max());
}

void Application::activateParsedTransferLogView()
{
    // 视图切换不调用 Lua；兼容开关仅用独立 UI parser 重建 rows -> frameRows。
    if (!runtimeConfig_.gui.replayRawHistoryOnSchemaSwitch || replayReceiveHistory_) return;
    rebuildTransferFrameRows();
}

void Application::applyHistoryLimits(const config::GuiLogHistoryConfig& config)
{
    dockStore_.setHistoryLimits(dock::DockHistoryLimits{
        .transferRawRows = config.transferRawLimit,
        .transferFrameRows = config.transferFrameLimit,
        .hostLogRows = config.hostLimit,
        .scriptLogRows = config.scriptLimit,
        .requestTraceRows = config.requestTraceLimit,
    });
    trimPendingTransferFrameRowsToLimit();
}

void Application::markCommConfigEdited(bool reconnectRequired)
{
    dockStore_.commState().reconnectRequired = reconnectRequired;
    dockStore_.markDirty(reconnectRequired ? "通讯配置已修改，需重新连接" : "通讯配置已修改");
}

bool Application::setSendHexMode(bool enabled)
{
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

bool Application::applyTransferFrameRuntimeProfileEvent(const scripting::StreamRuntimeProfileEvent& event,
                                                        std::string& error)
{
    if (!transferFrameParser_.has_value()) {
        resetTransferFrameParser();
    }
    if (!transferFrameParser_.has_value()) {
        return true;
    }

    const auto applyToParser = [&](scripting::FrameStreamParser& parser) {
        if (event.cleared) {
            if (event.frameName.empty()) {
                parser.clearRuntimeProfiles();
                return true;
            }
            return parser.clearRuntimeProfile(std::optional<std::string>{event.frameName}, error);
        }
        return parser.setRuntimeProfile(event.frameName,
                                        scripting::StreamRuntimeProfile{
                                            .length = event.length,
                                            .channelMap = event.channelMap,
                                        },
                                        error);
    };

    return applyToParser(transferFrameParser_->rx) && applyToParser(transferFrameParser_->tx);
}

void Application::setTransportFactoryForTest(
    std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> factory)
{
    transportFactoryForTest_ = std::move(factory);
}

std::unique_ptr<transport::ITransport> Application::createTransport(transport::TransportKind kind) const
{
    if (transportFactoryForTest_) {
        return transportFactoryForTest_(kind);
    }
    return transport::createTransport(kind);
}

transport::TransportConfig Application::currentTransportConfig(transport::TransportKind kind) const
{
    const auto& comm = dockStore_.commState();
    switch (kind) {
        case transport::TransportKind::TcpClient:
            return [&] {
                auto config = comm.tcpClient;
                config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
                return config;
            }();
        case transport::TransportKind::TcpServer:
            return [&] {
                auto config = comm.tcpServer;
                config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
                return config;
            }();
        case transport::TransportKind::Serial:
            return [&] {
                auto config = comm.serial;
                config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
                return config;
            }();
        case transport::TransportKind::UdpPeer:
            return [&] {
                auto config = comm.udpPeer;
                config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
                return config;
            }();
    }
    auto config = comm.tcpClient;
    config.readBufferBytes = runtimeConfig_.receive.transportReadBufferBytes;
    return config;
}

void Application::maybeLogCommPressureDebug(const dock::CommDockState& comm)
{
    if (loggingFacade_.currentConfig().level != config::LogLevel::Debug) {
        return;
    }

    CommPressureDebugSnapshot snapshot{
        .pendingRxBytes = comm.pendingRxBytes,
        .pendingTransferFrameRows = comm.pendingTransferFrameRows,
        .pendingPlotAppends = comm.pendingPlotAppends,
        .rxInputQueueBytes = comm.rxInputQueueBytes,
        .parserPendingBytes = comm.parserPendingBytes,
        .postprocessPendingBatches = comm.postprocessPendingBatches,
        .luaPendingItems = comm.luaPendingItems,
        .uiPendingItems = comm.uiPendingItems,
        .postprocessWorkerThreads = comm.postprocessWorkerThreads,
        .backlogWarning = comm.backlogWarning,
        .lastPumpEvents = comm.lastPumpEvents,
        .lastPumpRxBytes = comm.lastPumpRxBytes,
        .lastPumpStreamFrames = comm.lastPumpStreamFrames,
        .lastPumpStreamErrors = comm.lastPumpStreamErrors,
        .lastPumpTransportMs = comm.lastPumpTransportMs,
        .lastPumpParserMs = comm.lastPumpParserMs,
        .lastPumpCallbackMs = comm.lastPumpCallbackMs,
        .lastPumpScriptMs = comm.lastPumpScriptMs,
    };

    const auto hasPressure = [](const CommPressureDebugSnapshot& value) {
        return value.pendingRxBytes > 0U || value.pendingTransferFrameRows > 0U || value.pendingPlotAppends > 0U ||
               value.rxInputQueueBytes > 0U || value.parserPendingBytes > 0U || value.postprocessPendingBatches > 0U ||
               value.luaPendingItems > 0U || value.uiPendingItems > 0U || !value.backlogWarning.empty();
    };
    const auto pressureValuesChanged = [](const CommPressureDebugSnapshot& current,
                                          const CommPressureDebugSnapshot& previous) {
        return current.pendingRxBytes != previous.pendingRxBytes ||
               current.pendingTransferFrameRows != previous.pendingTransferFrameRows ||
               current.pendingPlotAppends != previous.pendingPlotAppends ||
               current.rxInputQueueBytes != previous.rxInputQueueBytes ||
               current.parserPendingBytes != previous.parserPendingBytes ||
               current.postprocessPendingBatches != previous.postprocessPendingBatches ||
               current.luaPendingItems != previous.luaPendingItems ||
               current.uiPendingItems != previous.uiPendingItems ||
               current.postprocessWorkerThreads != previous.postprocessWorkerThreads ||
               current.backlogWarning != previous.backlogWarning;
    };

    const bool active = hasPressure(snapshot);
    const bool previousActive = commPressureDebugLog_.hasSnapshot && hasPressure(commPressureDebugLog_.lastSnapshot);
    if (!active && !previousActive) {
        return;
    }

    const auto now = nowMs();
    const bool changed =
        !commPressureDebugLog_.hasSnapshot || pressureValuesChanged(snapshot, commPressureDebugLog_.lastSnapshot);
    const bool intervalElapsed = commPressureDebugLog_.lastLogMs == 0U ||
                                 now - commPressureDebugLog_.lastLogMs >= kCommPressureDebugLogIntervalMs;
    if (!changed && !intervalElapsed) {
        return;
    }

    // 调试日志节流：压力指标仍保留在状态结构中，但默认不再直接占用主界面。
    std::ostringstream message;
    message << std::fixed << std::setprecision(2);
    message << "通讯压力: rx_backlog=" << snapshot.pendingRxBytes << " bytes"
            << ", parser_backlog=" << snapshot.parserPendingBytes << " bytes"
            << ", frame_rows=" << snapshot.pendingTransferFrameRows << ", plot_appends=" << snapshot.pendingPlotAppends
            << ", rx_input=" << snapshot.rxInputQueueBytes << " bytes"
            << ", post_batches=" << snapshot.postprocessPendingBatches << ", lua_pending=" << snapshot.luaPendingItems
            << ", ui_pending=" << snapshot.uiPendingItems << ", post_threads=" << snapshot.postprocessWorkerThreads
            << ", last_pump_events=" << snapshot.lastPumpEvents << ", last_pump_rx=" << snapshot.lastPumpRxBytes
            << " bytes" << ", last_stream_frames=" << snapshot.lastPumpStreamFrames
            << ", last_stream_errors=" << snapshot.lastPumpStreamErrors << ", pump_ms=" << snapshot.lastPumpTransportMs
            << ", parser_ms=" << snapshot.lastPumpParserMs << ", lua_callback_ms=" << snapshot.lastPumpCallbackMs
            << ", script_ms=" << snapshot.lastPumpScriptMs
            << ", warning=" << (snapshot.backlogWarning.empty() ? "none" : snapshot.backlogWarning);
    loggingFacade_.debug("comm_pressure", message.str());

    commPressureDebugLog_.hasSnapshot = true;
    commPressureDebugLog_.lastSnapshot = std::move(snapshot);
    commPressureDebugLog_.lastLogMs = now;
}

bool Application::handleTransportEvents()
{
    resetTransportPumpMetrics();
    pullTransportEventsFromTransport();

    const auto startedAt = std::chrono::steady_clock::now();
    bool changed = false;
    std::size_t processed = 0;
    std::size_t processedRxBytes = 0;
    const auto maxRxBytes = rxBytesPerPump();
    changed = drainTransportEventQueues(startedAt, maxRxBytes, processed, processedRxBytes) || changed;
    auto& comm = dockStore_.commState();
    comm.lastPumpEvents = processed;
    comm.lastPumpTransportMs = elapsedMilliseconds(startedAt, std::chrono::steady_clock::now());
    return changed || !pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty();
}

void Application::resetTransportPumpMetrics()
{
    auto& comm = dockStore_.commState();
    comm.lastPumpEvents = 0;
    comm.lastPumpRxBytes = 0;
    comm.lastPumpStreamFrames = 0;
    comm.lastPumpStreamErrors = 0;
    comm.lastPumpTransportMs = 0.0;
    comm.lastPumpParserMs = 0.0;
    comm.lastPumpCallbackMs = 0.0;
    comm.lastPumpScriptMs = 0.0;
}

void Application::pullTransportEventsFromTransport()
{
    if (!transport_) return;
    auto events = transport_->takeEvents();
    pendingTransportEvents_.insert(
        pendingTransportEvents_.end(), std::make_move_iterator(events.begin()), std::make_move_iterator(events.end()));
}

bool Application::drainTransportEventQueues(const std::chrono::steady_clock::time_point& startedAt,
                                            const std::size_t maxRxBytes,
                                            std::size_t& processedEvents,
                                            std::size_t& processedRxBytes)
{
    bool changed = false;
    while (processedEvents < kTransportEventsPerPump) {
        if (!pendingRxByteChunks_.empty()) {
            const auto remainingRxBudget = maxRxBytes > processedRxBytes ? maxRxBytes - processedRxBytes : 0U;
            if (remainingRxBudget == 0U) break;
            const auto before = pendingRxByteCount();
            changed = processPendingRxBytes(remainingRxBudget) || changed;
            const auto after = pendingRxByteCount();
            processedRxBytes += before >= after ? before - after : 0U;
            if (before == after) break;
            ++processedEvents;
            if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) break;
            continue;
        }

        if (pendingTransportEvents_.empty()) {
            break;
        }

        if (auto* bytes = std::get_if<transport::TransportBytesEvent>(&pendingTransportEvents_.front());
            bytes != nullptr) {
            // 空RX不需要进入worker；必须消费该事件，避免它卡住后续ACK及超时drain。
            if (bytes->bytes.empty()) {
                pendingTransportEvents_.pop_front();
                ++processedEvents;
                if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) break;
                continue;
            }
            if (runtimeConfig_.scripting.workerEnabled && bytes->context.readyForIo) {
                if (!activeConnection_.has_value() ||
                    activeConnection_->connectionId != bytes->context.connectionId) {
                    pendingTransportEvents_.pop_front();
                    ++processedEvents;
                    if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) break;
                    continue;
                }
                const auto accepted = tryProcessLiveRxBytes(*bytes, maxRxBytes - processedRxBytes);
                if (accepted == 0U) break;
                processedRxBytes += accepted;
                if (accepted == bytes->bytes.size()) pendingTransportEvents_.pop_front();
                else bytes->bytes.erase(bytes->bytes.begin(),
                                        bytes->bytes.begin() + static_cast<std::ptrdiff_t>(accepted));
                ++processedEvents;
                if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) break;
                continue;
            }
            auto pending = std::move(*bytes);
            pendingTransportEvents_.pop_front();
            if (!pending.bytes.empty()) enqueuePendingRxBytes(std::move(pending));
            continue;
        }
        auto event = std::move(pendingTransportEvents_.front());
        pendingTransportEvents_.pop_front();
        changed = processTransportEvent(event) || changed;
        if (!std::holds_alternative<transport::TransportBytesEvent>(event)) {
            changed = flushScriptOutputs() || changed;
        }
        ++processedEvents;
        if (processedEvents >= kTransportEventsPerPump) {
            break;
        }
        if (std::chrono::steady_clock::now() - startedAt >= kTransportEventBudget) {
            break;
        }
    }
    return changed;
}

bool Application::processPendingRxBytes(const std::size_t maxBytes)
{
    if (pendingRxByteChunks_.empty() || maxBytes == 0U) {
        return false;
    }

    auto& pending = pendingRxByteChunks_.front();
    const auto remaining = pending.bytes.size() - pending.offset;
    const auto chunkSize = (std::min)(remaining, maxBytes);
    transport::TransportBytesEvent chunk{
        .context = pending.context,
        .bytes =
            std::vector<std::uint8_t>(pending.bytes.begin() + static_cast<std::ptrdiff_t>(pending.offset),
                                      pending.bytes.begin() + static_cast<std::ptrdiff_t>(pending.offset + chunkSize)),
    };

    // 脚本 worker 的容量检查与接收原子完成；拒绝时pending offset不前进，录制/日志也不重复。
    const bool asyncWorker = runtimeConfig_.scripting.workerEnabled;
    const auto acceptedBytes = asyncWorker && pending.context.readyForIo
                                   ? tryProcessLiveRxBytes(chunk, chunkSize)
                                   : chunkSize;
    if (acceptedBytes == 0U) return false;
    const bool changed = asyncWorker && pending.context.readyForIo ? true : processTransportEvent(chunk);
    pending.offset += acceptedBytes;
    if (pending.offset >= pending.bytes.size()) pendingRxByteChunks_.pop_front();
    return changed;
}

void Application::enqueuePendingRxBytes(transport::TransportBytesEvent event)
{
    pendingRxByteChunks_.push_back(PendingRxBytes{
        .context = event.context,
        .bytes = std::move(event.bytes),
        .offset = 0,
    });
}

void Application::detachPendingRealtimeBacklogFromConnection()
{
    for (auto& pending : pendingRxByteChunks_) {
        pending.context.readyForIo = false;
    }

    std::deque<PendingRxBytes> detachedRxBytes;
    while (!pendingTransportEvents_.empty()) {
        auto event = std::move(pendingTransportEvents_.front());
        pendingTransportEvents_.pop_front();
        if (auto* bytes = std::get_if<transport::TransportBytesEvent>(&event);
            bytes != nullptr && !bytes->bytes.empty()) {
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

Application::RealtimeBacklogDiscardCounts Application::clearPendingRealtimeBacklog()
{
    RealtimeBacklogDiscardCounts counts{
        .transportEvents = pendingTransportEvents_.size(),
        .rxBytes = pendingRxByteCount(),
        .transferFrameRows = pendingTransferFrameRows_.size(),
    };
    pendingTransportEvents_.clear();
    pendingRxByteChunks_.clear();
    pendingTransferFrameRows_.clear();
    counts.plotAppends += pendingScriptPlotAppends_.size();
    pendingScriptPlotAppends_.clear();

    const auto scriptCounts = scriptWorker_.clearPendingRealtimeOutputs();
    counts.plotAppends += scriptCounts.plotAppends;
    counts.scriptLogs = scriptCounts.logs;
    counts.scriptEvents = scriptCounts.events;
    return counts;
}

void Application::logRealtimeBacklogDiscard(const RealtimeBacklogDiscardCounts& counts)
{
    const auto total = counts.transportEvents + counts.rxBytes + counts.transferFrameRows + counts.plotAppends +
                       counts.scriptLogs + counts.scriptEvents;
    if (total == 0U) {
        return;
    }

    std::ostringstream message;
    message << "断开时已丢弃实时 UI backlog: transport_events=" << counts.transportEvents
            << ", rx_bytes=" << counts.rxBytes << ", transfer_frame_rows=" << counts.transferFrameRows
            << ", plot_appends=" << counts.plotAppends << ", script_logs=" << counts.scriptLogs
            << ", script_events=" << counts.scriptEvents;
    loggingFacade_.host(config::LogLevel::Warn, "BACKLOG_DROP", "realtime", message.str(), nowMs());
}

std::size_t Application::pendingRxByteCount() const
{
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

bool Application::hasPendingRequestDrainWork() const
{
    const auto snapshot = scriptWorker_.snapshot();
    return !pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty() || scriptWorker_.pendingRxBytes() > 0U ||
           snapshot.inputQueueSize > 0U || snapshot.outputQueueSize > 0U || !scriptWorker_.idle();
}

bool Application::drainRequestTimeoutBacklog()
{
    if (!hasPendingRequestDrainWork()) {
        return false;
    }

    // 核心流程：超时边界有未处理RX时先非阻塞推进transport，不因worker反向等待UI drain。
    // 下一轮pump继续处理保留输入；只有完全无backlog时才允许下面判定超时。
    static_cast<void>(handleTransportEvents());
    return true;
}

bool Application::processTransportEvent(const transport::TransportEvent& event)
{
    return std::visit(
        [this]<typename T0>(const T0& evt) {
            using T = std::decay_t<T0>;
            if constexpr (std::is_same_v<T, transport::TransportOpenEvent>) {
                return processTransportOpenEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportCloseEvent>) {
                return processTransportCloseEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportErrorEvent>) {
                return processTransportErrorEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportBytesEvent>) {
                return processTransportBytesEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportTxEvent>) {
                return processTransportTxEvent(evt);
            } else if constexpr (std::is_same_v<T, transport::TransportWriteEvent>) {
                plot::RawCaptureEvent raw{
                    .type = plot::RawCaptureEventType::TxBytes,
                    .timestampMs = evt.context.timestampMs,
                    .bytes = evt.bytes,
                    .endpoint = evt.context.endpoint,
                    .sequence = rawEventSequence_ + 1,
                    .writeStatus = evt.status,
                };
                appendRawCaptureEvent(raw);
                if (evt.status != "sent") {
                    appendTransferRow({.timestampMs = evt.context.timestampMs, .direction = "TX",
                        .endpoint = evt.context.endpoint, .bytes = evt.bytes, .message = evt.status});
                }
                return true;
            }
            return false;
        },
        event);
}

bool Application::processTransportOpenEvent(const transport::TransportOpenEvent& event)
{
    if (event.context.readyForIo) {
        activeConnection_ = event.context;
        scriptWorker_.postTransportOpen(event);
    }
    loggingFacade_.host(config::LogLevel::Info,
                        "OPEN",
                        event.context.endpoint,
                        "transport open" + transportContextFields("transport_open", event.context) +
                            " state=" + stateMessage(dockStore_.commState().state),
                        event.context.timestampMs);
    return true;
}

bool Application::processTransportCloseEvent(const transport::TransportCloseEvent& event)
{
    if (event.context.readyForIo) {
        scriptWorker_.postTransportClose(event);
    }
    loggingFacade_.host(
        config::LogLevel::Info,
        "CLOSE",
        event.context.endpoint,
        "transport close" + transportContextFields("transport_close", event.context) + " reason=" + event.reason,
        event.context.timestampMs);
    resetStreamBufferAlertState(event.context.connectionId);
    if (activeConnection_.has_value() && activeConnection_->connectionId == event.context.connectionId) {
        activeConnection_.reset();
    }
    cancelAllTxRequests(event.reason.empty() ? "连接已关闭" : event.reason);
    std::string recordingError;
    if (rawCaptureRecording_.isOpen() && !stopRawCaptureRecording(recordingError)) {
        loggingFacade_.error("raw_capture", "停止完整原始数据录制失败: " + recordingError);
    }
    return true;
}

bool Application::processTransportErrorEvent(const transport::TransportErrorEvent& event)
{
    if (event.context.readyForIo) {
        scriptWorker_.postTransportError(event);
    }
    loggingFacade_.host(
        config::LogLevel::Error,
        "ERROR",
        event.context.endpoint,
        "transport error" + transportContextFields("transport_error", event.context) + " error=" + event.message,
        event.context.timestampMs);
    dockStore_.commState().lastError = event.message;
    return true;
}

std::size_t Application::tryProcessLiveRxBytes(const transport::TransportBytesEvent& event,
                                               const std::size_t maxBytes)
{
    if (event.bytes.empty() || maxBytes == 0U) return 0U;
    const auto receiveLimit = (std::min)(event.bytes.size(), maxBytes);
    if (event.context.readyForIo &&
        (!activeConnection_.has_value() || activeConnection_->connectionId != event.context.connectionId)) {
        // 实时RX必须属于已处理Open事件确认的活动代次；Open前迟到/已断连数据不得取得归属。
        return receiveLimit;
    }
    std::size_t accepted = 0U;
    if (!runtimeConfig_.scripting.workerEnabled) {
        // 同步兼容模式仍采用旧 post 入口，保持按pump预算分块和处理完成后返回的契约。
        accepted = receiveLimit;
    } else {
        accepted = scriptWorker_.tryPostTransportBytes(event, receiveLimit, false);
        if (accepted == 0U) return 0U;
    }
    transport::TransportBytesEvent acceptedEvent{
        .context = event.context,
        .bytes = std::vector<std::uint8_t>(event.bytes.begin(),
                                           event.bytes.begin() + static_cast<std::ptrdiff_t>(accepted))};
    if (!runtimeConfig_.scripting.workerEnabled) {
        scriptWorker_.postTransportBytes(acceptedEvent, false);
    }
    replayReceiveHistory_ = false;
    appendLiveRawCapture(acceptedEvent);
    loggingFacade_.trace("transport",
                         "transport rx" + transportContextFields("transport_rx", acceptedEvent.context) +
                             " bytes=" + std::to_string(acceptedEvent.bytes.size()) +
                             payloadPreviewSuffix(loggingFacade_.currentConfig(), acceptedEvent.bytes));
    if (acceptedEvent.context.readyForIo) activeConnection_ = acceptedEvent.context;
    appendTransferRow(dock::ReceiveRow{.timestampMs = acceptedEvent.context.timestampMs,
                                       .direction = "RX",
                                       .endpoint = acceptedEvent.context.endpoint,
                                       .bytes = std::move(acceptedEvent.bytes),
                                       .message = {}});
    return accepted;
}

bool Application::processTransportBytesEvent(const transport::TransportBytesEvent& event)
{
    if (event.bytes.empty()) return false;
    if (event.context.readyForIo) {
        const auto accepted = tryProcessLiveRxBytes(event, event.bytes.size());
        return accepted == event.bytes.size();
    }
    replayReceiveHistory_ = false;
    appendLiveRawCapture(event);
    scriptWorker_.postTransportBytes(event);
    loggingFacade_.trace("transport",
                         "transport rx" + transportContextFields("transport_rx", event.context) +
                             " bytes=" + std::to_string(event.bytes.size()) +
                             payloadPreviewSuffix(loggingFacade_.currentConfig(), event.bytes));
    appendTransferRow(dock::ReceiveRow{.timestampMs = event.context.timestampMs,
                                       .direction = "RX",
                                       .endpoint = event.context.endpoint,
                                       .bytes = event.bytes,
                                       .message = {}});
    return true;
}

bool Application::processTransportTxEvent(const transport::TransportTxEvent& event)
{
    if (!activeWrite_.has_value() || activeWrite_->request.id != event.requestId) {
        return false;
    }

    auto activeWrite = *activeWrite_;
    activeWrite_.reset();
    const auto state = toScriptTxState(event.state);
    const std::optional<std::string> error =
        event.error.empty() ? std::nullopt : std::optional<std::string>{event.error};
    if (state == scripting::TxEventState::Sent && activeWrite.request.kind == scripting::TxRequestKind::Request) {
        activeWrite.sentAtMs = event.finishedAtMs;
        activeWrite.waitDeadlineMs = event.finishedAtMs + activeWrite.request.timeoutMs;
        activeHalfDuplexRequest_ = activeWrite;
        scriptWorker_.postRequestAwaitingCompletion(true);
        dockStore_.appendRequestTraceRow(
            makeRequestTraceRow(activeWrite.request, dock::RequestTraceState::Sent, error, event.finishedAtMs));
        // 核心流程：先进入等待 ACK 状态再回调 on_tx，允许脚本在 sent 事件中立即调用 proto.request_done。
        scriptWorker_.postTxEvent(
            activeWrite.request.connection,
            scripting::TxEvent{
                .id = activeWrite.request.id,
                .kind = activeWrite.request.kind,
                .state = scripting::TxEventState::Sent,
                .tag = activeWrite.request.tag,
                .bytes = event.bytes,
                .queuedMs = activeWrite.request.createdAtMs,
                .finishedMs = event.finishedAtMs,
                .guarded = activeWrite.request.guarded,
                .attempt = activeWrite.request.attempt,
                .maxAttempts = activeWrite.request.maxAttempts,
                .guardState = guardStateForEvent(activeWrite.request, scripting::TxEventState::Sent),
                .error = error,
            });
        if (!runtimeConfig_.scripting.workerEnabled) {
            while (!scriptWorker_.idle()) {
                flushScriptOutputs();
                std::this_thread::yield();
            }
            flushScriptOutputs();
        }
        loggingFacade_.trace(
            "tx",
            txRequestLogMessage("sent", activeWrite.request, pendingTxQueue_.size(), std::nullopt, event.finishedAtMs) +
                payloadPreviewSuffix(loggingFacade_.currentConfig(), activeWrite.request.payload));
        appendTransferRow(dock::ReceiveRow{
            .timestampMs = event.finishedAtMs,
            .direction = "TX",
            .endpoint = activeWrite.request.connection.endpoint,
            .bytes = activeWrite.request.payload,
            .message = {},
        });
        return true;
    }

    finishTxRequest(activeWrite.request, state, error, event.finishedAtMs);
    bool changed = true;
    changed = driveTxScheduler() || changed;
    return changed;
}

bool Application::applyScriptTransportStats(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    if (batch.transportStats.has_value()) {
        const auto& stats = *batch.transportStats;
        auto& comm = dockStore_.commState();
        comm.lastPumpRxBytes += stats.bytes;
        comm.lastPumpStreamFrames += stats.streamFrames;
        comm.lastPumpStreamErrors += stats.streamErrors;
        comm.lastPumpParserMs += stats.parserMs;
        comm.lastPumpCallbackMs += stats.callbackMs;
        comm.lastPumpScriptMs += stats.totalMs;
        const auto snapshot = scriptWorker_.snapshot();
        if (snapshot.pendingWorkerRxBytes > 0U || snapshot.inputQueueSize > 0U || snapshot.outputQueueSize > 0U) {
            loggingFacade_.trace("worker",
                                 "worker backlog kind=worker_backlog queue_size=" +
                                     std::to_string(snapshot.inputQueueSize + snapshot.outputQueueSize) +
                                     " bytes=" + std::to_string(snapshot.pendingWorkerRxBytes) +
                                     " elapsed_ms=" + std::to_string(static_cast<std::uint64_t>(stats.totalMs)));
        }
        changed = true;
    }
    return changed;
}

bool Application::applyScriptTxOutputs(const scripting::ScriptRuntimeOutputBatch& batch)
{
    // 离线回调的发送意图直接丢弃，不能留在队列里等重新连接后执行。
    if (dataImportActive_ || rawCaptureReplay_.loaded || replayReceiveHistory_) return false;
    bool changed = false;
    for (const auto& connection : batch.requestGuardResets) {
        txRequestGuardHalted_ = false;
        const auto resetAtMs = nowMs();
        loggingFacade_.info("tx", "request guard reset" + transportContextFields("request_guard", connection));
        dockStore_.appendRequestTraceRow(dock::RequestTraceRow{
            .timestampMs = resetAtMs,
            .id = 0,
            .kind = dock::RequestTraceKind::Request,
            .state = dock::RequestTraceState::GuardReset,
            .endpoint = connection.endpoint,
            .tag = "request_guard",
            .queuedMs = resetAtMs,
            .finishedMs = resetAtMs,
            .guarded = true,
            .guardState = "reset",
            .error = {},
        });
        scriptWorker_.postTxEvent(connection,
                                  scripting::TxEvent{
                                      .id = 0,
                                      .kind = scripting::TxRequestKind::Request,
                                      .state = scripting::TxEventState::Completed,
                                      .tag = "request_guard",
                                      .queuedMs = resetAtMs,
                                      .finishedMs = resetAtMs,
                                      .guarded = true,
                                      .guardState = std::string("reset"),
                                  });
        changed = true;
    }

    for (auto request : batch.txRequests) {
        loggingFacade_.trace("tx",
                             txRequestLogMessage("received",
                                                 request,
                                                 pendingTxQueue_.size() + (activeWrite_.has_value() ? 1U : 0U) +
                                                     (activeHalfDuplexRequest_.has_value() ? 1U : 0U)) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));
        enqueueTxRequest(std::move(request));
        changed = true;
    }

    bool completionConsumed = false;
    for (const auto& result : batch.requestDoneResults) {
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
        scriptWorker_.postRequestAwaitingCompletion(false);
        if (result.ok && !result.message.empty() && dockStore_.commState().lastError == result.message) {
            dockStore_.commState().lastError.clear();
        }
        finishTxRequest(
            activeRequest,
            result.ok ? scripting::TxEventState::Completed : scripting::TxEventState::Failed,
            (!result.ok && !result.message.empty()) ? std::optional<std::string>{result.message} : std::nullopt,
            result.timestampMs);
        completionConsumed = true;
        changed = true;
    }
    return changed;
}

bool Application::processScriptRequestCompletions()
{
    return false;
}

bool Application::processRequestTimeouts()
{
    if (!activeHalfDuplexRequest_.has_value()) {
        return false;
    }
    const auto currentMs = nowMs();
    if (currentMs < activeHalfDuplexRequest_->waitDeadlineMs) {
        return false;
    }

    // timeout只需先派发transport/RX backlog；普通tick等worker命令不得压住请求deadline。
    if (!pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty()) {
        static_cast<void>(handleTransportEvents());
        return true;
    }
    // 独立RX执行计数覆盖callback和输出发布，但不把RX字节占用期延长到输出队列消费，
    // 避免改变worker背压水位；已发布ACK输出仍由主线程drain。
    const auto workerSnapshot = scriptWorker_.snapshot();
    if (scriptWorker_.executingRxBytes() > 0U || workerSnapshot.pendingWorkerRxBytes > 0U ||
        workerSnapshot.outputQueueSize > 0U) {
        return true;
    }
    if (!activeHalfDuplexRequest_.has_value()) {
        return true;
    }

    auto request = activeHalfDuplexRequest_->request;
    activeHalfDuplexRequest_.reset();
    scriptWorker_.postRequestAwaitingCompletion(false);
    finishTxRequest(request, scripting::TxEventState::Timeout, std::string("等待 request_done 超时"), currentMs);
    if (request.guarded && request.attempt < request.maxAttempts) {
        // 核心流程：guarded request 的重试只属于当前请求；
        // 超时后把下一次 attempt 放回队首，避免后续请求抢在重试前发送。
        ++request.attempt;
        loggingFacade_.trace(
            "tx",
            txRequestLogMessage("retry_queued", request, pendingTxQueue_.size(), std::nullopt, currentMs) +
                payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));
        dockStore_.appendRequestTraceRow(
            makeRequestTraceRow(request, dock::RequestTraceState::Queued, std::nullopt, currentMs));
        pendingTxQueue_.push_front(std::move(request));
    }
    driveTxScheduler();
    return true;
}

bool Application::driveTxScheduler()
{
    if (dataImportActive_ || rawCaptureReplay_.loaded || replayReceiveHistory_) return false;
    bool changed = false;
    while (!activeWrite_.has_value() && !activeHalfDuplexRequest_.has_value() && !pendingTxQueue_.empty()) {
        if (!transport_ || transport_->state() != transport::TransportState::Open) {
            auto request = std::move(pendingTxQueue_.front());
            pendingTxQueue_.pop_front();
            loggingFacade_.trace(
                "tx",
                txRequestLogMessage(
                    "rejected", request, pendingTxQueue_.size(), std::string_view("连接未打开"), nowMs()));
            finishTxRequest(request, scripting::TxEventState::Rejected, std::string("连接未打开"), nowMs());
            changed = true;
            continue;
        }

        ActiveTxRequest active{};
        active.request = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();

        if (active.request.guarded && txRequestGuardHalted_) {
            loggingFacade_.warn(
                "tx",
                txRequestLogMessage("guard_halted",
                                    active.request,
                                    pendingTxQueue_.size(),
                                    std::string_view("guarded request 已熔断，请先调用 proto.reset_request_guard()"),
                                    nowMs()));
            finishTxRequest(active.request,
                            scripting::TxEventState::Rejected,
                            std::string("guarded request 已熔断，请先调用 proto.reset_request_guard()"),
                            nowMs());
            changed = true;
            continue;
        }

        transport::TransportTxTask task{};
        task.requestId = active.request.id;
        task.kind = toTransportTxKind(active.request.kind);
        task.payload = active.request.payload;
        task.timeoutMs = active.request.timeoutMs;
        task.queuedAtMs = active.request.createdAtMs;
        if (!transport_->enqueueSend(std::move(task))) {
            loggingFacade_.warn("tx",
                                txRequestLogMessage("enqueue_failed",
                                                    active.request,
                                                    pendingTxQueue_.size(),
                                                    std::string_view("transport enqueueSend 失败"),
                                                    nowMs()));
            finishTxRequest(
                active.request, scripting::TxEventState::Rejected, std::string("transport enqueueSend 失败"), nowMs());
            changed = true;
            continue;
        }

        loggingFacade_.trace(
            "tx",
            txRequestLogMessage("sending", active.request, pendingTxQueue_.size(), std::nullopt, nowMs()) +
                payloadPreviewSuffix(loggingFacade_.currentConfig(), active.request.payload));
        activeWrite_ = std::move(active);
        changed = true;
    }
    return changed;
}

bool Application::enqueueTxRequest(scripting::TxRequest request)
{
    request.timeoutMs = request.timeoutMs > 0 ? request.timeoutMs
                                              : (request.kind == scripting::TxRequestKind::Request
                                                     ? runtimeConfig_.protocol.tx.requestTimeoutMs
                                                     : runtimeConfig_.protocol.tx.sendTimeoutMs);
    request.maxAttempts = std::max<std::uint32_t>(1U, request.maxAttempts);
    request.attempt = std::max<std::uint32_t>(1U, request.attempt);

    if (request.guarded && txRequestGuardHalted_) {
        loggingFacade_.warn(
            "tx",
            txRequestLogMessage("guard_halted",
                                request,
                                pendingTxQueue_.size(),
                                std::string_view("guarded request 已熔断，请先调用 proto.reset_request_guard()"),
                                nowMs()));
        finishTxRequest(request,
                        scripting::TxEventState::Rejected,
                        std::string("guarded request 已熔断，请先调用 proto.reset_request_guard()"),
                        nowMs());
        return true;
    }

    const std::size_t activeCount = pendingTxQueue_.size() + (activeWrite_.has_value() ? 1U : 0U) +
                                    (activeHalfDuplexRequest_.has_value() ? 1U : 0U);
    if (activeCount < runtimeConfig_.protocol.tx.maxPending) {
        loggingFacade_.trace("tx",
                             txRequestLogMessage("queued", request, activeCount + 1U, std::nullopt, nowMs()) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));
        dockStore_.appendRequestTraceRow(
            makeRequestTraceRow(request, dock::RequestTraceState::Queued, std::nullopt, nowMs()));
        pendingTxQueue_.push_back(std::move(request));
        return true;
    }

    const std::string overflowMessage = "发送队列已满";
    if (runtimeConfig_.protocol.tx.overflowPolicy == "drop_oldest_waiting" && !pendingTxQueue_.empty()) {
        auto dropped = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();
        loggingFacade_.warn(
            "tx", txRequestLogMessage("dropped", dropped, activeCount, std::string_view(overflowMessage), nowMs()));
        finishTxRequest(dropped, scripting::TxEventState::Dropped, overflowMessage, nowMs());
        loggingFacade_.trace("tx",
                             txRequestLogMessage("queued", request, activeCount, std::nullopt, nowMs()) +
                                 payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));
        dockStore_.appendRequestTraceRow(
            makeRequestTraceRow(request, dock::RequestTraceState::Queued, std::nullopt, nowMs()));
        pendingTxQueue_.push_back(std::move(request));
        notifyTxOverflow(overflowMessage);
        return true;
    }

    if (runtimeConfig_.protocol.tx.overflowPolicy == "drop_newest") {
        loggingFacade_.warn(
            "tx", txRequestLogMessage("dropped", request, activeCount, std::string_view(overflowMessage), nowMs()));
        finishTxRequest(request, scripting::TxEventState::Dropped, overflowMessage, nowMs());
        notifyTxOverflow(overflowMessage);
        return true;
    }

    loggingFacade_.warn(
        "tx", txRequestLogMessage("rejected", request, activeCount, std::string_view(overflowMessage), nowMs()));
    finishTxRequest(request, scripting::TxEventState::Rejected, overflowMessage, nowMs());
    notifyTxOverflow(overflowMessage);
    return true;
}

void Application::cancelPendingTxRequests(const std::string& reason, std::uint64_t finishedAtMs)
{
    while (!pendingTxQueue_.empty()) {
        auto request = std::move(pendingTxQueue_.front());
        pendingTxQueue_.pop_front();
        finishTxRequest(request, scripting::TxEventState::Canceled, reason, finishedAtMs);
    }
}

void Application::finishTxRequest(const scripting::TxRequest& request,
                                  scripting::TxEventState state,
                                  std::optional<std::string> error,
                                  std::uint64_t finishedAtMs)
{
    if (isGuardedTerminalFailure(request, state)) {
        txRequestGuardHalted_ = true;
    }

    if (state == scripting::TxEventState::Canceled && activeHalfDuplexRequest_.has_value() &&
        activeHalfDuplexRequest_->request.id == request.id) {
        scriptWorker_.postRequestAwaitingCompletion(false);
    }

    dockStore_.appendRequestTraceRow(makeRequestTraceRow(request, toRequestTraceState(state), error, finishedAtMs));
    const auto stateText = txEventStateName(state);
    loggingFacade_.trace("tx",
                         txRequestLogMessage(stateText,
                                             request,
                                             pendingTxQueue_.size(),
                                             error.has_value() ? std::optional<std::string_view>{*error} : std::nullopt,
                                             finishedAtMs) +
                             payloadPreviewSuffix(loggingFacade_.currentConfig(), request.payload));

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

    scriptWorker_.postTxEvent(
        request.connection,
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
            .progress = request.fileTotal == 0 ? 0.0
                                               : static_cast<double>(request.fileOffset + request.payload.size()) /
                                                     static_cast<double>(request.fileTotal),
            .guarded = request.guarded,
            .attempt = request.attempt,
            .maxAttempts = request.maxAttempts,
            .guardState = guardStateForEvent(request, state),
            .error = std::move(error),
        });
}

void Application::cancelAllTxRequests(const std::string& reason)
{
    const auto finishedAtMs = nowMs();
    if (activeWrite_.has_value()) {
        finishTxRequest(activeWrite_->request, scripting::TxEventState::Canceled, reason, finishedAtMs);
        activeWrite_.reset();
    }
    if (activeHalfDuplexRequest_.has_value()) {
        scriptWorker_.postRequestAwaitingCompletion(false);
        finishTxRequest(activeHalfDuplexRequest_->request, scripting::TxEventState::Canceled, reason, finishedAtMs);
        activeHalfDuplexRequest_.reset();
    }
    cancelPendingTxRequests(reason, finishedAtMs);
}

void Application::notifyTxOverflow(const std::string& message)
{
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
                                          const scripting::StreamBufferDefinition& bufferDefinition)
{
    if (batch.bufferCapacity == 0 || batch.overflowed) {
        return;
    }

    const double thresholdRatio = runtimeConfig_.receive.streamBuffer.nearOverflowThreshold > 0.0
                                      ? runtimeConfig_.receive.streamBuffer.nearOverflowThreshold
                                      : bufferDefinition.nearOverflowThresholdRatio;
    const bool nearOverflow =
        static_cast<double>(batch.bufferSize) / static_cast<double>(batch.bufferCapacity) >= thresholdRatio;
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
    logMessage << status.str() << ", threshold=" << static_cast<int>(thresholdRatio * 100.0)
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
        .message = "当前连接 " + context.endpoint + " 的协议解析缓冲区已达到 " + std::to_string(batch.bufferSize) +
                   "/" + std::to_string(batch.bufferCapacity) + " bytes，告警阈值 " + thresholdText + "%。",
        .level = "warn",
        .dedupeKey = "receive.stream_buffer.near_overflow",
        .createdAtMs = createdAtMs,
    };
    enqueueDialogRequest(request);
    streamBufferAlertState_.popupOpen = true;
}

void Application::resetStreamBufferAlertState(const std::uint64_t connectionId)
{
    if (connectionId != 0 && streamBufferAlertState_.connectionId != connectionId) {
        return;
    }
    streamBufferAlertState_ = StreamBufferAlertState{};
}

void Application::enqueueTransferFrameRows(std::vector<dock::ReceiveRow> rows)
{
    if (rows.empty()) {
        return;
    }
    for (auto& row : rows) {
        pendingTransferFrameRows_.push_back(std::move(row));
    }
    trimPendingTransferFrameRowsToLimit();
}

void Application::trimPendingTransferFrameRowsToLimit()
{
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

bool Application::flushPendingTransferFrameRows(std::size_t maxRows)
{
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

} // namespace protoscope::app
