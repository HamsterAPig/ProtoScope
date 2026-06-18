#include "protoscope/dock/docks.hpp"
#include "protoscope/ui/protocol_ui_state.hpp"

#include "test_registry.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

void test_config_external_reload_state()
{
    protoscope::dock::DockStore store;
    auto& config = store.configState();

    require(!config.pendingExternalReload, "初始状态不应挂起外部重载");
    require(config.pendingExternalReloadTimestampMs == 0, "初始挂起时间戳应为 0");
    require(config.externalReloadMessage.empty(), "初始外部重载提示应为空");

    store.setPendingExternalReload(1234, "检测到外部配置更新");
    require(config.pendingExternalReload, "设置后应进入外部重载挂起态");
    require(config.pendingExternalReloadTimestampMs == 1234, "挂起时间戳应与设置值一致");
    require(config.externalReloadMessage == "检测到外部配置更新", "挂起提示应被记录");

    store.clearPendingExternalReload();
    require(!config.pendingExternalReload, "清理后不应继续挂起外部重载");
    require(config.pendingExternalReloadTimestampMs == 0, "清理后挂起时间戳应重置");
    require(config.externalReloadMessage.empty(), "清理后外部重载提示应清空");
}

void test_bounded_dock_history_limiter_trims_from_front()
{
    protoscope::dock::BoundedDockHistoryLimiter concreteLimiter;
    protoscope::dock::IDockHistoryLimiter& limiter = concreteLimiter;

    std::deque<protoscope::dock::ReceiveRow> rows{
        {.timestampMs = 1, .direction = "RX", .endpoint = "tcp", .bytes = {}, .message = "keep-1"},
        {.timestampMs = 2, .direction = "RX", .endpoint = "tcp", .bytes = {}, .message = "keep-2"},
        {.timestampMs = 3, .direction = "RX", .endpoint = "tcp", .bytes = {}, .message = "drop-3"},
        {.timestampMs = 4, .direction = "RX", .endpoint = "tcp", .bytes = {}, .message = "drop-4"},
    };

    const auto trimmed = limiter.trimRows(rows, 2);
    require(trimmed, "超过上限时 limiter 应返回已裁剪");
    require(rows.size() == 2, "limiter 应只保留最近的两条记录");
    require(rows[0].message == "drop-3" && rows[1].message == "drop-4", "limiter 应从前向后裁剪旧记录");

    const auto cleared = limiter.trimRows(rows, 0);
    require(cleared, "limit 为 0 时 limiter 应清空全部记录");
    require(rows.empty(), "limit 为 0 时 limiter 应清空全部记录");
}

void test_dock_log_and_script_split()
{
    protoscope::dock::DockStore store;

    store.appendReceiveRow({.timestampMs = 1, .direction = "RX", .endpoint = "tcp", .bytes = {0x01}, .message = {}});
    store.appendLogRow({.timestampMs = 2, .direction = "INFO", .endpoint = "host", .bytes = {}, .message = "saved"});
    store.appendLuaEvent({.name = "frame", .payload = "{status=ok}", .timestampMs = 3});
    store.appendScriptRow(
        {.timestampMs = 4, .direction = "LOG", .endpoint = "script", .bytes = {}, .message = "[info] opened"});

    require(store.receiveState().rows.size() == 1, "接收区应只保留 TX/RX 原始数据");
    require(store.logState().rows.size() == 1, "日志区应独立记录宿主日志");
    require(store.scriptState().rows.size() == 2, "脚本区应记录 Lua 事件与脚本日志");
    require(store.scriptState().rows[0].direction == "EVENT", "Lua 事件应写入脚本区");

    store.clearLogRows();
    store.clearScriptRows();
    require(store.logState().rows.empty(), "日志区应可单独清空");
    require(store.scriptState().rows.empty(), "脚本区应可单独清空");
}

void test_dock_history_limits_trim_all_log_types()
{
    protoscope::dock::DockStore store;
    store.setHistoryLimits({
        .transferRawRows = 3,
        .transferFrameRows = 4,
        .hostLogRows = 2,
        .scriptLogRows = 2,
        .requestTraceRows = 3,
    });

    for (std::size_t index = 0; index < 5; ++index) {
        store.appendReceiveRow({
            .timestampMs = static_cast<std::uint64_t>(index),
            .direction = "RX",
            .endpoint = "tcp",
            .bytes = {},
            .message = "raw-" + std::to_string(index),
        });
        store.appendLogRow({
            .timestampMs = static_cast<std::uint64_t>(index),
            .direction = "INFO",
            .endpoint = "host",
            .bytes = {},
            .message = "host-" + std::to_string(index),
        });
        store.appendScriptRow({
            .timestampMs = static_cast<std::uint64_t>(index),
            .direction = "LOG",
            .endpoint = "script",
            .bytes = {},
            .message = "script-" + std::to_string(index),
        });
        store.appendRequestTraceRow({
            .timestampMs = static_cast<std::uint64_t>(index),
            .id = index + 1,
            .kind = protoscope::dock::RequestTraceKind::Request,
            .state = protoscope::dock::RequestTraceState::Queued,
            .endpoint = "tcp",
            .tag = "request-" + std::to_string(index),
            .guardState = {},
            .error = {},
        });
    }

    std::vector<protoscope::dock::ReceiveRow> frameRows;
    for (std::size_t index = 0; index < 6; ++index) {
        frameRows.push_back({
            .timestampMs = static_cast<std::uint64_t>(index),
            .direction = "RX",
            .endpoint = "tcp",
            .bytes = {},
            .message = "frame-" + std::to_string(index),
        });
    }
    store.appendTransferFrameRows(std::move(frameRows));

    require(store.receiveState().rows.size() == 3, "原始收发记录应按上限裁剪");
    require(store.receiveState().frameRows.size() == 4, "逐帧收发记录应按上限裁剪");
    require(store.logState().rows.size() == 2, "宿主日志应按上限裁剪");
    require(store.scriptState().rows.size() == 2, "脚本日志应按上限裁剪");
    require(store.requestTraceState().rows.size() == 3, "请求追踪应按上限裁剪");
    require(store.receiveState().rows.front().message == "raw-2", "原始收发记录应保留最近历史");
    require(store.receiveState().frameRows.front().message == "frame-2", "逐帧收发记录应保留最近历史");
    require(store.logState().rows.front().message == "host-3", "宿主日志应保留最近历史");
    require(store.scriptState().rows.front().message == "script-3", "脚本日志应保留最近历史");
    require(store.requestTraceState().rows.front().tag == "request-2", "请求追踪应保留最近历史");

    store.setHistoryLimits({
        .transferRawRows = 1,
        .transferFrameRows = 1,
        .hostLogRows = 1,
        .scriptLogRows = 1,
        .requestTraceRows = 1,
    });
    require(store.receiveState().rows.front().message == "raw-4", "调低原始记录上限应立即裁剪旧记录");
    require(store.receiveState().frameRows.front().message == "frame-5", "调低逐帧记录上限应立即裁剪旧记录");
    require(store.logState().rows.front().message == "host-4", "调低宿主日志上限应立即裁剪旧记录");
    require(store.scriptState().rows.front().message == "script-4", "调低脚本日志上限应立即裁剪旧记录");
    require(store.requestTraceState().rows.front().tag == "request-4", "调低请求追踪上限应立即裁剪旧记录");
}

void test_request_trace_filter_and_clear()
{
    protoscope::dock::DockStore store;
    store.appendRequestTraceRow({
        .timestampMs = 1,
        .id = 10,
        .kind = protoscope::dock::RequestTraceKind::Request,
        .state = protoscope::dock::RequestTraceState::Queued,
        .endpoint = "tcp://device",
        .tag = "read_version",
        .guarded = true,
        .guardState = "queued",
        .error = {},
    });
    store.appendRequestTraceRow({
        .timestampMs = 2,
        .id = 10,
        .kind = protoscope::dock::RequestTraceKind::Request,
        .state = protoscope::dock::RequestTraceState::Completed,
        .endpoint = "tcp://device",
        .tag = "read_version",
        .bytes = 4,
        .durationMs = 15,
        .guarded = true,
        .guardState = "active",
        .error = {},
    });
    store.appendRequestTraceRow({
        .timestampMs = 3,
        .id = 11,
        .kind = protoscope::dock::RequestTraceKind::Request,
        .state = protoscope::dock::RequestTraceState::Timeout,
        .endpoint = "tcp://device",
        .tag = "read_status",
        .guardState = {},
        .error = "等待 request_done 超时",
    });

    auto& trace = store.requestTraceState();
    trace.filter.keyword = "version";
    auto filtered = protoscope::dock::filteredRequestTraceRows(trace.rows, trace.filter);
    require(filtered.size() == 2, "请求追踪关键字应匹配 tag");

    trace.filter.keyword.clear();
    trace.filter.status = protoscope::dock::RequestTraceStatusFilter::All;
    filtered = protoscope::dock::filteredRequestTraceRows(trace.rows, trace.filter);
    require(filtered.size() == 3, "全部筛选应保留所有请求追踪记录");

    trace.filter.status = protoscope::dock::RequestTraceStatusFilter::Active;
    filtered = protoscope::dock::filteredRequestTraceRows(trace.rows, trace.filter);
    require(filtered.size() == 1, "进行中筛选应只保留排队和已发送记录");
    require(filtered[0]->state == protoscope::dock::RequestTraceState::Queued, "进行中筛选应保留排队记录");

    trace.filter.status = protoscope::dock::RequestTraceStatusFilter::Success;
    filtered = protoscope::dock::filteredRequestTraceRows(trace.rows, trace.filter);
    require(filtered.size() == 1, "成功筛选应只保留成功终态");
    require(filtered[0]->state == protoscope::dock::RequestTraceState::Completed, "成功筛选应保留完成记录");

    trace.filter.status = protoscope::dock::RequestTraceStatusFilter::Failure;
    filtered = protoscope::dock::filteredRequestTraceRows(trace.rows, trace.filter);
    require(filtered.size() == 1, "请求追踪失败筛选应只保留失败终态");
    require(filtered[0]->state == protoscope::dock::RequestTraceState::Timeout, "失败筛选应保留超时记录");

    std::vector<protoscope::dock::RequestTraceRow> exportRows;
    for (const auto* row : filtered) {
        exportRows.push_back(*row);
    }
    const auto filteredCsv = protoscope::dock::formatRequestTraceRowsCsv(exportRows, false);
    require(filteredCsv.find("read_status") != std::string::npos, "当前筛选导出数据应包含筛选后的请求");
    require(filteredCsv.find("read_version") == std::string::npos, "当前筛选导出数据不应包含被过滤的请求");

    const auto versionBeforeClear = trace.rowsVersion;
    store.clearRequestTraceRows();
    require(trace.rows.empty(), "请求追踪应可清空");
    require(trace.rowsVersion == versionBeforeClear + 1, "清空请求追踪应递增版本号");
}

void test_request_trace_csv_export_format()
{
    const std::vector<protoscope::dock::RequestTraceRow> rows{
        protoscope::dock::RequestTraceRow{
            .timestampMs = 0,
            .id = 0,
            .kind = protoscope::dock::RequestTraceKind::Send,
            .state = protoscope::dock::RequestTraceState::Queued,
            .endpoint = {},
            .tag = "tag, \"alpha\"",
            .bytes = 0,
            .durationMs = 0,
            .guardState = "guard\nstate",
            .error = {},
        },
        protoscope::dock::RequestTraceRow{
            .timestampMs = 1,
            .id = 42,
            .kind = protoscope::dock::RequestTraceKind::Request,
            .state = protoscope::dock::RequestTraceState::Failed,
            .endpoint = "tcp://device",
            .tag = {},
            .bytes = 7,
            .durationMs = 15,
            .attempt = 2,
            .maxAttempts = 3,
            .guardState = "ignored guard",
            .error = "error \"quoted\"\nnext",
        },
    };

    const auto headerOnly = protoscope::dock::formatRequestTraceRowsCsv({}, false);
    require(headerOnly == "\"ID\",\"类型\",\"状态\",\"Tag\",\"端点\",\"尝试\",\"字节\",\"耗时\",\"详情\"\n",
            "请求追踪空导出仍应写入 CSV 表头");

    const auto csv = protoscope::dock::formatRequestTraceRowsCsv(rows, false);
    require(csv.find("\"时间\"") == std::string::npos, "关闭时间列时 CSV 不应包含时间表头");
    require(csv.find("\"-\",\"send\",\"排队\",\"tag, \"\"alpha\"\"\",\"-\",\"1/1\",\"0\",\"-\",\"guard\nstate\"") !=
                std::string::npos,
            "CSV 应按表格显示值写入空 ID、空端点、排队耗时和转义后的 tag/详情");
    require(csv.find("ignored guard") == std::string::npos, "详情应优先使用 error 而不是 guardState");

    const auto rowCsv = protoscope::dock::formatRequestTraceRowCsv(rows[1], false);
    require(rowCsv == "\"42\",\"request\",\"失败\",\"-\",\"tcp://device\",\"2/3\",\"7\",\"15 ms\","
                      "\"error \"\"quoted\"\"\nnext\"",
            "单行 CSV 应复用导出字段顺序、耗时显示和引号/换行转义");

    const auto withTime = protoscope::dock::formatRequestTraceRowsCsv(rows, true);
    require(withTime.find("\"时间\",\"ID\",\"类型\"") == 0, "开启时间列时 CSV 表头应以时间列开头");
}

void test_dock_receive_row_single_line_hex_and_ascii()
{
    const protoscope::dock::ReceiveRow row{
        .timestampMs = 0,
        .direction = "RX",
        .endpoint = "tcp",
        .bytes = {0x41, 0x0A, 0x7F},
        .message = {},
    };

    const auto hexLine = protoscope::dock::formatReceiveRowSingleLine(row, false, true);
    require(hexLine == "RX tcp | 41 0A 7F", "HEX 单行展示不符合预期");

    const auto asciiLine = protoscope::dock::formatReceiveRowSingleLine(row, false, false);
    require(asciiLine == "RX tcp | A..", "ASCII 单行展示不符合预期");
}

void test_dock_receive_row_single_line_message_and_timestamp()
{
    const protoscope::dock::ReceiveRow row{
        .timestampMs = 0,
        .direction = "INFO",
        .endpoint = "host",
        .bytes = {0x41},
        .message = "first line\r\nsecond\tline",
    };

    const auto withTimestamp = protoscope::dock::formatReceiveRowSingleLine(row, true, true);
    require(withTimestamp.find("] INFO host | first line  second line") != std::string::npos,
            "带时间戳的消息单行展示不符合预期");
    require(withTimestamp.find('\n') == std::string::npos, "单行展示不应保留换行符");
    require(withTimestamp.find('\r') == std::string::npos, "单行展示不应保留回车符");

    const auto withoutTimestamp = protoscope::dock::formatReceiveRowSingleLine(row, false, true);
    require(withoutTimestamp == "INFO host | first line  second line", "关闭时间戳后的单行展示不符合预期");

    const auto headerOnly = protoscope::dock::formatReceiveRowSingleLine(
        protoscope::dock::ReceiveRow{.timestampMs = 0, .direction = "WARN", .endpoint = {}, .bytes = {}, .message = {}},
        false,
        false);
    require(headerOnly == "WARN", "空内容行不应追加多余分隔符");
}

void test_dock_receive_rows_text_export_format()
{
    const std::vector<protoscope::dock::ReceiveRow> rows{
        protoscope::dock::ReceiveRow{
            .timestampMs = 0,
            .direction = "RX",
            .endpoint = "tcp",
            .bytes = {0x41, 0x0A},
            .message = {},
        },
        protoscope::dock::ReceiveRow{
            .timestampMs = 1,
            .direction = "INFO",
            .endpoint = "host",
            .bytes = {},
            .message = "line1\nline2\r\n",
        },
        protoscope::dock::ReceiveRow{},
    };

    const auto hexText = protoscope::dock::formatReceiveRowsText(rows, false, true);
    require(hexText == "RX tcp | 41 0A\nINFO host | line1 line2  \n\n", "日志导出文本应保留行边界并压平消息换行");

    const auto asciiText = protoscope::dock::formatReceiveRowsText(rows, false, false);
    require(asciiText.find("RX tcp | A.") == 0, "ASCII 导出应复用当前 ASCII 视图格式");

    const auto timestampText = protoscope::dock::formatReceiveRowsText(rows, true, true);
    require(timestampText.find("[") == 0, "开启时间戳时导出文本应包含时间戳前缀");

    const auto emptyText = protoscope::dock::formatReceiveRowsText({}, true, true);
    require(emptyText.empty(), "空日志导出文本应为空字符串");
}

void test_dock_receive_row_visual_kind_classification()
{
    using protoscope::dock::classifyReceiveRow;
    using protoscope::dock::ReceiveRow;
    using protoscope::dock::ReceiveRowVisualKind;

    require(classifyReceiveRow(ReceiveRow{.direction = "RX"}) == ReceiveRowVisualKind::Rx, "RX 行应被分类为接收样式");
    require(classifyReceiveRow(ReceiveRow{.direction = "TX"}) == ReceiveRowVisualKind::Tx, "TX 行应被分类为发送样式");
    require(classifyReceiveRow(ReceiveRow{.direction = "WARN"}) == ReceiveRowVisualKind::Warn,
            "WARN 行应被分类为警告样式");
    require(classifyReceiveRow(ReceiveRow{.direction = "ERROR"}) == ReceiveRowVisualKind::Error,
            "ERROR 行应被分类为错误样式");
    require(classifyReceiveRow(ReceiveRow{.direction = "EVENT"}) == ReceiveRowVisualKind::Event,
            "Lua EVENT 行应被分类为事件样式");
    require(classifyReceiveRow(ReceiveRow{.direction = "LOG"}) == ReceiveRowVisualKind::ScriptLog,
            "Lua LOG 行应被分类为脚本日志样式");
    require(classifyReceiveRow(ReceiveRow{.direction = "unknown"}) == ReceiveRowVisualKind::Other,
            "未知方向应回退到通用样式");
}

void test_dock_send_history_deduplicates_and_trims()
{
    protoscope::dock::SendDockState send;

    protoscope::dock::rememberSendHistory(send, "AA 01", 3);
    protoscope::dock::rememberSendHistory(send, "BB 02", 3);
    protoscope::dock::rememberSendHistory(send, "CC 03", 3);
    protoscope::dock::rememberSendHistory(send, "AA 01", 3);
    protoscope::dock::rememberSendHistory(send, "DD 04", 3);

    require(send.history.size() == 3, "发送历史应按配置条数裁剪");
    require(send.history[0] == "DD 04", "最新发送内容应置顶");
    require(send.history[1] == "AA 01", "重复发送内容应去重后置顶");
    require(send.history[2] == "CC 03", "裁剪时应移除最旧历史");

    protoscope::dock::trimSendHistory(send, 2);
    require(send.history.size() == 2, "直接裁剪发送历史时应按配置条数保留");
    require(send.history[0] == "DD 04", "直接裁剪不应改变最新历史顺序");
    require(send.history[1] == "AA 01", "直接裁剪应丢弃末尾旧历史");

    protoscope::dock::rememberSendHistory(send, "EE 05", 0);
    require(send.history.empty(), "发送历史条数为 0 时应禁用并清空历史");
}

void test_log_filter_keeps_order_and_matches_status()
{
    std::vector<protoscope::dock::ReceiveRow> rows{
        {.timestampMs = 1, .direction = "RX", .endpoint = "usb", .bytes = {}, .message = "rx first"},
        {.timestampMs = 2, .direction = "TX", .endpoint = "usb", .bytes = {}, .message = "tx second"},
        {.timestampMs = 3, .direction = "rx", .endpoint = "usb", .bytes = {}, .message = "lowercase third"},
    };

    protoscope::dock::LogFilterState filter{};
    const auto allRows = protoscope::dock::filteredLogRows(rows, filter, true);
    require(allRows.size() == 3, "All 过滤应保留全部收发记录");
    require(allRows[0] == &rows[0] && allRows[1] == &rows[1] && allRows[2] == &rows[2], "All 过滤应保持原始顺序");

    filter.status = protoscope::dock::LogStatusFilter::Rx;
    const auto rxRows = protoscope::dock::filteredLogRows(rows, filter, true);
    require(rxRows.size() == 2, "Rx 过滤应保留大小写不同的 RX 记录");
    require(rxRows[0] == &rows[0] && rxRows[1] == &rows[2], "Rx 过滤应保持匹配行顺序");

    filter.status = protoscope::dock::LogStatusFilter::Tx;
    const auto txRows = protoscope::dock::filteredLogRows(rows, filter, true);
    require(txRows.size() == 1, "Tx 过滤应仅保留方向为 TX 的记录");
    require(txRows[0] == &rows[1], "Tx 过滤应只返回 TX 记录");
}

void test_log_filter_keyword_matches_metadata_and_bytes()
{
    std::vector<protoscope::dock::ReceiveRow> rows{
        {.timestampMs = 1, .direction = "INFO", .endpoint = "host", .bytes = {}, .message = "Lua runtime opened"},
        {.timestampMs = 2, .direction = "RX", .endpoint = "uart", .bytes = {0x41, 0x42}, .message = {}},
        {.timestampMs = 3, .direction = "WARN", .endpoint = "tcp", .bytes = {}, .message = "timeout"},
    };

    protoscope::dock::LogFilterState filter{.keyword = "runtime"};
    const auto messageRows = protoscope::dock::filteredLogRows(rows, filter, true);
    require(messageRows.size() == 1 && messageRows[0] == &rows[0], "关键字应匹配日志消息");

    filter.keyword = "UART";
    const auto endpointRows = protoscope::dock::filteredLogRows(rows, filter, true);
    require(endpointRows.size() == 1 && endpointRows[0] == &rows[1], "关键字应大小写不敏感匹配端点");

    filter.keyword = "41 42";
    const auto hexRows = protoscope::dock::filteredLogRows(rows, filter, true);
    require(hexRows.size() == 1 && hexRows[0] == &rows[1], "收发筛选应匹配 HEX 字节内容");

    const auto hostRows = protoscope::dock::filteredLogRows(rows, filter, false);
    require(hostRows.empty(), "宿主/脚本日志筛选不应匹配字节预览");
}

void test_log_filter_combines_status_and_keyword()
{
    std::vector<protoscope::dock::ReceiveRow> rows{
        {.timestampMs = 1, .direction = "WARN", .endpoint = "host", .bytes = {}, .message = "timeout on tcp"},
        {.timestampMs = 2, .direction = "ERROR", .endpoint = "host", .bytes = {}, .message = "timeout on lua"},
        {.timestampMs = 3, .direction = "WARN", .endpoint = "host", .bytes = {}, .message = "reconnected"},
    };

    const protoscope::dock::LogFilterState filter{
        .keyword = "timeout",
        .status = protoscope::dock::LogStatusFilter::Warn,
    };
    const auto rowsAfterFilter = protoscope::dock::filteredLogRows(rows, filter, false);
    require(rowsAfterFilter.size() == 1, "STATUS 与关键字应同时生效");
    require(rowsAfterFilter[0] == &rows[0], "组合筛选应只保留同时匹配的 WARN timeout 日志");
}

void test_wave_protocol_state_isolated_by_protocol_key()
{
    YAML::Node root;

    protoscope::plot::WaveDockState waveA;
    waveA.buffer.configureChannels(1);
    waveA.buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = 1.0, .offset = 0.0});
    waveA.view.showHoverReadout = false;
    waveA.view.preferWaveformHoverReadout = false;
    waveA.view.sampleFrequencyHz = 2048.0;
    waveA.view.sampleFrequencyInput = "2048";
    waveA.view.fft.enabled = true;
    waveA.view.fft.displayMode = protoscope::plot::WaveFftDisplayMode::CursorSplit;
    waveA.view.fft.pointCount = protoscope::plot::WaveFftPointCount::N1024;
    waveA.view.fft.window = protoscope::plot::WaveFftWindow::BlackmanHarris;
    waveA.view.fft.magnitudeMode = protoscope::plot::WaveFftMagnitudeMode::Decibel;
    waveA.view.fft.fundamentalMode = protoscope::plot::WaveFftFundamentalMode::Manual;
    waveA.view.fft.manualFundamentalHz = 50.0;
    waveA.view.showFftLegend = false;
    waveA.view.fftSourceWindowValid = true;
    waveA.view.fftSourceMinTime = 0.25;
    waveA.view.fftSourceMaxTime = 0.75;
    waveA.view.fftFrequencyMin = 10.0;
    waveA.view.fftFrequencyMax = 500.0;
    waveA.view.fftMagnitudeMin = -80.0;
    waveA.view.fftMagnitudeMax = 5.0;
    waveA.view.fftPhaseMin = -120.0;
    waveA.view.fftPhaseMax = 120.0;
    waveA.view.measurement.variance = true;
    waveA.view.measurement.cv = true;
    waveA.view.measurement.stddev = false;
    waveA.view.measurement.rmse = true;
    waveA.view.referenceMode = protoscope::plot::WaveMeasurementReferenceMode::ManualValue;
    waveA.view.referenceChannelIndex = 2;
    waveA.view.manualReferenceValue = 1.25;
    waveA.fftChannelEnabled = {1};
    waveA.hiddenChannelLabels = {"CH1"};
    waveA.toolsCollapsed = true;
    waveA.legendCollapsed = true;
    waveA.channelOverrides.resize(1);
    waveA.channelOverrides[0].labelOverridden = true;
    waveA.channelOverrides[0].label = "总线A";
    waveA.channelOverrides[0].scaleOverridden = true;
    waveA.channelOverrides[0].scale = 2.5;
    waveA.channelOverrides[0].offsetOverridden = true;
    waveA.channelOverrides[0].offset = -0.25;
    waveA.analysisMarkers = {{
        .id = 1001,
        .label = "标记A",
        .note = "协议A波形标记",
        .startTime = 0.125,
        .endTime = 0.250,
        .channelIndex = 0,
    }};
    protoscope::ui::storeWaveProtocolState(root, "proto_a", waveA);

    protoscope::plot::WaveDockState waveB;
    waveB.buffer.configureChannels(1);
    waveB.buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = 1.0, .offset = 0.0});
    waveB.view.showHoverReadout = true;
    waveB.view.sampleFrequencyHz = 512.0;
    waveB.view.sampleFrequencyInput = "512";
    waveB.channelOverrides.resize(1);
    waveB.channelOverrides[0].labelOverridden = true;
    waveB.channelOverrides[0].label = "总线B";
    waveB.channelOverrides[0].scaleOverridden = true;
    waveB.channelOverrides[0].scale = 0.5;
    protoscope::ui::storeWaveProtocolState(root, "proto_b", waveB);

    protoscope::plot::WaveDockState restoredA;
    restoredA.buffer.configureChannels(1);
    restoredA.buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = 1.0, .offset = 0.0});
    protoscope::ui::restoreWaveProtocolState(root, "proto_a", restoredA);

    const auto restoredASpec = restoredA.buffer.channelSpec(0);
    require(restoredASpec.has_value(), "proto_a 恢复后应保留通道配置");
    require(restoredASpec->label == "总线A", "proto_a 应恢复自己的标签覆盖");
    require(restoredASpec->scale == 2.5, "proto_a 应恢复自己的缩放覆盖");
    require(restoredASpec->offset == -0.25, "proto_a 应恢复自己的偏移覆盖");
    require(restoredA.view.sampleFrequencyHz == 2048.0, "proto_a 应恢复自己的采样频率");
    require(restoredA.view.fft.enabled, "proto_a 应恢复 FFT 开关");
    require(restoredA.view.fft.displayMode == protoscope::plot::WaveFftDisplayMode::CursorSplit,
            "proto_a 应恢复 FFT 显示模式");
    require(restoredA.view.fft.pointCount == protoscope::plot::WaveFftPointCount::N1024, "proto_a 应恢复 FFT 点数");
    require(restoredA.view.fft.window == protoscope::plot::WaveFftWindow::BlackmanHarris, "proto_a 应恢复 FFT 窗函数");
    require(restoredA.view.fft.magnitudeMode == protoscope::plot::WaveFftMagnitudeMode::Decibel,
            "proto_a 应恢复 FFT 幅值模式");
    require(restoredA.view.fft.fundamentalMode == protoscope::plot::WaveFftFundamentalMode::Manual,
            "proto_a 应恢复 FFT 基波模式");
    require(restoredA.view.fft.manualFundamentalHz == 50.0, "proto_a 应恢复手动基波频率");
    require(!restoredA.view.showFftLegend, "proto_a 应恢复 FFT 图例显示状态");
    require(restoredA.view.fftSourceWindowValid, "proto_a 应恢复 FFT 输入窗口状态");
    require(restoredA.view.fftSourceMinTime == 0.25 && restoredA.view.fftSourceMaxTime == 0.75,
            "proto_a 应恢复 FFT 输入窗口");
    require(restoredA.view.fftFrequencyMin == 10.0 && restoredA.view.fftFrequencyMax == 500.0,
            "proto_a 应恢复 FFT 频率轴");
    require(restoredA.view.fftMagnitudeMin == -80.0 && restoredA.view.fftMagnitudeMax == 5.0,
            "proto_a 应恢复 FFT 幅值轴");
    require(restoredA.view.fftPhaseMin == -120.0 && restoredA.view.fftPhaseMax == 120.0, "proto_a 应恢复 FFT 相位轴");
    require(restoredA.view.measurement.variance && restoredA.view.measurement.cv && !restoredA.view.measurement.stddev,
            "proto_a 应恢复测量项选择");
    require(restoredA.view.measurement.rmse, "proto_a 应恢复误差测量项选择");
    require(restoredA.view.referenceMode == protoscope::plot::WaveMeasurementReferenceMode::ManualValue,
            "proto_a 应恢复测量参考模式");
    require(restoredA.view.referenceChannelIndex == 2, "proto_a 应恢复参考通道");
    require(restoredA.view.manualReferenceValue == 1.25, "proto_a 应恢复手动标定值");
    require(restoredA.fftChannelEnabled.size() == 1 && restoredA.fftChannelEnabled[0] == 1,
            "proto_a 应恢复 FFT 通道选择");
    require(restoredA.hiddenChannelLabels.size() == 1 && restoredA.hiddenChannelLabels[0] == "CH1",
            "proto_a 应恢复自己的主图 Legend 隐藏通道");
    require(restoredA.toolsCollapsed, "proto_a 应恢复自己的工具栏折叠状态");
    require(restoredA.legendCollapsed, "proto_a 应恢复自己的图例折叠状态");
    require(!restoredA.view.showHoverReadout, "proto_a 应恢复自己的显示开关");
    require(!restoredA.view.preferWaveformHoverReadout, "proto_a 应恢复自己的悬浮读数优先级策略");
    require(restoredA.analysisMarkers.size() == 1 && restoredA.analysisMarkers[0].label == "标记A",
            "proto_a 应恢复自己的分析标记");

    protoscope::plot::WaveDockState restoredB;
    restoredB.buffer.configureChannels(1);
    restoredB.buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = 1.0, .offset = 0.0});
    protoscope::ui::restoreWaveProtocolState(root, "proto_b", restoredB);

    const auto restoredBSpec = restoredB.buffer.channelSpec(0);
    require(restoredBSpec.has_value(), "proto_b 恢复后应保留通道配置");
    require(restoredB.hiddenChannelLabels.empty(), "不同协议不应串用 proto_a 的主图 Legend 隐藏通道");
    require(!restoredB.legendCollapsed, "不同协议默认不应继承 proto_a 的图例折叠状态");
    require(restoredBSpec->label == "总线B", "不同协议不应串用 proto_a 标签");
    require(restoredBSpec->scale == 0.5, "不同协议不应串用 proto_a 缩放");
    require(restoredB.view.sampleFrequencyHz == 512.0, "不同协议不应串用 proto_a 采样频率");
    require(!restoredB.view.fft.enabled, "不同协议不应串用 proto_a FFT 开关");
    require(restoredB.view.fft.displayMode == protoscope::plot::WaveFftDisplayMode::FullSpectrum,
            "不同协议应保留默认完整频谱显示模式");
    require(restoredB.analysisMarkers.empty(), "不同协议不应串用 proto_a 分析标记");
    require(restoredB.view.measurement.stddev && !restoredB.view.measurement.variance,
            "老状态或其他协议应保留默认测量项");
}

void test_wave_protocol_state_missing_wave_node_clears_analysis_markers()
{
    YAML::Node root;
    root["protocols"]["other"]["wave"]["show_hover_readout"] = true;

    protoscope::plot::WaveDockState wave;
    wave.hiddenChannelLabels = {"CH1"};
    wave.analysisMarkers = {{
        .id = 1,
        .label = "残留标记",
        .note = "旧协议",
        .startTime = 0.0,
        .endTime = 1.0,
        .channelIndex = 0,
    }};

    protoscope::ui::restoreWaveProtocolState(root, "missing", wave);
    require(wave.hiddenChannelLabels.empty(), "缺失协议 wave 节点时应清空隐藏通道状态");
    require(wave.analysisMarkers.empty(), "缺失协议 wave 节点时应清空分析标记");
    require(wave.legendVisibilityRestorePending, "缺失协议 wave 节点时应重新应用图例可见性");
}

void test_wave_protocol_state_cursor_extreme_snap_policy()
{
    protoscope::plot::WaveDockState wave;
    wave.view.cursorExtremeSnapPolicy = protoscope::plot::WaveCursorExtremeSnapPolicy::ViewportZone;

    const auto encoded = protoscope::ui::encodeWaveProtocolState(wave);
    require(encoded["cursor_extreme_snap_policy"].as<std::string>() == "viewport_zone",
            "协议 UI 状态应写出游标极值吸附策略");

    protoscope::plot::WaveDockState restored;
    protoscope::ui::decodeWaveProtocolState(encoded, restored);
    require(restored.view.cursorExtremeSnapPolicy == protoscope::plot::WaveCursorExtremeSnapPolicy::ViewportZone,
            "协议 UI 状态应恢复 viewport_zone 策略");

    const auto legacy = YAML::Load("cursor_snap_mode: smart\ncursor_snap_scope: all_channels\n");
    protoscope::plot::WaveDockState legacyRestored;
    protoscope::ui::decodeWaveProtocolState(legacy, legacyRestored);
    require(
        legacyRestored.view.cursorExtremeSnapPolicy == protoscope::plot::WaveCursorExtremeSnapPolicy::NearestWaveform,
        "缺失游标极值吸附策略时应使用 nearest_waveform 默认值");
}

void test_wave_protocol_state_prefer_waveform_hover_readout_defaults_true()
{
    protoscope::plot::WaveDockState wave;
    wave.view.preferWaveformHoverReadout = false;
    wave.view.bitDisplayReadoutPolicy = protoscope::plot::WaveBitDisplayReadoutPolicy::ExplicitActivation;

    const auto encoded = protoscope::ui::encodeWaveProtocolState(wave);
    require(!encoded["prefer_waveform_hover_readout"].as<bool>(),
            "协议 UI 状态应写出 waveform hover 优先级策略");
    require(encoded["bit_display_readout_policy"].as<std::string>() == "explicit_activation",
            "协议 UI 状态应写出 bit display 读数策略");

    protoscope::plot::WaveDockState restored;
    protoscope::ui::decodeWaveProtocolState(encoded, restored);
    require(!restored.view.preferWaveformHoverReadout, "协议 UI 状态应恢复 false 策略");
    require(restored.view.bitDisplayReadoutPolicy == protoscope::plot::WaveBitDisplayReadoutPolicy::ExplicitActivation,
            "协议 UI 状态应恢复 bit display 读数策略");

    const auto legacy = YAML::Load("show_hover_readout: true\n");
    protoscope::plot::WaveDockState legacyRestored;
    legacyRestored.view.preferWaveformHoverReadout = true;
    protoscope::ui::decodeWaveProtocolState(legacy, legacyRestored);
    require(legacyRestored.view.preferWaveformHoverReadout, "旧状态缺字段时应保持默认 true");
    require(legacyRestored.view.bitDisplayReadoutPolicy == protoscope::plot::WaveBitDisplayReadoutPolicy::MixedNearest,
            "旧状态缺 bit display 读数策略时应保持 mixed_nearest 默认值");
}

void test_wave_protocol_state_view_mode_legend_overlay_and_color_override()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.buffer.setChannelSpec(0,
                               {.label = "CH1",
                                .unit = "V",
                                .color = std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F}});
    wave.view.viewMode = protoscope::plot::WaveViewMode::Split;
    wave.legendOverlay.expanded = true;
    wave.legendOverlay.offsetX = 24.0F;
    wave.legendOverlay.offsetY = 36.0F;
    wave.channelOverrides.resize(1);
    wave.channelOverrides[0].colorOverridden = true;
    wave.channelOverrides[0].color = std::array<float, 4>{0.8F, 0.7F, 0.6F, 1.0F};

    const auto encoded = protoscope::ui::encodeWaveProtocolState(wave);
    require(encoded["view_mode"].as<std::string>() == "split", "协议 UI 状态应写出分屏视图模式");
    require(encoded["legend_overlay"]["expanded"].as<bool>(), "协议 UI 状态应写出图例展开状态");
    require(encoded["legend_overlay"]["offset_x"].as<float>() == 24.0F, "协议 UI 状态应写出图例 X 偏移");
    require(encoded["channel_overrides"][0]["color_overridden"].as<bool>(), "协议 UI 状态应写出颜色覆盖标记");
    require(encoded["channel_overrides"][0]["color"].size() == 4U, "协议 UI 状态应写出 RGBA 颜色");

    protoscope::plot::WaveDockState restored;
    restored.buffer.configureChannels(1);
    restored.buffer.setChannelSpec(0,
                                   {.label = "CH1",
                                    .unit = "V",
                                    .color = std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F}});
    protoscope::ui::decodeWaveProtocolState(encoded, restored);
    const auto restoredSpec = restored.buffer.channelSpec(0);
    require(restored.view.viewMode == protoscope::plot::WaveViewMode::Split, "协议 UI 状态应恢复分屏模式");
    require(restored.legendOverlay.expanded && restored.legendOverlay.offsetX == 24.0F &&
                restored.legendOverlay.offsetY == 36.0F,
            "协议 UI 状态应恢复图例 overlay 状态");
    require(restoredSpec.has_value() && restoredSpec->color.has_value() && (*restoredSpec->color)[0] == 0.8F,
            "协议 UI 状态应把颜色覆盖应用到通道规格");

    const auto legacy = YAML::Load("view_mode: unknown\n");
    protoscope::plot::WaveDockState legacyRestored;
    protoscope::ui::decodeWaveProtocolState(legacy, legacyRestored);
    require(legacyRestored.view.viewMode == protoscope::plot::WaveViewMode::Overlay,
            "非法视图模式应回退 overlay");
    require(legacyRestored.legendOverlay.offsetX == 8.0F && legacyRestored.legendOverlay.offsetY == 8.0F,
            "旧状态缺图例 overlay 字段时应使用左上角默认值");
}

void test_dock_visibility_state_isolated_by_protocol_key()
{
    YAML::Node root;

    protoscope::ui::ProtocolDockVisibilityState protoA;
    protoA.showCommDock = false;
    protoA.showProtocolDock = true;
    protoA.showTransferDock = false;
    protoA.showRequestTraceDock = false;
    protoA.showOfflineReplayDock = false;
    protoA.showLogDock = true;
    protoA.showScriptDock = false;
    protoA.showWaveDock = true;
    protoA.luaDockVisibility["LuaDock:proto_a:main"] = false;
    protoA.luaDockVisibility["LuaDock:proto_a:advanced"] = true;
    protoscope::ui::storeDockVisibilityState(root, "proto_a", protoA);

    protoscope::ui::ProtocolDockVisibilityState protoB;
    protoB.showCommDock = true;
    protoB.showProtocolDock = false;
    protoB.showTransferDock = true;
    protoB.showRequestTraceDock = true;
    protoB.showOfflineReplayDock = true;
    protoB.showLogDock = false;
    protoB.showScriptDock = true;
    protoB.showWaveDock = false;
    protoB.luaDockVisibility["LuaDock:proto_b:main"] = true;
    protoscope::ui::storeDockVisibilityState(root, "proto_b", protoB);

    protoscope::ui::ProtocolDockVisibilityState restoredA;
    protoscope::ui::restoreDockVisibilityState(root, "proto_a", restoredA);
    require(!restoredA.showCommDock, "proto_a 应恢复自己的通讯配置可见性");
    require(restoredA.showProtocolDock, "proto_a 应恢复自己的协议脚本可见性");
    require(!restoredA.showTransferDock, "proto_a 应恢复自己的收发可见性");
    require(!restoredA.showRequestTraceDock, "proto_a 应恢复自己的请求追踪可见性");
    require(!restoredA.showOfflineReplayDock, "proto_a 应恢复自己的离线复现可见性");
    require(restoredA.showLogDock, "proto_a 应恢复自己的日志可见性");
    require(!restoredA.showScriptDock, "proto_a 应恢复自己的脚本可见性");
    require(restoredA.showWaveDock, "proto_a 应恢复自己的波形可见性");
    require(!restoredA.luaDockVisibility["LuaDock:proto_a:main"], "proto_a 应恢复 Lua Dock 关闭状态");
    require(restoredA.luaDockVisibility["LuaDock:proto_a:advanced"], "proto_a 应恢复 Lua Dock 打开状态");
    require(!restoredA.luaDockVisibility.contains("LuaDock:proto_b:main"),
            "proto_a 不应串用 proto_b 的 Lua Dock 可见性");

    protoscope::ui::ProtocolDockVisibilityState restoredB;
    protoscope::ui::restoreDockVisibilityState(root, "proto_b", restoredB);
    require(restoredB.showCommDock, "proto_b 应恢复自己的通讯配置可见性");
    require(!restoredB.showProtocolDock, "proto_b 应恢复自己的协议脚本可见性");
    require(restoredB.showTransferDock, "proto_b 应恢复自己的收发可见性");
    require(restoredB.showRequestTraceDock, "proto_b 应恢复自己的请求追踪可见性");
    require(restoredB.showOfflineReplayDock, "proto_b 应恢复自己的离线复现可见性");
    require(!restoredB.showLogDock, "proto_b 应恢复自己的日志可见性");
    require(restoredB.showScriptDock, "proto_b 应恢复自己的脚本可见性");
    require(!restoredB.showWaveDock, "proto_b 应恢复自己的波形可见性");
    require(restoredB.luaDockVisibility["LuaDock:proto_b:main"], "proto_b 应恢复 Lua Dock 打开状态");
    require(!restoredB.luaDockVisibility.contains("LuaDock:proto_a:main"),
            "proto_b 不应串用 proto_a 的 Lua Dock 可见性");
}

void test_dock_visibility_state_decode_missing_fields_defaults()
{
    protoscope::ui::ProtocolDockVisibilityState state;
    state.showCommDock = false;
    state.luaDockVisibility["LuaDock:before:stale"] = false;

    const YAML::Node emptyNode;
    protoscope::ui::decodeDockVisibilityState(emptyNode, state);
    require(!state.showCommDock, "空节点解码不应覆盖现有状态");
    require(!state.luaDockVisibility["LuaDock:before:stale"], "空节点解码不应清空现有 Lua 可见性");

    YAML::Node partialNode;
    partialNode["static"]["log"] = false;
    partialNode["lua"]["LuaDock:demo:panel"] = false;
    protoscope::ui::ProtocolDockVisibilityState defaultState;
    protoscope::ui::decodeDockVisibilityState(partialNode, defaultState);
    require(defaultState.showCommDock, "缺失的 comm 字段应保持默认可见");
    require(defaultState.showProtocolDock, "缺失的 protocol 字段应保持默认可见");
    require(defaultState.showTransferDock, "缺失的 transfer 字段应保持默认可见");
    require(defaultState.showRequestTraceDock, "缺失的 request_trace 字段应保持默认可见");
    require(defaultState.showOfflineReplayDock, "缺失的 offline_replay 字段应保持默认可见");
    require(!defaultState.showLogDock, "存在的 log 字段应按配置恢复");
    require(defaultState.showScriptDock, "缺失的 script 字段应保持默认可见");
    require(defaultState.showWaveDock, "缺失的 wave 字段应保持默认可见");
    require(!defaultState.luaDockVisibility["LuaDock:demo:panel"], "Lua 可见性字段应按配置恢复");
}
