#include "test_registry.hpp"

#include "protoscope/app/application.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/protocol_utils/codec.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
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

    bool enqueueSend(protoscope::transport::TransportTxTask task) override {
        sharedState_->sentBytes = std::move(task.payload);
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

struct QueuedEventTransport final : protoscope::transport::ITransport {
    struct State {
        bool openCalled{false};
        bool opened{false};
        std::optional<protoscope::transport::TransportConfig> lastConfig;
        std::vector<std::uint8_t> queuedRxBytes;
        std::vector<protoscope::transport::TransportEvent> pendingEvents;
        std::vector<protoscope::transport::TransportTxTask> sentTasks;
    };

    explicit QueuedEventTransport(std::shared_ptr<State> sharedState)
        : sharedState_(std::move(sharedState)) {}

    bool open(const protoscope::transport::TransportConfig& config) override {
        sharedState_->lastConfig = config;
        sharedState_->openCalled = true;
        sharedState_->opened = true;

        const protoscope::transport::ConnectionContext context{
            .endpoint = "queued://wave",
            .connectionId = 7,
            .timestampMs = 100,
            .readyForIo = true,
        };
        sharedState_->pendingEvents.push_back(protoscope::transport::TransportOpenEvent{context});
        if (!sharedState_->queuedRxBytes.empty()) {
            sharedState_->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{context, sharedState_->queuedRxBytes});
        }
        return true;
    }

    void close() override {
        sharedState_->opened = false;
    }

    bool send(std::vector<std::uint8_t> bytes) override {
        static_cast<void>(bytes);
        return sharedState_->opened;
    }

    bool enqueueSend(protoscope::transport::TransportTxTask task) override {
        sharedState_->pendingEvents.push_back(protoscope::transport::TransportTxEvent{
            .requestId = task.requestId,
            .kind = task.kind,
            .state = protoscope::transport::TransportTxState::Sent,
            .bytes = task.payload.size(),
            .queuedAtMs = task.queuedAtMs,
            .finishedAtMs = 120,
        });
        sharedState_->sentTasks.push_back(std::move(task));
        return sharedState_->opened;
    }

    protoscope::transport::TransportState state() const override {
        return sharedState_->opened ? protoscope::transport::TransportState::Open : protoscope::transport::TransportState::Closed;
    }

    std::vector<protoscope::transport::TransportEvent> takeEvents() override {
        std::vector<protoscope::transport::TransportEvent> drained;
        drained.swap(sharedState_->pendingEvents);
        return drained;
    }

    std::uint64_t txCount() const override {
        return 0;
    }

    std::uint64_t rxCount() const override {
        return static_cast<std::uint64_t>(sharedState_->queuedRxBytes.size());
    }

private:
    std::shared_ptr<State> sharedState_;
};

std::vector<std::uint8_t> makeRawImportStreamFrame(std::uint8_t value) {
    std::vector<std::uint8_t> frame{0xAA, 0x55, 0x01, value, 0x00, 0x00};
    const std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto crc = protoscope::protocol_utils::crc16Modbus(payload);
    frame[4] = static_cast<std::uint8_t>(crc & 0xFFU);
    frame[5] = static_cast<std::uint8_t>((crc >> 8U) & 0xFFU);
    return frame;
}

std::vector<std::uint8_t> makeRawImportStreamPayload(std::size_t frameCount) {
    std::vector<std::uint8_t> payload;
    payload.reserve(frameCount * 6U);
    for (std::size_t index = 0; index < frameCount; ++index) {
        const auto frame = makeRawImportStreamFrame(static_cast<std::uint8_t>(index & 0xFFU));
        payload.insert(payload.end(), frame.begin(), frame.end());
    }
    return payload;
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

std::uint64_t currentTimeMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::filesystem::path makeUniqueTempDir(const char* prefix) {
    const auto path = std::filesystem::temp_directory_path()
                    / (std::string(prefix) + "-" + std::to_string(currentTimeMs()));
    std::filesystem::create_directories(path);
    return path;
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

bool hasReceiveBytes(const protoscope::dock::ReceiveDockState& receiveState, const std::vector<std::uint8_t>& bytes) {
    std::vector<std::uint8_t> receivedBytes;
    for (const auto& row : receiveState.rows) {
        if (row.direction == "RX") {
            receivedBytes.insert(receivedBytes.end(), row.bytes.begin(), row.bytes.end());
        }
    }
    if (bytes.empty() || receivedBytes.size() < bytes.size()) {
        return false;
    }
    for (std::size_t offset = 0; offset + bytes.size() <= receivedBytes.size(); ++offset) {
        if (std::equal(bytes.begin(), bytes.end(), receivedBytes.begin() + offset)) {
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

    const std::vector<std::uint8_t> readVersionRequest{0xAA, 0x55, 0x01, 0x0D};
    const bool clientReceivedRequest = waitUntil([&]() {
        pumpPair(server, client);
        return hasReceiveBytes(client.docks().receiveState(), readVersionRequest);
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
        return hasReceiveBytes(server.docks().receiveState(), readVersionRequest);
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

void test_application_tx_overflow_popup_keeps_dialog_payload() {
    protoscope::app::Application application;
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");

    auto config = application.captureConfig();
    config.protocol.tx.maxPending = 0;
    config.protocol.tx.overflowPolicy = "reject_new";
    config.protocol.tx.overflowNotify = "popup_once";
    require(application.applyConfig(config), "应用应接受测试 TX 配置");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/dialog_requests", true), "dialog_requests 协议应可加载");

    application.updateControlValue("send_one", true);

    const auto dialogs = application.drainDialogRequests();
    require(dialogs.size() == 1, "发送队列溢出应生成一条弹窗请求");
    const auto& dialog = dialogs.front();
    require(dialog.kind == protoscope::scripting::DialogKind::Alert, "发送队列溢出应生成 alert");
    require(dialog.title == "发送队列已满", "溢出弹窗 title 不应改变");
    require(dialog.message == "发送队列已满", "溢出弹窗 message 不应改变");
    require(dialog.level == "warn", "溢出弹窗 level 不应改变");
    require(dialog.dedupeKey == "protocol.tx.overflow", "溢出弹窗 dedupeKey 不应改变");
    require(dialog.connection.endpoint.empty(), "无活动连接时应用层溢出弹窗仍应使用默认空 endpoint");
    require(dialog.connection.connectionId == 0, "无活动连接时应用层溢出弹窗 connectionId 应为 0");

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

void test_application_same_protocol_reload_keeps_runtime_stable() {
    const auto protocolDir = makeUniqueTempDir("protoscope-reload-table-request");
    {
        std::ofstream out(protocolDir / "main.lua");
        require(out.good(), "reload table request 测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"reload_request\", title = \"Reload Request\", controls = { { type = \"button\", id = \"send\", label = \"Send\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"send\" then\n";
        out << "    proto.request({ 0xff, 0x03, 0x00, 0x01 }, { timeout_ms = 1000, tag = \"reload-send\" })\n";
        out << "  end\n";
        out << "end\n";
    }

    auto transportState = std::make_shared<QueuedEventTransport::State>();
    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });

    require(application.initialize(), "应用应可初始化默认 Lua 工作区");
    require(application.reloadProtocolDirectory(protocolDir.generic_string(), true), "reload table request 协议应可加载");
    application.openTransport();
    application.pumpOnce();

    for (int attempt = 0; attempt < 5; ++attempt) {
        require(application.reloadProtocolDirectory(protocolDir.generic_string(), true), "同协议强制 reload 应持续成功");
        application.pumpOnce();
        const auto& lua = application.docks().luaState();
        require(lua.loaded, "同协议强制 reload 后协议仍应处于已加载状态");
        require(!lua.docks.empty(), "同协议强制 reload 后 Dock 快照应保持有效");
        require(!lua.controls.empty(), "同协议强制 reload 后控件快照应保持有效");
        require(lua.lastError.empty(), "同协议强制 reload 成功后不应残留错误");

        application.updateControlValue("send", true);
        require(transportState->sentTasks.size() == static_cast<std::size_t>(attempt + 1),
                "同协议强制 reload 后 Lua 控件仍应能发送 table payload request");
        const auto& sentTask = transportState->sentTasks.back();
        const std::vector<std::uint8_t> expectedPayload{0xff, 0x03, 0x00, 0x01};
        require(sentTask.payload == expectedPayload, "table payload 应按 number[] 转成原始字节，不应误走字符串转换");
    }

    application.shutdown();
}

void test_application_failed_reload_keeps_old_callbacks_alive() {
    const auto validProtocolDir = makeUniqueTempDir("protoscope-reload-valid");
    {
        std::ofstream out(validProtocolDir / "main.lua");
        require(out.good(), "有效 reload 隔离协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"stable\", title = \"Stable\", controls = { { type = \"button\", id = \"ping\", label = \"Ping\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"ping\" then proto.status.set(\"old callback alive\") end\n";
        out << "end\n";
    }

    const auto invalidProtocolDir = makeUniqueTempDir("protoscope-reload-invalid");
    {
        std::ofstream out(invalidProtocolDir / "main.lua");
        require(out.good(), "非法 reload 隔离协议应可写入");
        out << "function ui()\n";
        out << "  error(\"ui boom\")\n";
        out << "end\n";
        out << "function stream()\n";
        out << "  return \"bad stream\"\n";
        out << "end\n";
    }

    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(validProtocolDir.generic_string(), true), "有效协议应可加载");

    const auto before = application.docks().luaState();
    require(!application.reloadProtocolDirectory(invalidProtocolDir.generic_string(), true), "非法协议 reload 应失败");
    const auto& after = application.docks().luaState();
    require(after.protocolDir == before.protocolDir, "失败 reload 后旧协议目录应保留");
    require(after.docks.size() == before.docks.size(), "失败 reload 后旧 Dock 快照应保留");

    application.updateControlValue("ping", true);
    require(application.docks().configState().statusMessage == "old callback alive",
            "失败 reload 后旧协议控件回调仍应可用");

    application.shutdown();
}

void test_application_forced_reload_discards_old_tx_callback_outputs() {
    const auto protocolDir = makeUniqueTempDir("protoscope-reload-discard-old-tx");
    {
        std::ofstream out(protocolDir / "main.lua");
        require(out.good(), "reload 测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"reload_test\", title = \"Reload Test\", controls = { { type = \"button\", id = \"start\", label = \"Start\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"start\" then\n";
        out << "    proto.request({ 0x01 }, { timeout_ms = 1000, tag = \"first\" })\n";
        out << "  end\n";
        out << "end\n";
        out << "function on_tx(ctx, evt)\n";
        out << "  if evt.state == \"canceled\" then\n";
        out << "    proto.request({ 0x02 }, { timeout_ms = 1000, tag = \"old-cancel\" })\n";
        out << "    proto.status.set(\"old canceled leaked\", { level = \"error\" })\n";
        out << "  end\n";
        out << "end\n";
    }

    auto transportState = std::make_shared<QueuedEventTransport::State>();
    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });

    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir.generic_string(), true), "reload 测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("start", true);
    require(transportState->sentTasks.size() == 1, "首次控制应发送 1 条 request");
    application.pumpOnce();

    require(application.reloadProtocolDirectory(protocolDir.generic_string(), true), "强制重载应成功");
    application.pumpOnce();

    require(transportState->sentTasks.size() == 1, "强制重载应丢弃旧 on_tx 追加的 request");
    require(application.docks().configState().statusMessage.find("old canceled leaked") == std::string::npos,
            "强制重载应丢弃旧 on_tx 追加的状态");

    application.shutdown();
}

void test_application_request_done_success_does_not_set_comm_error() {
    const auto protocolDir = makeUniqueTempDir("protoscope-request-done-success");
    {
        std::ofstream out(protocolDir / "main.lua");
        require(out.good(), "request_done 成功测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"request_done_success\", title = \"Request Done\", controls = { { type = \"button\", id = \"read\", label = \"Read\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"read\" then proto.request({ 0x01 }, { timeout_ms = 1000, tag = \"read\" }) end\n";
        out << "end\n";
        out << "function on_tx(ctx, evt)\n";
        out << "  if evt.state == \"sent\" then proto.request_done({ ok = true, message = \"读取应答成功\" }) end\n";
        out << "end\n";
    }

    auto transportState = std::make_shared<QueuedEventTransport::State>();
    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });

    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir.generic_string(), true), "request_done 成功测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.docks().commState().lastError = "读取应答成功";
    application.updateControlValue("read", true);
    application.pumpOnce();

    require(application.docks().commState().lastError.empty(),
            "request_done ok=true 的成功消息不应显示到通讯配置错误");
    application.shutdown();
}

void test_application_request_done_failure_sets_comm_error() {
    const auto protocolDir = makeUniqueTempDir("protoscope-request-done-failure");
    {
        std::ofstream out(protocolDir / "main.lua");
        require(out.good(), "request_done 失败测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"request_done_failure\", title = \"Request Done\", controls = { { type = \"button\", id = \"read\", label = \"Read\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"read\" then proto.request({ 0x01 }, { timeout_ms = 1000, tag = \"read\" }) end\n";
        out << "end\n";
        out << "function on_tx(ctx, evt)\n";
        out << "  if evt.state == \"sent\" then proto.request_done({ ok = false, message = \"读取应答失败\" }) end\n";
        out << "end\n";
    }

    auto transportState = std::make_shared<QueuedEventTransport::State>();
    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });

    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir.generic_string(), true), "request_done 失败测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("read", true);
    application.pumpOnce();

    require(application.docks().commState().lastError == "读取应答失败",
            "request_done ok=false 的失败消息应显示到通讯配置错误");
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

void test_application_open_transport_uses_udp_peer_runtime_config() {
    protoscope::app::Application application;
    auto state = std::make_shared<RecordingTransport::State>();
    application.setTransportFactoryForTest([state](protoscope::transport::TransportKind kind) {
        require(kind == protoscope::transport::TransportKind::UdpPeer, "测试工厂应收到 UDP Peer transport kind");
        return std::unique_ptr<protoscope::transport::ITransport>(new RecordingTransport(state));
    });

    auto& comm = application.docks().commState();
    comm.kind = protoscope::transport::TransportKind::UdpPeer;
    comm.udpPeer.bindAddress = "127.0.0.1";
    comm.udpPeer.bindPort = 19001;
    comm.udpPeer.remoteHost = "192.0.2.10";
    comm.udpPeer.remotePort = 19002;

    application.openTransport();

    require(state->openCalled, "打开 UDP Peer 时应调用 transport.open");
    require(state->lastConfig.has_value(), "应记录 UDP Peer open 的真实入参");
    require(std::holds_alternative<protoscope::transport::UdpPeerConfig>(*state->lastConfig),
            "UDP Peer 模式应传入 UdpPeerConfig");

    const auto& udp = std::get<protoscope::transport::UdpPeerConfig>(*state->lastConfig);
    require(udp.bindAddress == "127.0.0.1", "open 应使用当前 UDP Peer 本地地址");
    require(udp.bindPort == 19001, "open 应使用当前 UDP Peer 本地端口");
    require(udp.remoteHost == "192.0.2.10", "open 应使用当前 UDP Peer 远端地址");
    require(udp.remotePort == 19002, "open 应使用当前 UDP Peer 远端端口");

    application.shutdown();
}

void test_application_set_log_level_updates_runtime_config() {
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");

    application.setLogLevel(protoscope::config::LogLevel::Error);

    const auto captured = application.captureConfig();
    require(captured.logging.level == protoscope::config::LogLevel::Error, "菜单切换应更新运行时日志等级");

    application.logger().warn("host", "warn should be filtered");
    application.logger().error("host", "error should be visible");
    const auto& logRows = application.docks().logState().rows;
    require(!logRows.empty(), "error 日志应进入日志面板");
    require(logRows.back().message == "error should be visible", "日志等级切换后应按新门限过滤");

    application.shutdown();
}

void test_application_wave_legend_visibility_config_roundtrip() {
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");

    auto config = application.captureConfig();
    config.gui.wave.showChannelLegend = false;
    config.gui.wave.showFftLegend = false;
    require(application.applyConfig(config), "图例显示配置应用失败");

    require(!application.docks().waveState().view.showChannelLegend, "应用配置后应隐藏图例");
    require(!application.docks().waveState().view.showFftLegend, "应用配置后应隐藏 FFT 图例");
    const auto captured = application.captureConfig();
    require(!captured.gui.wave.showChannelLegend, "captureConfig 应带出图例显示开关");
    require(!captured.gui.wave.showFftLegend, "captureConfig 应带出 FFT 图例显示开关");

    application.docks().waveState().view.showChannelLegend = true;
    application.docks().waveState().view.showFftLegend = true;
    const auto capturedLive = application.captureConfig();
    require(capturedLive.gui.wave.showChannelLegend, "captureConfig 不应覆盖 dock 中实时波形图例状态");
    require(capturedLive.gui.wave.showFftLegend, "captureConfig 不应覆盖 dock 中实时 FFT 图例状态");

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

void test_application_raw_capture_export_import_roundtrip() {
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    transportState->queuedRxBytes = {'O', 'K', '\r', '\n'};

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");

    application.openTransport();
    for (int i = 0; i < 4; ++i) {
        application.pumpOnce();
    }

    auto& wave = application.docks().waveState();
    wave.view.sampleFrequencyHz = 2048.0;
    wave.view.sampleFrequencyInput = "2048";
    require(wave.rawCapture.payload == transportState->queuedRxBytes, "实时 RX 后应缓存原始字节");

    const auto liveSnapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(!liveSnapshot.channels.empty(), "实时 RX 后应生成波形通道");
    require(liveSnapshot.channels.front().totalSamples > 0, "实时 RX 后应生成波形样本");

    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-application-roundtrip.psraw";
    std::filesystem::remove(tempPath);

    std::string error;
    require(application.exportWaveRawCapture(tempPath, error), "应用导出 psraw 应成功");
    const auto capture = protoscope::plot::readRawCaptureFile(tempPath, error);
    require(capture.has_value(), "导出后的 psraw 应可重新读取");
    require(capture->payload == transportState->queuedRxBytes, "导出文件应保留实时 RX 原始字节");

    application.resetWaveHistory();
    const auto emptySnapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity());
    require(emptySnapshot.channels.empty() || emptySnapshot.channels.front().totalSamples == 0, "清空历史后不应保留旧波形样本");

    require(application.importWaveRawCapture(*capture, error), "应用导入 psraw 应成功");
    const auto importedSnapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity());
    require(!importedSnapshot.channels.empty(), "导入 psraw 后应恢复波形通道");
    require(importedSnapshot.channels.front().totalSamples > 0, "导入 psraw 后应重新触发 on_bytes 生成样本");
    require(application.docks().waveState().view.sampleFrequencyHz == 2048.0, "导入 psraw 后应恢复文件中的采样频率");
    require(application.docks().waveState().rawCapture.payload == transportState->queuedRxBytes, "导入 psraw 后应回填原始缓冲");

    std::filesystem::remove(tempPath);
    application.shutdown();
}

void test_application_live_raw_capture_trims_to_limit() {
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    transportState->queuedRxBytes = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};

    protoscope::app::Application application;
    protoscope::config::AppConfig config;
    config.gui.rawCapture.liveLimitBytes = 4;
    require(application.initialize(), "应用初始化失败");
    require(application.applyConfig(config), "应用配置应可设置实时原始缓存上限");
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });

    application.openTransport();
    for (int i = 0; i < 4; ++i) {
        application.pumpOnce();
    }

    const auto& rawCapture = application.docks().waveState().rawCapture;
    const std::vector<std::uint8_t> expectedTail{0x04, 0x05, 0x06, 0x07};
    require(rawCapture.truncated, "实时原始缓存超过上限后应标记截断");
    require(rawCapture.payload == expectedTail, "实时原始缓存应只保留最新尾部字节");

    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-live-raw-capture-limit.psraw";
    std::filesystem::remove(tempPath);

    std::string error;
    require(application.exportWaveRawCapture(tempPath, error), "截断后的实时缓存仍应可导出");
    const auto capture = protoscope::plot::readRawCaptureFile(tempPath, error);
    require(capture.has_value(), "截断导出文件应可读取");
    require(capture->truncated, "截断标记应写入 psraw 文件头");
    require(capture->payload == expectedTail, "截断导出应只包含最近实时缓存");

    std::filesystem::remove(tempPath);
    application.shutdown();
}

void test_application_raw_capture_recording_preserves_full_rx_when_live_buffer_trims() {
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    transportState->queuedRxBytes = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};

    protoscope::app::Application application;
    protoscope::config::AppConfig config;
    config.gui.rawCapture.liveLimitBytes = 3;
    require(application.initialize(), "应用初始化失败");
    require(application.applyConfig(config), "应用配置应可设置实时原始缓存上限");
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });

    std::string error;
    require(application.stopRawCaptureRecording(error), "未开始录制时停止应无副作用");

    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-full-raw-recording.psraw";
    std::filesystem::remove(tempPath);
    require(application.startRawCaptureRecording(tempPath, error), "完整原始数据录制应可启动");
    require(application.isRawCaptureRecording(), "启动后应处于录制状态");

    application.openTransport();
    for (int index = 0; index < 4; ++index) {
        application.pumpOnce();
    }

    require(application.rawCaptureRecordingBytes() == transportState->queuedRxBytes.size(), "录制字节数应等于完整 RX 字节数");
    require(application.stopRawCaptureRecording(error), "完整原始数据录制应可停止");
    require(!application.isRawCaptureRecording(), "停止后不应处于录制状态");

    const auto& liveCapture = application.docks().waveState().rawCapture;
    require(liveCapture.truncated, "实时缓存仍应按上限截断");
    require(liveCapture.payload == std::vector<std::uint8_t>({0x14, 0x15, 0x16}), "实时缓存应只保留尾部字节");

    const auto recorded = protoscope::plot::readRawCaptureFile(tempPath, error);
    require(recorded.has_value(), "完整录制 psraw 应可读取");
    require(!recorded->truncated, "完整录制文件不应标记截断");
    require(recorded->payload == transportState->queuedRxBytes, "完整录制文件应保存全部 RX 原始字节");

    std::filesystem::remove(tempPath);
    application.shutdown();
}

void test_application_raw_capture_import_preserves_full_history() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/raw_import_history_limit";
    protoscope::app::Application application;
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");
    require(application.reloadProtocolDirectory(protocolDir, true), "低历史上限协议应可加载");

    const protoscope::plot::RawCaptureFileData capture{
        .protocolName = "raw_import_history_limit",
        .protocolDir = protocolDir,
        .sampleFrequencyHz = 1000.0,
        .capturedAtMs = 123,
        .payload = {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19},
    };

    std::string error;
    require(application.importWaveRawCapture(capture, error), "导入 psraw 应成功");
    const auto importedSnapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity());
    require(!importedSnapshot.channels.empty(), "导入后应生成波形通道");
    require(importedSnapshot.channels.front().totalSamples == capture.payload.size(),
            "导入回放应保留完整原始数据生成的波形历史");

    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-application-full-history.psraw";
    std::filesystem::remove(tempPath);
    require(application.exportWaveRawCapture(tempPath, error), "完整历史导入后应仍可导出 psraw");
    const auto exported = protoscope::plot::readRawCaptureFile(tempPath, error);
    require(exported.has_value(), "导出的 psraw 应可重新读取");
    require(exported->payload == capture.payload, "再次导出应保留完整原始 payload");
    std::filesystem::remove(tempPath);
    application.shutdown();
}

void test_application_raw_capture_import_replays_stream_in_chunks() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/raw_import_chunked_stream";
    constexpr std::size_t frameCount = 512;

    protoscope::app::Application application;
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");
    require(application.reloadProtocolDirectory(protocolDir, true), "分块导入 stream 协议应可加载");

    const auto payload = makeRawImportStreamPayload(frameCount);
    require(payload.size() > 2048, "测试 payload 应超过协议 stream buffer 容量");
    const protoscope::plot::RawCaptureFileData capture{
        .protocolName = "raw_import_chunked_stream",
        .protocolDir = protocolDir,
        .sampleFrequencyHz = 1000.0,
        .capturedAtMs = 123,
        .payload = payload,
    };

    std::string error;
    require(application.importWaveRawCapture(capture, error), "导入大 payload psraw 应成功");
    const auto importedSnapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity());
    require(!importedSnapshot.channels.empty(), "导入后应生成波形通道");
    require(importedSnapshot.channels.front().totalSamples == frameCount,
            "导入回放应按分块解析全部 stream 帧，而不是只保留尾部数据");
}

void test_application_transfer_log_frame_view_waits_for_rx_full_frame() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/stream_frame_only";
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    const auto frame = makeRawImportStreamFrame(0x34);
    transportState->queuedRxBytes.assign(frame.begin(), frame.begin() + 3);

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");

    application.openTransport();
    application.pumpOnce();
    require(application.docks().receiveState().rows.size() == 1, "原始收发记录应保留半包 chunk");
    require(application.docks().receiveState().frameRows.empty(), "逐帧视图不应把 RX 半包显示成碎行");

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 101,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{
        ctx,
        std::vector<std::uint8_t>(frame.begin() + 3, frame.end()),
    });
    application.pumpOnce();

    const auto& receive = application.docks().receiveState();
    require(receive.rows.size() == 2, "原始收发记录仍应保留两个 transport chunk");
    require(receive.frameRows.size() == 1, "逐帧视图应在完整帧到达后只显示一行");
    require(receive.frameRows.front().bytes == frame, "逐帧行应保存完整原始帧字节");
    require(receive.frameRows.front().message.find("stream_sample") != std::string::npos, "逐帧行应包含帧名");
    require(receive.frameRows.front().message.find("value=52") != std::string::npos, "逐帧行应包含字段值");
}

void test_application_transfer_log_frame_view_keeps_unmatched_tx_raw() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/stream_frame_only";
    auto state = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([state](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(state);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");
    application.openTransport();
    application.pumpOnce();

    require(application.sendManualPayload("01 02 03", true), "手动发送应成功");
    application.pumpOnce();
    const auto& receive = application.docks().receiveState();
    require(receive.rows.size() == 1, "原始收发记录应记录 TX chunk");
    require(receive.frameRows.size() == 1, "逐帧视图应保留无法匹配 schema 的 TX 原始行");
    require(receive.frameRows.front().direction == "TX", "TX fallback 行方向应保持 TX");
    require(receive.frameRows.front().bytes == std::vector<std::uint8_t>({0x01, 0x02, 0x03}), "TX fallback 行字节应保持原样");
}

void test_application_rx_events_are_processed_with_budget() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/stream_frame_only";
    constexpr std::size_t eventCount = 300;
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");

    application.openTransport();
    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 200,
        .readyForIo = true,
    };
    const auto frame = makeRawImportStreamFrame(0x21);
    for (std::size_t index = 0; index < eventCount; ++index) {
        transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{ctx, frame});
    }

    require(application.pumpOnce(), "第一轮 pump 应处理部分 RX 事件或保留待处理队列");
    const auto firstFrameRows = application.docks().receiveState().frameRows.size();
    require(firstFrameRows < eventCount, "单轮 pump 不应一次吃完超过预算的 RX 事件");

    for (int attempt = 0; attempt < 10 && application.docks().receiveState().frameRows.size() < eventCount; ++attempt) {
        application.pumpOnce();
    }
    require(application.docks().receiveState().frameRows.size() == eventCount, "后续 pump 应继续处理保留的 RX 事件");
}

void test_application_large_rx_event_drains_by_byte_budget() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/raw_import_chunked_stream";
    constexpr std::size_t frameCount = 5;
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");

    auto config = application.captureConfig();
    config.gui.realtimeBacklog.rxChunkBytesPerPump = 12;
    config.gui.realtimeBacklog.transferFrameRowsPerPump = 100;
    require(application.applyConfig(config), "实时 backlog 配置应可应用");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可重新加载");

    application.openTransport();
    application.pumpOnce();

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 300,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{ctx, makeRawImportStreamPayload(frameCount)});

    require(application.pumpOnce(), "第一轮 pump 应处理大 RX 的首个字节预算块");
    require(application.docks().receiveState().frameRows.size() < frameCount, "单个大 RX 事件不应在一轮 pump 内全部解析");
    require(application.docks().commState().pendingRxBytes > 0U, "剩余 RX 字节应保留到后续 pump");

    for (int attempt = 0; attempt < 10 && application.docks().receiveState().frameRows.size() < frameCount; ++attempt) {
        application.pumpOnce();
    }
    require(application.docks().receiveState().frameRows.size() == frameCount, "后续 pump 应继续 drain 大 RX 事件");
    require(application.docks().commState().pendingRxBytes == 0U, "大 RX drain 完成后不应残留 pending 字节");
}

void test_application_responsive_disconnect_discards_realtime_backlog() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/raw_import_chunked_stream";
    constexpr std::size_t frameCount = 5;
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");

    auto config = application.captureConfig();
    config.gui.realtimeBacklog.mode = "responsive";
    config.gui.realtimeBacklog.rxChunkBytesPerPump = 18;
    config.gui.realtimeBacklog.transferFrameRowsPerPump = 1;
    config.gui.realtimeBacklog.discardBacklogOnDisconnect = true;
    require(application.applyConfig(config), "responsive backlog 配置应可应用");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可重新加载");

    application.openTransport();
    application.pumpOnce();

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 400,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{ctx, makeRawImportStreamPayload(frameCount)});
    application.pumpOnce();

    require(application.docks().commState().pendingRxBytes > 0U, "断开前应存在 pending RX 字节");
    require(application.docks().commState().pendingTransferFrameRows > 0U, "断开前应存在 pending 逐帧行");

    application.closeTransport();
    require(application.docks().commState().state == protoscope::transport::TransportState::Closed, "断开后通讯状态应立即关闭");
    require(application.docks().commState().pendingRxBytes == 0U, "responsive 断开应清空 pending RX 字节");
    require(application.docks().commState().pendingTransferFrameRows == 0U, "responsive 断开应清空 pending 逐帧行");
}

void test_application_complete_disconnect_keeps_realtime_backlog() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/raw_import_chunked_stream";
    constexpr std::size_t frameCount = 5;
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");

    auto config = application.captureConfig();
    config.gui.realtimeBacklog.mode = "complete";
    config.gui.realtimeBacklog.rxChunkBytesPerPump = 18;
    config.gui.realtimeBacklog.transferFrameRowsPerPump = 1;
    require(application.applyConfig(config), "complete backlog 配置应可应用");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可重新加载");

    application.openTransport();
    application.pumpOnce();

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 500,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{ctx, makeRawImportStreamPayload(frameCount)});
    application.pumpOnce();

    require(application.docks().commState().pendingRxBytes > 0U, "complete 断开前应存在 pending RX 字节");
    require(application.docks().commState().pendingTransferFrameRows > 0U, "complete 断开前应存在 pending 逐帧行");

    application.closeTransport();
    require(application.docks().commState().state == protoscope::transport::TransportState::Closed, "complete 断开后通讯状态也应立即关闭");
    require(application.docks().commState().pendingRxBytes > 0U, "complete 断开后应保留 pending RX 字节");
    require(application.docks().commState().pendingTransferFrameRows > 0U, "complete 断开后应保留 pending 逐帧行");

    for (int attempt = 0; attempt < 20 && application.docks().receiveState().frameRows.size() < frameCount; ++attempt) {
        application.pumpOnce();
    }
    require(application.docks().receiveState().frameRows.size() == frameCount, "complete 模式应在后续 pump 小步补完逐帧 backlog");
    require(application.docks().commState().state == protoscope::transport::TransportState::Closed, "补完 backlog 不应重新打开通讯状态");
}

void test_application_transfer_frame_rows_drain_after_input_stops() {
    constexpr const char* protocolDir = "tests/fixtures/protocols/stream_frame_only";
    constexpr std::size_t frameCount = 2500;
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");

    application.openTransport();
    application.pumpOnce();

    for (std::size_t index = 0; index < frameCount; ++index) {
        require(application.sendManualPayload("01 02 03", true), "手动发送应成功");
    }

    application.pumpOnce();
    require(application.docks().receiveState().frameRows.size() == 2000,
            "单轮 pump 应按批量上限提交逐帧日志");
    require(application.pumpOnce(), "数据停止后 pending 逐帧日志仍应继续 drain");
    require(application.docks().receiveState().frameRows.size() == frameCount,
            "后续 pump 应补交剩余逐帧日志");
}

void test_application_plot_push_merges_same_channel_source() {
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/plot_merge", true), "plot_merge 协议应可加载");

    application.openTransport();
    application.pumpOnce();

    const auto snapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity());
    require(snapshot.channels.size() == 1, "plot_merge 应生成 1 个波形通道");
    require(snapshot.channels.front().totalSamples == 3, "同轮同源 push 合并后不应丢失较早时间样本");
    require(snapshot.channels.front().samples[0].time == 1.0, "合并后样本应按时间排序");
    require(snapshot.channels.front().samples[1].time == 2.0, "合并后应保留第二个同源样本");
    require(snapshot.channels.front().samples[2].time == 3.0, "不同 source 的后续样本仍应保留");
}

void test_application_plot_push_drains_with_budget_and_disconnect_discards_pending() {
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/plot_merge", true), "plot_merge 协议应可加载");

    auto config = application.captureConfig();
    config.gui.realtimeBacklog.plotAppendsPerPump = 1;
    require(application.applyConfig(config), "plot append 预算配置应可应用");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/plot_merge", true), "plot_merge 协议应可重新加载");

    application.openTransport();
    application.pumpOnce();

    auto snapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity());
    require(snapshot.channels.size() == 1, "plot_merge 应生成 1 个波形通道");
    require(snapshot.channels.front().totalSamples == 2, "同源 plot append 不应被预算拆开导致旧样本丢失");
    require(application.docks().commState().pendingPlotAppends == 1U, "剩余不同源 plot append 应保留到后续 pump");

    application.closeTransport();
    require(application.docks().commState().pendingPlotAppends == 0U, "responsive 断开应清空未提交 plot append");
    snapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::infinity());
    require(snapshot.channels.front().totalSamples == 2, "断开丢弃只影响未提交 backlog，不应破坏已入 buffer 样本");
}
