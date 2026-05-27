#include "test_registry.hpp"

#include "protoscope/app/application.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
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

struct RecordingTransport final : protoscope::transport::ITransport {
    struct State {
        bool openCalled{false};
        bool opened{false};
        std::optional<protoscope::transport::TransportConfig> lastConfig;
        std::vector<std::uint8_t> sentBytes;
    };

    explicit RecordingTransport(std::shared_ptr<State> sharedState)
        : sharedState_(std::move(sharedState)) {}

    bool open(const protoscope::transport::TransportConfig& config) override {
        sharedState_->lastConfig = config;
        sharedState_->openCalled = true;
        sharedState_->opened = true;
        return true;
    }

    void close() override {
        sharedState_->opened = false;
    }

    bool send(std::vector<std::uint8_t> bytes) override {
        sharedState_->sentBytes = std::move(bytes);
        return sharedState_->opened;
    }

    bool enqueueSend(std::vector<std::uint8_t> bytes) override {
        sharedState_->sentBytes = std::move(bytes);
        return sharedState_->opened;
    }

    protoscope::transport::TransportState state() const override {
        return sharedState_->opened ? protoscope::transport::TransportState::Open : protoscope::transport::TransportState::Closed;
    }

    std::vector<protoscope::transport::TransportEvent> takeEvents() override {
        return {};
    }

    std::uint64_t txCount() const override {
        return 0;
    }

    std::uint64_t rxCount() const override {
        return 0;
    }

    std::shared_ptr<State> sharedState_;
};

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

void test_application_failed_protocol_reload_keeps_previous_runtime() {
    protoscope::app::Application application;
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");
    require(application.reloadProtocolDirectory("protocols/lua_waveform_demo", true), "Lua 波形演示脚本应可加载");

    const auto before = application.docks().luaState();
    require(
        !application.reloadProtocolDirectory("tests/fixtures/protocols/invalid_controls", true),
        "非法协议脚本应加载失败");

    const auto& after = application.docks().luaState();
    require(after.protocolDir == before.protocolDir, "加载失败后不应改写当前运行协议目录");
    require(after.protocolName == before.protocolName, "加载失败后不应改写当前运行协议名称");
    require(after.scriptPath == before.scriptPath, "加载失败后不应改写当前入口脚本路径");
    require(!after.controlStates.empty(), "加载失败后应保留上一份动态控件快照");
    require(!after.lastError.empty(), "加载失败后应保留错误信息供界面展示");

    application.shutdown();
}

void test_application_open_transport_uses_serial_runtime_config() {
    protoscope::app::Application application;
    auto state = std::make_shared<RecordingTransport::State>();
    application.setTransportFactoryForTest([state](protoscope::transport::TransportKind kind) {
        require(kind == protoscope::transport::TransportKind::Serial, "测试工厂应收到串口 transport kind");
        return std::unique_ptr<protoscope::transport::ITransport>(new RecordingTransport(state));
    });

    auto& comm = application.docks().commState();
    comm.kind = protoscope::transport::TransportKind::Serial;
    comm.serial.portName = "COM42";
    comm.serial.baudRate = 230400;
    comm.serial.dataBits = 7;
    comm.serial.parity = "even";
    comm.serial.stopBits = "two";
    comm.serial.flowControl = "hardware";

    application.openTransport();

    require(state->openCalled, "打开连接时应调用 transport.open");
    require(state->lastConfig.has_value(), "应记录 open 的真实入参");
    require(std::holds_alternative<protoscope::transport::SerialConfig>(*state->lastConfig), "串口模式应传入 SerialConfig");

    const auto& serial = std::get<protoscope::transport::SerialConfig>(*state->lastConfig);
    require(serial.portName == "COM42", "open 应使用当前运行态端口名");
    require(serial.baudRate == 230400, "open 应使用当前运行态波特率");
    require(serial.dataBits == 7, "open 应使用当前运行态数据位");
    require(serial.parity == "even", "open 应使用当前运行态奇偶校验");
    require(serial.stopBits == "two", "open 应使用当前运行态停止位");
    require(serial.flowControl == "hardware", "open 应使用当前运行态流控");

    application.shutdown();
}

void test_application_logging_filters_script_and_host() {
    const auto tempRoot = std::filesystem::temp_directory_path() / "protoscope-logging-test";
    std::filesystem::create_directories(tempRoot);
    const auto logPath = tempRoot / "runtime.log";
    std::error_code ec;
    std::filesystem::remove(logPath, ec);

    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");

    auto config = application.captureConfig();
    config.logging.level = protoscope::config::LogLevel::Warn;
    config.logging.filePath = logPath.generic_string();
    require(application.applyConfig(config), "日志配置应用失败");

    application.logger().info("host", "this should be filtered");
    application.logger().warn("host", "host warn visible");
    application.logger().script("info", "script info filtered");
    application.logger().script("error", "script error visible");

    const auto& logRows = application.docks().logState().rows;
    const auto& scriptRows = application.docks().scriptState().rows;
    require(!logRows.empty(), "宿主 warn 日志应进入日志面板");
    require(logRows.back().message == "host warn visible", "日志面板应只保留通过阈值的宿主日志");
    require(logRows.back().direction == "WARN", "宿主 warn 日志方向应为 WARN");
    require(!scriptRows.empty(), "脚本 error 日志应进入脚本面板");
    require(scriptRows.back().message == "[error] script error visible", "脚本面板应只保留通过阈值的脚本日志");

    std::ifstream in(logPath);
    require(in.good(), "配置日志文件路径后应创建日志文件");
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(contents.find("host warn visible") != std::string::npos, "日志文件应包含宿主 warn 日志");
    require(contents.find("script error visible") != std::string::npos, "日志文件应包含脚本 error 日志");
    require(contents.find("this should be filtered") == std::string::npos, "日志文件不应包含被过滤的 info 日志");

    config.logging.filePath.clear();
    require(application.applyConfig(config), "禁用日志文件落盘失败");
    application.logger().error("host", "after disable file logging");
    const auto fileSize = std::filesystem::file_size(logPath);
    application.logger().error("host", "after disable file logging second");
    require(std::filesystem::file_size(logPath) == fileSize, "清空 file_path 后不应继续追加文件日志");

    application.shutdown();
}
