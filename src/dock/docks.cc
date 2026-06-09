#include "protoscope/dock/docks.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <string_view>
#include <utility>

namespace protoscope::dock {

namespace {
    std::uint64_t nowMs()
    {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count());
    }

    std::string formatTimestampText(std::uint64_t timestampMs)
    {
        const auto timePoint = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
        const auto secondsPoint = std::chrono::time_point_cast<std::chrono::seconds>(timePoint);
        const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(timePoint - secondsPoint).count();
        const std::time_t timeValue = std::chrono::system_clock::to_time_t(timePoint);

        std::tm localTm{};
#if defined(_WIN32)
        localtime_s(&localTm, &timeValue);
#else
        localtime_r(&timeValue, &localTm);
#endif

        char buffer[64]{};
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%04d-%02d-%02d %02d:%02d:%02d:%03d",
                      localTm.tm_year + 1900,
                      localTm.tm_mon + 1,
                      localTm.tm_mday,
                      localTm.tm_hour,
                      localTm.tm_min,
                      localTm.tm_sec,
                      static_cast<int>(millis));
        return buffer;
    }

    std::string bytesToAsciiPreview(const std::vector<std::uint8_t>& bytes)
    {
        std::string text;
        text.reserve(bytes.size());
        for (const auto byte : bytes) {
            const char ch = static_cast<char>(byte);
            text.push_back(std::isprint(static_cast<unsigned char>(ch)) ? ch : '.');
        }
        return text;
    }

    std::string flattenSingleLineText(std::string_view text)
    {
        std::string flattened;
        flattened.reserve(text.size());
        for (const char ch : text) {
            switch (ch) {
                case '\r':
                case '\n':
                case '\t':
                    flattened.push_back(' ');
                    break;
                default:
                    flattened.push_back(ch);
                    break;
            }
        }
        return flattened;
    }

    std::string csvEscape(std::string_view text)
    {
        std::string escaped;
        escaped.reserve(text.size() + 2U);
        escaped.push_back('"');
        for (const char ch : text) {
            if (ch == '"') {
                escaped.push_back('"');
            }
            escaped.push_back(ch);
        }
        escaped.push_back('"');
        return escaped;
    }

    void appendCsvField(std::string& line, std::string_view text)
    {
        if (!line.empty()) {
            line.push_back(',');
        }
        line.append(csvEscape(text));
    }

    std::string textOrDash(std::string_view text)
    {
        return text.empty() ? std::string("-") : std::string(text);
    }

    std::string uppercaseAscii(std::string text)
    {
        for (auto& ch : text) {
            ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        }
        return text;
    }

    std::string lowercaseAscii(std::string text)
    {
        for (auto& ch : text) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        return text;
    }

    bool containsIgnoreCase(std::string_view text, std::string_view keyword)
    {
        if (keyword.empty()) {
            return true;
        }

        const auto lowerText = lowercaseAscii(std::string(text));
        const auto lowerKeyword = lowercaseAscii(std::string(keyword));
        return lowerText.find(lowerKeyword) != std::string::npos;
    }

    bool matchesStatusFilter(const ReceiveRow& row, LogStatusFilter status)
    {
        if (status == LogStatusFilter::All) {
            return true;
        }

        switch (classifyReceiveRow(row)) {
            case ReceiveRowVisualKind::Rx:
                return status == LogStatusFilter::Rx;
            case ReceiveRowVisualKind::Tx:
                return status == LogStatusFilter::Tx;
            case ReceiveRowVisualKind::Debug:
                return status == LogStatusFilter::Debug;
            case ReceiveRowVisualKind::Info:
                return status == LogStatusFilter::Info;
            case ReceiveRowVisualKind::Warn:
                return status == LogStatusFilter::Warn;
            case ReceiveRowVisualKind::Error:
                return status == LogStatusFilter::Error;
            case ReceiveRowVisualKind::Event:
                return status == LogStatusFilter::Event;
            case ReceiveRowVisualKind::ScriptLog:
                return status == LogStatusFilter::ScriptLog;
            case ReceiveRowVisualKind::Other:
                return status == LogStatusFilter::Other;
        }
        return false;
    }

    bool matchesKeywordFilter(const ReceiveRow& row, std::string_view keyword, bool includeBytePreview)
    {
        if (keyword.empty()) {
            return true;
        }

        // 核心筛选逻辑：先匹配日志元信息和消息；收发记录再补充 HEX/ASCII 字节内容。
        if (containsIgnoreCase(row.direction, keyword) || containsIgnoreCase(row.endpoint, keyword) ||
            containsIgnoreCase(row.message, keyword)) {
            return true;
        }

        if (!includeBytePreview || row.bytes.empty()) {
            return false;
        }

        return containsIgnoreCase(protocol_utils::bytesToHex(row.bytes, true), keyword) ||
               containsIgnoreCase(bytesToAsciiPreview(row.bytes), keyword);
    }

    bool matchesRequestTraceKeyword(const RequestTraceRow& row, std::string_view keyword)
    {
        if (keyword.empty()) {
            return true;
        }
        return containsIgnoreCase(std::to_string(row.id), keyword) || containsIgnoreCase(row.endpoint, keyword) ||
               containsIgnoreCase(row.tag, keyword) || containsIgnoreCase(requestTraceKindLabel(row.kind), keyword) ||
               containsIgnoreCase(requestTraceStateLabel(row.state), keyword) ||
               containsIgnoreCase(row.guardState, keyword) || containsIgnoreCase(row.error, keyword);
    }
} // namespace

ReceiveRowVisualKind classifyReceiveRow(const ReceiveRow& row)
{
    const auto direction = uppercaseAscii(row.direction);
    if (direction == "RX") {
        return ReceiveRowVisualKind::Rx;
    }
    if (direction == "TX") {
        return ReceiveRowVisualKind::Tx;
    }
    if (direction == "DEBUG") {
        return ReceiveRowVisualKind::Debug;
    }
    if (direction == "INFO") {
        return ReceiveRowVisualKind::Info;
    }
    if (direction == "WARN" || direction == "WARNING") {
        return ReceiveRowVisualKind::Warn;
    }
    if (direction == "ERROR") {
        return ReceiveRowVisualKind::Error;
    }
    if (direction == "EVENT") {
        return ReceiveRowVisualKind::Event;
    }
    if (direction == "LOG") {
        return ReceiveRowVisualKind::ScriptLog;
    }
    return ReceiveRowVisualKind::Other;
}

bool matchesLogFilter(const ReceiveRow& row, const LogFilterState& filter, bool includeBytePreview)
{
    return matchesStatusFilter(row, filter.status) && matchesKeywordFilter(row, filter.keyword, includeBytePreview);
}

template <typename Rows>
std::vector<const ReceiveRow*> filteredLogRowsImpl(const Rows& rows,
                                                   const LogFilterState& filter,
                                                   bool includeBytePreview)
{
    std::vector<const ReceiveRow*> filtered;
    filtered.reserve(rows.size());
    for (const auto& row : rows) {
        if (matchesLogFilter(row, filter, includeBytePreview)) {
            filtered.push_back(&row);
        }
    }
    return filtered;
}

std::vector<const ReceiveRow*> filteredLogRows(const std::deque<ReceiveRow>& rows,
                                               const LogFilterState& filter,
                                               bool includeBytePreview)
{
    return filteredLogRowsImpl(rows, filter, includeBytePreview);
}

std::vector<const ReceiveRow*> filteredLogRows(const std::vector<ReceiveRow>& rows,
                                               const LogFilterState& filter,
                                               bool includeBytePreview)
{
    return filteredLogRowsImpl(rows, filter, includeBytePreview);
}

const char* requestTraceKindLabel(RequestTraceKind kind)
{
    switch (kind) {
        case RequestTraceKind::Send:
            return "send";
        case RequestTraceKind::Request:
            return "request";
    }
    return "send";
}

const char* requestTraceStateLabel(RequestTraceState state)
{
    switch (state) {
        case RequestTraceState::Queued:
            return "排队";
        case RequestTraceState::Sent:
            return "已发送";
        case RequestTraceState::Completed:
            return "完成";
        case RequestTraceState::Failed:
            return "失败";
        case RequestTraceState::Timeout:
            return "超时";
        case RequestTraceState::Rejected:
            return "拒绝";
        case RequestTraceState::Dropped:
            return "丢弃";
        case RequestTraceState::Canceled:
            return "取消";
        case RequestTraceState::GuardReset:
            return "熔断重置";
    }
    return "未知";
}

bool isRequestTraceFailure(RequestTraceState state)
{
    switch (state) {
        case RequestTraceState::Failed:
        case RequestTraceState::Timeout:
        case RequestTraceState::Rejected:
        case RequestTraceState::Dropped:
        case RequestTraceState::Canceled:
            return true;
        default:
            return false;
    }
}

bool matchesRequestTraceFilter(const RequestTraceRow& row, const RequestTraceFilterState& filter)
{
    if (!matchesRequestTraceKeyword(row, filter.keyword)) {
        return false;
    }

    switch (filter.status) {
        case RequestTraceStatusFilter::All:
            return true;
        case RequestTraceStatusFilter::Active:
            return row.state == RequestTraceState::Queued || row.state == RequestTraceState::Sent;
        case RequestTraceStatusFilter::Success:
            return row.state == RequestTraceState::Completed || row.state == RequestTraceState::GuardReset;
        case RequestTraceStatusFilter::Failure:
            return isRequestTraceFailure(row.state);
    }
    return true;
}

std::vector<const RequestTraceRow*> filteredRequestTraceRows(const std::deque<RequestTraceRow>& rows,
                                                            const RequestTraceFilterState& filter)
{
    std::vector<const RequestTraceRow*> filtered;
    filtered.reserve(rows.size());
    for (const auto& row : rows) {
        if (matchesRequestTraceFilter(row, filter)) {
            filtered.push_back(&row);
        }
    }
    return filtered;
}

std::string formatRequestTraceDuration(const RequestTraceRow& row)
{
    if (row.durationMs == 0U && row.state == RequestTraceState::Queued) {
        return "-";
    }
    return std::to_string(row.durationMs) + " ms";
}

std::string formatRequestTraceDetail(const RequestTraceRow& row)
{
    if (!row.error.empty()) {
        return row.error;
    }
    return row.guardState;
}

std::string formatRequestTraceRowCsv(const RequestTraceRow& row, bool showTimestamps)
{
    std::string line;
    if (showTimestamps) {
        appendCsvField(line, formatTimestampText(row.timestampMs));
    }
    appendCsvField(line, row.id == 0U ? std::string("-") : std::to_string(row.id));
    appendCsvField(line, requestTraceKindLabel(row.kind));
    appendCsvField(line, requestTraceStateLabel(row.state));
    appendCsvField(line, textOrDash(row.tag));
    appendCsvField(line, textOrDash(row.endpoint));
    appendCsvField(line, std::to_string(row.attempt) + "/" + std::to_string(row.maxAttempts));
    appendCsvField(line, std::to_string(row.bytes));
    appendCsvField(line, formatRequestTraceDuration(row));
    appendCsvField(line, textOrDash(formatRequestTraceDetail(row)));
    return line;
}

std::string formatRequestTraceRowsCsv(std::span<const RequestTraceRow> rows, bool showTimestamps)
{
    std::string csv;
    if (showTimestamps) {
        appendCsvField(csv, "时间");
    }
    appendCsvField(csv, "ID");
    appendCsvField(csv, "类型");
    appendCsvField(csv, "状态");
    appendCsvField(csv, "Tag");
    appendCsvField(csv, "端点");
    appendCsvField(csv, "尝试");
    appendCsvField(csv, "字节");
    appendCsvField(csv, "耗时");
    appendCsvField(csv, "详情");
    csv.push_back('\n');

    for (const auto& row : rows) {
        csv.append(formatRequestTraceRowCsv(row, showTimestamps));
        csv.push_back('\n');
    }
    return csv;
}

std::string formatReceiveRowContent(const ReceiveRow& row, bool showHex)
{
    if (!row.message.empty()) {
        return flattenSingleLineText(row.message);
    }
    if (row.bytes.empty()) {
        return {};
    }
    return showHex ? protocol_utils::bytesToHex(row.bytes, true) : bytesToAsciiPreview(row.bytes);
}

std::string formatReceiveRowSingleLine(const ReceiveRow& row, bool showTimestamps, bool showHex)
{
    std::string line;
    if (showTimestamps) {
        line.append("[");
        line.append(formatTimestampText(row.timestampMs));
        line.append("] ");
    }
    if (!row.direction.empty()) {
        line.append(row.direction);
    }
    if (!row.endpoint.empty()) {
        if (!line.empty() && line.back() != ' ') {
            line.push_back(' ');
        }
        line.append(row.endpoint);
    }

    const auto content = formatReceiveRowContent(row, showHex);
    if (!content.empty()) {
        if (!line.empty()) {
            line.append(" | ");
        }
        line.append(content);
    }
    return line;
}

std::string formatReceiveRowsText(std::span<const ReceiveRow> rows, bool showTimestamps, bool showHex)
{
    std::string text;
    for (const auto& row : rows) {
        text.append(formatReceiveRowSingleLine(row, showTimestamps, showHex));
        text.push_back('\n');
    }
    return text;
}

void trimSendHistory(SendDockState& sendState, std::size_t limit)
{
    if (limit == 0U) {
        sendState.history.clear();
        return;
    }
    if (sendState.history.size() > limit) {
        // 核心流程：发送历史按“最新在前”保存，一次性裁掉末尾旧记录，避免逐条弹出。
        sendState.history.resize(limit);
    }
}

void trimRequestTraceRows(RequestTraceDockState& traceState, std::size_t limit)
{
    if (limit == 0U) {
        traceState.rows.clear();
        return;
    }
    // 核心流程：请求追踪按时间线追加，只保留最新记录，避免长期联调时 UI 历史无限增长。
    while (traceState.rows.size() > limit) {
        traceState.rows.pop_front();
    }
}

void rememberSendHistory(SendDockState& sendState, std::string payload, std::size_t limit)
{
    if (limit == 0U || payload.empty()) {
        trimSendHistory(sendState, limit);
        return;
    }

    const auto duplicate = std::find(sendState.history.begin(), sendState.history.end(), payload);
    if (duplicate != sendState.history.end()) {
        sendState.history.erase(duplicate);
    }
    sendState.history.push_front(std::move(payload));
    trimSendHistory(sendState, limit);
}

void DockStore::clearReceiveRows()
{
    receive_.rows.clear();
    receive_.frameRows.clear();
    ++receive_.rowsVersion;
    ++receive_.frameRowsVersion;
}

void DockStore::appendReceiveRow(ReceiveRow row)
{
    receive_.rows.push_back(std::move(row));
    historyLimiter_->trimRows(receive_.rows, historyLimits_.transferRawRows);
    ++receive_.rowsVersion;
}

void DockStore::appendLogRow(ReceiveRow row)
{
    log_.rows.push_back(std::move(row));
    historyLimiter_->trimRows(log_.rows, historyLimits_.hostLogRows);
    ++log_.rowsVersion;
}

void DockStore::appendScriptRow(ReceiveRow row)
{
    script_.rows.push_back(std::move(row));
    historyLimiter_->trimRows(script_.rows, historyLimits_.scriptLogRows);
    ++script_.rowsVersion;
}

void DockStore::appendRequestTraceRow(RequestTraceRow row)
{
    requestTrace_.rows.push_back(std::move(row));
    trimRequestTraceRows(requestTrace_, historyLimits_.requestTraceRows);
    ++requestTrace_.rowsVersion;
}

void DockStore::clearLogRows()
{
    log_.rows.clear();
    ++log_.rowsVersion;
}

void DockStore::clearScriptRows()
{
    script_.rows.clear();
    ++script_.rowsVersion;
}

void DockStore::clearRequestTraceRows()
{
    requestTrace_.rows.clear();
    ++requestTrace_.rowsVersion;
}

void DockStore::appendTransferFrameRows(std::vector<ReceiveRow> rows)
{
    if (rows.empty()) {
        return;
    }

    for (auto& row : rows) {
        receive_.frameRows.push_back(std::move(row));
    }
    historyLimiter_->trimRows(receive_.frameRows, historyLimits_.transferFrameRows);
    ++receive_.frameRowsVersion;
}

void DockStore::clearTransferFrameRows()
{
    receive_.frameRows.clear();
    ++receive_.frameRowsVersion;
}

void DockStore::setHistoryLimits(DockHistoryLimits limits)
{
    historyLimits_ = limits;
    if (historyLimiter_->trimRows(receive_.rows, historyLimits_.transferRawRows)) {
        ++receive_.rowsVersion;
    }
    if (historyLimiter_->trimRows(receive_.frameRows, historyLimits_.transferFrameRows)) {
        ++receive_.frameRowsVersion;
    }
    if (historyLimiter_->trimRows(log_.rows, historyLimits_.hostLogRows)) {
        ++log_.rowsVersion;
    }
    if (historyLimiter_->trimRows(script_.rows, historyLimits_.scriptLogRows)) {
        ++script_.rowsVersion;
    }
    const auto beforeRequestTraceRows = requestTrace_.rows.size();
    trimRequestTraceRows(requestTrace_, historyLimits_.requestTraceRows);
    if (requestTrace_.rows.size() != beforeRequestTraceRows) {
        ++requestTrace_.rowsVersion;
    }
}

void DockStore::appendLuaEvent(const scripting::ScriptEvent& event)
{
    appendScriptRow(ReceiveRow{
        .timestampMs = event.timestampMs,
        .direction = "EVENT",
        .endpoint = "script",
        .bytes = {},
        .message = event.name + ": " + event.payload,
    });
}

void DockStore::appendRawReceive(const transport::ConnectionContext& ctx, const std::string& text)
{
    receive_.rows.push_back(ReceiveRow{
        .timestampMs = nowMs(),
        .direction = "RX",
        .endpoint = ctx.endpoint,
        .bytes = std::vector<std::uint8_t>(text.begin(), text.end()),
        .message = {},
    });
    historyLimiter_->trimRows(receive_.rows, historyLimits_.transferRawRows);
    ++receive_.rowsVersion;
}

void DockStore::appendRawSend(const transport::ConnectionContext& ctx, const std::string& text)
{
    receive_.rows.push_back(ReceiveRow{
        .timestampMs = nowMs(),
        .direction = "TX",
        .endpoint = ctx.endpoint,
        .bytes = std::vector<std::uint8_t>(text.begin(), text.end()),
        .message = {},
    });
    historyLimiter_->trimRows(receive_.rows, historyLimits_.transferRawRows);
    ++receive_.rowsVersion;
}

CommDockState& DockStore::commState()
{
    return comm_;
}

ReceiveDockState& DockStore::receiveState()
{
    return receive_;
}

LogDockState& DockStore::logState()
{
    return log_;
}

ScriptDockState& DockStore::scriptState()
{
    return script_;
}

RequestTraceDockState& DockStore::requestTraceState()
{
    return requestTrace_;
}

SendDockState& DockStore::sendState()
{
    return send_;
}

LuaDockState& DockStore::luaState()
{
    return lua_;
}

plot::WaveDockState& DockStore::waveState()
{
    return wave_;
}

ConfigDockState& DockStore::configState()
{
    return config_;
}

const CommDockState& DockStore::commState() const
{
    return comm_;
}

const ReceiveDockState& DockStore::receiveState() const
{
    return receive_;
}

const LogDockState& DockStore::logState() const
{
    return log_;
}

const ScriptDockState& DockStore::scriptState() const
{
    return script_;
}

const RequestTraceDockState& DockStore::requestTraceState() const
{
    return requestTrace_;
}

const SendDockState& DockStore::sendState() const
{
    return send_;
}

const LuaDockState& DockStore::luaState() const
{
    return lua_;
}

const plot::WaveDockState& DockStore::waveState() const
{
    return wave_;
}

const ConfigDockState& DockStore::configState() const
{
    return config_;
}

void DockStore::markDirty(const std::string& statusMessage)
{
    config_.dirty = true;
    config_.statusMessage = statusMessage;
}

void DockStore::clearDirty(const std::string& statusMessage)
{
    config_.dirty = false;
    config_.statusMessage = statusMessage;
}

void DockStore::setPendingExternalReload(std::uint64_t timestampMs, std::string message)
{
    config_.pendingExternalReload = true;
    config_.pendingExternalReloadTimestampMs = timestampMs;
    config_.externalReloadMessage = std::move(message);
}

void DockStore::clearPendingExternalReload()
{
    config_.pendingExternalReload = false;
    config_.pendingExternalReloadTimestampMs = 0;
    config_.externalReloadMessage.clear();
}

void DockStore::setConflict(std::string message)
{
    config_.conflict.detected = true;
    config_.conflict.message = std::move(message);
}

void DockStore::clearConflict()
{
    config_.conflict.detected = false;
    config_.conflict.message.clear();
}

} // namespace protoscope::dock
