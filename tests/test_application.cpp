#include "test_registry.hpp"

#include "protoscope/app/application.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool waitUntil(auto&& predicate) {
    for (int i = 0; i < 80; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

std::optional<std::uint16_t> findListenPort(const protoscope::dock::LogDockState& logState) {
    for (const auto& row : logState.rows) {
        if (row.direction != "OPEN") {
            continue;
        }
        const auto pos = row.endpoint.rfind(':');
        if (pos == std::string::npos) {
            continue;
        }
        return static_cast<std::uint16_t>(std::stoi(row.endpoint.substr(pos + 1)));
    }
    return std::nullopt;
}

bool hasReceiveMessage(const protoscope::dock::ReceiveDockState& receiveState, const std::string& message) {
    for (const auto& row : receiveState.rows) {
        if (row.direction != "RX") {
            continue;
        }
        if (row.message.find(message) != std::string::npos) {
            return true;
        }
        const std::string payload(row.bytes.begin(), row.bytes.end());
        if (payload.find(message) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasScriptEvent(const protoscope::dock::ScriptDockState& scriptState, const std::string& name) {
    for (const auto& row : scriptState.rows) {
        if (row.direction == "EVENT" && row.message.find(name + ":") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void pumpPair(protoscope::app::Application& server, protoscope::app::Application& client) {
    server.pumpOnce();
    client.pumpOnce();
}

int countOpenRows(const protoscope::dock::LogDockState& logState) {
    int count = 0;
    for (const auto& row : logState.rows) {
        if (row.direction == "OPEN") {
            ++count;
        }
    }
    return count;
}

} // namespace

void test_application_tcp_lua_read_version_roundtrip() {
    using protoscope::app::Application;
    using protoscope::transport::TcpClientConfig;
    using protoscope::transport::TcpServerConfig;
    using protoscope::transport::TransportKind;

    Application server;
    Application client;

    require(server.initialize(), "服务端应用初始化失败");
    require(client.initialize(), "客户端应用初始化失败");

    auto& serverComm = server.docks().commState();
    serverComm.kind = TransportKind::TcpServer;
    serverComm.tcpServer = TcpServerConfig{.bindAddress = "127.0.0.1", .port = 0, .rejectNewConnection = false};
    server.openTransport();

    const bool serverListening = waitUntil([&]() {
        pumpPair(server, client);
        return findListenPort(server.docks().logState()).has_value();
    });
    require(serverListening, "服务端未进入监听状态");

    const auto listenPort = findListenPort(server.docks().logState());
    require(listenPort.has_value(), "未获取到监听端口");

    auto& clientComm = client.docks().commState();
    clientComm.kind = TransportKind::TcpClient;
    clientComm.tcpClient = TcpClientConfig{.host = "127.0.0.1", .port = *listenPort};
    client.openTransport();

    const bool connected = waitUntil([&]() {
        pumpPair(server, client);
        return server.docks().commState().state == protoscope::transport::TransportState::Open &&
               client.docks().commState().state == protoscope::transport::TransportState::Open &&
               countOpenRows(server.docks().logState()) >= 2 &&
               countOpenRows(client.docks().logState()) >= 1;
    });
    require(connected, "TCP 双端未成功连通");

    server.updateControlValue("device_id", std::string("O"));
    client.updateControlValue("device_id", std::string("O"));
    server.updateControlValue("hex_send", false);
    client.updateControlValue("hex_send", false);
    server.updateControlValue("read_version", true);

    const bool clientReceivedRequest = waitUntil([&]() {
        pumpPair(server, client);
        return hasReceiveMessage(client.docks().receiveState(), "READ O");
    });
    require(clientReceivedRequest, "客户端未收到服务端 Lua 发出的读版本请求");

    client.sendManualPayload("4F 4B 0D 0A", true);

    const bool serverParsedFrame = waitUntil([&]() {
        pumpPair(server, client);
        return hasScriptEvent(server.docks().scriptState(), "frame");
    });
    require(serverParsedFrame, "服务端未解析到读版本响应 frame 事件");

    client.updateControlValue("read_version", true);

    const bool serverReceivedRequest = waitUntil([&]() {
        pumpPair(server, client);
        return hasReceiveMessage(server.docks().receiveState(), "READ O");
    });
    require(serverReceivedRequest, "服务端未收到客户端 Lua 发出的读版本请求");

    server.sendManualPayload("4F 4B 0D 0A", true);

    const bool clientParsedFrame = waitUntil([&]() {
        pumpPair(server, client);
        return hasScriptEvent(client.docks().scriptState(), "frame");
    });
    require(clientParsedFrame, "客户端未解析到读版本响应 frame 事件");

    server.shutdown();
    client.shutdown();
}

void test_application_lua_controls_without_connection() {
    protoscope::app::Application application;
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");
    require(application.reloadProtocolDirectory("protocols/lua_waveform_demo", true), "Lua 波形演示脚本应可加载");

    application.updateControlValue("pause", true);
    require(application.docks().commState().lastError.empty(), "未连接时 Lua 本地控件不应报告连接错误");

    application.updateControlValue("clear_history", true);
    application.pumpOnce();
    require(application.docks().waveState().buffer.channelCount() == 4, "清空历史应能在未连接时重建波形通道");

    application.shutdown();
}
