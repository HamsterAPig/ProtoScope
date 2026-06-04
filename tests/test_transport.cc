#include "protoscope/transport/transport.hpp"

#include "test_registry.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint16_t parsePort(const std::string& endpoint)
{
    const auto pos = endpoint.rfind(':');
    require(pos != std::string::npos, "endpoint 缺少端口");
    return static_cast<std::uint16_t>(std::stoi(endpoint.substr(pos + 1)));
}

template <typename Predicate> bool waitUntil(Predicate predicate)
{
    for (int i = 0; i < 50; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

} // namespace

void test_tcp_transport_roundtrip()
{
    using namespace protoscope::transport;

    TcpServerTransport server;
    require(server.open(TcpServerConfig{.bindAddress = "127.0.0.1", .port = 0, .rejectNewConnection = true}),
            "服务端打开失败");

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

void test_transport_enqueue_send_async_roundtrip()
{
    using namespace protoscope::transport;

    TcpServerTransport server;
    require(server.open(TcpServerConfig{.bindAddress = "127.0.0.1", .port = 0, .rejectNewConnection = true}),
            "服务端打开失败");

    const auto serverEvents = server.takeEvents();
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

    const bool serverAccepted = waitUntil([&]() {
        for (const auto& event : server.takeEvents()) {
            if (const auto* opened = std::get_if<TransportOpenEvent>(&event)) {
                if (opened->context.readyForIo) {
                    return true;
                }
            }
        }
        return false;
    });
    require(serverAccepted, "服务端未接受客户端连接");

    const std::vector<std::uint8_t> payload{'Q', 'U', 'E', 'U', 'E'};
    const auto enqueueStart = std::chrono::steady_clock::now();
    require(client.enqueueSend(TransportTxTask{
                .requestId = 7,
                .kind = TransportTxKind::Send,
                .payload = payload,
                .timeoutMs = 1000,
                .queuedAtMs = 123,
            }),
            "客户端异步发送入队失败");
    const auto enqueueElapsed = std::chrono::steady_clock::now() - enqueueStart;
    require(enqueueElapsed < std::chrono::milliseconds(50), "enqueueSend 不应在调用线程里长时间阻塞");

    const bool serverReceived = waitUntil([&]() {
        for (const auto& event : server.takeEvents()) {
            if (const auto* bytes = std::get_if<TransportBytesEvent>(&event)) {
                if (bytes->bytes == payload) {
                    return true;
                }
            }
        }
        return false;
    });
    require(serverReceived, "服务端未收到 enqueueSend 投递的报文");

    const bool clientObservedTxEvent = waitUntil([&]() {
        for (const auto& event : client.takeEvents()) {
            if (const auto* tx = std::get_if<TransportTxEvent>(&event)) {
                if (tx->requestId == 7 && tx->state == TransportTxState::Sent) {
                    return true;
                }
            }
        }
        return false;
    });
    require(clientObservedTxEvent, "客户端未收到带 requestId 的发送结果事件");

    client.close();
    server.close();
}

void test_tcp_server_connection_takeover_replaces_active_client()
{
    using namespace protoscope::transport;

    TcpServerTransport server;
    require(server.open(TcpServerConfig{.bindAddress = "127.0.0.1", .port = 0, .rejectNewConnection = false}),
            "服务端打开失败");

    auto serverEvents = server.takeEvents();
    std::optional<std::uint16_t> listenPort;
    for (const auto& event : serverEvents) {
        if (const auto* opened = std::get_if<TransportOpenEvent>(&event)) {
            listenPort = parsePort(opened->context.endpoint);
            break;
        }
    }
    require(listenPort.has_value(), "未拿到服务端监听端口");

    TcpClientTransport clientA;
    require(clientA.open(TcpClientConfig{.host = "127.0.0.1", .port = *listenPort}), "第一个客户端连接失败");

    std::uint64_t firstConnectionId = 0;
    const bool firstAccepted = waitUntil([&]() {
        for (const auto& event : server.takeEvents()) {
            if (const auto* opened = std::get_if<TransportOpenEvent>(&event)) {
                if (opened->context.readyForIo) {
                    firstConnectionId = opened->context.connectionId;
                    return true;
                }
            }
        }
        return false;
    });
    require(firstAccepted, "服务端未接受第一个客户端");

    TcpClientTransport clientB;
    require(clientB.open(TcpClientConfig{.host = "127.0.0.1", .port = *listenPort}), "第二个客户端连接失败");

    std::uint64_t secondConnectionId = 0;
    bool sawTakeoverClose = false;
    const bool secondAccepted = waitUntil([&]() {
        for (const auto& event : server.takeEvents()) {
            if (const auto* closed = std::get_if<TransportCloseEvent>(&event)) {
                if (closed->context.readyForIo && closed->context.connectionId == firstConnectionId &&
                    closed->reason == "新客户端已接管旧连接") {
                    sawTakeoverClose = true;
                }
            }
            if (const auto* opened = std::get_if<TransportOpenEvent>(&event)) {
                if (opened->context.readyForIo && opened->context.connectionId != firstConnectionId) {
                    secondConnectionId = opened->context.connectionId;
                    return true;
                }
            }
        }
        return false;
    });
    require(secondAccepted, "服务端未接受第二个客户端");
    require(sawTakeoverClose, "服务端未报告旧连接被接管");
    require(secondConnectionId != 0 && secondConnectionId != firstConnectionId, "第二个连接 ID 不应复用旧连接");

    const std::vector<std::uint8_t> payloadFromSecond{'N', 'E', 'W'};
    require(clientB.send(payloadFromSecond), "第二个客户端发送失败");

    bool receivedFromSecond = false;
    bool receivedFromOldConnection = false;
    const bool serverReceived = waitUntil([&]() {
        for (const auto& event : server.takeEvents()) {
            if (const auto* bytes = std::get_if<TransportBytesEvent>(&event)) {
                if (bytes->bytes == payloadFromSecond) {
                    if (bytes->context.connectionId == secondConnectionId) {
                        receivedFromSecond = true;
                        return true;
                    }
                    if (bytes->context.connectionId == firstConnectionId) {
                        receivedFromOldConnection = true;
                    }
                }
            }
        }
        return false;
    });
    require(serverReceived, "服务端未收到第二个客户端报文");
    require(!receivedFromOldConnection, "第二个客户端报文不应记到旧连接");

    clientA.close();
    clientB.close();
    server.close();
}

void test_serial_transport_error_path()
{
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

void test_udp_peer_transport_roundtrip()
{
    using namespace protoscope::transport;

    UdpPeerTransport peerA;
    UdpPeerTransport peerB;

    require(peerA.open(UdpPeerConfig{
                .bindAddress = "127.0.0.1",
                .bindPort = 0,
                .remoteHost = "127.0.0.1",
                .remotePort = 9000,
            }),
            "UDP Peer A 打开失败");
    auto peerAEvents = peerA.takeEvents();
    require(!peerAEvents.empty(), "UDP Peer A 应产生打开事件");

    std::optional<std::uint16_t> peerAPort;
    for (const auto& event : peerAEvents) {
        if (const auto* opened = std::get_if<TransportOpenEvent>(&event)) {
            peerAPort = parsePort(opened->context.endpoint.substr(0, opened->context.endpoint.find(" -> ")));
            break;
        }
    }
    require(peerAPort.has_value(), "未拿到 UDP Peer A 本地端口");

    require(peerB.open(UdpPeerConfig{
                .bindAddress = "127.0.0.1",
                .bindPort = 0,
                .remoteHost = "127.0.0.1",
                .remotePort = *peerAPort,
            }),
            "UDP Peer B 打开失败");
    auto peerBEvents = peerB.takeEvents();
    require(!peerBEvents.empty(), "UDP Peer B 应产生打开事件");

    std::optional<std::uint16_t> peerBPort;
    for (const auto& event : peerBEvents) {
        if (const auto* opened = std::get_if<TransportOpenEvent>(&event)) {
            peerBPort = parsePort(opened->context.endpoint.substr(0, opened->context.endpoint.find(" -> ")));
            break;
        }
    }
    require(peerBPort.has_value(), "未拿到 UDP Peer B 本地端口");

    peerA.close();
    require(peerA.open(UdpPeerConfig{
                .bindAddress = "127.0.0.1",
                .bindPort = *peerAPort,
                .remoteHost = "127.0.0.1",
                .remotePort = *peerBPort,
            }),
            "UDP Peer A 使用真实远端端口重新打开失败");
    peerA.takeEvents();

    const std::vector<std::uint8_t> payloadA{'U', 'D', 'P', 'A'};
    require(peerA.send(payloadA), "UDP Peer A 发送失败");
    const bool peerBReceived = waitUntil([&]() {
        for (const auto& event : peerB.takeEvents()) {
            if (const auto* bytes = std::get_if<TransportBytesEvent>(&event)) {
                if (bytes->bytes == payloadA && bytes->context.kind == TransportKind::UdpPeer) {
                    return true;
                }
            }
        }
        return false;
    });
    require(peerBReceived, "UDP Peer B 未收到 A 的报文");

    const std::vector<std::uint8_t> payloadB{'U', 'D', 'P', 'B'};
    require(peerB.enqueueSend(TransportTxTask{
                .requestId = 33,
                .kind = TransportTxKind::Send,
                .payload = payloadB,
                .timeoutMs = 1000,
                .queuedAtMs = 1,
            }),
            "UDP Peer B 异步发送入队失败");

    bool peerAReceived = false;
    bool txObserved = false;
    const bool udpDone = waitUntil([&]() {
        for (const auto& event : peerA.takeEvents()) {
            if (const auto* bytes = std::get_if<TransportBytesEvent>(&event)) {
                if (bytes->bytes == payloadB) {
                    peerAReceived = true;
                }
            }
        }
        for (const auto& event : peerB.takeEvents()) {
            if (const auto* tx = std::get_if<TransportTxEvent>(&event)) {
                if (tx->requestId == 33 && tx->state == TransportTxState::Sent) {
                    txObserved = true;
                }
            }
        }
        return peerAReceived && txObserved;
    });
    require(udpDone, "UDP Peer 异步发送未完成收发与 TX 事件");

    peerA.close();
    peerB.close();
    require(peerA.state() == TransportState::Closed, "UDP Peer A close 后应为 Closed");
    require(peerB.state() == TransportState::Closed, "UDP Peer B close 后应为 Closed");
}
