#include "test_registry.hpp"

#include "protoscope/transport/transport.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint16_t parsePort(const std::string& endpoint) {
    const auto pos = endpoint.rfind(':');
    require(pos != std::string::npos, "endpoint 缺少端口");
    return static_cast<std::uint16_t>(std::stoi(endpoint.substr(pos + 1)));
}

template <typename Predicate>
bool waitUntil(Predicate predicate) {
    for (int i = 0; i < 50; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

void test_tcp_transport_roundtrip() {
    using namespace protoscope::transport;

    TcpServerTransport server;
    require(server.open(TcpServerConfig{.bindAddress = "127.0.0.1", .port = 0, .rejectNewConnection = true}), "服务端打开失败");

    auto serverEvents = server.takeEvents();
    require(!serverEvents.empty(), "服务端应产生监听事件");

    std::optional<std::uint16_t> listenPort;
    for (const auto& event : serverEvents) {
        if (const auto* opened = std::get_if<TransportOpenEvent>(&event)) {
            listenPort = parsePort(opened->context.endpoint);
            break;
        }
    }
    require(listenPort.has_value(), "未拿到服务端监听端口");

    TcpClientTransport client;
    require(client.open(TcpClientConfig{.host = "127.0.0.1", .port = *listenPort}), "客户端连接失败");

    bool serverAccepted = waitUntil([&]() {
        auto events = server.takeEvents();
        for (const auto& event : events) {
            if (const auto* opened = std::get_if<TransportOpenEvent>(&event)) {
                if (opened->context.readyForIo) {
                    return true;
                }
            }
        }
        return false;
    });
    require(serverAccepted, "服务端未接受客户端连接");

    const std::vector<std::uint8_t> clientPayload{'P', 'I', 'N', 'G'};
    require(client.send(clientPayload), "客户端发送失败");

    bool serverReceived = waitUntil([&]() {
        auto events = server.takeEvents();
        for (const auto& event : events) {
            if (const auto* bytes = std::get_if<TransportBytesEvent>(&event)) {
                if (bytes->bytes == clientPayload) {
                    return true;
                }
            }
        }
        return false;
    });
    require(serverReceived, "服务端未收到客户端报文");

    const std::vector<std::uint8_t> serverPayload{'P', 'O', 'N', 'G'};
    require(server.send(serverPayload), "服务端发送失败");

    bool clientReceived = waitUntil([&]() {
        auto events = client.takeEvents();
        for (const auto& event : events) {
            if (const auto* bytes = std::get_if<TransportBytesEvent>(&event)) {
                if (bytes->bytes == serverPayload) {
                    return true;
                }
            }
        }
        return false;
    });
    require(clientReceived, "客户端未收到服务端报文");

    client.close();
    server.close();
}

void test_serial_transport_error_path() {
    using namespace protoscope::transport;

    SerialTransport serial;
    const bool opened = serial.open(SerialConfig{.portName = "__PROTO_SCOPE_INVALID_PORT__", .baudRate = 115200});
    require(!opened, "无效串口不应打开成功");
    require(serial.state() == TransportState::Error, "无效串口应进入错误态");

    const auto events = serial.takeEvents();
    bool hasError = false;
    for (const auto& event : events) {
        if (const auto* error = std::get_if<TransportErrorEvent>(&event)) {
            hasError = !error->message.empty();
        }
    }
    require(hasError, "无效串口应产生错误事件");
}
