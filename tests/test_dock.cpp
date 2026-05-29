#include "test_registry.hpp"

#include "protoscope/dock/docks.hpp"
#include "protoscope/ui/protocol_ui_state.hpp"

#include <stdexcept>
#include <yaml-cpp/yaml.h>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

void test_config_external_reload_state() {
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

void test_dock_log_and_script_split() {
    protoscope::dock::DockStore store;

    store.appendReceiveRow({.timestampMs = 1, .direction = "RX", .endpoint = "tcp", .bytes = {0x01}, .message = {}});
    store.appendLogRow({.timestampMs = 2, .direction = "INFO", .endpoint = "host", .bytes = {}, .message = "saved"});
    store.appendLuaEvent({.name = "frame", .payload = "{status=ok}", .timestampMs = 3});
    store.appendScriptRow({.timestampMs = 4, .direction = "LOG", .endpoint = "script", .bytes = {}, .message = "[info] opened"});

    require(store.receiveState().rows.size() == 1, "接收区应只保留 TX/RX 原始数据");
    require(store.logState().rows.size() == 1, "日志区应独立记录宿主日志");
    require(store.scriptState().rows.size() == 2, "脚本区应记录 Lua 事件与脚本日志");
    require(store.scriptState().rows[0].direction == "EVENT", "Lua 事件应写入脚本区");

    store.clearLogRows();
    store.clearScriptRows();
    require(store.logState().rows.empty(), "日志区应可单独清空");
    require(store.scriptState().rows.empty(), "脚本区应可单独清空");
}

void test_dock_receive_row_single_line_hex_and_ascii() {
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

void test_dock_receive_row_single_line_message_and_timestamp() {
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

void test_wave_protocol_state_isolated_by_protocol_key() {
    YAML::Node root;

    protoscope::plot::WaveDockState waveA;
    waveA.buffer.configureChannels(1);
    waveA.buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = 1.0, .offset = 0.0});
    waveA.view.showHoverReadout = false;
    waveA.view.sampleFrequencyHz = 2048.0;
    waveA.view.sampleFrequencyInput = "2048";
    waveA.toolsCollapsed = true;
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
    require(restoredA.toolsCollapsed, "proto_a 应恢复自己的工具栏折叠状态");
    require(!restoredA.view.showHoverReadout, "proto_a 应恢复自己的显示开关");

    protoscope::plot::WaveDockState restoredB;
    restoredB.buffer.configureChannels(1);
    restoredB.buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = 1.0, .offset = 0.0});
    protoscope::ui::restoreWaveProtocolState(root, "proto_b", restoredB);

    const auto restoredBSpec = restoredB.buffer.channelSpec(0);
    require(restoredBSpec.has_value(), "proto_b 恢复后应保留通道配置");
    require(restoredBSpec->label == "总线B", "不同协议不应串用 proto_a 标签");
    require(restoredBSpec->scale == 0.5, "不同协议不应串用 proto_a 缩放");
    require(restoredB.view.sampleFrequencyHz == 512.0, "不同协议不应串用 proto_a 采样频率");
}
