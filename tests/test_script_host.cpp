#include "test_registry.hpp"

#include "protoscope/config/config.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <chrono>
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

    const auto sendQueue = host.drainSendQueue();
    require(sendQueue.size() == 1, "read_version 应产生一次发送");
    require(sendQueue[0].size() >= 5, "发送帧长度不正确");

    const std::vector<std::uint8_t> response{'O', 'K', '\r', '\n'};
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, response});
    host.tick(nowMs() + 2000);

    bool foundFrame = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "frame") {
            foundFrame = true;
        }
    }
    require(foundFrame, "收到 OK 响应后应产生 frame 事件");
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
    host.tick(nowMs() + 2000);

    bool foundWarning = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "warning") {
            foundWarning = true;
        }
    }
    require(foundWarning, "超时应产生 warning 事件");
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
    {"script_crc_bridge", &test_script_crc_bridge},
    {"script_read_version_flow", &test_script_read_version_flow},
    {"script_read_version_split_flow", &test_script_read_version_split_flow},
    {"script_timeout_flow", &test_script_timeout_flow},
    {"script_missing_callbacks_allowed", &test_script_missing_callbacks_allowed},
    {"script_invalid_controls_fail", &test_script_invalid_controls_fail},
    {"script_runtime_error_logged", &test_script_runtime_error_logged},
    {"protocol_directory_reload", &test_protocol_directory_reload},
    {"config_default_roundtrip", &test_config_default_roundtrip},
    {"config_default_script_workspace", &test_config_default_script_workspace},
    {"protocol_scan_and_root_roundtrip", &test_protocol_scan_and_root_roundtrip},
    {"dock_log_and_script_split", &test_dock_log_and_script_split},
    {"tcp_transport_roundtrip", &test_tcp_transport_roundtrip},
    {"transport_enqueue_send_async_roundtrip", &test_transport_enqueue_send_async_roundtrip},
    {"tcp_server_connection_takeover_replaces_active_client", &test_tcp_server_connection_takeover_replaces_active_client},
    {"serial_transport_error_path", &test_serial_transport_error_path},
    {"application_tcp_lua_read_version_roundtrip", &test_application_tcp_lua_read_version_roundtrip},
};

} // namespace

const TestCase* allTests() {
    return kAllTests;
}

int testCount() {
    return static_cast<int>(sizeof(kAllTests) / sizeof(kAllTests[0]));
}
