#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <asio/io_context.hpp>

namespace protoscope::transport {

enum class TransportKind {
    TcpClient,
    TcpServer,
    Serial,
    UdpPeer,
};

enum class TransportState {
    Closed,
    Opening,
    Open,
    Error,
};

struct TcpClientConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{9000};
    std::size_t readBufferBytes{64U * 1024U};
};

struct TcpServerConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{9000};
    bool rejectNewConnection{true};
    std::size_t readBufferBytes{64U * 1024U};
};

struct SerialConfig {
    std::string portName{"COM1"};
    std::uint32_t baudRate{115200};
    std::uint32_t dataBits{8};
    std::string parity{"none"};
    std::string stopBits{"one"};
    std::string flowControl{"none"};
    std::size_t readBufferBytes{64U * 1024U};
};

struct UdpPeerConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t bindPort{9001};
    std::string remoteHost{"127.0.0.1"};
    std::uint16_t remotePort{9000};
    std::size_t readBufferBytes{64U * 1024U};
};

std::vector<std::string> normalizeSerialPortNames(std::vector<std::string> ports);
std::vector<std::string> listAvailableSerialPorts();

using TransportConfig = std::variant<TcpClientConfig, TcpServerConfig, SerialConfig, UdpPeerConfig>;

class ITransport;

struct TransportDescriptor {
    TransportKind kind{TransportKind::TcpClient};
    std::string_view id{"tcp_client"};
    std::string_view label{"TCP 客户端"};
};

const std::vector<TransportDescriptor>& transportDescriptors();
std::optional<TransportKind> transportKindFromId(std::string_view id);
std::string_view transportKindId(TransportKind kind);
std::string_view transportKindLabel(TransportKind kind);
std::unique_ptr<ITransport> createTransport(TransportKind kind);

struct ConnectionContext {
    TransportKind kind{TransportKind::TcpClient};
    std::string endpoint{};
    std::uint64_t connectionId{0};
    std::uint64_t timestampMs{0};
    bool readyForIo{true};
};

struct TransportOpenEvent {
    ConnectionContext context;
};

struct TransportCloseEvent {
    ConnectionContext context;
    std::string reason;
};

struct TransportErrorEvent {
    ConnectionContext context;
    std::string message;
};

struct TransportBytesEvent {
    ConnectionContext context;
    std::vector<std::uint8_t> bytes;
};

enum class TransportTxKind {
    Send,
    Request,
};

enum class TransportTxState {
    Sent,
    Timeout,
    Rejected,
    Dropped,
    Canceled,
};

struct TransportTxTask {
    std::uint64_t requestId{0};
    TransportTxKind kind{TransportTxKind::Send};
    std::vector<std::uint8_t> payload;
    std::uint64_t timeoutMs{1000};
    std::uint64_t queuedAtMs{0};
};

struct TransportTxEvent {
    std::uint64_t requestId{0};
    TransportTxKind kind{TransportTxKind::Send};
    TransportTxState state{TransportTxState::Sent};
    std::string error;
    std::size_t bytes{0};
    std::uint64_t queuedAtMs{0};
    std::uint64_t finishedAtMs{0};
};

using TransportEvent =
    std::variant<TransportOpenEvent, TransportCloseEvent, TransportErrorEvent, TransportBytesEvent, TransportTxEvent>;

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool open(const TransportConfig& config) = 0;
    virtual void close() = 0;
    virtual bool send(std::vector<std::uint8_t> bytes) = 0;
    virtual bool enqueueSend(TransportTxTask task) = 0;
    virtual TransportState state() const = 0;
    virtual std::vector<TransportEvent> takeEvents() = 0;
    virtual std::uint64_t txCount() const = 0;
    virtual std::uint64_t rxCount() const = 0;
};

class TransportBase : public ITransport {
public:
    TransportBase();
    ~TransportBase() override;

    TransportState state() const override;
    std::vector<TransportEvent> takeEvents() override;
    std::uint64_t txCount() const override;
    std::uint64_t rxCount() const override;

protected:
    void pushEvent(TransportEvent event);
    void setState(TransportState next);
    void addTx(std::size_t size);
    void addRx(std::size_t size);
    static std::uint64_t nowMs();
    bool enqueueSendCommon(TransportTxTask task,
                           std::optional<ConnectionContext>& context,
                           asio::io_context& ioContext,
                           std::atomic<bool>& stopping,
                           std::function<std::pair<bool, std::string>(const std::vector<std::uint8_t>&)> writeBytes);

private:
    mutable std::mutex eventsMutex_;
    std::vector<TransportEvent> events_;
    std::atomic<TransportState> state_;
    std::atomic<std::uint64_t> txCount_;
    std::atomic<std::uint64_t> rxCount_;
};

class TcpClientTransport final : public TransportBase {
public:
    TcpClientTransport();
    ~TcpClientTransport() override;

    bool open(const TransportConfig& config) override;
    void close() override;
    bool send(std::vector<std::uint8_t> bytes) override;
    bool enqueueSend(TransportTxTask task) override;

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
    std::optional<ConnectionContext> context_;
};

class TcpServerTransport final : public TransportBase {
public:
    TcpServerTransport();
    ~TcpServerTransport() override;

    bool open(const TransportConfig& config) override;
    void close() override;
    bool send(std::vector<std::uint8_t> bytes) override;
    bool enqueueSend(TransportTxTask task) override;

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
    std::optional<ConnectionContext> listenContext_;
    std::optional<ConnectionContext> clientContext_;
    bool rejectNewConnection_{true};
};

class SerialTransport final : public TransportBase {
public:
    SerialTransport();
    ~SerialTransport() override;

    bool open(const TransportConfig& config) override;
    void close() override;
    bool send(std::vector<std::uint8_t> bytes) override;
    bool enqueueSend(TransportTxTask task) override;

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
    std::optional<ConnectionContext> context_;
};

class UdpPeerTransport final : public TransportBase {
public:
    UdpPeerTransport();
    ~UdpPeerTransport() override;

    bool open(const TransportConfig& config) override;
    void close() override;
    bool send(std::vector<std::uint8_t> bytes) override;
    bool enqueueSend(TransportTxTask task) override;

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
    std::optional<ConnectionContext> context_;
    std::string remoteEndpointText_;
};

} // namespace protoscope::transport
