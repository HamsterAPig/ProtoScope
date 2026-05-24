#include "protoscope/transport/transport.hpp"

#include <array>
#include <atomic>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace protoscope::transport {

namespace {

std::uint64_t makeConnectionId() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

SocketHandle socketFromStorage(std::intptr_t value) {
    return static_cast<SocketHandle>(value);
}

void storeSocket(std::intptr_t& slot, SocketHandle socket) {
    slot = static_cast<std::intptr_t>(socket);
}

bool ensureSocketRuntime(std::string* error = nullptr) {
#if defined(_WIN32)
    static const bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();

    if (!initialized && error != nullptr) {
        *error = "WSAStartup 初始化失败";
    }
    return initialized;
#else
    (void)error;
    return true;
#endif
}

void closeSocket(std::intptr_t& slot) {
    auto socket = socketFromStorage(slot);
    if (socket == kInvalidSocket) {
        return;
    }

#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
    storeSocket(slot, kInvalidSocket);
}

std::string socketErrorText(const char* action) {
#if defined(_WIN32)
    return std::string(action) + " 失败，错误码=" + std::to_string(WSAGetLastError());
#else
    return std::string(action) + " 失败: " + std::strerror(errno);
#endif
}

bool waitReadable(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return false;
    }

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 0;
    return ::select(static_cast<int>(socket + 1), &readSet, nullptr, nullptr, &timeout) > 0 && FD_ISSET(socket, &readSet);
}

bool sendAll(SocketHandle socket, const std::vector<std::uint8_t>& bytes, std::string* error) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
#if defined(_WIN32)
        const int sent = ::send(socket,
                                reinterpret_cast<const char*>(bytes.data() + offset),
                                static_cast<int>(bytes.size() - offset),
                                0);
#else
        const auto sent = ::send(socket, bytes.data() + offset, bytes.size() - offset, 0);
#endif
        if (sent <= 0) {
            if (error != nullptr) {
                *error = socketErrorText("send");
            }
            return false;
        }
        offset += static_cast<std::size_t>(sent);
    }
    return true;
}

std::optional<std::uint16_t> localPort(SocketHandle socket) {
    sockaddr_storage address{};
    socklen_t size = static_cast<socklen_t>(sizeof(address));
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        return std::nullopt;
    }

    if (address.ss_family == AF_INET) {
        return ntohs(reinterpret_cast<const sockaddr_in&>(address).sin_port);
    }
    if (address.ss_family == AF_INET6) {
        return ntohs(reinterpret_cast<const sockaddr_in6&>(address).sin6_port);
    }
    return std::nullopt;
}

std::string endpointFromSockaddr(const sockaddr_storage& address) {
    char host[NI_MAXHOST]{};
    char service[NI_MAXSERV]{};
    const auto* raw = reinterpret_cast<const sockaddr*>(&address);
    const auto length = static_cast<socklen_t>(
        address.ss_family == AF_INET ? sizeof(sockaddr_in) : sizeof(sockaddr_in6));
    if (::getnameinfo(raw, length, host, sizeof(host), service, sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
        return "unknown";
    }

    std::string endpoint(host);
    endpoint.push_back(':');
    endpoint += service;
    return endpoint;
}

} // namespace

TransportBase::TransportBase()
    : state_(TransportState::Closed), txCount_(0), rxCount_(0) {}

TransportBase::~TransportBase() = default;

bool TransportBase::send(std::vector<std::uint8_t> bytes) {
    if (state() != TransportState::Open) {
        return false;
    }

    addTx(bytes.size());
    return true;
}

TransportState TransportBase::state() const {
    return state_.load(std::memory_order_relaxed);
}

std::vector<TransportEvent> TransportBase::takeEvents() {
    pumpIo();

    std::lock_guard lock(eventsMutex_);
    std::vector<TransportEvent> out;
    out.swap(events_);
    return out;
}

std::uint64_t TransportBase::txCount() const {
    return txCount_.load(std::memory_order_relaxed);
}

std::uint64_t TransportBase::rxCount() const {
    return rxCount_.load(std::memory_order_relaxed);
}

void TransportBase::pushEvent(TransportEvent event) {
    std::lock_guard lock(eventsMutex_);
    events_.push_back(std::move(event));
}

void TransportBase::setState(TransportState next) {
    state_.store(next, std::memory_order_relaxed);
}

void TransportBase::addTx(std::size_t size) {
    txCount_.fetch_add(size, std::memory_order_relaxed);
}

void TransportBase::addRx(std::size_t size) {
    rxCount_.fetch_add(size, std::memory_order_relaxed);
}

std::uint64_t TransportBase::nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void TransportBase::pumpIo() {}

bool TcpClientTransport::open(const TransportConfig& config) {
    if (!std::holds_alternative<TcpClientConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpClient, "", 0, nowMs(), true}, "配置类型不是 TCP Client"});
        return false;
    }

    const auto& tcp = std::get<TcpClientConfig>(config);
    setState(TransportState::Opening);

    std::string runtimeError;
    if (!ensureSocketRuntime(&runtimeError)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpClient, tcp.host, 0, nowMs(), true}, runtimeError});
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo* result = nullptr;
    const auto service = std::to_string(tcp.port);
    if (::getaddrinfo(tcp.host.c_str(), service.c_str(), &hints, &result) != 0) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpClient, tcp.host + ":" + service, 0, nowMs(), true}, "解析目标地址失败"});
        return false;
    }

    SocketHandle socket = kInvalidSocket;
    for (auto* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
        socket = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (socket == kInvalidSocket) {
            continue;
        }

        if (::connect(socket, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
            break;
        }

#if defined(_WIN32)
        closesocket(socket);
#else
        close(socket);
#endif
        socket = kInvalidSocket;
    }
    ::freeaddrinfo(result);

    if (socket == kInvalidSocket) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpClient, tcp.host + ":" + service, 0, nowMs(), true}, "TCP 客户端连接失败"});
        return false;
    }

    storeSocket(socket_, socket);
    context_ = ConnectionContext{
        .kind = TransportKind::TcpClient,
        .endpoint = tcp.host + ":" + service,
        .connectionId = makeConnectionId(),
        .timestampMs = nowMs(),
        .readyForIo = true,
    };

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*context_});
    return true;
}

void TcpClientTransport::close() {
    if (context_.has_value()) {
        pushEvent(TransportCloseEvent{*context_, "用户主动关闭"});
    }
    closeSocket(socket_);
    context_.reset();
    setState(TransportState::Closed);
}

bool TcpClientTransport::send(std::vector<std::uint8_t> bytes) {
    const auto socket = socketFromStorage(socket_);
    if (state() != TransportState::Open || socket == kInvalidSocket || bytes.empty()) {
        return false;
    }

    std::string error;
    if (!sendAll(socket, bytes, &error)) {
        setState(TransportState::Error);
        if (context_.has_value()) {
            pushEvent(TransportErrorEvent{*context_, error});
        }
        return false;
    }

    addTx(bytes.size());
    return true;
}

void TcpClientTransport::pumpIo() {
    const auto socket = socketFromStorage(socket_);
    if (socket == kInvalidSocket || !context_.has_value() || !waitReadable(socket)) {
        return;
    }

    std::array<std::uint8_t, 4096> buffer{};
#if defined(_WIN32)
    const int received = ::recv(socket, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
    const auto received = ::recv(socket, buffer.data(), buffer.size(), 0);
#endif
    if (received > 0) {
        std::vector<std::uint8_t> bytes(buffer.begin(), buffer.begin() + received);
        addRx(bytes.size());
        pushEvent(TransportBytesEvent{*context_, std::move(bytes)});
        return;
    }

    pushEvent(TransportCloseEvent{*context_, "对端已关闭连接"});
    closeSocket(socket_);
    context_.reset();
    setState(TransportState::Closed);
}

bool TcpServerTransport::open(const TransportConfig& config) {
    if (!std::holds_alternative<TcpServerConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpServer, "", 0, nowMs(), false}, "配置类型不是 TCP Server"});
        return false;
    }

    const auto& tcp = std::get<TcpServerConfig>(config);
    rejectNewConnection_ = tcp.rejectNewConnection;
    setState(TransportState::Opening);

    std::string runtimeError;
    if (!ensureSocketRuntime(&runtimeError)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpServer, tcp.bindAddress, 0, nowMs(), false}, runtimeError});
        return false;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const auto service = std::to_string(tcp.port);
    const char* host = (tcp.bindAddress.empty() || tcp.bindAddress == "0.0.0.0") ? nullptr : tcp.bindAddress.c_str();
    if (::getaddrinfo(host, service.c_str(), &hints, &result) != 0) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpServer, tcp.bindAddress, 0, nowMs(), false}, "解析监听地址失败"});
        return false;
    }

    SocketHandle listener = kInvalidSocket;
    for (auto* candidate = result; candidate != nullptr; candidate = candidate->ai_next) {
        listener = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (listener == kInvalidSocket) {
            continue;
        }

        int reuse = 1;
        ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (::bind(listener, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0 &&
            ::listen(listener, 1) == 0) {
            break;
        }

#if defined(_WIN32)
        closesocket(listener);
#else
        close(listener);
#endif
        listener = kInvalidSocket;
    }
    ::freeaddrinfo(result);

    if (listener == kInvalidSocket) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpServer, tcp.bindAddress + ":" + service, 0, nowMs(), false}, "TCP 服务端监听失败"});
        return false;
    }

    storeSocket(listenerSocket_, listener);
    const auto actualPort = localPort(listener).value_or(tcp.port);
    context_ = ConnectionContext{
        .kind = TransportKind::TcpServer,
        .endpoint = (host == nullptr ? std::string("0.0.0.0") : tcp.bindAddress) + ":" + std::to_string(actualPort),
        .connectionId = makeConnectionId(),
        .timestampMs = nowMs(),
        .readyForIo = false,
    };

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*context_});
    return true;
}

void TcpServerTransport::close() {
    if (clientContext_.has_value()) {
        pushEvent(TransportCloseEvent{*clientContext_, "客户端连接已关闭"});
    }
    if (context_.has_value()) {
        pushEvent(TransportCloseEvent{*context_, "服务端关闭监听"});
    }

    closeSocket(clientSocket_);
    closeSocket(listenerSocket_);
    clientContext_.reset();
    context_.reset();
    setState(TransportState::Closed);
}

bool TcpServerTransport::send(std::vector<std::uint8_t> bytes) {
    const auto socket = socketFromStorage(clientSocket_);
    if (state() != TransportState::Open || socket == kInvalidSocket || !clientContext_.has_value() || bytes.empty()) {
        return false;
    }

    std::string error;
    if (!sendAll(socket, bytes, &error)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{*clientContext_, error});
        return false;
    }

    addTx(bytes.size());
    return true;
}

void TcpServerTransport::pumpIo() {
    const auto listener = socketFromStorage(listenerSocket_);
    if (listener != kInvalidSocket && waitReadable(listener)) {
        sockaddr_storage peerAddress{};
        socklen_t peerSize = static_cast<socklen_t>(sizeof(peerAddress));
        const SocketHandle accepted = ::accept(listener, reinterpret_cast<sockaddr*>(&peerAddress), &peerSize);
        if (accepted != kInvalidSocket) {
            const auto acceptedEndpoint = endpointFromSockaddr(peerAddress);
            if (socketFromStorage(clientSocket_) != kInvalidSocket && rejectNewConnection_) {
                SocketHandle rejected = accepted;
#if defined(_WIN32)
                closesocket(rejected);
#else
                close(rejected);
#endif
                if (context_.has_value()) {
                    pushEvent(TransportErrorEvent{*context_, "已拒绝新的客户端连接: " + acceptedEndpoint});
                }
            } else {
                if (clientContext_.has_value()) {
                    pushEvent(TransportCloseEvent{*clientContext_, "已切换到新的客户端连接"});
                }
                closeSocket(clientSocket_);

                storeSocket(clientSocket_, accepted);
                clientContext_ = ConnectionContext{
                    .kind = TransportKind::TcpServer,
                    .endpoint = acceptedEndpoint,
                    .connectionId = makeConnectionId(),
                    .timestampMs = nowMs(),
                    .readyForIo = true,
                };
                pushEvent(TransportOpenEvent{*clientContext_});
            }
        }
    }

    const auto client = socketFromStorage(clientSocket_);
    if (client == kInvalidSocket || !clientContext_.has_value() || !waitReadable(client)) {
        return;
    }

    std::array<std::uint8_t, 4096> buffer{};
#if defined(_WIN32)
    const int received = ::recv(client, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0);
#else
    const auto received = ::recv(client, buffer.data(), buffer.size(), 0);
#endif
    if (received > 0) {
        std::vector<std::uint8_t> bytes(buffer.begin(), buffer.begin() + received);
        addRx(bytes.size());
        pushEvent(TransportBytesEvent{*clientContext_, std::move(bytes)});
        return;
    }

    pushEvent(TransportCloseEvent{*clientContext_, "客户端已断开连接"});
    closeSocket(clientSocket_);
    clientContext_.reset();
}

bool SerialTransport::open(const TransportConfig& config) {
    if (!std::holds_alternative<SerialConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::Serial, "", 0, nowMs(), true}, "配置类型不是串口"});
        return false;
    }

    const auto& serial = std::get<SerialConfig>(config);
    setState(TransportState::Opening);

    context_ = ConnectionContext{
        .kind = TransportKind::Serial,
        .endpoint = serial.portName + "@" + std::to_string(serial.baudRate),
        .connectionId = makeConnectionId(),
        .timestampMs = nowMs(),
        .readyForIo = true,
    };

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*context_});
    return true;
}

void SerialTransport::close() {
    if (context_.has_value()) {
        pushEvent(TransportCloseEvent{*context_, "串口关闭"});
    }
    context_.reset();
    setState(TransportState::Closed);
}

} // namespace protoscope::transport
