#include "protoscope/transport/transport.hpp"

#include <atomic>

namespace protoscope::transport {

namespace {
std::uint64_t makeConnectionId() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
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

bool TcpClientTransport::open(const TransportConfig& config) {
    if (!std::holds_alternative<TcpClientConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpClient, "", 0, nowMs()}, "配置类型不是 TCP Client"});
        return false;
    }

    const auto& tcp = std::get<TcpClientConfig>(config);
    setState(TransportState::Opening);

    context_ = ConnectionContext{
        .kind = TransportKind::TcpClient,
        .endpoint = tcp.host + ":" + std::to_string(tcp.port),
        .connectionId = makeConnectionId(),
        .timestampMs = nowMs(),
    };

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*context_});

    // 核心流程：第一版先模拟一次回环收到数据，打通收包->Lua->展示链路。
    const std::vector<std::uint8_t> sample{'O', 'K', '\r', '\n'};
    addRx(sample.size());
    pushEvent(TransportBytesEvent{*context_, sample});
    return true;
}

void TcpClientTransport::close() {
    if (context_.has_value()) {
        pushEvent(TransportCloseEvent{*context_, "用户主动关闭"});
    }
    context_.reset();
    setState(TransportState::Closed);
}

bool TcpServerTransport::open(const TransportConfig& config) {
    if (!std::holds_alternative<TcpServerConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpServer, "", 0, nowMs()}, "配置类型不是 TCP Server"});
        return false;
    }

    const auto& tcp = std::get<TcpServerConfig>(config);
    rejectNewConnection_ = tcp.rejectNewConnection;
    setState(TransportState::Opening);

    context_ = ConnectionContext{
        .kind = TransportKind::TcpServer,
        .endpoint = tcp.bindAddress + ":" + std::to_string(tcp.port),
        .connectionId = makeConnectionId(),
        .timestampMs = nowMs(),
    };

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*context_});
    if (rejectNewConnection_) {
        pushEvent(TransportErrorEvent{*context_, "当前版本仅保留单连接，新的连接将被拒绝"});
    }
    return true;
}

void TcpServerTransport::close() {
    if (context_.has_value()) {
        pushEvent(TransportCloseEvent{*context_, "服务端关闭监听"});
    }
    context_.reset();
    setState(TransportState::Closed);
}

bool SerialTransport::open(const TransportConfig& config) {
    if (!std::holds_alternative<SerialConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::Serial, "", 0, nowMs()}, "配置类型不是串口"});
        return false;
    }

    const auto& serial = std::get<SerialConfig>(config);
    setState(TransportState::Opening);

    context_ = ConnectionContext{
        .kind = TransportKind::Serial,
        .endpoint = serial.portName + "@" + std::to_string(serial.baudRate),
        .connectionId = makeConnectionId(),
        .timestampMs = nowMs(),
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
