#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace protoscope::transport {

enum class TransportKind {
    TcpClient,
    TcpServer,
    Serial,
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
};

struct TcpServerConfig {
    std::string bindAddress{"0.0.0.0"};
    std::uint16_t port{9000};
    bool rejectNewConnection{true};
};

struct SerialConfig {
    std::string portName{"COM1"};
    std::uint32_t baudRate{115200};
    std::uint32_t dataBits{8};
    std::string parity{"none"};
    std::string stopBits{"one"};
    std::string flowControl{"none"};
};

using TransportConfig = std::variant<TcpClientConfig, TcpServerConfig, SerialConfig>;

struct ConnectionContext {
    TransportKind kind{TransportKind::TcpClient};
    std::string endpoint;
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

using TransportEvent = std::variant<TransportOpenEvent, TransportCloseEvent, TransportErrorEvent, TransportBytesEvent>;

class ITransport {
public:
    virtual ~ITransport() = default;

    virtual bool open(const TransportConfig& config) = 0;
    virtual void close() = 0;
    virtual bool send(std::vector<std::uint8_t> bytes) = 0;
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

private:
    struct Runtime;
    std::unique_ptr<Runtime> runtime_;
    std::optional<ConnectionContext> context_;
};

} // namespace protoscope::transport
