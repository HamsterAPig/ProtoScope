#include "protoscope/dock/docks.hpp"
#include "protoscope/ui/protocol_ui_state.hpp"

#include "test_registry.hpp"

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
    require(store.receiveState().rows.front().message == "raw-2", "原始收发记录应保留最近历史");
    require(store.receiveState().frameRows.front().message == "frame-2", "逐帧收发记录应保留最近历史");
    require(store.logState().rows.front().message == "host-3", "宿主日志应保留最近历史");
    require(store.scriptState().rows.front().message == "script-3", "脚本日志应保留最近历史");

    store.setHistoryLimits({
        .transferRawRows = 1,
        .transferFrameRows = 1,
        .hostLogRows = 1,
        .scriptLogRows = 1,
    });
    require(store.receiveState().rows.front().message == "raw-4", "调低原始记录上限应立即裁剪旧记录");
    require(store.receiveState().frameRows.front().message == "frame-5", "调低逐帧记录上限应立即裁剪旧记录");
    require(store.logState().rows.front().message == "host-4", "调低宿主日志上限应立即裁剪旧记录");
    require(store.scriptState().rows.front().message == "script-4", "调低脚本日志上限应立即裁剪旧记录");
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
    waveA.view.sampleFrequencyHz = 2048.0;
    waveA.view.sampleFrequencyInput = "2048";
    waveA.view.fft.enabled = true;
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
    require(restoredB.view.measurement.stddev && !restoredB.view.measurement.variance,
            "老状态或其他协议应保留默认测量项");
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

void test_dock_visibility_state_isolated_by_protocol_key()
{
    YAML::Node root;

    protoscope::ui::ProtocolDockVisibilityState protoA;
    protoA.showCommDock = false;
    protoA.showProtocolDock = true;
    protoA.showTransferDock = false;
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
    require(!defaultState.showLogDock, "存在的 log 字段应按配置恢复");
    require(defaultState.showScriptDock, "缺失的 script 字段应保持默认可见");
    require(defaultState.showWaveDock, "缺失的 wave 字段应保持默认可见");
    require(!defaultState.luaDockVisibility["LuaDock:demo:panel"], "Lua 可见性字段应按配置恢复");
}
