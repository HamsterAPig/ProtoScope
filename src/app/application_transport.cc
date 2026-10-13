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
    constexpr std::size_t kTransportBytesPerPump = 64U * 1024U;
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

void Application::rememberTransferProfile(const plot::RawCaptureEvent& event)
{
    if (event.type != plot::RawCaptureEventType::ProfileSet && event.type != plot::RawCaptureEventType::ProfileClear) return;
    auto profiles = transferProfiles_ ? *transferProfiles_ : std::vector<plot::RawCaptureEvent>{};
    if (event.type == plot::RawCaptureEventType::ProfileClear && event.profile.frameName.empty()) profiles.clear();
    else {
        std::erase_if(profiles, [&](const auto& old) { return old.profile.frameName == event.profile.frameName; });
        if (event.type == plot::RawCaptureEventType::ProfileSet) profiles.push_back(event);
    }
    transferProfiles_ = std::make_shared<const std::vector<plot::RawCaptureEvent>>(std::move(profiles));
}

void Application::appendHistoryReceiveRow(dock::ReceiveRow row)
{
    if (!row.displayPosition) row.displayPosition = ++transferPosition_;
    transferPosition_ = (std::max)(transferPosition_, row.displayPosition);
    row.historyProfiles = transferProfiles_;
    dockStore_.appendReceiveRow(std::move(row));
}

void Application::appendTransferRow(dock::ReceiveRow row)
{
    if (!row.displayPosition) row.displayPosition = ++transferPosition_;
    transferPosition_ = (std::max)(transferPosition_, row.displayPosition);
    // 追赶只保存位置与大小；字节仍由面板持有，裁剪时取消，不延长旧字节寿命。
    const bool queued = historyRebuild_ && queueHistoryFollowing({
        .type = row.direction == "TX" ? plot::RawCaptureEventType::TxBytes : plot::RawCaptureEventType::RxBytes,
        .direction = row.direction, .timestampMs = row.timestampMs, .endpoint = row.endpoint,
        .position = row.displayPosition, .byteSize = row.bytes.size(),
        .rowIndex = dockStore_.receiveState().rows.size(),
        .rowFrontPosition = dockStore_.receiveState().rows.empty() ? row.displayPosition :
                            dockStore_.receiveState().rows.front().displayPosition});
    // 原展示 parser 保留取消/裁剪时的新数据结果；任务存在时提交被阻止，完成只使用追赶结果替换。
    static_cast<void>(queued);
    if (replayReceiveHistory_ || dockStore_.receiveState().displayMode == dock::TransferLogDisplayMode::ParsedFrames) {
        appendTransferFrameRows(row);
    }
    appendHistoryReceiveRow(std::move(row));
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
    if (!transferFrameParser_.has_value() &&
        (!rawCaptureReplay_.loaded || rawCaptureReplay_.processing == OfflineReplayProcessing::BrowseRaw)) {
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
    // 协议重载也只安排任务，不复制或同步解析整份历史；来源 schema 不能沿用旧 parser。
    invalidateHistorySwitchWork("重新生成逐帧历史");
    if (rawCaptureReplay_.loaded && rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw) {
        transferFrameParser_ = offlineScriptWorker_ ? makeTransferFrameParserState(offlineScriptWorker_->snapshot())
                                                   : std::nullopt;
    } else {
        resetTransferFrameParser();
    }
    const auto scope = runtimeConfig_.gui.historyScope;
    const auto upperBound = scope == config::TransferLogHistoryScope::RetainedRows
        ? dockStore_.receiveState().rows.size()
        : (rawCaptureReplay_.loaded ? rawCaptureReplay_.eventIndex : dockStore_.waveState().rawCapture.events.size());
    beginHistoryRebuild(scope, historySwitchGeneration(), upperBound);
}

void Application::clearTransferLogHistory()
{
    // 清空使历史切换任务/旧确认失效，过期结果不得提交到新来源。
    invalidateHistorySwitchWork("清空");
    dockStore_.clearReceiveRows();
    dockStore_.clearTransferFrameRows();
    pendingTransferFrameRows_.clear();
    // 清空展示不切换当前协议来源，也不丢运行时 profile；仅丢残留半帧。
    if (transferFrameParser_) {
        transferFrameParser_->rx.reset(); transferFrameParser_->tx.reset();
    }
}

void Application::activateParsedTransferLogView()
{
    // 兼容旧调用方先修改 displayMode 的方式，但所有切换均走同一个三态入口。
    dockStore_.receiveState().displayMode = dock::TransferLogDisplayMode::RawChunks;
    requestParsedHistorySwitch();
}

// 历史切换的来源键：有 loaded timeline 用回放代次，否则用导入重建代次；两者皆无（实时）用稳定 live 代次。
// live 代次由 invalidateHistorySwitchWork 在所有显式失效入口递增，
// 实时事件不递增，避免重建期间任一实时事件使任务被误失效。
std::uint64_t Application::historySwitchGeneration() const
{
    if (rawCaptureReplay_.loaded) return rawCaptureReplay_.generation;
    if (importedRebuildGeneration_.has_value()) return *importedRebuildGeneration_;
    return liveGeneration_;
}

void Application::invalidateHistorySwitchWork(const std::string_view reason)
{
    if (historyRebuild_.has_value() || historySwitchConfirmationOpen_) {
        loggingFacade_.info("transfer", std::string("历史切换失效: ") + std::string(reason));
    }
    ++liveGeneration_;
    historyRebuild_.reset();
    historySwitchConfirmationOpen_ = false;
}

bool Application::historySourceAvailable() const
{
    if (runtimeConfig_.gui.historyScope == config::TransferLogHistoryScope::RetainedRows)
        return !dockStore_.receiveState().rows.empty();
    const auto& capture = rawCaptureReplay_.loaded ? rawCaptureReplay_.capture : dockStore_.waveState().rawCapture;
    return rawCaptureReplay_.loaded ? rawCaptureReplay_.eventIndex != 0 :
        (!capture.events.empty() || !capture.payload.empty());
}

bool Application::queueHistoryFollowing(HistoryRebuildSourceEvent event)
{
    if (!historyRebuild_) return false;
    auto& task = *historyRebuild_;
    const auto limit = runtimeConfig_.gui.rawCapture.liveLimitBytes;
    if (event.byteSize > limit || task.followingBytes > limit - event.byteSize ||
        task.following.size() >= runtimeConfig_.gui.logHistory.transferRawLimit) {
        invalidateHistorySwitchWork("历史重建追赶队列达到保留预算");
        dockStore_.waveState().statusMessage = "历史重建已取消：新增数据达到保留预算，继续显示新数据";
        return false;
    }
    task.followingBytes += event.byteSize;
    task.following.push_back(std::move(event));
    return true;
}

bool Application::beginHistoryRebuild(const config::TransferLogHistoryScope scope,
                                      const std::uint64_t generation,
                                      const std::size_t upperBound)
{
    // 只冻结位置与上界；字节由来源拥有，每轮读取一个有界切片，不同步复制历史。
    const auto& capture = rawCaptureReplay_.loaded ? rawCaptureReplay_.capture : dockStore_.waveState().rawCapture;
    std::optional<TransferFrameParserState> parser;
    if (transferFrameParser_) {
        parser = TransferFrameParserState{
            .rx = scripting::FrameStreamParser(transferFrameParser_->rx.bufferDefinition(), transferFrameParser_->rx.frameDefinitions()),
            .tx = scripting::FrameStreamParser(transferFrameParser_->tx.bufferDefinition(), transferFrameParser_->tx.frameDefinitions())};
    } else if (!rawCaptureReplay_.loaded || rawCaptureReplay_.processing == OfflineReplayProcessing::BrowseRaw)
        parser = makeTransferFrameParserState();
    // 缺 schema 不创建空任务，尤其不能在后续 pump 中清掉仍可查看的旧帧。
    if (!parser) return false;
    if (parser) {
        parser->rx.reset(); parser->tx.reset();
        parser->rx.clearRuntimeProfiles(); parser->tx.clearRuntimeProfiles();
    }
    HistoryRebuildTask task;
    task.hasLoadedTimeline = rawCaptureReplay_.loaded;
    task.generation = generation;
    task.upperBound = upperBound;
    task.sourceCount = rawCaptureReplay_.loaded ? rawCaptureReplay_.eventIndex : capture.events.size();
    if (scope == config::TransferLogHistoryScope::RetainedCapture)
        task.sourceCount = (std::min)(task.sourceCount, upperBound);
    task.rowsOnly = scope == config::TransferLogHistoryScope::RetainedRows;
    task.legacyPayload = capture.events.empty() && !capture.payload.empty();
    task.firstPosition = capture.events.empty() ? 0 : capture.events.front().displayPosition;
    task.parser = std::move(parser);
    task.boundaryProfiles = transferProfiles_;
    if (task.rowsOnly) {
        const auto& rows = dockStore_.receiveState().rows;
        task.sourceCount = (std::min)(upperBound, rows.size());
        if (task.sourceCount) {
            task.rowBegin = rows.front().displayPosition;
            task.rowEnd = rows[task.sourceCount - 1].displayPosition;
        }
        task.legacyPayload = false;
    }
    if (task.legacyPayload) task.sourceCount = 1;
    // 旧 pending 不加入重建结果；保留至提交或取消，完成时统一替换，避免两次追加。
    historyRebuild_ = std::move(task);
    return true;
}

bool Application::pumpHistoryRebuild()
{
    if (!historyRebuild_) return false;
    auto& task = *historyRebuild_;
    const auto& capture = task.hasLoadedTimeline ? rawCaptureReplay_.capture : dockStore_.waveState().rawCapture;
    if (task.generation != historySwitchGeneration() ||
        (task.rowsOnly && task.sourceCount && (dockStore_.receiveState().rows.size() < task.sourceCount ||
          dockStore_.receiveState().rows.front().displayPosition != task.rowBegin)) ||
        (!task.rowsOnly && !task.legacyPayload && task.sourceCount && (capture.events.empty() ||
          capture.events.front().displayPosition != task.firstPosition || capture.events.size() < task.sourceCount))) {
        invalidateHistorySwitchWork("来源被替换或裁剪");
        dockStore_.waveState().statusMessage = "历史重建已取消：来源已裁剪或替换";
        return true;
    }
    if (!task.parser) {
        // 防御失效任务：保留完整缓存和 pending，不以不可解析冒充成功的空结果。
        historyRebuild_.reset();
        dockStore_.waveState().statusMessage = "历史重建已取消：当前来源没有可用 stream schema";
        return true;
    }
    const auto started = std::chrono::steady_clock::now();
    std::size_t events = 0, bytes = 0;
    const auto keepRow = [&](dock::ReceiveRow row) {
        const auto limit = runtimeConfig_.gui.logHistory.transferFrameLimit;
        if (!limit) return;
        task.rebuilt.push_back(std::move(row));
        while (task.rebuilt.size() > limit) task.rebuilt.pop_front();
    };
    while (events < kTransportEventsPerPump && bytes < kTransportBytesPerPump &&
           std::chrono::steady_clock::now() - started < kTransportEventBudget) {
        const bool historical = task.eventCursor < task.sourceCount;
        // 面板最后一行之后仍可能接纳 profile 控制事件；在追赶前恢复冻结上界的 profile，保留尾半帧。
        if (!historical && task.rowsOnly) {
            if (!task.boundaryProfilesStarted) {
                task.parser->rx.clearRuntimeProfiles(); task.parser->tx.clearRuntimeProfiles();
                task.boundaryProfilesStarted = true;
            }
            if (task.boundaryProfiles && task.boundaryProfileCursor < task.boundaryProfiles->size()) {
                const auto& profile = (*task.boundaryProfiles)[task.boundaryProfileCursor++].profile;
                std::string error;
                for (auto* parser : {&task.parser->rx, &task.parser->tx}) {
                    if (!parser->setRuntimeProfile(profile.frameName,
                        scripting::StreamRuntimeProfile{.length = profile.length, .channelMap = profile.channelMap}, error)) {
                        invalidateHistorySwitchWork("上界 profile 无效");
                        dockStore_.waveState().statusMessage = "历史重建失败：" + error;
                        return true;
                    }
                }
                ++events; continue;
            }
        }
        if (!historical && task.following.empty()) break;
        const std::vector<std::uint8_t>* input = nullptr;
        std::string direction, endpoint;
        std::uint64_t timestamp = 0;
        plot::RawCaptureEventType type = plot::RawCaptureEventType::RxBytes;
        const plot::RawCaptureProfileEventData* profile = nullptr;
        bool selected = true;
        if (historical && task.rowsOnly) {
            const auto& rows = dockStore_.receiveState().rows;
            if (!task.currentRow) {
                const auto found = rows.begin() + static_cast<std::ptrdiff_t>(task.eventCursor);
                task.currentRow = dock::ReceiveRow{.timestampMs = found->timestampMs, .direction = found->direction,
                    .endpoint = found->endpoint, .displayPosition = found->displayPosition,
                    .historyProfiles = found->historyProfiles}; // 只保留元数据，不复制字节。
                task.rowProfileCursor = 0;
                task.parser->rx.clearRuntimeProfiles(); task.parser->tx.clearRuntimeProfiles();
            }
            const auto& row = *task.currentRow;
            if (row.historyProfiles && task.rowProfileCursor < row.historyProfiles->size()) {
                const auto& event = (*row.historyProfiles)[task.rowProfileCursor++];
                std::string error;
                for (auto* parser : {&task.parser->rx, &task.parser->tx}) {
                    if (!parser->setRuntimeProfile(event.profile.frameName,
                        scripting::StreamRuntimeProfile{.length = event.profile.length, .channelMap = event.profile.channelMap}, error)) {
                        invalidateHistorySwitchWork("面板 profile 基线无效");
                        dockStore_.waveState().statusMessage = "历史重建失败：" + error;
                        return true;
                    }
                }
                ++events; continue;
            }
            const auto retained = rows.begin() + static_cast<std::ptrdiff_t>(task.eventCursor);
            if (retained->displayPosition != row.displayPosition) {
                invalidateHistorySwitchWork("面板来源被裁剪");
                dockStore_.waveState().statusMessage = "历史重建已取消：面板来源被裁剪";
                return true;
            }
            input = &retained->bytes; direction = row.direction; endpoint = row.endpoint; timestamp = row.timestampMs;
            type = direction == "TX" ? plot::RawCaptureEventType::TxBytes : plot::RawCaptureEventType::RxBytes;
            selected = direction == "RX" || direction == "TX";
        } else if (historical && task.legacyPayload) {
            input = &capture.payload; direction = "RX"; endpoint = capture.source; timestamp = capture.capturedAtMs;
            selected = !task.rowsOnly || task.rowEnd != 0;
        } else if (historical) {
            const auto& event = capture.events[task.eventCursor];
            input = &event.bytes; type = event.type; profile = &event.profile;
            direction = type == plot::RawCaptureEventType::TxBytes ? "TX" : "RX";
            endpoint = event.endpoint.empty() ? capture.source : event.endpoint;
            timestamp = event.timestampMs;
            selected = !task.rowsOnly || (task.rowBegin && event.displayPosition >= task.rowBegin &&
                                          event.displayPosition <= task.rowEnd);
            // 控制事件从起点扫描：只折叠 profile，不引入范围外字节。
        } else {
            const auto& event = task.following.front();
            type = event.type; profile = &event.profile;
            direction = event.direction; endpoint = event.endpoint; timestamp = event.timestampMs;
            if (type == plot::RawCaptureEventType::RxBytes || type == plot::RawCaptureEventType::TxBytes) {
                const auto& rows = dockStore_.receiveState().rows;
                if (rows.empty() || rows.front().displayPosition != event.rowFrontPosition ||
                    event.rowIndex >= rows.size() || rows[event.rowIndex].displayPosition != event.position ||
                    rows[event.rowIndex].bytes.size() != event.byteSize) {
                    invalidateHistorySwitchWork("追赶来源已裁剪");
                    dockStore_.waveState().statusMessage = "历史重建已取消：新增来源已裁剪，继续显示新数据";
                    return true;
                }
                input = &rows[event.rowIndex].bytes;
            }
        }
        if (type == plot::RawCaptureEventType::ProfileSet || type == plot::RawCaptureEventType::ProfileClear) {
            std::string error;
            bool ok = true;
            for (auto* parser : {&task.parser->rx, &task.parser->tx}) {
                if (type == plot::RawCaptureEventType::ProfileSet)
                    ok = parser->setRuntimeProfile(profile->frameName,
                        scripting::StreamRuntimeProfile{.length = profile->length, .channelMap = profile->channelMap}, error) && ok;
                else if (profile->frameName.empty()) parser->clearRuntimeProfiles();
                else ok = parser->clearRuntimeProfile(std::optional<std::string>{profile->frameName}, error) && ok;
            }
            if (!ok) {
                invalidateHistorySwitchWork("历史 profile 无效");
                dockStore_.waveState().statusMessage = "历史重建失败：" + error;
                return true;
            }
        } else if (selected && (type == plot::RawCaptureEventType::RxBytes || type == plot::RawCaptureEventType::TxBytes)) {
            auto& parser = direction == "TX" ? task.parser->tx : task.parser->rx;
            // 小于 parser 容量的切片也避免单大事件覆盖环形缓冲；总量严格不超过 64KiB。
            const auto amount = (std::min)({input->size() - task.byteOffset, kTransportBytesPerPump - bytes,
                (std::max)(std::size_t{1}, parser.bufferDefinition().capacity / 2)});
            std::vector<std::uint8_t> slice(input->begin() + static_cast<std::ptrdiff_t>(task.byteOffset),
                input->begin() + static_cast<std::ptrdiff_t>(task.byteOffset + amount));
            const auto batch = parser.pushBytes(slice);
            dock::ReceiveRow row{.timestampMs = timestamp, .direction = direction, .endpoint = endpoint};
            for (const auto& frame : batch.frames) keepRow(makeTransferFrameRow(row, frame));
            if (batch.frames.empty() && (!batch.errors.empty() || (direction == "TX" && batch.bufferSize == 0))) {
                row.bytes = std::move(slice); keepRow(std::move(row));
            }
            task.byteOffset += amount; bytes += amount;
            if (task.byteOffset < input->size()) { ++events; continue; }
        }
        task.byteOffset = 0; ++events;
        if (historical) {
            if (task.currentRow) {
                task.currentRow.reset();
            }
            ++task.eventCursor;
        }
        else {
            task.followingBytes -= task.following.front().byteSize;
            task.following.pop_front();
        }
    }
    if (task.eventCursor < task.sourceCount || !task.following.empty() ||
        (task.rowsOnly && (!task.boundaryProfilesStarted ||
         (task.boundaryProfiles && task.boundaryProfileCursor < task.boundaryProfiles->size())))) return true;
    // 交接同一个 UI parser 的尾缓冲/profile；历史后的新字节已按接纳顺序追赶，不能重解析。
    transferFrameParser_ = std::move(task.parser);
    dockStore_.clearTransferFrameRows();
    pendingTransferFrameRows_.clear();
    pendingTransferFrameRows_ = std::move(task.rebuilt);
    historyRebuild_.reset();
    flushPendingTransferFrameRows(transferFrameRowsPerPump());
    return true;
}
std::string Application::parsedTransferUnavailableReason() const
{
    // 显式解析时间轴只认其独立 worker；实时和 BrowseRaw 使用当前有效 Runtime。
    const bool offline = rawCaptureReplay_.loaded &&
        rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw;
    const auto snapshot = offline
        ? (offlineScriptWorker_ ? offlineScriptWorker_->snapshot() : scripting::ScriptRuntimeSnapshot{})
        : scriptWorker_.snapshot();
    // 最近一次加载失败不会抹掉旧有效 Runtime，不能只用 lastError 判断能力。
    if (snapshot.streamBuffer && !snapshot.streamFrames.empty()) return {};
    if (!snapshot.scriptPath.empty()) return "当前协议没有 stream schema，无法生成逐帧记录";
    if (!snapshot.lastError.empty() || (offline && !rawCaptureReplay_.error.empty()))
        return "协议加载失败，当前没有可用协议，无法生成逐帧记录";
    return "尚未加载协议，无法生成逐帧记录";
}

void Application::requestParsedHistorySwitch()
{
    auto& receive = dockStore_.receiveState();
    if (receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames) {
        // 已在逐帧视图：切回原始。切回原始使任务/旧确认失效。
        receive.displayMode = dock::TransferLogDisplayMode::RawChunks;
        invalidateHistorySwitchWork("切回原始");
        return;
    }
    const bool hasHistory = historySourceAvailable();
    if (!parsedTransferUnavailableReason().empty() || !hasHistory) {
        receive.displayMode = dock::TransferLogDisplayMode::ParsedFrames;
        return;
    }
    switch (runtimeConfig_.gui.historyOnParsedSwitch) {
        case config::TransferLogHistoryOnParsedSwitch::Ask:
            if (!hasHistory) {
                receive.displayMode = dock::TransferLogDisplayMode::ParsedFrames;
                return;
            }
            // 确认前保持原始模式，采集不停止；上界在 resolveHistorySwitch 生效时固定。
            if (!historySwitchConfirmationOpen_) ++historySwitchConfirmationId_;
            historySwitchConfirmationOpen_ = true;
            return;
        case config::TransferLogHistoryOnParsedSwitch::UseHistory: {
            // 自动策略：请求处理时固定上界。
            const auto upperBound =
                runtimeConfig_.gui.historyScope == config::TransferLogHistoryScope::RetainedRows
                    ? dockStore_.receiveState().rows.size()
                    : (rawCaptureReplay_.loaded ? rawCaptureReplay_.eventIndex
                                                : dockStore_.waveState().rawCapture.events.size());
            receive.displayMode = dock::TransferLogDisplayMode::ParsedFrames;
            beginHistoryRebuild(runtimeConfig_.gui.historyScope, historySwitchGeneration(), upperBound);
            return;
        }
        case config::TransferLogHistoryOnParsedSwitch::SkipHistory:
            // 不使用历史：不重放旧字节，保留已有完整帧，只清残留半帧。
            receive.displayMode = dock::TransferLogDisplayMode::ParsedFrames;
            historyRebuild_.reset();
            // 只清半帧；保留已有完整帧、待提交帧以及当前来源的 schema/runtime profiles。
            if (transferFrameParser_) {
                transferFrameParser_->rx.reset();
                transferFrameParser_->tx.reset();
            }
            return;
    }
}

void Application::resolveHistorySwitch(const config::TransferLogHistoryOnParsedSwitch decision)
{
    if (!historySwitchConfirmationOpen_) return;
    historySwitchConfirmationOpen_ = false;
    auto& receive = dockStore_.receiveState();
    // 弹窗期间协议/历史可能变化，选择生效时再次检查，不创建无效重建任务。
    if (!parsedTransferUnavailableReason().empty() || !historySourceAvailable()) {
        receive.displayMode = dock::TransferLogDisplayMode::ParsedFrames;
        return;
    }
    if (decision == config::TransferLogHistoryOnParsedSwitch::UseHistory) {
        // 询问上界在用户选择生效时固定。
        const auto upperBound =
            runtimeConfig_.gui.historyScope == config::TransferLogHistoryScope::RetainedRows
                ? dockStore_.receiveState().rows.size()
                : (rawCaptureReplay_.loaded ? rawCaptureReplay_.eventIndex
                                            : dockStore_.waveState().rawCapture.events.size());
        receive.displayMode = dock::TransferLogDisplayMode::ParsedFrames;
        beginHistoryRebuild(runtimeConfig_.gui.historyScope, historySwitchGeneration(), upperBound);
        return;
    }
    // 拒绝历史：切到逐帧且只清半帧；取消另走 cancelHistorySwitch，保持原始模式和缓存。
    receive.displayMode = dock::TransferLogDisplayMode::ParsedFrames;
    historyRebuild_.reset();
    // 拒绝历史不丢完整帧，也不得用实时 schema 替换显式离线 parser。
    if (transferFrameParser_) {
        transferFrameParser_->rx.reset();
        transferFrameParser_->tx.reset();
    }
}

void Application::cancelHistorySwitch()
{
    // Escape 等同取消：关闭 modal，保持原始模式，采集不停止。
    historySwitchConfirmationOpen_ = false;
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
    rememberTransferProfile({.type = event.cleared ? plot::RawCaptureEventType::ProfileClear : plot::RawCaptureEventType::ProfileSet,
        .profile = {.frameName = event.frameName, .length = event.length, .channelMap = event.channelMap}});
    if (historyRebuild_ && queueHistoryFollowing({
        .type = event.cleared ? plot::RawCaptureEventType::ProfileClear : plot::RawCaptureEventType::ProfileSet,
        .profile = {.frameName = event.frameName, .length = event.length, .channelMap = event.channelMap}})) {
        // 同步维持旧展示 parser，任务取消时仍能继续；重建结果只使用独立 parser。
    }
    if (!transferFrameParser_.has_value() &&
        (!rawCaptureReplay_.loaded || rawCaptureReplay_.processing == OfflineReplayProcessing::BrowseRaw)) {
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
                        .endpoint = evt.context.endpoint, .bytes = evt.bytes, .message = evt.status, .messageIsWriteStatus = true});
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
    // 三个 RX 阶段必须来自同一时刻；分开读取会在出队/发布切换时误判没有待处理 ACK。
    const auto workerSnapshot = scriptWorker_.snapshot();
    if (workerSnapshot.executingRxBytes > 0U || workerSnapshot.pendingWorkerRxBytes > 0U ||
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
    // 待提交队列独立有界；提交时 DockStore 裁掉旧展示帧，不能因展示已满而丢掉所有新帧。
    if (pendingTransferFrameRows_.size() <= limit) {
        return;
    }
    auto excess = pendingTransferFrameRows_.size() - limit;
    while (excess > 0 && !pendingTransferFrameRows_.empty()) {
        pendingTransferFrameRows_.pop_front();
        --excess;
    }
}

bool Application::flushPendingTransferFrameRows(std::size_t maxRows)
{
    if (historyRebuild_ || pendingTransferFrameRows_.empty() || maxRows == 0) {
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
