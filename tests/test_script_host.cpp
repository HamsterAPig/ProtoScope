#include "test_registry.hpp"

#include "protoscope/config/config.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <chrono>
#include <filesystem>
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
            std::chrono::steady_clock::now().time_since_epoch())
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

} // namespace

void test_script_controls_snapshot() {
    protoscope::scripting::ScriptHost host;
    const auto controls = host.controlsSnapshot();
    require(!controls.empty(), "controls 不能为空");
    require(controls[0].id == "read_version", "第一个控件应为 read_version");
}

void test_script_read_version_flow() {
    protoscope::scripting::ScriptHost host;
    host.loadProtocolDirectory("protocols/default_protocol");

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

void test_script_timeout_flow() {
    protoscope::scripting::ScriptHost host;
    host.loadProtocolDirectory("protocols/default_protocol");

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

void test_protocol_directory_reload() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议目录应可加载");
    require(host.protocolDirectory() == "protocols/default_protocol", "协议目录应被记录");
    require(host.scriptPath().find("main.lua") != std::string::npos, "协议入口应固定为 main.lua");
}

void test_config_default_roundtrip() {
    protoscope::config::ConfigStore store;
    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-config-roundtrip.yaml";

    auto loaded = store.load(tempPath);
    require(loaded.config.protocol.selectedDir == "protocols/default_protocol", "默认协议目录不正确");

    loaded.config.communication.kind = protoscope::transport::TransportKind::Serial;
    loaded.config.communication.serial.portName = "COM9";
    loaded.config.communication.serial.baudRate = 9600;

    std::string error;
    require(store.save(tempPath, loaded.config, error), "YAML 保存应成功");

    const auto reloaded = store.load(tempPath);
    require(reloaded.config.communication.kind == protoscope::transport::TransportKind::Serial, "串口模式 roundtrip 失败");
    require(reloaded.config.communication.serial.portName == "COM9", "串口端口 roundtrip 失败");
}

namespace {

static const TestCase kAllTests[] = {
    {"hex_roundtrip", &test_hex_roundtrip},
    {"hex_invalid_input", &test_hex_invalid_input},
    {"crc_known_vectors", &test_crc_known_vectors},
    {"script_controls_snapshot", &test_script_controls_snapshot},
    {"script_read_version_flow", &test_script_read_version_flow},
    {"script_timeout_flow", &test_script_timeout_flow},
    {"protocol_directory_reload", &test_protocol_directory_reload},
    {"config_default_roundtrip", &test_config_default_roundtrip},
};

} // namespace

const TestCase* allTests() {
    return kAllTests;
}

int testCount() {
    return static_cast<int>(sizeof(kAllTests) / sizeof(kAllTests[0]));
}
