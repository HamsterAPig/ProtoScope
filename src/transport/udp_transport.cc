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
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/udp.hpp>
#include <asio/post.hpp>

namespace protoscope::transport {

namespace {

    std::uint64_t makeUdpConnectionId()
    {
        static std::atomic<std::uint64_t> counter{1};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }

    std::string endpointText(const asio::ip::udp::endpoint& endpoint)
    {
        return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
    }

    std::string endpointText(const std::string& host, std::uint16_t port)
    {
        return host + ":" + std::to_string(port);
    }

    std::pair<bool, std::string> sendUdpBytes(asio::ip::udp::socket& socket,
                                              std::mutex& mutex,
                                              const asio::ip::udp::endpoint& remote,
                                              const std::vector<std::uint8_t>& bytes)
    {
        try {
            std::lock_guard lock(mutex);
            socket.send_to(asio::buffer(bytes), remote);
            return {true, {}};
        } catch (const std::exception& ex) {
            return {false, ex.what()};
        }
    }

} // namespace

struct UdpPeerTransport::Runtime {
    using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

    asio::io_context ioContext;
    std::optional<WorkGuard> workGuard{asio::make_work_guard(ioContext)};
    asio::ip::udp::resolver resolver{ioContext};
    asio::ip::udp::socket socket{ioContext};
    asio::ip::udp::endpoint remoteEndpoint;
    asio::ip::udp::endpoint senderEndpoint;
    std::thread ioThread;
    std::vector<std::uint8_t> readBuffer{};
    std::mutex socketMutex;
    std::atomic<bool> stopping{false};
};

UdpPeerTransport::UdpPeerTransport() : runtime_(std::make_unique<Runtime>()) {}

UdpPeerTransport::~UdpPeerTransport()
{
    close();
}

bool UdpPeerTransport::open(const TransportConfig& config)
{
    close();

    if (!std::holds_alternative<UdpPeerConfig>(config)) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::UdpPeer, "", 0, nowMs(), true}, "配置类型不是 UDP Peer"});
        return false;
    }

    const auto& udp = std::get<UdpPeerConfig>(config);
    runtime_->readBuffer.resize(udp.readBufferBytes == 0U ? 1U : udp.readBufferBytes);
    const auto fallbackEndpoint = endpointText(udp.remoteHost, udp.remotePort);
    setState(TransportState::Opening);

    try {
        const auto bindAddress = asio::ip::make_address(udp.bindAddress);
        runtime_->socket.open(bindAddress.is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4());
        runtime_->socket.bind(asio::ip::udp::endpoint(bindAddress, udp.bindPort));

        auto results = runtime_->resolver.resolve(
            runtime_->socket.local_endpoint().protocol(), udp.remoteHost, std::to_string(udp.remotePort));
        runtime_->remoteEndpoint = results.begin()->endpoint();
    } catch (const std::exception& ex) {
        setState(TransportState::Error);
        pushEvent(TransportErrorEvent{{TransportKind::UdpPeer, fallbackEndpoint, 0, nowMs(), true}, ex.what()});
        return false;
    }

    try {
        remoteEndpointText_ = endpointText(runtime_->remoteEndpoint);
        context_ = ConnectionContext{
            .kind = TransportKind::UdpPeer,
            .endpoint = endpointText(runtime_->socket.local_endpoint()) + " -> " + remoteEndpointText_,
            .connectionId = makeUdpConnectionId(),
            .timestampMs = nowMs(),
            .readyForIo = true,
        };
    } catch (const std::exception&) {
        remoteEndpointText_ = fallbackEndpoint;
        context_ = ConnectionContext{
            .kind = TransportKind::UdpPeer,
            .endpoint = fallbackEndpoint,
            .connectionId = makeUdpConnectionId(),
            .timestampMs = nowMs(),
            .readyForIo = true,
        };
    }

    setState(TransportState::Open);
    pushEvent(TransportOpenEvent{*context_});

    auto scheduleRead = std::make_shared<std::function<void()>>();
    *scheduleRead = [this, scheduleRead]() {
        runtime_->socket.async_receive_from(
            asio::buffer(runtime_->readBuffer),
            runtime_->senderEndpoint,
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
                    eventContext.endpoint = endpointText(runtime_->senderEndpoint);
                    eventContext.timestampMs = nowMs();
                    pushEvent(TransportBytesEvent{
                        std::move(eventContext),
                        std::vector<std::uint8_t>(
                            runtime_->readBuffer.begin(),
                            runtime_->readBuffer.begin() + static_cast<std::ptrdiff_t>(bytesRead)),
                    });
                }
                (*scheduleRead)();
            });
    };

    (*scheduleRead)();
    runtime_->ioThread = std::thread([this]() { runtime_->ioContext.run(); });
    return true;
}

void UdpPeerTransport::close()
{
    const auto previous = context_;
    runtime_->stopping.store(true, std::memory_order_relaxed);
    asio::error_code ignored;
    {
        std::lock_guard lock(runtime_->socketMutex);
        runtime_->socket.cancel(ignored);
        runtime_->socket.close(ignored);
    }
    runtime_->workGuard.reset();
    runtime_->ioContext.stop();
    if (runtime_->ioThread.joinable()) {
        runtime_->ioThread.join();
    }
    context_.reset();
    remoteEndpointText_.clear();
    runtime_ = std::make_unique<Runtime>();
    setState(TransportState::Closed);
    if (previous.has_value()) {
        auto closedContext = *previous;
        closedContext.timestampMs = nowMs();
        pushEvent(TransportCloseEvent{std::move(closedContext), "UDP Peer 已关闭"});
    }
}

bool UdpPeerTransport::send(std::vector<std::uint8_t> bytes)
{
    if (state() != TransportState::Open || !context_.has_value() || bytes.empty()) {
        return false;
    }

    const auto [ok, error] = sendUdpBytes(runtime_->socket, runtime_->socketMutex, runtime_->remoteEndpoint, bytes);
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

bool UdpPeerTransport::enqueueSend(TransportTxTask task)
{
    if (state() != TransportState::Open || !context_.has_value() || task.payload.empty()) {
        return false;
    }
    asio::post(runtime_->ioContext, [this, txTask = std::move(task)]() mutable {
        const auto writeStartedAtMs = nowMs();
        const auto [ok, error] =
            sendUdpBytes(runtime_->socket, runtime_->socketMutex, runtime_->remoteEndpoint, txTask.payload);
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
            if (txTask.timeoutMs > 0 && finishedAtMs > writeStartedAtMs &&
                finishedAtMs - writeStartedAtMs > txTask.timeoutMs) {
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
    return true;
}

} // namespace protoscope::transport
