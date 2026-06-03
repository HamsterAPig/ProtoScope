#include "protoscope/transport/transport.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <asio/error.hpp>
#include <asio/connect.hpp>
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/serial_port.hpp>
#include <asio/write.hpp>

namespace protoscope::transport {

namespace {

std::uint64_t makeConnectionId() {
    static std::atomic<std::uint64_t> counter{1};
    return counter.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t currentTimeMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string endpointText(const asio::ip::tcp::endpoint& endpoint) {
    return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
}

std::string endpointText(const std::string& host, std::uint16_t port) {
    return host + ":" + std::to_string(port);
}

asio::serial_port_base::parity::type parseParity(const std::string& parity) {
    if (parity == "odd") {
        return asio::serial_port_base::parity::odd;
    }
    if (parity == "even") {
        return asio::serial_port_base::parity::even;
    }
    return asio::serial_port_base::parity::none;
}

asio::serial_port_base::stop_bits::type parseStopBits(const std::string& stopBits) {
    if (stopBits == "one_point_five") {
        return asio::serial_port_base::stop_bits::onepointfive;
    }
    if (stopBits == "two") {
        return asio::serial_port_base::stop_bits::two;
    }
    return asio::serial_port_base::stop_bits::one;
}

asio::serial_port_base::flow_control::type parseFlowControl(const std::string& flowControl) {
    if (flowControl == "software") {
        return asio::serial_port_base::flow_control::software;
    }
    if (flowControl == "hardware") {
        return asio::serial_port_base::flow_control::hardware;
    }
    return asio::serial_port_base::flow_control::none;
}

template <typename Writer>
bool enqueueWrite(asio::io_context& ioContext, std::atomic<bool>& stopping, TransportTxTask task, Writer&& writer) {
    if (task.payload.empty() || stopping.load(std::memory_order_relaxed)) {
        return false;
    }
    asio::post(ioContext, [task = std::move(task), writer = std::forward<Writer>(writer)]() mutable {
        writer(task);
    });
    return true;
}

template <typename Writable>
std::pair<bool, std::string> writeBytes(Writable& writable, std::mutex& mutex, const std::vector<std::uint8_t>& bytes) {
    try {
        std::lock_guard lock(mutex);
        asio::write(writable, asio::buffer(bytes));
        return {true, {}};
    } catch (const std::exception& ex) {
        return {false, ex.what()};
    }
}

} // namespace

struct TcpClientTransport::Runtime {
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    asio::io_context ioContext;
    std::optional<WorkGuard> workGuard{asio::make_work_guard(ioContext)};
    asio::ip::tcp::resolver resolver{ioContext};
    asio::ip::tcp::socket socket{ioContext};
    std::thread ioThread;
    std::vector<std::uint8_t> readBuffer{};
    std::mutex socketMutex;
    std::atomic<bool> stopping{false};
};

struct TcpServerTransport::Runtime {
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    asio::io_context ioContext;
    std::optional<WorkGuard> workGuard{asio::make_work_guard(ioContext)};
    asio::ip::tcp::acceptor acceptor{ioContext};
    asio::ip::tcp::socket clientSocket{ioContext};
    std::thread ioThread;
    std::vector<std::uint8_t> readBuffer{};
    std::mutex socketMutex;
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> clientGeneration{0};
};

struct SerialTransport::Runtime {
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    asio::io_context ioContext;
    std::optional<WorkGuard> workGuard{asio::make_work_guard(ioContext)};
    asio::serial_port serialPort{ioContext};
    std::thread ioThread;
    std::vector<std::uint8_t> readBuffer{};
    std::mutex portMutex;
    std::atomic<bool> stopping{false};
};

TransportBase::TransportBase()
    : state_(TransportState::Closed), txCount_(0), rxCount_(0) {}

TransportBase::~TransportBase() = default;

TransportState TransportBase::state() const {
    return state_.load(std::memory_order_relaxed);
}

std::vector<TransportEvent> TransportBase::takeEvents() {
    // UI 主线程只做无阻塞取事件，不再主动驱动底层 I/O 轮询。
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
    return currentTimeMs();
}

TcpClientTransport::TcpClientTransport()
    : runtime_(std::make_unique<Runtime>()) {}

TcpClientTransport::~TcpClientTransport() {
    close();
}

TcpServerTransport::TcpServerTransport()
    : runtime_(std::make_unique<Runtime>()) {}

TcpServerTransport::~TcpServerTransport() {
    close();
}

SerialTransport::SerialTransport()
    : runtime_(std::make_unique<Runtime>()) {}

SerialTransport::~SerialTransport() {
    close();
}

const std::vector<TransportDescriptor>& transportDescriptors() {
    static const std::vector<TransportDescriptor> descriptors{
        {TransportKind::TcpClient, "tcp_client", "TCP 客户端"},
        {TransportKind::TcpServer, "tcp_server", "TCP 服务端"},
        {TransportKind::Serial, "serial", "串口"},
        {TransportKind::UdpPeer, "udp_peer", "UDP Peer"},
    };
    return descriptors;
}

std::optional<TransportKind> transportKindFromId(std::string_view id) {
    for (const auto& descriptor : transportDescriptors()) {
        if (descriptor.id == id) {
            return descriptor.kind;
        }
    }
    return std::nullopt;
}

std::string_view transportKindId(TransportKind kind) {
    for (const auto& descriptor : transportDescriptors()) {
        if (descriptor.kind == kind) {
            return descriptor.id;
        }
    }
    return "tcp_client";
}

std::string_view transportKindLabel(TransportKind kind) {
    for (const auto& descriptor : transportDescriptors()) {
        if (descriptor.kind == kind) {
            return descriptor.label;
        }
    }
    return "TCP 客户端";
}

std::unique_ptr<ITransport> createTransport(TransportKind kind) {
    switch (kind) {
    case TransportKind::TcpClient:
        return std::make_unique<TcpClientTransport>();
    case TransportKind::TcpServer:
        return std::make_unique<TcpServerTransport>();
    case TransportKind::Serial:
        return std::make_unique<SerialTransport>();
    case TransportKind::UdpPeer:
        return std::make_unique<UdpPeerTransport>();
    }
    return nullptr;
}

ConnectionContext makeListenContext(const asio::ip::tcp::endpoint& endpoint) {
    return ConnectionContext{
        .kind = TransportKind::TcpServer,
        .endpoint = endpointText(endpoint),
        .connectionId = makeConnectionId(),
        .timestampMs = currentTimeMs(),
        .readyForIo = false,
    };
}

ConnectionContext makeClientContext(const asio::ip::tcp::endpoint& endpoint) {
    return ConnectionContext{
        .kind = TransportKind::TcpServer,
        .endpoint = endpointText(endpoint),
        .connectionId = makeConnectionId(),
        .timestampMs = currentTimeMs(),
        .readyForIo = true,
    };
}

bool TcpClientTransport::open(const TransportConfig& config) {
    close();

    if (!std::holds_alternative<TcpClientConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpClient, "", 0, nowMs(), true}, "配置类型不是 TCP Client"});
        return false;
    }

    const auto& tcp = std::get<TcpClientConfig>(config);
    runtime_->readBuffer.resize(tcp.readBufferBytes == 0U ? 1U : tcp.readBufferBytes);
    const auto fallbackEndpoint = endpointText(tcp.host, tcp.port);
    setState(TransportState::Opening);

    try {
        auto results = runtime_->resolver.resolve(tcp.host, std::to_string(tcp.port));
        asio::connect(runtime_->socket, results);
    } catch (const std::exception& ex) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpClient, fallbackEndpoint, 0, nowMs(), true}, ex.what()});
        return false;
    }

    try {
        context_ = ConnectionContext{
            .kind = TransportKind::TcpClient,
            .endpoint = endpointText(runtime_->socket.remote_endpoint()),
            .connectionId = makeConnectionId(),
            .timestampMs = nowMs(),
            .readyForIo = true,
        };
    } catch (const std::exception&) {
        context_ = ConnectionContext{
            .kind = TransportKind::TcpClient,
            .endpoint = fallbackEndpoint,
            .connectionId = makeConnectionId(),
            .timestampMs = nowMs(),
            .readyForIo = true,
        };
    }

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*context_});

    auto scheduleRead = std::make_shared<std::function<void()>>();
    *scheduleRead = [this, scheduleRead]() {
        // 持续异步收包，收到后直接进入线程安全事件队列。
        runtime_->socket.async_read_some(
            asio::buffer(runtime_->readBuffer),
            [this, scheduleRead](const asio::error_code& ec, std::size_t bytesRead) {
                if (ec) {
                    if (!runtime_->stopping.load(std::memory_order_relaxed) && context_.has_value()) {
                        setState(TransportState::Error);
                        auto errorContext = *context_;
                        errorContext.timestampMs = nowMs();
                        pushEvent(TransportErrorEvent{std::move(errorContext), ec.message()});
                    }
                    return;
                }

                if (bytesRead > 0 && context_.has_value()) {
                    addRx(bytesRead);
                    auto eventContext = *context_;
                    eventContext.timestampMs = nowMs();
                    pushEvent(TransportBytesEvent{
                        std::move(eventContext),
                        std::vector<std::uint8_t>(runtime_->readBuffer.begin(),
                                                  runtime_->readBuffer.begin() + static_cast<std::ptrdiff_t>(bytesRead)),
                    });
                }
                (*scheduleRead)();
            });
    };

    (*scheduleRead)();
    runtime_->ioThread = std::thread([this]() {
        runtime_->ioContext.run();
    });
    return true;
}

void TcpClientTransport::close() {
    if (runtime_ == nullptr) {
        return;
    }

    const auto previous = context_;
    // 先停掉后台 I/O，再重建运行时，避免旧连接残留到下一次 open。
    runtime_->stopping.store(true, std::memory_order_relaxed);
    asio::error_code ignored;
    runtime_->resolver.cancel();
    {
        std::lock_guard lock(runtime_->socketMutex);
        runtime_->socket.cancel(ignored);
        runtime_->socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        runtime_->socket.close(ignored);
    }
    runtime_->workGuard.reset();
    runtime_->ioContext.stop();
    if (runtime_->ioThread.joinable()) {
        runtime_->ioThread.join();
    }
    context_.reset();
    runtime_ = std::make_unique<Runtime>();
    setState(TransportState::Closed);
    if (previous.has_value()) {
        auto closedContext = *previous;
        closedContext.timestampMs = nowMs();
        pushEvent(TransportCloseEvent{std::move(closedContext), "用户主动关闭"});
    }
}

bool TcpClientTransport::send(std::vector<std::uint8_t> bytes) {
    if (state() != TransportState::Open || !context_.has_value() || bytes.empty()) {
        return false;
    }

    const auto [ok, error] = writeBytes(runtime_->socket, runtime_->socketMutex, bytes);
    if (ok) {
        addTx(bytes.size());
        return true;
    }

    setState(TransportState::Error);
    auto errorContext = *context_;
    errorContext.timestampMs = nowMs();
    pushEvent(TransportErrorEvent{std::move(errorContext), error});
    return false;
}

bool TcpClientTransport::enqueueSend(TransportTxTask task) {
    if (state() != TransportState::Open || !context_.has_value()) {
        return false;
    }
    return enqueueWrite(runtime_->ioContext, runtime_->stopping, std::move(task), [this](TransportTxTask txTask) {
        const auto writeStartedAtMs = nowMs();
        const auto [ok, error] = writeBytes(runtime_->socket, runtime_->socketMutex, txTask.payload);
        const auto finishedAtMs = nowMs();

        TransportTxState state = TransportTxState::Sent;
        if (!ok) {
            setState(TransportState::Error);
            if (context_.has_value()) {
                auto errorContext = *context_;
                errorContext.timestampMs = finishedAtMs;
                pushEvent(TransportErrorEvent{std::move(errorContext), error});
            }
            state = TransportTxState::Rejected;
        } else {
            addTx(txTask.payload.size());
            if (txTask.timeoutMs > 0 && finishedAtMs > writeStartedAtMs
                && finishedAtMs - writeStartedAtMs > txTask.timeoutMs) {
                state = TransportTxState::Timeout;
            }
        }

        pushEvent(TransportTxEvent{
            .requestId = txTask.requestId,
            .kind = txTask.kind,
            .state = state,
            .error = error,
            .bytes = txTask.payload.size(),
            .queuedAtMs = txTask.queuedAtMs,
            .finishedAtMs = finishedAtMs,
        });
    });
}

bool TcpServerTransport::open(const TransportConfig& config) {
    close();

    if (!std::holds_alternative<TcpServerConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpServer, "", 0, nowMs(), false}, "配置类型不是 TCP Server"});
        return false;
    }

    const auto& tcp = std::get<TcpServerConfig>(config);
    runtime_->readBuffer.resize(tcp.readBufferBytes == 0U ? 1U : tcp.readBufferBytes);
    rejectNewConnection_ = tcp.rejectNewConnection;
    setState(TransportState::Opening);

    try {
        const auto address = asio::ip::make_address(tcp.bindAddress);
        const asio::ip::tcp::endpoint endpoint(address, tcp.port);
        runtime_->acceptor.open(endpoint.protocol());
        runtime_->acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        runtime_->acceptor.bind(endpoint);
        runtime_->acceptor.listen();
        listenContext_ = makeListenContext(runtime_->acceptor.local_endpoint());
    } catch (const std::exception& ex) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::TcpServer, endpointText(tcp.bindAddress, tcp.port), 0, nowMs(), false}, ex.what()});
        return false;
    }

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*listenContext_});

    auto scheduleRead = std::make_shared<std::function<void(std::uint64_t)>>();
    *scheduleRead = [this, scheduleRead](std::uint64_t generation) {
        // 核心流程：双窗口联调允许新连接接管旧连接，因此每次读回调都绑定一代连接编号。
        // 一旦 accept 了新客户端，旧回调即便迟到返回，也必须直接丢弃，避免把旧连接事件记到新连接上。
        runtime_->clientSocket.async_read_some(
            asio::buffer(runtime_->readBuffer),
            [this, scheduleRead, generation](const asio::error_code& ec, std::size_t bytesRead) {
                if (generation != runtime_->clientGeneration.load(std::memory_order_relaxed)) {
                    return;
                }
                if (ec) {
                    if (!runtime_->stopping.load(std::memory_order_relaxed) && clientContext_.has_value()) {
                        auto closedContext = *clientContext_;
                        closedContext.timestampMs = nowMs();
                        pushEvent(TransportCloseEvent{std::move(closedContext), ec.message()});
                        clientContext_.reset();
                    }
                    return;
                }

                if (bytesRead > 0 && clientContext_.has_value()) {
                    addRx(bytesRead);
                    auto eventContext = *clientContext_;
                    eventContext.timestampMs = nowMs();
                    pushEvent(TransportBytesEvent{
                        std::move(eventContext),
                        std::vector<std::uint8_t>(runtime_->readBuffer.begin(),
                                                  runtime_->readBuffer.begin() + static_cast<std::ptrdiff_t>(bytesRead)),
                    });
                }
                (*scheduleRead)(generation);
            });
    };

    auto scheduleAccept = std::make_shared<std::function<void()>>();
    *scheduleAccept = [this, scheduleAccept, scheduleRead]() {
        // 保持单活动连接语义：监听成功与真实接入客户端分开投递事件。
        runtime_->acceptor.async_accept(
            [this, scheduleAccept, scheduleRead](const asio::error_code& ec, asio::ip::tcp::socket socket) mutable {
                if (ec) {
                    if (!runtime_->stopping.load(std::memory_order_relaxed) && listenContext_.has_value()) {
                        auto errorContext = *listenContext_;
                        errorContext.timestampMs = nowMs();
                        pushEvent(TransportErrorEvent{std::move(errorContext), ec.message()});
                    }
                    return;
                }

                if (clientContext_.has_value() && rejectNewConnection_) {
                    asio::error_code ignored;
                    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
                    socket.close(ignored);
                    if (listenContext_.has_value()) {
                        auto errorContext = *listenContext_;
                        errorContext.timestampMs = nowMs();
                        pushEvent(TransportErrorEvent{std::move(errorContext), "已有活动连接，已拒绝新客户端"});
                    }
                } else {
                    if (clientContext_.has_value()) {
                        auto closedContext = *clientContext_;
                        closedContext.timestampMs = nowMs();
                        pushEvent(TransportCloseEvent{std::move(closedContext), "新客户端已接管旧连接"});
                    }

                    runtime_->clientSocket = std::move(socket);
                    const auto generation = runtime_->clientGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
                    clientContext_ = makeClientContext(runtime_->clientSocket.remote_endpoint());
                    pushEvent(TransportOpenEvent{*clientContext_});
                    (*scheduleRead)(generation);
                }

                (*scheduleAccept)();
            });
    };

    (*scheduleAccept)();
    runtime_->ioThread = std::thread([this]() {
        runtime_->ioContext.run();
    });
    return true;
}

void TcpServerTransport::close() {
    if (runtime_ == nullptr) {
        return;
    }

    const auto previousClient = clientContext_;
    const auto previousListen = listenContext_;
    runtime_->stopping.store(true, std::memory_order_relaxed);
    asio::error_code ignored;
    {
        std::lock_guard lock(runtime_->socketMutex);
        runtime_->acceptor.cancel(ignored);
        runtime_->acceptor.close(ignored);
        runtime_->clientSocket.cancel(ignored);
        runtime_->clientSocket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
        runtime_->clientSocket.close(ignored);
    }
    runtime_->workGuard.reset();
    runtime_->ioContext.stop();
    if (runtime_->ioThread.joinable()) {
        runtime_->ioThread.join();
    }
    clientContext_.reset();
    listenContext_.reset();
    runtime_ = std::make_unique<Runtime>();
    setState(TransportState::Closed);

    if (previousClient.has_value()) {
        auto closedContext = *previousClient;
        closedContext.timestampMs = nowMs();
        pushEvent(TransportCloseEvent{std::move(closedContext), "用户主动关闭"});
    }
    if (previousListen.has_value()) {
        auto closedContext = *previousListen;
        closedContext.timestampMs = nowMs();
        pushEvent(TransportCloseEvent{std::move(closedContext), "监听已关闭"});
    }
}

bool TcpServerTransport::send(std::vector<std::uint8_t> bytes) {
    if (state() != TransportState::Open || !clientContext_.has_value() || bytes.empty()) {
        return false;
    }

    const auto [ok, error] = writeBytes(runtime_->clientSocket, runtime_->socketMutex, bytes);
    if (ok) {
        addTx(bytes.size());
        return true;
    }

    setState(TransportState::Error);
    auto errorContext = *clientContext_;
    errorContext.timestampMs = nowMs();
    pushEvent(TransportErrorEvent{std::move(errorContext), error});
    return false;
}

bool TcpServerTransport::enqueueSend(TransportTxTask task) {
    if (state() != TransportState::Open || !clientContext_.has_value()) {
        return false;
    }
    return enqueueWrite(runtime_->ioContext, runtime_->stopping, std::move(task), [this](TransportTxTask txTask) {
        const auto writeStartedAtMs = nowMs();
        const auto [ok, error] = writeBytes(runtime_->clientSocket, runtime_->socketMutex, txTask.payload);
        const auto finishedAtMs = nowMs();

        TransportTxState state = TransportTxState::Sent;
        if (!ok) {
            setState(TransportState::Error);
            if (clientContext_.has_value()) {
                auto errorContext = *clientContext_;
                errorContext.timestampMs = finishedAtMs;
                pushEvent(TransportErrorEvent{std::move(errorContext), error});
            }
            state = TransportTxState::Rejected;
        } else {
            addTx(txTask.payload.size());
            if (txTask.timeoutMs > 0 && finishedAtMs > writeStartedAtMs
                && finishedAtMs - writeStartedAtMs > txTask.timeoutMs) {
                state = TransportTxState::Timeout;
            }
        }

        pushEvent(TransportTxEvent{
            .requestId = txTask.requestId,
            .kind = txTask.kind,
            .state = state,
            .error = error,
            .bytes = txTask.payload.size(),
            .queuedAtMs = txTask.queuedAtMs,
            .finishedAtMs = finishedAtMs,
        });
    });
}

bool SerialTransport::open(const TransportConfig& config) {
    close();

    if (!std::holds_alternative<SerialConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::Serial, "", 0, nowMs(), true}, "配置类型不是 Serial"});
        return false;
    }

    const auto& serial = std::get<SerialConfig>(config);
    runtime_->readBuffer.resize(serial.readBufferBytes == 0U ? 1U : serial.readBufferBytes);
    setState(TransportState::Opening);

    try {
        runtime_->serialPort.open(serial.portName);
        runtime_->serialPort.set_option(asio::serial_port_base::baud_rate(serial.baudRate));
        runtime_->serialPort.set_option(asio::serial_port_base::character_size(serial.dataBits));
        runtime_->serialPort.set_option(asio::serial_port_base::parity(parseParity(serial.parity)));
        runtime_->serialPort.set_option(asio::serial_port_base::stop_bits(parseStopBits(serial.stopBits)));
        runtime_->serialPort.set_option(asio::serial_port_base::flow_control(parseFlowControl(serial.flowControl)));
    } catch (const std::exception& ex) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::Serial, serial.portName, 0, nowMs(), true}, ex.what()});
        return false;
    }

    context_ = ConnectionContext{
        .kind = TransportKind::Serial,
        .endpoint = serial.portName + "@" + std::to_string(serial.baudRate),
        .connectionId = makeConnectionId(),
        .timestampMs = nowMs(),
        .readyForIo = true,
    };

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*context_});

    auto scheduleRead = std::make_shared<std::function<void()>>();
    *scheduleRead = [this, scheduleRead]() {
        runtime_->serialPort.async_read_some(
            asio::buffer(runtime_->readBuffer),
            [this, scheduleRead](const asio::error_code& ec, std::size_t bytesRead) {
                if (ec) {
                    if (!runtime_->stopping.load(std::memory_order_relaxed) && context_.has_value()) {
                        setState(TransportState::Error);
                        auto errorContext = *context_;
                        errorContext.timestampMs = nowMs();
                        pushEvent(TransportErrorEvent{std::move(errorContext), ec.message()});
                    }
                    return;
                }

                if (bytesRead > 0 && context_.has_value()) {
                    addRx(bytesRead);
                    auto eventContext = *context_;
                    eventContext.timestampMs = nowMs();
                    pushEvent(TransportBytesEvent{
                        std::move(eventContext),
                        std::vector<std::uint8_t>(runtime_->readBuffer.begin(),
                                                  runtime_->readBuffer.begin() + static_cast<std::ptrdiff_t>(bytesRead)),
                    });
                }
                (*scheduleRead)();
            });
    };

    (*scheduleRead)();
    runtime_->ioThread = std::thread([this]() {
        runtime_->ioContext.run();
    });
    return true;
}

void SerialTransport::close() {
    if (runtime_ == nullptr) {
        return;
    }

    const auto previous = context_;
    // 串口关闭与 TCP 一样，先停线程，再重建运行时对象。
    runtime_->stopping.store(true, std::memory_order_relaxed);
    asio::error_code ignored;
    {
        std::lock_guard lock(runtime_->portMutex);
        runtime_->serialPort.cancel(ignored);
        runtime_->serialPort.close(ignored);
    }
    runtime_->workGuard.reset();
    runtime_->ioContext.stop();
    if (runtime_->ioThread.joinable()) {
        runtime_->ioThread.join();
    }
    context_.reset();
    runtime_ = std::make_unique<Runtime>();
    setState(TransportState::Closed);
    if (previous.has_value()) {
        auto closedContext = *previous;
        closedContext.timestampMs = nowMs();
        pushEvent(TransportCloseEvent{std::move(closedContext), "串口关闭"});
    }
}

bool SerialTransport::send(std::vector<std::uint8_t> bytes) {
    if (state() != TransportState::Open || !context_.has_value() || bytes.empty()) {
        return false;
    }

    const auto [ok, error] = writeBytes(runtime_->serialPort, runtime_->portMutex, bytes);
    if (ok) {
        addTx(bytes.size());
        return true;
    }

    setState(TransportState::Error);
    auto errorContext = *context_;
    errorContext.timestampMs = nowMs();
    pushEvent(TransportErrorEvent{std::move(errorContext), error});
    return false;
}

bool SerialTransport::enqueueSend(TransportTxTask task) {
    if (state() != TransportState::Open || !context_.has_value()) {
        return false;
    }
    return enqueueWrite(runtime_->ioContext, runtime_->stopping, std::move(task), [this](TransportTxTask txTask) {
        const auto writeStartedAtMs = nowMs();
        const auto [ok, error] = writeBytes(runtime_->serialPort, runtime_->portMutex, txTask.payload);
        const auto finishedAtMs = nowMs();

        TransportTxState state = TransportTxState::Sent;
        if (!ok) {
            setState(TransportState::Error);
            if (context_.has_value()) {
                auto errorContext = *context_;
                errorContext.timestampMs = finishedAtMs;
                pushEvent(TransportErrorEvent{std::move(errorContext), error});
            }
            state = TransportTxState::Rejected;
        } else {
            addTx(txTask.payload.size());
            if (txTask.timeoutMs > 0 && finishedAtMs > writeStartedAtMs
                && finishedAtMs - writeStartedAtMs > txTask.timeoutMs) {
                state = TransportTxState::Timeout;
            }
        }

        pushEvent(TransportTxEvent{
            .requestId = txTask.requestId,
            .kind = txTask.kind,
            .state = state,
            .error = error,
            .bytes = txTask.payload.size(),
            .queuedAtMs = txTask.queuedAtMs,
            .finishedAtMs = finishedAtMs,
        });
    });
}

} // namespace protoscope::transport
