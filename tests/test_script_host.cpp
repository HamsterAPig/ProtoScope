#include "test_registry.hpp"

#include "protoscope/config/config.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

protoscope::transport::ConnectionContext sampleCtx() {
    return protoscope::transport::ConnectionContext{
        .kind = protoscope::transport::TransportKind::TcpClient,
        .endpoint = "127.0.0.1:9000",
        .connectionId = 42,
        .timestampMs = nowMs(),
    };
}

std::filesystem::path fixtureProtocolDir(const char* name) {
    return std::filesystem::path("tests/fixtures/protocols") / name;
}

} // namespace

void test_script_controls_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto controls = host.controlsSnapshot();
    require(!controls.empty(), "controls 不能为空");
    require(controls[0].id == "read_version", "第一个控件应为 read_version");
    require(controls.size() >= 6, "默认协议脚本应暴露完整控件集合");
}

void test_script_on_open_log() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});
    bool foundOpenLog = false;
    for (const auto& log : host.drainLogs()) {
        if (log.message.find("连接已打开") != std::string::npos) {
            foundOpenLog = true;
        }
    }
    require(foundOpenLog, "on_open 应写入打开日志");
}

void test_script_on_close_log() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportClose(protoscope::transport::TransportCloseEvent{ctx, "manual_close"});
    bool foundCloseLog = false;
    for (const auto& log : host.drainLogs()) {
        if (log.message.find("连接已关闭") != std::string::npos) {
            foundCloseLog = true;
        }
    }
    require(foundCloseLog, "on_close 应写入关闭日志");
}

void test_script_on_error_log() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportError(protoscope::transport::TransportErrorEvent{ctx, "socket_failure"});
    bool foundErrorLog = false;
    const auto logs = host.drainLogs();
    for (const auto& log : logs) {
        if (log.message.find("连接错误") != std::string::npos) {
            foundErrorLog = true;
        }
    }
    if (!foundErrorLog) {
        std::ostringstream message;
        message << "on_error 应写入异常日志，实际日志条数=" << logs.size();
        for (const auto& log : logs) {
            message << " [" << log.level << "] " << log.message;
        }
        throw std::runtime_error(message.str());
    }
}

void test_script_multi_dock_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("multi_dock").generic_string()), "multi_dock 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 2, "多 Dock 协议应产出两个 dock");
    require(docks[0].descriptor.id == "protocol", "第一个 dock id 不正确");
    require(docks[1].descriptor.title == "高级参数", "第二个 dock 标题不正确");

    const auto controls = host.controlsSnapshot();
    bool foundReadVersionButton = false;
    for (const auto& control : controls) {
        if (control.type == protoscope::scripting::ControlType::Button && control.id == "read_version") {
            foundReadVersionButton = true;
            break;
        }
    }
    require(foundReadVersionButton, "应存在 read_version 按钮控件");
}

void test_script_dock_layout_fields() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("multi_dock").generic_string()), "multi_dock 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 2, "多 Dock 协议应产出两个 dock");
    require(docks[0].descriptor.anchor == "left_bottom", "第一个 dock 应解析 anchor");
    require(docks[1].descriptor.anchor == "left_bottom", "第二个 dock 应解析 anchor");
    require(docks[0].descriptor.tabGroup == "protocol_tools", "第一个 dock 应解析 tab_group");
    require(docks[1].descriptor.tabGroup == "protocol_tools", "第二个 dock 应解析 tab_group");
    require(!docks[0].descriptor.layout.has_value(), "未声明 layout 的 dock 应保持旧 flow 行为");
    require(!docks[1].descriptor.layout.has_value(), "未声明 layout 的 dock 应保持旧 flow 行为");
}

void test_script_table_layout_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("table_layout").generic_string()), "table_layout 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "table_layout 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "table_layout 应解析 layout");
    require(docks[0].descriptor.layout->kind == protoscope::scripting::DockLayoutKind::Table, "layout.kind 应为 table");

    const auto& table = docks[0].descriptor.layout->table;
    require(table.columns == 2, "table_layout 列数应为 2");
    require(table.rows.size() == 3, "table_layout 应解析三行");
    require(table.rows[0].cells.size() == 2, "第一行应有两列");
    require(table.rows[0].cells[0].controlId == "device_id", "第一行第一列应绑定 device_id");
    require(table.rows[1].cells[1].spacer, "第二行第二列应为 spacer");
    require(table.rows[2].cells[0].controlId == "read_version", "第三行第一列应绑定 read_version");
}

void test_script_form_layout_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("form_layout").generic_string()), "form_layout 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "form_layout 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "form_layout 应解析 layout");
    require(docks[0].descriptor.layout->kind == protoscope::scripting::DockLayoutKind::Form, "layout.kind 应为 form");

    const auto& items = docks[0].descriptor.layout->form.items;
    require(items.size() == 5, "form_layout 应解析 5 个顶层布局项");
    require(items[0].kind == protoscope::scripting::FormLayoutItemKind::Text, "第一项应为 text");
    require(items[1].kind == protoscope::scripting::FormLayoutItemKind::Controls, "第二项应为 controls");
    require(items[1].controls.controlIds.size() == 2, "controls 应包含两个控件");
    require(items[1].controls.controlIds[0] == "read_version", "同排第一个控件顺序错误");
    require(items[1].controls.controlIds[1] == "device_id", "同排第二个控件顺序错误");
    require(items[2].kind == protoscope::scripting::FormLayoutItemKind::Separator, "第三项应为 separator");
    require(items[3].kind == protoscope::scripting::FormLayoutItemKind::Group, "第四项应为 group");
    require(items[3].group && items[3].group->items.size() == 1, "group 内应有 1 个子项");
    require(items[3].group->items[0].controls.controlIds[0] == "hex_send", "group 内第一个控件顺序错误");
    require(items[3].group->items[0].controls.controlIds[1] == "mode", "group 内第二个控件顺序错误");
    require(items[4].kind == protoscope::scripting::FormLayoutItemKind::Collapse, "第五项应为 collapse");
    require(items[4].collapse && !items[4].collapse->defaultOpen, "collapse 默认展开状态解析错误");
    require(items[4].collapse->items[0].controls.controlIds[0] == "timeout_ms", "collapse 内第一个控件顺序错误");
    require(items[4].collapse->items[0].controls.controlIds[1] == "scale", "collapse 内第二个控件顺序错误");
}

void test_script_crc_bridge() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("crc_probe").generic_string()), "crc_probe 协议应可加载");

    const auto ctx = sampleCtx();
    host.onControl(ctx, "probe_crc", true);

    bool found = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "crc") {
            if (event.payload.empty()) {
                throw std::runtime_error("crc payload 为空");
            }
            found = event.payload.find("modbus=") != std::string::npos &&
                    event.payload.find("ccitt=") != std::string::npos &&
                    event.payload.find("ieee=") != std::string::npos;
            if (!found) {
                throw std::runtime_error("crc payload=" + event.payload);
            }
        }
    }
    require(found, "CRC 桥接结果不正确");
}

void test_script_read_version_flow() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "device_id", std::string("01"));
    host.onControl(ctx, "read_version", true);

    const auto requests = host.drainTxRequests();
    require(requests.size() == 1, "read_version 应产生一次请求");
    require(requests[0].kind == protoscope::scripting::TxRequestKind::Request, "read_version 应走 proto.request");
    require(requests[0].payload.size() >= 4, "发送帧长度不正确");

    const std::vector<std::uint8_t> response{'O', 'K', '\r', '\n'};
    host.setRequestAwaitingCompletion(true);
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, response});

    bool foundFrame = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "frame") {
            foundFrame = true;
        }
    }
    require(foundFrame, "收到 OK 响应后应产生 frame 事件");
    require(!host.drainRequestDoneResults().empty(), "收到完整帧后应调用 proto.request_done");
}

void test_script_read_version_split_flow() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "read_version", true);

    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, {'O'}});
    bool foundFrameEarly = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "frame") {
            foundFrameEarly = true;
        }
    }
    require(!foundFrameEarly, "首包只有 O 时不应提前产出 frame");

    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, {'K', '\r', '\n'}});
    bool foundFrame = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "frame") {
            foundFrame = true;
        }
    }
    require(foundFrame, "分包收到 OK 后仍应产出 frame");
}

void test_script_timeout_flow() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "read_version", true);
    host.onTxEvent(ctx,
                   protoscope::scripting::TxEvent{
                       .id = 1,
                       .kind = protoscope::scripting::TxRequestKind::Request,
                       .state = protoscope::scripting::TxEventState::Timeout,
                       .tag = "read_version",
                       .bytes = 4,
                       .queuedMs = nowMs(),
                       .finishedMs = nowMs(),
                       .error = std::string("timeout"),
                   });

    bool foundWarnStatus = false;
    for (const auto& update : host.drainStatusUpdates()) {
        if (update.text.find("超时") != std::string::npos) {
            foundWarnStatus = true;
        }
    }
    require(foundWarnStatus, "超时应更新状态栏");
    require(!host.drainDialogRequests().empty(), "超时应弹出 alert");
}

void test_luals_api_sync_contains_tx_and_dialog_api() {
    std::ifstream input("protocols/protoscope_api.lua");
    require(input.good(), "应能读取 protoscope_api.lua");
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();
    require(text.find("function proto.request(payload, opts) end") != std::string::npos, "LuaLS API 应声明 proto.request");
    require(text.find("function proto.request_done(result) end") != std::string::npos, "LuaLS API 应声明 proto.request_done");
    require(text.find("function proto.ui.alert(opts) end") != std::string::npos, "LuaLS API 应声明 proto.ui.alert");
    require(text.find("function on_tx(ctx, evt) end") != std::string::npos, "LuaLS API 应声明 on_tx");
    require(text.find("function on_dialog(ctx, evt) end") != std::string::npos, "LuaLS API 应声明 on_dialog");
    require(text.find("@field color? string") != std::string::npos, "LuaLS API 应声明 ProtoPlotChannel.color");
}

void test_script_missing_callbacks_allowed() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("missing_callbacks").generic_string()), "缺失回调脚本也应允许加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "ping", true);

    bool foundEvent = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "ping") {
            foundEvent = true;
        }
    }
    require(foundEvent, "缺失非必要回调时 on_control 仍应可执行");
}

void test_script_invalid_controls_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_controls").generic_string()), "非法 controls() 应加载失败");
    require(!host.lastError().empty(), "非法 controls() 失败时应记录错误");
}

void test_script_invalid_dock_anchor_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_dock_anchor").generic_string()), "非法 dock anchor 应加载失败");
    require(host.lastError().find("dock anchor 不支持") != std::string::npos, "非法 dock anchor 应给出清晰错误");
}

void test_script_table_layout_unknown_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_unknown_control").generic_string()), "引用未知控件的 table layout 应加载失败");
    require(host.lastError().find("未声明控件") != std::string::npos, "未知控件错误应包含未声明控件提示");
}

void test_script_table_layout_duplicate_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_duplicate_control").generic_string()), "重复引用控件的 table layout 应加载失败");
    require(host.lastError().find("重复引用控件") != std::string::npos, "重复引用错误应包含重复控件提示");
}

void test_script_table_layout_missing_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_missing_control").generic_string()), "遗漏控件的 table layout 应加载失败");
    require(host.lastError().find("缺少控件") != std::string::npos, "遗漏控件错误应包含缺少控件提示");
}

void test_script_table_layout_row_overflow_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_row_overflow").generic_string()), "超出列数的 table layout 应加载失败");
    require(host.lastError().find("单元格数量不能超过 columns") != std::string::npos, "超列错误应包含 columns 提示");
}

void test_script_form_layout_unknown_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_unknown_control").generic_string()), "引用未知控件的 form layout 应加载失败");
    require(host.lastError().find("未声明控件") != std::string::npos, "未知控件错误应包含未声明控件提示");
}

void test_script_form_layout_duplicate_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_duplicate_control").generic_string()), "重复引用控件的 form layout 应加载失败");
    require(host.lastError().find("重复引用控件") != std::string::npos, "重复引用错误应包含重复控件提示");
}

void test_script_form_layout_missing_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_missing_control").generic_string()), "遗漏控件的 form layout 应加载失败");
    require(host.lastError().find("缺少控件") != std::string::npos, "遗漏控件错误应包含缺少控件提示");
}

void test_script_runtime_error_logged() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("runtime_error").generic_string()), "运行时错误脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "explode", true);

    bool foundErrorLog = false;
    for (const auto& log : host.drainLogs()) {
        if (log.message.find("on_control 执行失败") != std::string::npos) {
            foundErrorLog = true;
        }
    }
    require(foundErrorLog, "运行时报错应写入错误日志");

    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, {0x01, 0x02}});
    bool foundEvent = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "after_error") {
            foundEvent = true;
        }
    }
    require(foundEvent, "脚本回调报错后宿主仍应继续运行");
}

void test_protocol_directory_reload() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议目录应可加载");
    require(host.protocolDirectory() == "protocols/default_protocol", "协议目录应被记录");
    require(host.scriptPath().find("main.lua") != std::string::npos, "协议入口应固定为 main.lua");
}

void test_config_default_roundtrip() {
    protoscope::config::ConfigStore store;
    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-config-roundtrip.yaml";

    auto config = store.load(tempPath).config;
    config.communication.kind = protoscope::transport::TransportKind::Serial;
    config.communication.serial.portName = "COM9";
    config.communication.serial.dataBits = 7;
    config.communication.serial.parity = "even";
    config.communication.serial.stopBits = "two";
    config.communication.serial.flowControl = "hardware";
    config.app.configHotReload.enabled = true;
    config.gui.wave.maxRenderPointsPerChannel = 64;
    config.gui.wave.maxRenderVertices = 4096;
    config.gui.wave.overviewMaxSamples = 128;
    config.gui.wave.minVisibleTimeSpan = 0.0025;

    std::string error;
    require(store.save(tempPath, config, error), "默认配置写回失败");

    const auto reloaded = store.load(tempPath);
    require(reloaded.config.communication.kind == protoscope::transport::TransportKind::Serial, "串口模式 roundtrip 失败");
    require(reloaded.config.communication.serial.portName == "COM9", "串口端口 roundtrip 失败");
    require(reloaded.config.communication.serial.dataBits == 7, "串口数据位 roundtrip 失败");
    require(reloaded.config.communication.serial.parity == "even", "串口奇偶校验 roundtrip 失败");
    require(reloaded.config.communication.serial.stopBits == "two", "串口停止位 roundtrip 失败");
    require(reloaded.config.communication.serial.flowControl == "hardware", "串口流控 roundtrip 失败");
    require(reloaded.config.app.configHotReload.enabled, "配置热重载开关 roundtrip 失败");
    require(reloaded.config.gui.wave.maxRenderPointsPerChannel == 64, "波形每通道渲染点数 roundtrip 失败");
    require(reloaded.config.gui.wave.maxRenderVertices == 4096, "波形顶点预算 roundtrip 失败");
    require(reloaded.config.gui.wave.overviewMaxSamples == 128, "波形概览点数 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.minVisibleTimeSpan - 0.0025) < 1e-12, "波形最小可视跨度 roundtrip 失败");
}

void test_config_logging_roundtrip() {
    protoscope::config::ConfigStore store;
    const auto tempRoot = std::filesystem::temp_directory_path() / "protoscope-config-logging";
    const auto tempPath = tempRoot / "logging.yaml";
    std::filesystem::create_directories(tempRoot);

    auto base = store.load(tempPath).config;
    base.logging.level = protoscope::config::LogLevel::Warn;
    base.logging.filePath = "logs/protoscope.log";

    std::string error;
    require(store.save(tempPath, base, error), "日志配置写回失败");
    auto reloaded = store.load(tempPath).config;
    require(reloaded.logging.level == protoscope::config::LogLevel::Warn, "日志等级 roundtrip 失败");
    require(reloaded.logging.filePath == "logs/protoscope.log", "日志相对路径 roundtrip 失败");

    base.logging.filePath.clear();
    require(store.save(tempPath, base, error), "空日志路径写回失败");
    reloaded = store.load(tempPath).config;
    require(reloaded.logging.filePath.empty(), "空日志路径应保持为空");

    const auto missingPath = tempRoot / "missing.yaml";
    const auto missing = store.load(missingPath).config;
    require(missing.logging.filePath.empty(), "缺失日志路径时应默认为空");
    require(missing.logging.level == protoscope::config::LogLevel::Info, "缺失日志等级时应回退到 info");
}

void test_config_default_script_workspace() {
    protoscope::config::ConfigStore store;
    std::string error;
    require(store.ensureDefaultScriptWorkspace(error), "scripts 工作区初始化失败");
    require(std::filesystem::exists(store.defaultScriptWorkspaceDir()), "scripts 目录应存在");
    require(std::filesystem::exists(store.defaultScriptHelpPath()), "README.txt 应存在");
    require(std::filesystem::exists(store.mainLuaPath(store.defaultProtocolDir())), "默认协议脚本应存在");
}

void test_protocol_scan_and_root_roundtrip() {
    protoscope::config::ConfigStore store;
    const auto tempRoot = std::filesystem::temp_directory_path() / "protoscope-protocol-scan";
    const auto alphaDir = tempRoot / "alpha";
    const auto betaDir = tempRoot / "beta";
    std::string error;

    require(store.ensureDefaultProtocolScript(alphaDir, error), "alpha 协议脚本补建失败");
    require(store.ensureDefaultProtocolScript(betaDir, error), "beta 协议脚本补建失败");

    const auto scanned = store.scanProtocolDirectories(tempRoot);
    require(scanned.size() == 2, "协议目录扫描数量不正确");
    require(scanned[0].find("alpha") != std::string::npos, "扫描结果应包含 alpha");
    require(scanned[1].find("beta") != std::string::npos, "扫描结果应包含 beta");

    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-protocol-root-roundtrip.yaml";
    auto config = store.load(tempPath).config;
    config.protocol.rootDir = tempRoot.generic_string();
    config.protocol.selectedDir = betaDir.generic_string();

    require(store.save(tempPath, config, error), "协议根目录保存失败");
    const auto reloaded = store.load(tempPath);
    require(reloaded.config.protocol.rootDir == tempRoot.generic_string(), "协议根目录 roundtrip 失败");
    require(reloaded.config.protocol.selectedDir == betaDir.generic_string(), "协议目录 roundtrip 失败");

    const auto normalized = store.normalizeProtocolDir(tempRoot, tempRoot / "missing");
    require(normalized == alphaDir, "root-aware 协议目录归一化应优先回退到当前 root 下的有效目录");

}

void test_script_plot_api_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("plot_stream").generic_string()), "plot_stream 协议应可加载");

    protoscope::transport::TransportOpenEvent openEvent{.context = sampleCtx()};
    host.onTransportOpen(openEvent);

    auto setups = host.drainPlotSetups();
    auto appends = host.drainPlotAppends();
    require(setups.size() == 1, "打开连接后应生成 1 次 plot.setup");
    require(setups[0].channels.size() == 2, "plot.setup 应声明 2 个通道");
    require(std::abs(setups[0].channels[0].scale - 2.0) < 1e-12, "CH1 scale 解析错误");
    require(std::abs(setups[0].channels[1].scale - 1.0) < 1e-12, "CH2 scale 默认值错误");
    require(std::abs(setups[0].channels[0].offset - 0.0) < 1e-12, "CH1 offset 解析错误");
    require(std::abs(setups[0].channels[1].offset - 1.0) < 1e-12, "CH2 offset 解析错误");
    require(appends.size() == 2, "打开连接后应推送 2 组通道数据");
    require(appends[0].second.samples.size() == 3, "通道采样点数量不正确");
}

namespace {

static const TestCase kAllTests[] = {
    {"hex_roundtrip", &test_hex_roundtrip},
    {"hex_invalid_input", &test_hex_invalid_input},
    {"hex_normalize_input", &test_hex_normalize_input},
    {"hex_editor_cursor_normalize", &test_hex_editor_cursor_normalize},
    {"crc_known_vectors", &test_crc_known_vectors},
    {"config_external_reload_state", &test_config_external_reload_state},
    {"script_controls_snapshot", &test_script_controls_snapshot},
    {"script_on_open_log", &test_script_on_open_log},
    {"script_on_close_log", &test_script_on_close_log},
    {"script_on_error_log", &test_script_on_error_log},
    {"script_multi_dock_snapshot", &test_script_multi_dock_snapshot},
    {"script_dock_layout_fields", &test_script_dock_layout_fields},
    {"script_table_layout_snapshot", &test_script_table_layout_snapshot},
    {"script_form_layout_snapshot", &test_script_form_layout_snapshot},
    {"script_crc_bridge", &test_script_crc_bridge},
    {"script_read_version_flow", &test_script_read_version_flow},
    {"script_read_version_split_flow", &test_script_read_version_split_flow},
    {"script_timeout_flow", &test_script_timeout_flow},
    {"luals_api_sync_contains_tx_and_dialog_api", &test_luals_api_sync_contains_tx_and_dialog_api},
    {"script_missing_callbacks_allowed", &test_script_missing_callbacks_allowed},
    {"script_invalid_controls_fail", &test_script_invalid_controls_fail},
    {"script_invalid_dock_anchor_fail", &test_script_invalid_dock_anchor_fail},
    {"script_table_layout_unknown_control_fail", &test_script_table_layout_unknown_control_fail},
    {"script_table_layout_duplicate_control_fail", &test_script_table_layout_duplicate_control_fail},
    {"script_table_layout_missing_control_fail", &test_script_table_layout_missing_control_fail},
    {"script_table_layout_row_overflow_fail", &test_script_table_layout_row_overflow_fail},
    {"script_form_layout_unknown_control_fail", &test_script_form_layout_unknown_control_fail},
    {"script_form_layout_duplicate_control_fail", &test_script_form_layout_duplicate_control_fail},
    {"script_form_layout_missing_control_fail", &test_script_form_layout_missing_control_fail},
    {"script_runtime_error_logged", &test_script_runtime_error_logged},
    {"protocol_directory_reload", &test_protocol_directory_reload},
    {"config_default_roundtrip", &test_config_default_roundtrip},
    {"config_logging_roundtrip", &test_config_logging_roundtrip},
    {"config_default_script_workspace", &test_config_default_script_workspace},
    {"protocol_scan_and_root_roundtrip", &test_protocol_scan_and_root_roundtrip},
    {"script_plot_api_snapshot", &test_script_plot_api_snapshot},
    {"dock_log_and_script_split", &test_dock_log_and_script_split},
    {"dock_receive_row_single_line_hex_and_ascii", &test_dock_receive_row_single_line_hex_and_ascii},
    {"dock_receive_row_single_line_message_and_timestamp", &test_dock_receive_row_single_line_message_and_timestamp},
    {"lua_dock_layout_key_uses_protocol_and_script", &test_lua_dock_layout_key_uses_protocol_and_script},
    {"lua_dock_layout_key_falls_back_to_script_directory", &test_lua_dock_layout_key_falls_back_to_script_directory},
    {"lua_dock_layout_paths_prefer_user_layout", &test_lua_dock_layout_paths_prefer_user_layout},
    {"lua_dock_layout_paths_detect_legacy_source", &test_lua_dock_layout_paths_detect_legacy_source},
    {"lua_dock_layout_meta_path_is_sibling_yaml", &test_lua_dock_layout_meta_path_is_sibling_yaml},
    {"lua_dock_layout_meta_schema_v3_marks_modern_layout", &test_lua_dock_layout_meta_schema_v3_marks_modern_layout},
    {"lua_dock_layout_meta_schema_v2_marks_modern_layout", &test_lua_dock_layout_meta_schema_v2_marks_modern_layout},
    {"lua_dock_layout_meta_read_failure_falls_back_to_legacy", &test_lua_dock_layout_meta_read_failure_falls_back_to_legacy},
    {"lua_dock_layout_dock_id_sharing_does_not_mark_modern_legacy", &test_lua_dock_layout_dock_id_sharing_does_not_mark_modern_legacy},
    {"lua_dock_window_name_keeps_stable_id", &test_lua_dock_window_name_keeps_stable_id},
    {"lua_dock_layout_requests_group_tabs", &test_lua_dock_layout_requests_group_tabs},
    {"lua_dock_settings_filter_keeps_current_protocol_windows", &test_lua_dock_settings_filter_keeps_current_protocol_windows},
    {"lua_dock_settings_filter_keeps_current_windows_without_active_docks", &test_lua_dock_settings_filter_keeps_current_windows_without_active_docks},
    {"lua_dock_settings_filter_keeps_same_dock_id_tab_stack", &test_lua_dock_settings_filter_keeps_same_dock_id_tab_stack},
    {"workspace_layout_mode_after_load_prefers_default_build_only_when_missing", &test_workspace_layout_mode_after_load_prefers_default_build_only_when_missing},
    {"protocol_workspace_switch_decision_uses_draft_only_until_reload", &test_protocol_workspace_switch_decision_uses_draft_only_until_reload},
    {"protocol_workspace_switch_decision_reloads_draft_when_clicked", &test_protocol_workspace_switch_decision_reloads_draft_when_clicked},
    {"protocol_switch_resets_lua_default_dock_state_only_when_changed", &test_protocol_switch_resets_lua_default_dock_state_only_when_changed},
    {"lua_default_dock_layout_runs_only_during_default_build", &test_lua_default_dock_layout_runs_only_during_default_build},
    {"plot_cursor_snap_by_time_and_measurement", &test_plot_cursor_snap_by_time_and_measurement},
    {"wave_cursor_smart_snap_edge", &test_wave_cursor_smart_snap_edge},
    {"wave_cursor_smart_snap_extreme", &test_wave_cursor_smart_snap_extreme},
    {"wave_cursor_smart_snap_fallback_to_nearest", &test_wave_cursor_smart_snap_fallback_to_nearest},
    {"wave_cursor_drag_time_uses_smart_snap", &test_wave_cursor_drag_time_uses_smart_snap},
    {"tcp_transport_roundtrip", &test_tcp_transport_roundtrip},
    {"transport_enqueue_send_async_roundtrip", &test_transport_enqueue_send_async_roundtrip},
    {"tcp_server_connection_takeover_replaces_active_client", &test_tcp_server_connection_takeover_replaces_active_client},
    {"serial_transport_error_path", &test_serial_transport_error_path},
    {"serial_port_name_normalization", &test_serial_port_name_normalization},
    {"application_tcp_lua_read_version_roundtrip", &test_application_tcp_lua_read_version_roundtrip},
    {"application_lua_controls_without_connection", &test_application_lua_controls_without_connection},
    {"application_failed_protocol_reload_keeps_previous_runtime", &test_application_failed_protocol_reload_keeps_previous_runtime},
    {"application_open_transport_uses_serial_runtime_config", &test_application_open_transport_uses_serial_runtime_config},
    {"application_logging_filters_script_and_host", &test_application_logging_filters_script_and_host},
    {"plot_history_trim_and_envelope", &test_plot_history_trim_and_envelope},
    {"wave_layout_solver_clamps_without_overflow", &test_wave_layout_solver_clamps_without_overflow},
    {"plot_limited_envelope_preserves_spikes", &test_plot_limited_envelope_preserves_spikes},
    {"plot_low_density_envelope_keeps_single_value_line", &test_plot_low_density_envelope_keeps_single_value_line},
    {"plot_cursor_snap_and_delta", &test_plot_cursor_snap_and_delta},
    {"plot_channel_scale_and_offset_apply_to_display_only", &test_plot_channel_scale_and_offset_apply_to_display_only},
    {"plot_cursor_snap_scope_selection", &test_plot_cursor_snap_scope_selection},
    {"plot_limited_envelope_edges", &test_plot_limited_envelope_edges},
    {"wave_frequency_parse_and_axis_mapping", &test_wave_frequency_parse_and_axis_mapping},
    {"wave_viewport_zoom_modes_and_clamp", &test_wave_viewport_zoom_modes_and_clamp},
    {"wave_overview_viewport_normalize", &test_wave_overview_viewport_normalize},
    {"wave_cursor_position_in_viewport", &test_wave_cursor_position_in_viewport},
    {"wave_cursor_interval_text_by_axis", &test_wave_cursor_interval_text_by_axis},
    {"wave_cursor_interval_lock", &test_wave_cursor_interval_lock},
};

} // namespace

const TestCase* allTests() {
    return kAllTests;
}

int testCount() {
    return static_cast<int>(sizeof(kAllTests) / sizeof(kAllTests[0]));
}
