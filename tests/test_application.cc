#include "protoscope/app/application.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/session/session_package.hpp"

#include "test_helpers.hpp"
#include "test_registry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <elf_static_view/project.hpp>

namespace {

using protoscope::tests::ScopedTempPath;
using protoscope::tests::makeUniqueTempDir;
using protoscope::tests::require;

std::uint64_t currentTimeMs();

struct RecordingTransport final : protoscope::transport::ITransport {
    struct State {
        bool openCalled{false};
        bool opened{false};
        std::optional<protoscope::transport::TransportConfig> lastConfig;
        std::vector<std::uint8_t> sentBytes;
    };

    explicit RecordingTransport(std::shared_ptr<State> sharedState) : sharedState_(std::move(sharedState)) {}

    bool open(const protoscope::transport::TransportConfig& config) override
    {
        sharedState_->lastConfig = config;
        sharedState_->openCalled = true;
        sharedState_->opened = true;
        return true;
    }

    void close() override { sharedState_->opened = false; }

    bool send(std::vector<std::uint8_t> bytes) override
    {
        sharedState_->sentBytes = std::move(bytes);
        return sharedState_->opened;
    }

    bool enqueueSend(protoscope::transport::TransportTxTask task) override
    {
        sharedState_->sentBytes = std::move(task.payload);
        return sharedState_->opened;
    }

    protoscope::transport::TransportState state() const override
    {
        return sharedState_->opened ? protoscope::transport::TransportState::Open
                                    : protoscope::transport::TransportState::Closed;
    }

    std::vector<protoscope::transport::TransportEvent> takeEvents() override { return {}; }

    std::uint64_t txCount() const override { return 0; }

    std::uint64_t rxCount() const override { return 0; }

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

    explicit QueuedEventTransport(std::shared_ptr<State> sharedState) : sharedState_(std::move(sharedState)) {}

    bool open(const protoscope::transport::TransportConfig& config) override
    {
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
            sharedState_->pendingEvents.push_back(
                protoscope::transport::TransportBytesEvent{context, sharedState_->queuedRxBytes});
        }
        return true;
    }

    void close() override { sharedState_->opened = false; }

    bool send(std::vector<std::uint8_t> bytes) override
    {
        static_cast<void>(bytes);
        return sharedState_->opened;
    }

    bool enqueueSend(protoscope::transport::TransportTxTask task) override
    {
        sharedState_->pendingEvents.push_back(protoscope::transport::TransportTxEvent{
            .requestId = task.requestId,
            .kind = task.kind,
            .state = protoscope::transport::TransportTxState::Sent,
            .error = {},
            .bytes = task.payload.size(),
            .queuedAtMs = task.queuedAtMs,
            .finishedAtMs = currentTimeMs(),
        });
        sharedState_->sentTasks.push_back(std::move(task));
        return sharedState_->opened;
    }

    protoscope::transport::TransportState state() const override
    {
        return sharedState_->opened ? protoscope::transport::TransportState::Open
                                    : protoscope::transport::TransportState::Closed;
    }

    std::vector<protoscope::transport::TransportEvent> takeEvents() override
    {
        std::vector<protoscope::transport::TransportEvent> drained;
        drained.swap(sharedState_->pendingEvents);
        return drained;
    }

    std::uint64_t txCount() const override { return 0; }

    std::uint64_t rxCount() const override { return static_cast<std::uint64_t>(sharedState_->queuedRxBytes.size()); }

private:
    std::shared_ptr<State> sharedState_;
};

std::vector<std::uint8_t> makeRawImportStreamFrame(std::uint8_t value)
{
    std::vector<std::uint8_t> frame{0xAA, 0x55, 0x01, value, 0x00, 0x00};
    const std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto crc = protoscope::protocol_utils::crc16Modbus(payload);
    frame[4] = static_cast<std::uint8_t>(crc & 0xFFU);
    frame[5] = static_cast<std::uint8_t>((crc >> 8U) & 0xFFU);
    return frame;
}

std::vector<std::uint8_t> makeRawImportStreamPayload(std::size_t frameCount)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(frameCount * 6U);
    for (std::size_t index = 0; index < frameCount; ++index) {
        const auto frame = makeRawImportStreamFrame(static_cast<std::uint8_t>(index & 0xFFU));
        payload.insert(payload.end(), frame.begin(), frame.end());
    }
    return payload;
}

std::filesystem::path makeSchemaSwitchReplayProtocolDir()
{
    const auto protocolDir = makeUniqueTempDir("protoscope-schema-switch-replay");
    std::ofstream script(protocolDir / "main.lua");
    script << "local function noop(...) end\n";
    script << "function stream()\n";
    script << "  return {\n";
    script << "    buffer = { capacity = 64, overflow = 'drop_oldest' },\n";
    script << "    frames = {\n";
    script << "      {\n";
    script << "        name = 'stream_sample',\n";
    script << "        header = { 0xAA, 0x55 },\n";
    script << "        len = { offset = 3, type = 'u8', means = 'payload', extra = 5 },\n";
    script << "        crc = { type = 'crc16_modbus', order = 'lo_hi' },\n";
    script << "        fields = {\n";
    script << "          { name = 'value', type = 'u8', offset = 4 },\n";
    script << "        },\n";
    script << "        on_frame = noop,\n";
    script << "      },\n";
    script << "    },\n";
    script << "    on_error = noop,\n";
    script << "  }\n";
    script << "end\n";
    return protocolDir;
}

bool waitUntil(auto&& predicate)
{
    return protoscope::tests::waitUntil(std::forward<decltype(predicate)>(predicate), 80);
}

std::uint64_t currentTimeMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

protoscope::plot::RawCaptureEvent makePlotSetupEvent(bool resetHistory = true)
{
    protoscope::plot::RawCaptureEvent event;
    event.type = protoscope::plot::RawCaptureEventType::PlotSetup;
    event.timestampMs = 100;
    event.plotSetup.source = "raw_snapshot";
    event.plotSetup.resetHistory = resetHistory;
    event.plotSetup.channels = {
        {.label = "温度A",
         .unit = "℃",
         .ratio = 0.5,
         .scale = 2.0,
         .offset = -1.0,
         .color = std::array<float, 4>{26.0F / 255.0F, 51.0F / 255.0F, 76.0F / 255.0F, 1.0F},
         .lineWidth = std::optional<float>{3.25F}},
    };
    event.plotSetup.view.timeScale = 0.5;
    event.plotSetup.view.timeUnit = "ms";
    event.plotSetup.view.verticalMin = -20.0;
    event.plotSetup.view.verticalMax = 100.0;
    event.plotSetup.view.verticalUnit = "℃";
    event.plotSetup.view.historyLimit = 128;
    return event;
}

std::optional<std::uint16_t> findListenPort(const protoscope::dock::LogDockState& logState)
{
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

bool hasReceiveBytes(const protoscope::dock::ReceiveDockState& receiveState, const std::vector<std::uint8_t>& bytes)
{
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

bool hasHostLogMessage(const protoscope::dock::LogDockState& logState,
                       const std::string& endpoint,
                       const std::string& messageFragment)
{
    for (const auto& row : logState.rows) {
        if (row.endpoint == endpoint && row.message.find(messageFragment) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool hasScriptEvent(const protoscope::dock::ScriptDockState& scriptState, const std::string& name)
{
    for (const auto& row : scriptState.rows) {
        if (row.direction == "EVENT" && row.message.find(name + ":") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void pumpPair(protoscope::app::Application& server, protoscope::app::Application& client)
{
    server.pumpOnce();
    client.pumpOnce();
}

int countOpenRows(const protoscope::dock::LogDockState& logState)
{
    int count = 0;
    for (const auto& row : logState.rows) {
        if (row.direction == "OPEN") {
            ++count;
        }
    }
    return count;
}

elf_static_view::ExpandedNode makeElfSymbolNode(std::string path, std::string typeName, std::uint64_t address)
{
    return elf_static_view::ExpandedNode{
        .path = std::move(path),
        .display_name = "target",
        .type_name = std::move(typeName),
        .type_id = {},
        .type_kind = elf_static_view::TypeKind::Base,
        .availability = elf_static_view::Availability::StaticAddressKnown,
        .absolute_address = address,
        .relative_offset = std::nullopt,
        .byte_size = std::nullopt,
        .array_count = std::nullopt,
        .array_stride = std::nullopt,
        .depth = 0,
        .children_lazy = false,
        .children = {},
        .export_path = {},
    };
}

void writeElfSymbolDump(const std::filesystem::path& path,
                        std::optional<std::uint64_t> targetAddress,
                        std::string targetType)
{
    elf_static_view::ProjectModel model;
    if (targetAddress.has_value()) {
        model.expanded.push_back(makeElfSymbolNode("global.target", std::move(targetType), *targetAddress));
    }
    model.expanded.push_back(makeElfSymbolNode("global.target_shadow", "uint8_t", 0x20000080ULL));

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << elf_static_view::render_dump_json(model);
}

const protoscope::scripting::ElfSymbolValue* findElfSymbolControl(const protoscope::app::Application& application,
                                                                  const std::string& id)
{
    const auto& controls = application.docks().luaState().controlStates;
    const auto iter = std::find_if(controls.begin(), controls.end(), [&](const auto& control) {
        return control.descriptor.id == id;
    });
    if (iter == controls.end()) {
        return nullptr;
    }
    return std::get_if<protoscope::scripting::ElfSymbolValue>(&iter->value);
}

std::size_t countScriptEvents(const protoscope::dock::ScriptDockState& scriptState, const std::string& name)
{
    return static_cast<std::size_t>(std::count_if(scriptState.rows.begin(), scriptState.rows.end(), [&](const auto& row) {
        return row.direction == "EVENT" && row.message.find(name + ":") != std::string::npos;
    }));
}

void writeTextFile(const std::filesystem::path& path, std::string_view text)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.good()) {
        throw std::runtime_error("无法写入测试文件: " + path.generic_string());
    }
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

} // namespace

void test_application_tcp_lua_read_version_roundtrip()
{
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
               countOpenRows(server.docks().logState()) >= 2 && countOpenRows(client.docks().logState()) >= 1;
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

void test_application_lua_controls_without_connection()
{
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

void test_application_tx_overflow_popup_keeps_dialog_payload()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");

    auto config = application.captureConfig();
    config.protocol.tx.maxPending = 0;
    config.protocol.tx.overflowPolicy = "reject_new";
    config.protocol.tx.overflowNotify = "popup_once";
    require(application.applyConfig(config), "应用应接受测试 TX 配置");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/dialog_requests", true),
            "dialog_requests 协议应可加载");

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

void test_application_refreshes_selected_elf_symbol_controls_silently()
{
    const ScopedTempPath tempDir(makeUniqueTempDir("protoscope-elf-refresh-silent"));
    const auto elfPath = tempDir.path() / "symbols.json";
    writeElfSymbolDump(elfPath, 0x20000010ULL, "uint32_t");

    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/elf_symbol_combo", true),
            "elf_symbol_combo 协议应可加载");
    std::string error;
    require(application.loadElfStaticAddressFile(elfPath, error), "初始 ELF 数据应可加载");

    application.updateControlValue("target",
                                   protoscope::scripting::ElfSymbolValue{
                                       .label = "global.target",
                                       .value = "0x20000010",
                                       .type = "uint32_t",
                                   });
    const auto beforeEvents = countScriptEvents(application.docks().scriptState(), "symbol");

    writeElfSymbolDump(elfPath, 0x20000044ULL, "uint64_t");
    require(application.loadElfStaticAddressFile(elfPath, error), "更新后的 ELF 数据应可加载");
    application.refreshSelectedElfSymbolControls();

    const auto* refreshed = findElfSymbolControl(application, "target");
    require(refreshed != nullptr, "应能找到已选 ELF 控件值");
    require(refreshed->label == "global.target", "静默刷新不应改变 label");
    require(refreshed->value == "0x20000044", "静默刷新应更新地址");
    require(refreshed->type == "uint64_t", "静默刷新应更新类型");
    require(countScriptEvents(application.docks().scriptState(), "symbol") == beforeEvents,
            "默认静默刷新不应触发 on_control");

    writeElfSymbolDump(elfPath, std::nullopt, "uint32_t");
    require(application.loadElfStaticAddressFile(elfPath, error), "缺失目标 label 的 ELF 数据应可加载");
    application.refreshSelectedElfSymbolControls();
    refreshed = findElfSymbolControl(application, "target");
    require(refreshed != nullptr && refreshed->value == "0x20000044" && refreshed->type == "uint64_t",
            "label 消失时旧地址和类型应保持不变");

    application.shutdown();
}

void test_application_refreshes_selected_elf_symbol_controls_with_on_control()
{
    const ScopedTempPath tempDir(makeUniqueTempDir("protoscope-elf-refresh-emit"));
    const auto elfPath = tempDir.path() / "symbols.json";
    writeElfSymbolDump(elfPath, 0x20000010ULL, "uint32_t");

    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");
    auto config = application.captureConfig();
    config.gui.elfSymbolCombo.autoRefreshEmitOnControl = true;
    require(application.applyConfig(config), "应用应接受 ELF 自动刷新回调配置");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/elf_symbol_combo", true),
            "elf_symbol_combo 协议应可加载");
    std::string error;
    require(application.loadElfStaticAddressFile(elfPath, error), "初始 ELF 数据应可加载");

    application.updateControlValue("target",
                                   protoscope::scripting::ElfSymbolValue{
                                       .label = "global.target",
                                       .value = "0x20000010",
                                       .type = "uint32_t",
                                   });
    const auto beforeEvents = countScriptEvents(application.docks().scriptState(), "symbol");

    writeElfSymbolDump(elfPath, 0x20000088ULL, "uint16_t");
    require(application.loadElfStaticAddressFile(elfPath, error), "更新后的 ELF 数据应可加载");
    application.refreshSelectedElfSymbolControls();

    const auto* refreshed = findElfSymbolControl(application, "target");
    require(refreshed != nullptr, "应能找到已选 ELF 控件值");
    require(refreshed->value == "0x20000088", "回调刷新应更新地址");
    require(refreshed->type == "uint16_t", "回调刷新应更新类型");
    require(countScriptEvents(application.docks().scriptState(), "symbol") == beforeEvents + 1,
            "开启 emit 后刷新应触发一次 on_control");

    application.shutdown();
}

void test_application_clear_elf_static_address_file_resets_queries()
{
    const ScopedTempPath tempDir(makeUniqueTempDir("protoscope-elf-clear-app"));
    const auto elfPath = tempDir.path() / "symbols.json";
    writeElfSymbolDump(elfPath, 0x20000010ULL, "uint32_t");

    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");
    std::string error;
    require(application.loadElfStaticAddressFile(elfPath, error), "初始 ELF 数据应可加载");
    require(!application.queryElfStaticAddresses("target", 16).empty(), "加载后应可查询 ELF 符号");
    const auto loadedRevision = application.elfStaticAddressRevision();

    application.clearElfStaticAddressFile();

    require(application.queryElfStaticAddresses("target", 16).empty(), "清理后查询结果应为空");
    require(application.elfStaticAddressRevision() > loadedRevision, "清理已加载模型应推进 ELF revision");

    application.shutdown();
}

void test_application_failed_protocol_reload_keeps_previous_runtime()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");
    require(application.reloadProtocolDirectory("protocols/lua_waveform_demo", true), "Lua 波形演示脚本应可加载");

    const auto before = application.docks().luaState();
    require(!application.reloadProtocolDirectory("tests/fixtures/protocols/invalid_controls", true),
            "非法协议脚本应加载失败");

    const auto& after = application.docks().luaState();
    require(after.protocolDir == before.protocolDir, "加载失败后不应改写当前运行协议目录");
    require(after.protocolName == before.protocolName, "加载失败后不应改写当前运行协议名称");
    require(after.scriptPath == before.scriptPath, "加载失败后不应改写当前入口脚本路径");
    require(!after.controlStates.empty(), "加载失败后应保留上一份动态控件快照");
    require(!after.lastError.empty(), "加载失败后应保留错误信息供界面展示");

    application.shutdown();
}

void test_application_same_protocol_reload_keeps_runtime_stable()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-reload-table-request"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "reload table request 测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"reload_request\", title = \"Reload Request\", controls = { { type = \"button\", id "
               "= \"send\", label = \"Send\" } } } }\n";
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
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "reload table request 协议应可加载");
    application.openTransport();
    application.pumpOnce();

    for (int attempt = 0; attempt < 5; ++attempt) {
        require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
                "同协议强制 reload 应持续成功");
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

void test_application_same_protocol_reload_without_force_preserves_runtime_state()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-reload-no-force-state"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "非强制 reload 测试协议应可写入");
        out << "local counter = 0\n";
        out << "function ui()\n";
        out << "  return { { id = \"counter\", title = \"Counter\", controls = { { type = \"button\", id = \"tick\", "
               "label = \"Tick\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"tick\" then\n";
        out << "    counter = counter + 1\n";
        out << "    proto.status.set(\"count:\" .. counter)\n";
        out << "  end\n";
        out << "end\n";
    }

    protoscope::app::Application application;
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "非强制 reload 测试协议应可加载");

    application.updateControlValue("tick", true);
    require(application.docks().configState().statusMessage == "count:1", "首次触发应把计数写入状态栏");

    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), false), "同协议非强制 reload 应成功");
    const auto& lua = application.docks().luaState();
    require(lua.loaded, "同协议非强制 reload 后协议仍应处于已加载状态");
    require(lua.protocolDir == protocolDir.path().generic_string(), "同协议非强制 reload 后协议目录应保持不变");
    require(lua.lastError.empty(), "同协议非强制 reload 成功后不应残留错误");

    application.updateControlValue("tick", true);
    require(application.docks().configState().statusMessage == "count:2", "同协议非强制 reload 不应重置 Lua 本地状态");

    application.shutdown();
}

void test_application_failed_reload_keeps_old_callbacks_alive()
{
    const ScopedTempPath validProtocolDir(makeUniqueTempDir("protoscope-reload-valid"));
    {
        std::ofstream out(validProtocolDir.path() / "main.lua");
        require(out.good(), "有效 reload 隔离协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"stable\", title = \"Stable\", controls = { { type = \"button\", id = \"ping\", "
               "label = \"Ping\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"ping\" then proto.status.set(\"old callback alive\") end\n";
        out << "end\n";
    }

    const ScopedTempPath invalidProtocolDir(makeUniqueTempDir("protoscope-reload-invalid"));
    {
        std::ofstream out(invalidProtocolDir.path() / "main.lua");
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
    require(application.reloadProtocolDirectory(validProtocolDir.path().generic_string(), true), "有效协议应可加载");

    const auto before = application.docks().luaState();
    require(!application.reloadProtocolDirectory(invalidProtocolDir.path().generic_string(), true), "非法协议 reload 应失败");
    const auto& after = application.docks().luaState();
    require(after.protocolDir == before.protocolDir, "失败 reload 后旧协议目录应保留");
    require(after.docks.size() == before.docks.size(), "失败 reload 后旧 Dock 快照应保留");

    application.updateControlValue("ping", true);
    require(application.docks().configState().statusMessage == "old callback alive",
            "失败 reload 后旧协议控件回调仍应可用");

    application.shutdown();
}

void test_application_forced_reload_discards_old_tx_callback_outputs()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-reload-discard-old-tx"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "reload 测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"reload_test\", title = \"Reload Test\", controls = { { type = \"button\", id = "
               "\"start\", label = \"Start\" } } } }\n";
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
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "reload 测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("start", true);
    require(transportState->sentTasks.size() == 1, "首次控制应发送 1 条 request");
    application.pumpOnce();

    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "强制重载应成功");
    application.pumpOnce();

    require(transportState->sentTasks.size() == 1, "强制重载应丢弃旧 on_tx 追加的 request");
    require(application.docks().configState().statusMessage.find("old canceled leaked") == std::string::npos,
            "强制重载应丢弃旧 on_tx 追加的状态");

    application.shutdown();
}

void test_application_request_done_success_does_not_set_comm_error()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-request-done-success"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "request_done 成功测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"request_done_success\", title = \"Request Done\", controls = { { type = "
               "\"button\", id = \"read\", label = \"Read\" } } } }\n";
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
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "request_done 成功测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.docks().commState().lastError = "读取应答成功";
    application.updateControlValue("read", true);
    application.pumpOnce();

    require(application.docks().commState().lastError.empty(), "request_done ok=true 的成功消息不应显示到通讯配置错误");
    const auto& requestTraceRows = application.docks().requestTraceState().rows;
    const auto hasTraceState = [&](protoscope::dock::RequestTraceState state) {
        return std::any_of(requestTraceRows.begin(), requestTraceRows.end(), [&](const auto& row) {
            return row.tag == "read" && row.state == state;
        });
    };
    require(hasTraceState(protoscope::dock::RequestTraceState::Queued), "请求追踪应记录 request 排队事件");
    require(hasTraceState(protoscope::dock::RequestTraceState::Sent), "请求追踪应记录 request 已发送事件");
    require(hasTraceState(protoscope::dock::RequestTraceState::Completed), "请求追踪应记录 request 完成事件");
    application.shutdown();
}

void test_application_request_timeout_drains_pending_rx_before_timeout()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-request-timeout-drain"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "request timeout drain 测试协议应可写入");
        out << "local seen = {}\n";
        out << "function ui()\n";
        out << "  return { { id = \"request_timeout_drain\", title = \"Request Drain\", controls = { { type = "
               "\"button\", id = \"read\", label = \"Read\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"read\" then proto.request({ 0x01 }, { timeout_ms = 20, tag = \"read\" }) end\n";
        out << "end\n";
        out << "function on_bytes(ctx, bytes)\n";
        out << "  for i = 1, #bytes do seen[#seen + 1] = bytes[i] end\n";
        out << "  if #seen >= 2 then proto.request_done({ ok = true, message = \"ACK 已处理\" }) end\n";
        out << "end\n";
    }

    auto transportState = std::make_shared<QueuedEventTransport::State>();
    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });

    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "request timeout drain 测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("read", true);
    const protoscope::transport::ConnectionContext context{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 300,
        .readyForIo = true,
    };
    for (std::size_t index = 0; index < 256; ++index) {
        transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{context, {}});
    }
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{context, {0xAA, 0x55}});

    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    application.pumpOnce();

    require(application.docks().commState().lastError.empty(),
            "已排队 ACK 应先 drain 并完成 request，不应误报等待 request_done 超时");
    application.shutdown();
}

void test_application_guarded_request_timeout_retry_then_success_keeps_guard_active()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-guarded-retry-success"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "guarded retry success 测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"guarded_retry\", title = \"Guarded Retry\", controls = {\n";
        out << "    { type = \"button\", id = \"read\", label = \"Read\" },\n";
        out << "    { type = \"button\", id = \"follow\", label = \"Follow\" },\n";
        out << "  } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"read\" then\n";
        out << "    proto.request_guarded({ 0x01 }, { timeout_ms = 20, tag = \"read\", max_attempts = 2 })\n";
        out << "  elseif id == \"follow\" then\n";
        out << "    proto.request_guarded({ 0x02 }, { timeout_ms = 1000, tag = \"follow\", max_attempts = 1 })\n";
        out << "  end\n";
        out << "end\n";
        out << "function on_tx(ctx, evt)\n";
        out << "  if evt.state == \"sent\" and evt.tag == \"read\" and evt.attempt == 2 then\n";
        out << "    proto.request_done({ ok = true, message = \"read ok\" })\n";
        out << "  elseif evt.state == \"sent\" and evt.tag == \"follow\" then\n";
        out << "    proto.request_done({ ok = true, message = \"follow ok\" })\n";
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
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "guarded retry success 测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("read", true);
    application.pumpOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    application.pumpOnce();
    application.pumpOnce();

    require(transportState->sentTasks.size() == 2, "同一个 guarded request 应先超时再重发一次");
    require(transportState->sentTasks[0].payload == std::vector<std::uint8_t>{0x01}, "第 1 次 attempt payload 应保持不变");
    require(transportState->sentTasks[1].payload == std::vector<std::uint8_t>{0x01}, "第 2 次 attempt payload 应保持不变");

    application.updateControlValue("follow", true);
    application.pumpOnce();
    require(transportState->sentTasks.size() == 3, "中间 attempt 成功后后续 guarded request 仍应可发送");
    application.shutdown();
}

void test_application_guarded_request_final_timeout_halts_followup_guarded()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-guarded-final-timeout"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "guarded final timeout 测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"guarded_halt\", title = \"Guarded Halt\", controls = { { type = \"button\", id = "
               "\"go\", label = \"Go\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"go\" then\n";
        out << "    proto.request_guarded({ 0x01 }, { timeout_ms = 20, tag = \"primary\", max_attempts = 2 })\n";
        out << "    proto.request_guarded({ 0x02 }, { timeout_ms = 20, tag = \"follow\", max_attempts = 1 })\n";
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
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "guarded final timeout 测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("go", true);
    application.pumpOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    application.pumpOnce();
    application.pumpOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    application.pumpOnce();
    application.pumpOnce();

    require(transportState->sentTasks.size() == 2,
            "当前 guarded request 达到 max_attempts 后，后续 guarded request 不应发出");
    require(transportState->sentTasks[0].payload == std::vector<std::uint8_t>{0x01}, "第 1 次 attempt 应发送 primary");
    require(transportState->sentTasks[1].payload == std::vector<std::uint8_t>{0x01}, "第 2 次 attempt 应重发 primary");
    require(application.docks().commState().lastError.find("熔断") != std::string::npos,
            "guarded 熔断后应拒绝后续 guarded request");
    application.shutdown();
}

void test_application_guarded_requests_count_attempts_independently()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-guarded-independent-attempts"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "guarded independent attempts 测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"guarded_independent\", title = \"Guarded Independent\", controls = { { type = "
               "\"button\", id = \"go\", label = \"Go\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"go\" then\n";
        out << "    proto.request_guarded({ 0x01 }, { timeout_ms = 20, tag = \"first\", max_attempts = 2 })\n";
        out << "    proto.request_guarded({ 0x02 }, { timeout_ms = 20, tag = \"second\", max_attempts = 2 })\n";
        out << "  end\n";
        out << "end\n";
        out << "function on_tx(ctx, evt)\n";
        out << "  if evt.state == \"sent\" and evt.attempt == 2 then\n";
        out << "    proto.request_done({ ok = true, message = evt.tag .. \" ok\" })\n";
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
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "guarded independent attempts 测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("go", true);
    for (int iteration = 0; iteration < 4; ++iteration) {
        application.pumpOnce();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        application.pumpOnce();
    }

    require(transportState->sentTasks.size() == 4, "两个 guarded request 应各自独立重试到第 2 次");
    require(transportState->sentTasks[0].payload == std::vector<std::uint8_t>{0x01}, "first 第 1 次 attempt 应发送 0x01");
    require(transportState->sentTasks[1].payload == std::vector<std::uint8_t>{0x01}, "first 第 2 次 attempt 应仍发送 0x01");
    require(transportState->sentTasks[2].payload == std::vector<std::uint8_t>{0x02}, "second 第 1 次 attempt 应发送 0x02");
    require(transportState->sentTasks[3].payload == std::vector<std::uint8_t>{0x02}, "second 第 2 次 attempt 应仍发送 0x02");
    application.shutdown();
}

void test_application_guarded_request_reset_allows_new_attempts()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-guarded-reset"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "guarded reset 测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"guarded_reset\", title = \"Guarded Reset\", controls = {\n";
        out << "    { type = \"button\", id = \"fail\", label = \"Fail\" },\n";
        out << "    { type = \"button\", id = \"reset\", label = \"Reset\" },\n";
        out << "    { type = \"button\", id = \"again\", label = \"Again\" },\n";
        out << "  } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  if id == \"fail\" then\n";
        out << "    proto.request_guarded({ 0x01 }, { timeout_ms = 20, tag = \"fail\", max_attempts = 1 })\n";
        out << "  elseif id == \"reset\" then\n";
        out << "    proto.reset_request_guard()\n";
        out << "  elseif id == \"again\" then\n";
        out << "    proto.request_guarded({ 0x03 }, { timeout_ms = 1000, tag = \"again\", max_attempts = 2 })\n";
        out << "  end\n";
        out << "end\n";
        out << "function on_tx(ctx, evt)\n";
        out << "  if evt.state == \"sent\" and evt.tag == \"again\" then\n";
        out << "    proto.request_done({ ok = true, message = \"again ok\" })\n";
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
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "guarded reset 测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("fail", true);
    application.pumpOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    application.pumpOnce();
    require(transportState->sentTasks.size() == 1, "max_attempts=1 的 guarded request 应只发送一次");

    application.updateControlValue("reset", true);
    application.pumpOnce();
    application.updateControlValue("again", true);
    application.pumpOnce();

    require(transportState->sentTasks.size() == 2, "reset 后新的 guarded request 应从 attempt=1 重新发送");
    require(transportState->sentTasks[1].payload == std::vector<std::uint8_t>{0x03}, "reset 后的新请求 payload 应发送");
    application.shutdown();
}

void test_application_request_done_failure_sets_comm_error()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-request-done-failure"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "request_done 失败测试协议应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"request_done_failure\", title = \"Request Done\", controls = { { type = "
               "\"button\", id = \"read\", label = \"Read\" } } } }\n";
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
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "request_done 失败测试协议应可加载");
    application.openTransport();
    application.pumpOnce();

    application.updateControlValue("read", true);
    application.pumpOnce();

    require(application.docks().commState().lastError == "读取应答失败",
            "request_done ok=false 的失败消息应显示到通讯配置错误");
    application.shutdown();
}

void test_application_open_transport_uses_serial_runtime_config()
{
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
    require(std::holds_alternative<protoscope::transport::SerialConfig>(*state->lastConfig),
            "串口模式应传入 SerialConfig");

    const auto& serial = std::get<protoscope::transport::SerialConfig>(*state->lastConfig);
    require(serial.portName == "COM42", "open 应使用当前运行态端口名");
    require(serial.baudRate == 230400, "open 应使用当前运行态波特率");
    require(serial.dataBits == 7, "open 应使用当前运行态数据位");
    require(serial.parity == "even", "open 应使用当前运行态奇偶校验");
    require(serial.stopBits == "two", "open 应使用当前运行态停止位");
    require(serial.flowControl == "hardware", "open 应使用当前运行态流控");

    application.shutdown();
}

void test_application_open_transport_uses_udp_peer_runtime_config()
{
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

void test_application_set_log_level_updates_runtime_config()
{
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

void test_application_capture_config_preserves_protocol_tx_runtime_config()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");

    auto config = application.captureConfig();
    config.protocol.tx.sendTimeoutMs = 111;
    config.protocol.tx.requestTimeoutMs = 222;
    config.protocol.tx.maxPending = 3;
    config.protocol.tx.overflowPolicy = "drop_newest";
    config.protocol.tx.overflowNotify = "status";
    require(application.applyConfig(config), "应用应接受协议 TX 运行时配置");

    const auto captured = application.captureConfig();
    require(captured.protocol.tx.sendTimeoutMs == 111, "captureConfig 应保留 send_timeout_ms");
    require(captured.protocol.tx.requestTimeoutMs == 222, "captureConfig 应保留 request_timeout_ms");
    require(captured.protocol.tx.maxPending == 3, "captureConfig 应保留 max_pending");
    require(captured.protocol.tx.overflowPolicy == "drop_newest", "captureConfig 应保留 overflow_policy");
    require(captured.protocol.tx.overflowNotify == "status", "captureConfig 应保留 overflow_notify");

    application.shutdown();
}

void test_application_wave_legend_visibility_config_roundtrip()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");

    auto config = application.captureConfig();
    config.gui.wave.showChannelLegend = false;
    config.gui.wave.showFftLegend = false;
    config.gui.wave.hiddenChannelPolicy = protoscope::plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews;
    require(application.applyConfig(config), "图例显示配置应用失败");

    require(!application.docks().waveState().view.showChannelLegend, "应用配置后应隐藏图例");
    require(!application.docks().waveState().view.showFftLegend, "应用配置后应隐藏 FFT 图例");
    require(application.docks().waveState().view.hiddenChannelPolicy ==
                protoscope::plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews,
            "应用配置后应切换隐藏 CH 策略");
    const auto captured = application.captureConfig();
    require(!captured.gui.wave.showChannelLegend, "captureConfig 应带出图例显示开关");
    require(!captured.gui.wave.showFftLegend, "captureConfig 应带出 FFT 图例显示开关");
    require(captured.gui.wave.hiddenChannelPolicy == protoscope::plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews,
            "captureConfig 应带出隐藏 CH 策略");

    application.docks().waveState().view.showChannelLegend = true;
    application.docks().waveState().view.showFftLegend = true;
    application.docks().waveState().view.hiddenChannelPolicy =
        protoscope::plot::WaveHiddenChannelPolicy::IncludeInDerivedViews;
    const auto capturedLive = application.captureConfig();
    require(capturedLive.gui.wave.showChannelLegend, "captureConfig 不应覆盖 dock 中实时波形图例状态");
    require(capturedLive.gui.wave.showFftLegend, "captureConfig 不应覆盖 dock 中实时 FFT 图例状态");
    require(
        capturedLive.gui.wave.hiddenChannelPolicy == protoscope::plot::WaveHiddenChannelPolicy::IncludeInDerivedViews,
        "captureConfig 不应覆盖 dock 中实时隐藏 CH 策略");

    application.shutdown();
}

void test_application_wave_zoom_selection_auto_exit_config_roundtrip()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");

    auto config = application.captureConfig();
    require(!config.gui.wave.zoomSelectionAutoExit, "captureConfig 默认应保持手动退出");
    require(config.gui.wave.xAxisDoubleClickAction == protoscope::plot::WaveXAxisDoubleClickAction::FitFullHistory,
            "captureConfig 默认应保持 X 轴双击全历史缩放");

    config.gui.wave.zoomSelectionAutoExit = true;
    config.gui.wave.xAxisDoubleClickAction = protoscope::plot::WaveXAxisDoubleClickAction::FitVisibleWindow;
    require(application.applyConfig(config), "框选放大自动退出配置应用失败");
    require(application.docks().waveState().view.zoomSelectionAutoExit, "应用配置后应同步更新框选放大退出模式");
    require(application.docks().waveState().view.xAxisDoubleClickAction ==
                protoscope::plot::WaveXAxisDoubleClickAction::FitVisibleWindow,
            "应用配置后应同步 X 轴双击行为");

    const auto captured = application.captureConfig();
    require(captured.gui.wave.zoomSelectionAutoExit, "captureConfig 应带出框选放大退出模式");
    require(captured.gui.wave.xAxisDoubleClickAction == protoscope::plot::WaveXAxisDoubleClickAction::FitVisibleWindow,
            "captureConfig 应带出 X 轴双击行为");

    application.docks().waveState().view.zoomSelectionAutoExit = false;
    application.docks().waveState().view.xAxisDoubleClickAction =
        protoscope::plot::WaveXAxisDoubleClickAction::FitFullHistory;
    const auto capturedLive = application.captureConfig();
    require(!capturedLive.gui.wave.zoomSelectionAutoExit, "captureConfig 不应覆盖 dock 中实时框选放大退出模式");
    require(
        capturedLive.gui.wave.xAxisDoubleClickAction == protoscope::plot::WaveXAxisDoubleClickAction::FitFullHistory,
        "captureConfig 不应覆盖 dock 中实时 X 轴双击行为");

    application.shutdown();
}

void test_application_logging_filters_script_and_host()
{
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-logging-test"));
    const auto logPath = tempRoot.path() / "runtime.log";
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

void test_application_raw_capture_export_import_roundtrip()
{
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

    require(waitUntil([&application] {
                application.pumpOnce();
                const auto snapshot = application.docks().waveState().buffer.snapshot(
                    -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
                return !snapshot.channels.empty() && snapshot.channels.front().totalSamples > 0;
            }),
            "实时 RX 后应异步生成波形样本");
    const auto liveSnapshot =
        wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(!liveSnapshot.channels.empty(), "实时 RX 后应生成波形通道");
    require(liveSnapshot.channels.front().totalSamples > 0, "实时 RX 后应生成波形样本");

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-application-roundtrip"));
    const auto tempPath = tempRoot.path() / "capture.psraw";

    std::string error;
    require(application.exportWaveRawCapture(tempPath, error), "应用导出 psraw 应成功");
    const auto capture = protoscope::plot::readRawCaptureFile(tempPath, error);
    if (!capture.has_value()) {
        throw std::runtime_error("导出后的 psraw 应可重新读取: " + error);
    }
    require(capture->payload == transportState->queuedRxBytes, "导出文件应保留实时 RX 原始字节");

    application.resetWaveHistory();
    const auto emptySnapshot = application.docks().waveState().buffer.snapshot(-std::numeric_limits<double>::infinity(),
                                                                               std::numeric_limits<double>::infinity());
    require(emptySnapshot.channels.empty() || emptySnapshot.channels.front().totalSamples == 0,
            "清空历史后不应保留旧波形样本");

    require(application.importWaveRawCapture(*capture, error), "应用导入 psraw 应成功");
    const auto importedSnapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(!importedSnapshot.channels.empty(), "导入 psraw 后应恢复波形通道");
    require(importedSnapshot.channels.front().totalSamples > 0, "导入 psraw 后应重新触发 on_bytes 生成样本");
    require(application.docks().waveState().view.sampleFrequencyHz == 2048.0, "导入 psraw 后应恢复文件中的采样频率");
    require(application.docks().waveState().rawCapture.payload == transportState->queuedRxBytes,
            "导入 psraw 后应回填原始缓冲");

    require(application.importWaveRawCapture(*capture, error), "同一 psraw 第二次导入仍应成功");
    const auto secondImportedSnapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(!secondImportedSnapshot.channels.empty(), "第二次导入 psraw 后应恢复波形通道");
    require(secondImportedSnapshot.channels.front().totalSamples == importedSnapshot.channels.front().totalSamples,
            "第二次导入应清空旧波形后完整回放，不应被第一次导入的样本挡住");

    application.shutdown();
}

void test_application_session_package_export_contains_replay_assets()
{
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    transportState->queuedRxBytes = {'P', 'K', 'G', '\n'};

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
    wave.view.sampleFrequencyHz = 4096.0;
    wave.view.sampleFrequencyInput = "4096";
    wave.analysisMarkers = {{
        .id = 42,
        .label = "startup edge",
        .note = "field note",
        .startTime = 0.125,
        .endTime = 0.250,
        .channelIndex = 1,
    }};
    require(wave.rawCapture.payload == transportState->queuedRxBytes, "会话包导出前应已有实时原始字节");

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-session-package"));
    const auto packagePath = tempRoot.path() / "field.pssession";
    std::string error;
    require(application.exportSessionPackage(packagePath, error), "应用导出现场会话包应成功");

    const auto package = protoscope::session::readSessionPackage(packagePath, error);
    if (!package.has_value()) {
        throw std::runtime_error("导出后的现场会话包应可读取: " + error);
    }

    const auto* manifestEntry = protoscope::session::findSessionPackageEntry(*package, "manifest.yaml");
    const auto* rawEntry = protoscope::session::findSessionPackageEntry(*package, "raw_capture.psraw");
    const auto* configEntry = protoscope::session::findSessionPackageEntry(*package, "config.yaml");
    const auto* luaEntry = protoscope::session::findSessionPackageEntry(*package, "protocol/main.lua");
    const auto* markersEntry = protoscope::session::findSessionPackageEntry(*package, "analysis/markers.yaml");
    const auto* summaryEntry = protoscope::session::findSessionPackageEntry(*package, "logs/summary.txt");
    require(manifestEntry != nullptr, "现场会话包应包含 manifest.yaml");
    require(rawEntry != nullptr, "现场会话包应包含 raw_capture.psraw");
    require(configEntry != nullptr, "现场会话包应包含配置 YAML");
    require(luaEntry != nullptr, "现场会话包应包含当前协议 main.lua");
    require(markersEntry != nullptr, "现场会话包应包含分析标记 YAML");
    require(summaryEntry != nullptr, "现场会话包应包含日志摘要");

    const std::string_view rawBytes(reinterpret_cast<const char*>(rawEntry->bytes.data()), rawEntry->bytes.size());
    const auto capture = protoscope::plot::decodeRawCaptureFile(rawBytes, error);
    if (!capture.has_value()) {
        throw std::runtime_error("会话包内 raw_capture.psraw 应可解析: " + error);
    }
    require(capture->payload == transportState->queuedRxBytes, "会话包内 psraw 应保留实时 RX 原始字节");
    require(capture->sampleFrequencyHz == 4096.0, "会话包内 psraw 应保留采样频率");

    const std::string configText(configEntry->bytes.begin(), configEntry->bytes.end());
    const std::string luaText(luaEntry->bytes.begin(), luaEntry->bytes.end());
    const std::string manifestText(manifestEntry->bytes.begin(), manifestEntry->bytes.end());
    const std::string markersText(markersEntry->bytes.begin(), markersEntry->bytes.end());
    const std::string summaryText(summaryEntry->bytes.begin(), summaryEntry->bytes.end());
    require(manifestText.find("raw_capture.psraw") != std::string::npos, "会话包 manifest 应列出原始波形");
    require(manifestText.find("protocol/main.lua") != std::string::npos, "会话包 manifest 应列出协议脚本");
    require(manifestText.find("analysis/markers.yaml") != std::string::npos, "会话包 manifest 应列出分析标记");
    require(configText.find("protocol:") != std::string::npos, "会话包配置应包含 protocol 配置域");
    require(luaText.find("proto") != std::string::npos, "会话包协议脚本应包含 Lua 协议内容");
    require(markersText.find("startup edge") != std::string::npos, "会话包分析标记应保留 label");
    require(summaryText.find("raw_capture_bytes: 4") != std::string::npos, "会话包日志摘要应包含原始字节数");

    application.shutdown();

    protoscope::app::Application importedApplication;
    require(importedApplication.initialize(), "导入应用初始化失败");
    require(importedApplication.importSessionPackage(packagePath, error), "导入现场会话包应成功");
    const auto& importedLua = importedApplication.docks().luaState();
    require(importedLua.protocolDir.find("ProtoScope-session-protocol-") != std::string::npos,
            "导入现场包应使用释放出的临时协议目录");
    const auto importedConfig = importedApplication.captureConfig();
    require(importedConfig.protocol.selectedDir.find("ProtoScope-session-protocol-") == std::string::npos,
            "保存配置时不应写入现场包临时协议目录");
    require(importedApplication.docks().waveState().rawCapture.payload == transportState->queuedRxBytes,
            "导入现场包后应回放包内 raw payload");
    const auto& importedMarkers = importedApplication.docks().waveState().analysisMarkers;
    require(importedMarkers.size() == 1, "导入现场包后应恢复分析标记");
    require(importedMarkers.front().label == "startup edge", "导入现场包后分析标记 label 应保留");
    require(importedMarkers.front().channelIndex == 1, "导入现场包后分析标记通道应保留");
    importedApplication.shutdown();
}

void test_application_session_package_exports_protocol_directory_and_import_requires_helper()
{
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    transportState->queuedRxBytes = {'H', 'E', 'L', 'P'};

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-session-package-protocol-dir"));
    const auto protocolRoot = tempRoot.path() / "protocols";
    const auto protocolDir = protocolRoot / "custom_protocol";
    writeTextFile(protocolDir / "helper.lua",
                  R"(local M = {}

function M.value()
  return "loaded"
end

return M
)");
    writeTextFile(protocolDir / "assets" / "info.txt", "nested resource\n");
    writeTextFile(protocolDir / "main.lua",
                  R"(local helper = require("helper")

function ui()
  return {}
end

function on_bytes(ctx, bytes)
  proto.emit("helper", {
    value = helper.value(),
    size = #bytes,
  })
end
)");

    protoscope::app::Application exporter;
    exporter.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(exporter.initialize(), "导出应用初始化失败");
    auto config = exporter.captureConfig();
    config.protocol.rootDir = protocolRoot.generic_string();
    config.protocol.selectedDir = protocolDir.generic_string();
    require(exporter.applyConfig(config), "导出应用应可切换到测试协议目录");
    require(exporter.reloadProtocolDirectory(protocolDir.generic_string(), true), "测试协议目录应可加载");
    exporter.openTransport();
    for (int i = 0; i < 4; ++i) {
        exporter.pumpOnce();
    }
    require(exporter.docks().waveState().rawCapture.payload == transportState->queuedRxBytes,
            "导出现场包前应已有测试协议的原始字节");

    const auto packagePath = tempRoot.path() / "with-protocol-dir.pssession";
    std::string error;
    require(exporter.exportSessionPackage(packagePath, error), "导出包含完整协议目录的现场包应成功");
    exporter.shutdown();

    const auto package = protoscope::session::readSessionPackage(packagePath, error);
    if (!package.has_value()) {
        throw std::runtime_error("包含协议目录的现场包应可读取: " + error);
    }
    const auto* helperEntry = protoscope::session::findSessionPackageEntry(*package, "protocol/helper.lua");
    const auto* assetEntry = protoscope::session::findSessionPackageEntry(*package, "protocol/assets/info.txt");
    const auto* manifestEntry = protoscope::session::findSessionPackageEntry(*package, "manifest.yaml");
    require(helperEntry != nullptr, "现场包应包含协议目录下的 helper.lua");
    require(assetEntry != nullptr, "现场包应包含协议子目录资源文件");
    require(manifestEntry != nullptr, "现场包应包含 manifest.yaml");
    const std::string manifestText(manifestEntry->bytes.begin(), manifestEntry->bytes.end());
    require(manifestText.find("protocol/helper.lua") != std::string::npos, "manifest 应列出 helper.lua");
    require(manifestText.find("protocol/assets/info.txt") != std::string::npos, "manifest 应列出子目录资源文件");

    protoscope::app::Application importer;
    require(importer.initialize(), "导入应用初始化失败");
    require(importer.importSessionPackage(packagePath, error), "包含 helper.lua 的现场包应可导入");
    const auto& importedLua = importer.docks().luaState();
    require(importedLua.protocolDir.find("ProtoScope-session-protocol-") != std::string::npos,
            "导入现场包应释放到临时协议目录");
    require(std::filesystem::exists(std::filesystem::path(importedLua.protocolDir) / "helper.lua"),
            "导入现场包后临时协议目录应包含 helper.lua");
    require(std::filesystem::exists(std::filesystem::path(importedLua.protocolDir) / "assets" / "info.txt"),
            "导入现场包后临时协议目录应包含子目录资源文件");
    require(importer.docks().waveState().rawCapture.payload == transportState->queuedRxBytes,
            "导入现场包后应回放包内 raw payload");
    importer.shutdown();
}

void test_application_session_package_import_rejects_unsafe_entries()
{
    protoscope::app::Application exporter;
    require(exporter.initialize(), "导出应用初始化失败");

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-session-package-unsafe"));
    const auto basePackagePath = tempRoot.path() / "base.pssession";
    std::string error;
    require(exporter.exportSessionPackage(basePackagePath, error), "导出基础现场包应成功");
    exporter.shutdown();

    const auto basePackage = protoscope::session::readSessionPackage(basePackagePath, error);
    if (!basePackage.has_value()) {
        throw std::runtime_error("基础现场包应可读取: " + error);
    }

    const auto writeInvalidPackage = [&](std::string entryName, const std::filesystem::path& outputPath) {
        auto invalidPackage = *basePackage;
        invalidPackage.entries.push_back(protoscope::session::SessionPackageEntry{
            .name = std::move(entryName),
            .bytes = std::vector<std::uint8_t>{'x'},
        });
        require(protoscope::session::writeSessionPackage(outputPath, invalidPackage, error), "无效现场包应可写入");
    };

    const auto parentEscapePath = tempRoot.path() / "parent-escape.pssession";
    writeInvalidPackage("protocol/../escape.lua", parentEscapePath);
    protoscope::app::Application parentEscapeImporter;
    require(parentEscapeImporter.initialize(), "父目录逃逸导入应用初始化失败");
    error.clear();
    require(!parentEscapeImporter.importSessionPackage(parentEscapePath, error),
            "包含 protocol/../x 的现场包应被拒绝");
    require(error.find("不安全条目") != std::string::npos, "父目录逃逸错误应说明不安全条目");
    parentEscapeImporter.shutdown();

    const auto absolutePath = tempRoot.path() / "absolute.pssession";
    writeInvalidPackage("C:/escape.lua", absolutePath);
    protoscope::app::Application absoluteImporter;
    require(absoluteImporter.initialize(), "绝对路径导入应用初始化失败");
    error.clear();
    require(!absoluteImporter.importSessionPackage(absolutePath, error), "包含绝对路径 entry 的现场包应被拒绝");
    require(error.find("不安全条目") != std::string::npos, "绝对路径错误应说明不安全条目");
    absoluteImporter.shutdown();
}

void test_application_wave_analysis_report_exports_summary_and_markers()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");

    auto& wave = application.docks().waveState();
    wave.view.sampleFrequencyHz = 2048.0;
    wave.view.viewMinTime = 1.0;
    wave.view.viewMaxTime = 2.0;
    wave.rawCapture.payload = {0x10, 0x20, 0x30};
    wave.rawCapture.events = {{
        .type = protoscope::plot::RawCaptureEventType::RxBytes,
        .timestampMs = 10,
        .bytes = {0x10, 0x20, 0x30},
        .profile = {},
        .plotSetup = {},
    }};
    wave.analysisMarkers = {{
        .id = 7,
        .label = "M1,\"edge\"",
        .note = "peak, stable",
        .startTime = 1.25,
        .endTime = 1.50,
        .channelIndex = 2,
    }};

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-wave-analysis"));
    const auto reportPath = tempRoot.path() / "analysis.csv";
    std::string error;
    require(application.exportWaveAnalysisReport(reportPath, error), "波形分析报告应可导出");

    std::ifstream in(reportPath, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    require(text.find("section,key,value\n") != std::string::npos, "分析报告应包含 summary 表头");
    require(text.find("summary,sample_frequency_hz,2048") != std::string::npos, "分析报告应包含采样频率");
    require(text.find("summary,raw_capture_bytes,3") != std::string::npos, "分析报告应包含原始字节数");
    require(text.find("summary,markers,1") != std::string::npos, "分析报告应包含 marker 总数");
    require(text.find("marker,7,\"M1,\"\"edge\"\"\",\"peak, stable\",2,1.25,1.5") != std::string::npos,
            "分析报告应导出并转义 marker 字段");

    application.shutdown();
}

void test_application_session_package_import_without_markers_clears_existing_state()
{
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    transportState->queuedRxBytes = {'O', 'L', 'D'};

    protoscope::app::Application exporter;
    exporter.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(exporter.initialize(), "导出应用初始化失败");
    exporter.openTransport();
    for (int i = 0; i < 4; ++i) {
        exporter.pumpOnce();
    }

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-session-package-legacy"));
    const auto fullPackagePath = tempRoot.path() / "full.pssession";
    const auto legacyPackagePath = tempRoot.path() / "legacy.pssession";
    std::string error;
    require(exporter.exportSessionPackage(fullPackagePath, error), "导出完整现场包应成功");
    exporter.shutdown();

    auto package = protoscope::session::readSessionPackage(fullPackagePath, error);
    if (!package.has_value()) {
        throw std::runtime_error("完整现场包应可读取: " + error);
    }
    package->entries.erase(
        std::remove_if(package->entries.begin(),
                       package->entries.end(),
                       [](const protoscope::session::SessionPackageEntry& entry) {
                           return entry.name == "analysis/markers.yaml";
                       }),
        package->entries.end());
    require(protoscope::session::writeSessionPackage(legacyPackagePath, *package, error), "旧现场包应可重新写入");

    protoscope::app::Application importer;
    require(importer.initialize(), "导入应用初始化失败");
    importer.docks().waveState().analysisMarkers = {{
        .id = 99,
        .label = "旧标记",
        .note = "导入前残留",
        .startTime = 0.0,
        .endTime = 1.0,
        .channelIndex = 0,
    }};
    require(importer.importSessionPackage(legacyPackagePath, error), "缺少 markers 的旧现场包也应可导入");
    require(importer.docks().waveState().analysisMarkers.empty(), "旧现场包缺少 markers 时应清空导入前分析标记");
    importer.shutdown();
}

void test_application_session_package_import_invalid_protocol_rolls_back_runtime()
{
    protoscope::app::Application exporter;
    require(exporter.initialize(), "导出应用初始化失败");

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-session-package-invalid"));
    const auto fullPackagePath = tempRoot.path() / "full.pssession";
    const auto invalidPackagePath = tempRoot.path() / "invalid.pssession";
    std::string error;
    require(exporter.exportSessionPackage(fullPackagePath, error), "导出现场包应成功");
    exporter.shutdown();

    auto package = protoscope::session::readSessionPackage(fullPackagePath, error);
    if (!package.has_value()) {
        throw std::runtime_error("现场包应可读取: " + error);
    }
    bool scriptReplaced = false;
    for (auto& entry : package->entries) {
        if (entry.name == "protocol/main.lua") {
            const std::string invalidScript = "function ui(\n";
            entry.bytes.assign(invalidScript.begin(), invalidScript.end());
            scriptReplaced = true;
            break;
        }
    }
    require(scriptReplaced, "测试现场包应包含 protocol/main.lua");
    require(protoscope::session::writeSessionPackage(invalidPackagePath, *package, error), "无效协议现场包应可写入");

    protoscope::app::Application importer;
    require(importer.initialize(), "导入应用初始化失败");
    const auto beforeLua = importer.docks().luaState();
    const auto beforeConfig = importer.captureConfig();
    require(!importer.importSessionPackage(invalidPackagePath, error), "无效协议现场包导入应失败");
    const auto& afterLua = importer.docks().luaState();
    const auto afterConfig = importer.captureConfig();
    require(afterLua.protocolDir == beforeLua.protocolDir, "现场包导入失败后运行协议目录应回滚");
    require(afterLua.scriptPath == beforeLua.scriptPath, "现场包导入失败后运行脚本路径应回滚");
    require(afterConfig.protocol.selectedDir == beforeConfig.protocol.selectedDir,
            "现场包导入失败后保存配置不应指向临时协议目录");
    importer.shutdown();
}

void test_application_raw_capture_replay_timeline_steps_events()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");
    const auto& lua = application.docks().luaState();

    protoscope::plot::RawCaptureFileData capture{
        .protocolName = lua.protocolName,
        .protocolDir = lua.protocolDir,
        .sampleFrequencyHz = 1000.0,
        .capturedAtMs = 123,
        .payload = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66},
        .events = {{.type = protoscope::plot::RawCaptureEventType::RxBytes,
                    .timestampMs = 223,
                    .bytes = {0x11, 0x22},
                    .profile = {},
                    .plotSetup = {}},
                   {.type = protoscope::plot::RawCaptureEventType::RxBytes,
                    .timestampMs = 243,
                    .bytes = {0x33, 0x44},
                    .profile = {},
                    .plotSetup = {}},
                   {.type = protoscope::plot::RawCaptureEventType::RxBytes,
                    .timestampMs = 263,
                    .bytes = {0x55, 0x66},
                    .profile = {},
                    .plotSetup = {}}},
    };

    std::string error;
    require(application.loadRawCaptureReplayTimeline(capture, error), "载入原始回放时间轴应成功");
    auto status = application.rawCaptureReplayStatus();
    require(status.loaded && !status.playing, "载入后应处于已载入且暂停状态");
    require(status.eventIndex == 0 && status.eventCount == 3, "载入后时间轴应指向首个事件");
    require(status.progress == 0.0, "载入后进度应为 0");

    require(application.playRawCaptureReplay(error), "继续回放应成功");
    application.pumpOnce();
    status = application.rawCaptureReplayStatus();
    require(status.loaded && status.playing, "初始等待未满足时仍应保持播放状态");
    require(status.eventIndex == 0, "首个事件应等待 capturedAt 到事件 timestamp 的间隔");
    application.pauseRawCaptureReplay();

    require(application.stepRawCaptureReplay(error), "单步回放首个事件应成功");
    status = application.rawCaptureReplayStatus();
    require(status.loaded && !status.playing, "单步后仍应保留时间轴上下文");
    require(status.eventIndex == 1 && status.eventCount == 3, "单步后事件索引应前进");
    require(status.progress > 0.3 && status.progress < 0.4, "单步后进度应反映当前事件位置");
    require(application.docks().waveState().rawCapture.payload == capture.payload, "单步后应保留原始回放 payload");

    require(application.seekRawCaptureReplay(2, error), "回放时间轴应可定位到中间事件");
    status = application.rawCaptureReplayStatus();
    require(status.loaded && !status.playing, "暂停状态定位到中间事件后仍应暂停");
    require(status.eventIndex == 2 && status.eventCount == 3, "定位到中间事件后事件索引应正确");

    require(application.playRawCaptureReplay(error), "中间位置继续播放应成功");
    require(application.seekRawCaptureReplay(1, error), "播放中定位应成功并恢复播放状态");
    status = application.rawCaptureReplayStatus();
    require(status.loaded && status.playing, "播放中定位到未结束位置后应恢复播放状态");
    require(status.eventIndex == 1, "播放中定位后事件索引应正确");
    application.pauseRawCaptureReplay();

    require(application.seekRawCaptureReplay(status.eventCount, error), "回放时间轴应可定位到末尾");
    status = application.rawCaptureReplayStatus();
    require(status.loaded && !status.playing, "定位到末尾后应停止播放");
    require(status.eventIndex == status.eventCount && status.progress == 1.0, "定位到末尾后进度应为 100%");

    require(application.seekRawCaptureReplay(0, error), "回放时间轴应可重新定位到开头");
    require(application.playRawCaptureReplay(error), "回放时间轴继续播放应成功");
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    application.pumpOnce();
    status = application.rawCaptureReplayStatus();
    require(status.loaded && !status.playing, "自动播放到末尾后应保留时间轴且停止播放");
    require(status.eventIndex == 3 && status.eventCount == 3, "自动播放到末尾后应保留末尾位置");
    require(application.seekRawCaptureReplay(0, error), "自动播放结束后仍应可重新定位");
    application.unloadRawCaptureReplayTimeline();
    status = application.rawCaptureReplayStatus();
    require(!status.loaded && !status.playing && status.eventCount == 0, "卸载后应清空回放时间轴状态");

    application.shutdown();
}

void test_application_loads_protocol_action_templates()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");

    require(application.reloadProtocolDirectory("protocols/templates/file_dialog", true),
            "文件对话框模板应可加载");
    require(application.reloadProtocolDirectory("protocols/templates/send_file", true),
            "文件发送模板应可加载");
    require(application.reloadProtocolDirectory("protocols/templates/request_guarded", true),
            "受保护请求模板应可加载");

    application.shutdown();
}

void test_application_live_raw_capture_trims_to_limit()
{
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

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-live-raw-capture-limit"));
    const auto tempPath = tempRoot.path() / "capture.psraw";

    std::string error;
    require(application.exportWaveRawCapture(tempPath, error), "截断后的实时缓存仍应可导出");
    const auto capture = protoscope::plot::readRawCaptureFile(tempPath, error);
    if (!capture.has_value()) {
        throw std::runtime_error("截断导出文件应可读取: " + error);
    }
    require(capture->truncated, "截断标记应写入 psraw 文件头");
    if (capture->payload != expectedTail) {
        throw std::runtime_error("截断导出应只包含最近实时缓存: actual=" + std::to_string(capture->payload.size()) +
                                 " expected=" + std::to_string(expectedTail.size()));
    }

    application.shutdown();
}

void test_application_live_raw_capture_trim_keeps_runtime_profile_event()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-live-runtime-profile"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "runtime profile 裁剪测试协议应可写入");
        out << "function on_open(ctx)\n";
        out << "  proto.stream.set_profile({ frame = 'dynamic_profile', length = 8, channel_map = { 2, 1 } })\n";
        out << "end\n";
        out << "local function on_frame(ctx, frame)\n";
        out << "  proto.emit('runtime_profile_frame', frame.name)\n";
        out << "end\n";
        out << "function stream()\n";
        out << "  return { buffer = { capacity = 64, overflow = 'drop_oldest' }, frames = { {\n";
        out << "    name = 'dynamic_profile', header = { 0xFF, 0x26 }, runtime_profile = true,\n";
        out << "    crc = { type = 'crc16_modbus', order = 'hi_lo' },\n";
        out << "    fields = { { name = 'values', type = 'i16_be', offset = 3, count = { op = 'remaining', unit = 2 } "
               "} },\n";
        out << "    on_frame = on_frame,\n";
        out << "  } } }\n";
        out << "end\n";
    }

    const auto makeRuntimeFrame = [](std::uint8_t high, std::uint8_t low) {
        std::vector<std::uint8_t> raw{0xFF, 0x26, 0x00, high, 0x00, low};
        const auto crc = protoscope::protocol_utils::crc16Modbus(raw);
        raw.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
        raw.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
        return raw;
    };
    const auto firstFrame = makeRuntimeFrame(0x11, 0x22);
    const auto secondFrame = makeRuntimeFrame(0x33, 0x44);

    auto transportState = std::make_shared<QueuedEventTransport::State>();
    protoscope::app::Application application;
    protoscope::config::AppConfig config;
    config.gui.rawCapture.liveLimitBytes = secondFrame.size();
    require(application.initialize(), "应用初始化失败");
    require(application.applyConfig(config), "应用配置应可设置实时原始缓存上限");
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "runtime profile 裁剪协议应可加载");
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });

    application.openTransport();
    application.pumpOnce();
    const protoscope::transport::ConnectionContext context{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 101,
        .readyForIo = true,
    };
    std::vector<std::uint8_t> allBytes = firstFrame;
    allBytes.insert(allBytes.end(), secondFrame.begin(), secondFrame.end());
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{context, allBytes});
    for (int i = 0; i < 4; ++i) {
        application.pumpOnce();
    }

    const auto& rawCapture = application.docks().waveState().rawCapture;
    require(rawCapture.truncated, "实时 raw 超限后应被裁剪");
    require(rawCapture.payload == secondFrame, "实时 raw 裁剪后应保留完整最新帧");
    const auto rxEventCount = static_cast<std::size_t>(
        std::count_if(rawCapture.events.begin(), rawCapture.events.end(), [](const auto& event) {
            return event.type == protoscope::plot::RawCaptureEventType::RxBytes;
        }));
    require(rxEventCount <= 1, "裁剪后事件流不应继续保留旧 RX 事件");
    require(!rawCapture.events.empty() &&
                rawCapture.events.front().type == protoscope::plot::RawCaptureEventType::ProfileSet,
            "裁剪窗口前的活动 profile 应被补到事件流开头");

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-live-runtime-profile-trim"));
    const auto tempPath = tempRoot.path() / "capture.psraw";
    std::string error;
    require(application.exportWaveRawCapture(tempPath, error), "裁剪后的 runtime profile raw 应可导出");
    const auto exported = protoscope::plot::readRawCaptureFile(tempPath, error);
    if (!exported.has_value()) {
        throw std::runtime_error("裁剪导出的 runtime profile psraw 应可读取: " + error);
    }
    require(exported->payload == secondFrame, "导出文件应只包含当前可见 raw");
    require(
        !exported->events.empty() && exported->events.front().type == protoscope::plot::RawCaptureEventType::ProfileSet,
        "普通导出应保留当前 raw 依赖的 profile_set");

    protoscope::app::Application importedApplication;
    require(importedApplication.initialize(), "导入验证应用初始化失败");
    require(importedApplication.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "导入验证协议应可加载");
    require(importedApplication.importWaveRawCapture(*exported, error), "导出的 runtime profile raw 应可重新导入");
}

void test_application_raw_capture_recording_preserves_full_rx_when_live_buffer_trims()
{
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

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-full-raw-recording"));
    const auto tempPath = tempRoot.path() / "capture.psraw";
    require(application.startRawCaptureRecording(tempPath, error), "完整原始数据录制应可启动");
    require(application.isRawCaptureRecording(), "启动后应处于录制状态");

    application.openTransport();
    for (int index = 0; index < 4; ++index) {
        application.pumpOnce();
    }

    require(application.rawCaptureRecordingBytes() == transportState->queuedRxBytes.size(),
            "录制字节数应等于完整 RX 字节数");
    require(application.stopRawCaptureRecording(error), "完整原始数据录制应可停止");
    require(!application.isRawCaptureRecording(), "停止后不应处于录制状态");

    const auto& liveCapture = application.docks().waveState().rawCapture;
    require(liveCapture.truncated, "实时缓存仍应按上限截断");
    require(liveCapture.payload == std::vector<std::uint8_t>({0x14, 0x15, 0x16}), "实时缓存应只保留尾部字节");

    const auto recorded = protoscope::plot::readRawCaptureFile(tempPath, error);
    if (!recorded.has_value()) {
        throw std::runtime_error("完整录制 psraw 应可读取: " + error);
    }
    require(!recorded->truncated, "完整录制文件不应标记截断");
    require(recorded->payload == transportState->queuedRxBytes, "完整录制文件应保存全部 RX 原始字节");

    application.shutdown();
}

void test_application_raw_capture_import_preserves_full_history()
{
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
        .events = {},
    };

    std::string error;
    require(application.importWaveRawCapture(capture, error), "导入 psraw 应成功");
    const auto importedSnapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(!importedSnapshot.channels.empty(), "导入后应生成波形通道");
    require(importedSnapshot.channels.front().totalSamples == capture.payload.size(),
            "导入回放应保留完整原始数据生成的波形历史");

    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-application-full-history"));
    const auto tempPath = tempRoot.path() / "capture.psraw";
    require(application.exportWaveRawCapture(tempPath, error), "完整历史导入后应仍可导出 psraw");
    const auto exported = protoscope::plot::readRawCaptureFile(tempPath, error);
    if (!exported.has_value()) {
        throw std::runtime_error("导出的 psraw 应可重新读取: " + error);
    }
    require(exported->payload == capture.payload, "再次导出应保留完整原始 payload");
    application.shutdown();
}

void test_application_raw_capture_import_replays_runtime_profile_events()
{
    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/runtime_profile_stream", true),
            "runtime_profile_stream 协议应可加载");

    protoscope::plot::RawCaptureFileData capture;
    capture.protocolName = "runtime_profile_stream";
    capture.protocolDir = "tests/fixtures/protocols/runtime_profile_stream";
    capture.sampleFrequencyHz = 2048.0;
    capture.capturedAtMs = 100;
    capture.events.push_back(protoscope::plot::RawCaptureEvent{
        .type = protoscope::plot::RawCaptureEventType::ProfileSet,
        .timestampMs = 100,
        .bytes = {},
        .profile = {.frameName = "dynamic_profile", .length = 8, .channelMap = {1, 0}},
        .plotSetup = {},
    });
    std::vector<std::uint8_t> raw{0xFF, 0x26, 0x00, 0x11, 0x00, 0x22};
    const auto crc = protoscope::protocol_utils::crc16Modbus(raw);
    raw.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    raw.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    capture.events.push_back(protoscope::plot::RawCaptureEvent{
        .type = protoscope::plot::RawCaptureEventType::RxBytes,
        .timestampMs = 101,
        .bytes = raw,
        .profile = {},
        .plotSetup = {},
    });
    capture.events.push_back(protoscope::plot::RawCaptureEvent{
        .type = protoscope::plot::RawCaptureEventType::ProfileClear,
        .timestampMs = 102,
        .bytes = {},
        .profile = {.frameName = "dynamic_profile", .length = 0, .channelMap = {}},
        .plotSetup = {},
    });
    capture.payload = raw;

    std::string error;
    require(application.importWaveRawCapture(capture, error), "事件流 psraw 导入应成功");
    if (application.docks().waveState().rawCapture.events.size() != 3) {
        throw std::runtime_error("导入后应保留事件流: actual=" +
                                 std::to_string(application.docks().waveState().rawCapture.events.size()));
    }
    application.shutdown();
}

void test_application_raw_capture_import_replays_plot_setup_snapshot()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-raw-plot-setup-import"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "plot_setup 导入测试协议应可写入");
        out << "function on_bytes(ctx, bytes)\n";
        out << "  local samples = {}\n";
        out << "  for i = 1, #bytes do\n";
        out << "    samples[#samples + 1] = { t = (i - 1) * 0.001, y = bytes[i] }\n";
        out << "  end\n";
        out << "  proto.plot.push(1, { samples = samples })\n";
        out << "end\n";
    }

    protoscope::plot::RawCaptureFileData capture;
    capture.protocolName = "raw_plot_setup_import";
    capture.protocolDir = protocolDir.path().generic_string();
    capture.sampleFrequencyHz = 1000.0;
    capture.capturedAtMs = 100;
    capture.events.push_back(makePlotSetupEvent(true));
    capture.events.push_back(protoscope::plot::RawCaptureEvent{
        .type = protoscope::plot::RawCaptureEventType::RxBytes,
        .timestampMs = 101,
        .bytes = {1, 2, 3},
        .profile = {},
        .plotSetup = {},
    });
    capture.payload = {1, 2, 3};

    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "plot_setup 导入协议应可加载");
    std::string error;
    require(application.importWaveRawCapture(capture, error), "带 plot_setup 的 psraw 导入应成功");

    const auto spec = application.docks().waveState().buffer.channelSpec(0);
    require(spec.has_value(), "导入 plot_setup 后应恢复通道配置");
    require(spec->label == "温度A", "导入 plot_setup 后应恢复通道 label");
    require(spec->unit == "℃", "导入 plot_setup 后应恢复通道 unit");
    require(std::abs(spec->ratio - 0.5) < 1e-12, "导入 plot_setup 后应恢复 ratio");
    require(std::abs(spec->scale - 2.0) < 1e-12, "导入 plot_setup 后应恢复 scale");
    require(std::abs(spec->offset + 1.0) < 1e-12, "导入 plot_setup 后应恢复 offset");
    require(spec->color.has_value(), "导入 plot_setup 后应恢复颜色");
    require(spec->lineWidth.has_value(), "导入 plot_setup 后应恢复 line_width");
    require(std::abs(*spec->lineWidth - 3.25F) < 1e-6F, "导入 plot_setup 后 line_width 错误");
    const auto viewConfig = application.docks().waveState().buffer.viewConfig();
    require(viewConfig.historyLimit == 128U, "导入 plot_setup 后应恢复 history_limit");
    require(viewConfig.timeUnit == "ms", "导入 plot_setup 后应恢复 time_unit");
    const auto snapshot = application.docks().waveState().buffer.snapshot(-std::numeric_limits<double>::infinity(),
                                                                          std::numeric_limits<double>::infinity());
    require(!snapshot.channels.empty(), "导入 plot_setup 后应有波形通道");
    require(snapshot.channels.front().lineWidth.has_value(), "导入 plot_setup 后快照应携带 line_width");
    require(std::abs(*snapshot.channels.front().lineWidth - 3.25F) < 1e-6F,
            "导入 plot_setup 后快照 line_width 错误");
    require(snapshot.channels.front().totalSamples == 3U, "导入 plot_setup 后应回放 raw 样本");
    application.shutdown();
}

void test_application_raw_capture_import_skips_duplicate_plot_setup_reset()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-raw-plot-setup-duplicate"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "重复 plot_setup 测试协议应可写入");
        out << "local function setup()\n";
        out << "  proto.plot.setup({\n";
        out << "    source = 'raw_snapshot', reset_history = true,\n";
        out << "    time_scale = 0.5, time_unit = 'ms', vertical_min = -20, vertical_max = 100, vertical_unit = '℃', "
               "history_limit = 128,\n";
        out << "    channels = { { label = '温度A', unit = '℃', ratio = 0.5, scale = 2.0, offset = -1.0, color = "
               "'#1a334cff', line_width = 3.25 } }\n";
        out << "  })\n";
        out << "end\n";
        out << "function on_bytes(ctx, bytes)\n";
        out << "  setup()\n";
        out << "  local samples = {}\n";
        out << "  for i = 1, #bytes do\n";
        out << "    samples[#samples + 1] = { t = (i - 1) * 0.001, y = bytes[i] }\n";
        out << "  end\n";
        out << "  proto.plot.push(1, { samples = samples })\n";
        out << "end\n";
    }

    protoscope::plot::RawCaptureFileData capture;
    capture.protocolName = "raw_plot_setup_duplicate";
    capture.protocolDir = protocolDir.path().generic_string();
    capture.sampleFrequencyHz = 1000.0;
    capture.capturedAtMs = 100;
    capture.events.push_back(protoscope::plot::RawCaptureEvent{
        .type = protoscope::plot::RawCaptureEventType::RxBytes,
        .timestampMs = 101,
        .bytes = {1, 2, 3, 4},
        .profile = {},
        .plotSetup = {},
    });
    capture.events.push_back(makePlotSetupEvent(true));
    capture.payload = {1, 2, 3, 4};

    protoscope::app::Application application;
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true),
            "重复 plot_setup 导入协议应可加载");
    std::string error;
    require(application.importWaveRawCapture(capture, error), "重复 plot_setup psraw 导入应成功");

    const auto snapshot = application.docks().waveState().buffer.snapshot(-std::numeric_limits<double>::infinity(),
                                                                          std::numeric_limits<double>::infinity());
    require(!snapshot.channels.empty(), "重复 plot_setup 导入后应有波形通道");
    require(snapshot.channels.front().totalSamples == 4U, "重复 plot_setup 不应因 reset_history 二次清空样本");
    application.shutdown();
}

void test_application_raw_capture_import_replays_stream_in_chunks()
{
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
        .events = {},
    };

    std::string error;
    require(application.importWaveRawCapture(capture, error), "导入大 payload psraw 应成功");
    const auto importedSnapshot = application.docks().waveState().buffer.snapshot(
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(!importedSnapshot.channels.empty(), "导入后应生成波形通道");
    require(importedSnapshot.channels.front().totalSamples == frameCount,
            "导入回放应按分块解析全部 stream 帧，而不是只保留尾部数据");
}

void test_application_raw_capture_import_updates_last_pump_diagnostics()
{
    constexpr const char* protocolDir = "tests/fixtures/protocols/raw_import_chunked_stream";

    auto transportState = std::make_shared<QueuedEventTransport::State>();
    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();

    application.openTransport();
    application.pumpOnce();

    const auto frame = makeRawImportStreamFrame(0x34);
    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 300,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{ctx, frame});

    require(application.pumpOnce(), "处理 stream 帧应成功推进一次 pump");
    require(waitUntil([&application, expectedBytes = frame.size()] {
                application.pumpOnce();
                const auto& comm = application.docks().commState();
                return comm.lastPumpRxBytes == expectedBytes && comm.lastPumpStreamFrames == 1U;
            }),
            "worker 应异步回写 stream 解析统计");

    const auto& comm = application.docks().commState();
    require(comm.lastPumpRxBytes == frame.size(), "上轮 RX 字节统计应等于本次输入大小");
    require(comm.lastPumpStreamFrames == 1U, "stream 解析统计应记录 1 帧");
    require(comm.lastPumpStreamErrors == 0U, "stream 解析统计不应产生错误");
    require(comm.lastPumpTransportMs >= 0.0, "pump 耗时统计应已写回");
    require(comm.lastPumpParserMs >= 0.0, "parser 耗时统计应已写回");
    require(comm.lastPumpCallbackMs >= 0.0, "Lua 回调耗时统计应已写回");
    require(comm.lastPumpScriptMs >= 0.0, "脚本总耗时统计应已写回");
}

void test_application_reload_rebuilds_frame_rows_with_count_expression()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-rebuild-count-expression"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "reload count 表达式测试协议应可写入");
        out << "function controls()\n";
        out << "  return {}\n";
        out << "end\n";
        out << "local function on_frame(ctx, frame)\n";
        out << "  proto.emit(\"side_effect\", { count = #frame.fields.values })\n";
        out << "end\n";
        out << "function stream()\n";
        out << "  return { frames = { { name = \"expr_frame\", header = { 0xAA, 0x55 }, len = { offset = 3, type = "
               "\"u8\", means = \"payload\", extra = 5 }, crc = { type = \"crc16_modbus\", order = \"lo_hi\" }, fields "
               "= {\n";
        out << "    { name = \"byte_count\", type = \"u8\", offset = 4 },\n";
        out << "    { name = \"values\", type = \"u16_be\", offset = 5, count = { op = \"div\", field = "
               "\"byte_count\", by = 2 } },\n";
        out << "  }, on_frame = on_frame } } }\n";
        out << "end\n";
    }

    auto transportState = std::make_shared<QueuedEventTransport::State>();
    auto frame = std::vector<std::uint8_t>{0xAA, 0x55, 0x05, 0x04, 0x00, 0x11, 0x00, 0x22};
    const auto crc = protoscope::protocol_utils::crc16Modbus(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    transportState->queuedRxBytes = frame;

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用应可初始化默认 Lua 工作区");
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "count 表达式协议应可加载");
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();
    application.openTransport();
    require(waitUntil([&application] {
                application.pumpOnce();
                return application.docks().receiveState().frameRows.size() == 1U;
            }),
            "实时接收应生成逐帧记录");
    require(application.docks().receiveState().frameRows.front().message.find("expr_frame") != std::string::npos,
            "逐帧记录应包含 count 表达式帧名");
    require(waitUntil([&application] {
                application.pumpOnce();
                return !application.docks().scriptState().rows.empty();
            }),
            "实时 on_frame 副作用应进入脚本事件");

    application.docks().scriptState().rows.clear();
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "同协议 reload 应成功");
    require(application.docks().receiveState().frameRows.size() == 1U, "reload 后应重建历史逐帧记录");
    require(application.docks().receiveState().frameRows.front().message.find("values=[17, 34]") != std::string::npos,
            "历史重建应完整解析 count 表达式字段");
    require(application.docks().scriptState().rows.empty(), "历史重建不应执行 on_frame 副作用");
}

void test_application_transfer_log_frame_view_waits_for_rx_full_frame()
{
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
    auto config = application.captureConfig();
    config.gui.replayRawHistoryOnSchemaSwitch = true;
    require(application.applyConfig(config), "旧 raw 历史回放配置应可应用");

    application.openTransport();
    require(waitUntil([&application] {
                application.pumpOnce();
                return application.docks().receiveState().rows.size() == 1;
            }),
            "原始收发记录应保留半包 chunk");
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
    require(receive.frameRows.empty(), "RawChunks 模式不应实时生成逐帧行");
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();
    require(receive.frameRows.size() == 1, "切换 ParsedFrames 后应按 raw 历史回放出一行");
    require(receive.frameRows.front().bytes == frame, "逐帧行应保存完整原始帧字节");
    require(receive.frameRows.front().message.find("stream_sample") != std::string::npos, "逐帧行应包含帧名");
    require(receive.frameRows.front().message.find("value=52") != std::string::npos, "逐帧行应包含字段值");
}

void test_application_transfer_log_frame_view_keeps_unmatched_tx_raw()
{
    constexpr const char* protocolDir = "tests/fixtures/protocols/stream_frame_only";
    auto state = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([state](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(state);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可加载");
    auto config = application.captureConfig();
    config.gui.replayRawHistoryOnSchemaSwitch = true;
    require(application.applyConfig(config), "旧 raw 历史回放配置应可应用");
    application.openTransport();
    application.pumpOnce();

    require(application.sendManualPayload("01 02 03", true), "手动发送应成功");
    application.pumpOnce();
    const auto& receive = application.docks().receiveState();
    require(receive.rows.size() == 1, "原始收发记录应记录 TX chunk");
    require(receive.frameRows.empty(), "RawChunks 模式不应实时生成 TX 逐帧行");
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();
    require(receive.frameRows.size() == 1, "切换 ParsedFrames 后应保留无法匹配 schema 的 TX 原始行");
    require(receive.frameRows.front().direction == "TX", "TX fallback 行方向应保持 TX");
    require(receive.frameRows.front().bytes == std::vector<std::uint8_t>({0x01, 0x02, 0x03}),
            "TX fallback 行字节应保持原样");
}

void test_application_switching_to_parsed_view_defaults_to_new_stream_only()
{
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    const ScopedTempPath protocolDir(makeSchemaSwitchReplayProtocolDir());

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory(protocolDir.path().generic_string(), true), "临时 stream 协议应可加载");

    application.openTransport();
    application.pumpOnce();

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 200,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{ctx, {0xAA, 0x55, 0x00, 0x20}});
    application.pumpOnce();
    require(hasReceiveBytes(application.docks().receiveState(), {0xAA, 0x55, 0x00, 0x20}),
            "raw 模式下应先记录旧 chunk");

    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();
    require(application.docks().receiveState().frameRows.empty(), "旧 raw 历史只有半帧时不应凭空生成逐帧行");

    const auto frame = makeRawImportStreamFrame(0x34);
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{ctx, frame});
    application.pumpOnce();

    const auto& receive = application.docks().receiveState();
    require(receive.frameRows.size() == 1, "默认不回放旧 raw 历史时，新完整帧应可独立解析");
    require(receive.frameRows.front().bytes == frame, "默认不回放旧 raw 历史时，逐帧行应来自切换后的新数据");
    require(receive.frameRows.front().message.find("value=52") != std::string::npos,
            "默认不回放旧 raw 历史时，逐帧行应包含新帧字段值");
}

void test_application_switching_to_parsed_view_can_replay_old_raw_history()
{
    auto transportState = std::make_shared<QueuedEventTransport::State>();
    const ScopedTempPath protocolDir(makeSchemaSwitchReplayProtocolDir());

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");

    auto config = application.captureConfig();
    config.gui.replayRawHistoryOnSchemaSwitch = true;
    config.protocol.selectedDir = protocolDir.path().generic_string();
    require(application.applyConfig(config), "schema 切换回放配置应可应用");

    application.openTransport();
    application.pumpOnce();

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 260,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(protoscope::transport::TransportBytesEvent{ctx, {0xAA, 0x55, 0x00, 0x20}});
    application.pumpOnce();

    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();
    require(application.docks().receiveState().frameRows.empty(), "旧 raw 历史只有半帧时不应凭空生成逐帧行");

    transportState->pendingEvents.push_back(
        protoscope::transport::TransportBytesEvent{ctx, makeRawImportStreamFrame(0x56)});
    application.pumpOnce();

    require(application.docks().receiveState().frameRows.empty(),
            "开启旧历史回放后，旧半帧应继续占住 parser，复现 raw->schema 切换卡住场景");
}

void test_application_rx_events_are_processed_with_budget()
{
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
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();

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

void test_application_large_rx_event_drains_by_byte_budget()
{
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
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();

    application.openTransport();
    application.pumpOnce();

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 300,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(
        protoscope::transport::TransportBytesEvent{ctx, makeRawImportStreamPayload(frameCount)});

    require(application.pumpOnce(), "第一轮 pump 应快速转交大 RX 到脚本 worker");

    for (int attempt = 0; attempt < 10 && application.docks().receiveState().frameRows.size() < frameCount; ++attempt) {
        application.pumpOnce();
    }
    require(application.docks().receiveState().frameRows.size() == frameCount, "worker 应异步 drain 大 RX 事件");
    require(application.docks().commState().pendingRxBytes == 0U, "worker drain 完成后不应残留 pending 字节");
}

void test_application_comm_pressure_debug_log_respects_log_level()
{
    struct ScenarioResult {
        bool pressureLogVisible{false};
        std::size_t pendingTransferFrameRows{0};
        std::string backlogWarning;
    };

    const auto runScenario = [](protoscope::config::LogLevel level) {
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
        config.logging.level = level;
        config.gui.realtimeBacklog.transferFrameRowsPerPump = 1;
        require(application.applyConfig(config), "实时 backlog 与日志配置应可应用");
        require(application.reloadProtocolDirectory(protocolDir, true), "stream 协议应可重新加载");
        application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
        application.activateParsedTransferLogView();

        application.openTransport();
        application.pumpOnce();

        const protoscope::transport::ConnectionContext ctx{
            .endpoint = "queued://wave",
            .connectionId = 7,
            .timestampMs = 301,
            .readyForIo = true,
        };
        transportState->pendingEvents.push_back(
            protoscope::transport::TransportBytesEvent{ctx, makeRawImportStreamPayload(frameCount)});

        require(application.pumpOnce(), "第一轮 pump 应生成实时 backlog 压力");
        const auto& comm = application.docks().commState();
        ScenarioResult result{
            .pressureLogVisible =
                hasHostLogMessage(application.docks().logState(), "comm_pressure", "派生 UI backlog"),
            .pendingTransferFrameRows = comm.pendingTransferFrameRows,
            .backlogWarning = comm.backlogWarning,
        };
        application.shutdown();
        return result;
    };

    const auto debugResult = runScenario(protoscope::config::LogLevel::Debug);
    require(debugResult.pendingTransferFrameRows > 0U, "Debug 场景应真实构造 pending 逐帧 backlog");
    require(debugResult.backlogWarning.find("派生 UI backlog") != std::string::npos,
            "Debug 场景应保留 commState backlog 告警");
    require(debugResult.pressureLogVisible, "Debug 日志级别应把通讯压力写入宿主日志");

    const auto infoResult = runScenario(protoscope::config::LogLevel::Info);
    require(infoResult.pendingTransferFrameRows > 0U, "Info 场景应同样保留 commState pending 逐帧 backlog");
    require(infoResult.backlogWarning.find("派生 UI backlog") != std::string::npos,
            "Info 场景应同样保留 commState backlog 告警");
    require(!infoResult.pressureLogVisible, "Info 日志级别不应显示通讯压力 Debug 日志");
}

void test_application_responsive_disconnect_discards_realtime_backlog()
{
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
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();

    application.openTransport();
    application.pumpOnce();

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 400,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(
        protoscope::transport::TransportBytesEvent{ctx, makeRawImportStreamPayload(frameCount)});
    application.pumpOnce();

    require(application.docks().commState().pendingTransferFrameRows > 0U, "断开前应存在 pending 逐帧行");

    application.closeTransport();
    require(application.docks().commState().state == protoscope::transport::TransportState::Closed,
            "断开后通讯状态应立即关闭");
    require(application.docks().commState().pendingRxBytes == 0U, "responsive 断开应清空 pending RX 字节");
    require(application.docks().commState().pendingTransferFrameRows == 0U, "responsive 断开应清空 pending 逐帧行");
}

void test_application_complete_disconnect_keeps_realtime_backlog()
{
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
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();

    application.openTransport();
    application.pumpOnce();

    const protoscope::transport::ConnectionContext ctx{
        .endpoint = "queued://wave",
        .connectionId = 7,
        .timestampMs = 500,
        .readyForIo = true,
    };
    transportState->pendingEvents.push_back(
        protoscope::transport::TransportBytesEvent{ctx, makeRawImportStreamPayload(frameCount)});
    application.pumpOnce();

    require(application.docks().commState().pendingTransferFrameRows > 0U, "complete 断开前应存在 pending 逐帧行");

    application.closeTransport();
    require(application.docks().commState().state == protoscope::transport::TransportState::Closed,
            "complete 断开后通讯状态也应立即关闭");
    require(application.docks().commState().pendingRxBytes == 0U, "complete 模式不应保留已转交 worker 的 RX 字节");
    require(application.docks().commState().pendingTransferFrameRows > 0U, "complete 断开后应保留 pending 逐帧行");

    for (int attempt = 0; attempt < 20 && application.docks().receiveState().frameRows.size() < frameCount; ++attempt) {
        application.pumpOnce();
    }
    require(application.docks().receiveState().frameRows.size() == frameCount,
            "complete 模式应在后续 pump 小步补完逐帧 backlog");
    require(application.docks().commState().state == protoscope::transport::TransportState::Closed,
            "补完 backlog 不应重新打开通讯状态");
}

void test_application_transfer_frame_rows_drain_after_input_stops()
{
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
    application.docks().receiveState().displayMode = protoscope::dock::TransferLogDisplayMode::ParsedFrames;
    application.activateParsedTransferLogView();

    application.openTransport();
    application.pumpOnce();

    for (std::size_t index = 0; index < frameCount; ++index) {
        require(application.sendManualPayload("01 02 03", true), "手动发送应成功");
    }

    application.pumpOnce();
    require(application.docks().receiveState().frameRows.size() == 2000, "单轮 pump 应按批量上限提交逐帧日志");
    require(application.pumpOnce(), "数据停止后 pending 逐帧日志仍应继续 drain");
    require(application.docks().receiveState().frameRows.size() == frameCount, "后续 pump 应补交剩余逐帧日志");
}

void test_application_plot_push_merges_same_channel_source()
{
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/plot_merge", true),
            "plot_merge 协议应可加载");

    application.openTransport();
    application.pumpOnce();

    const auto snapshot = application.docks().waveState().buffer.snapshot(-std::numeric_limits<double>::infinity(),
                                                                          std::numeric_limits<double>::infinity());
    require(snapshot.channels.size() == 1, "plot_merge 应生成 1 个波形通道");
    require(snapshot.channels.front().totalSamples == 3, "同轮同源 push 合并后不应丢失较早时间样本");
    require(snapshot.channels.front().samples[0].time == 1.0, "合并后样本应按时间排序");
    require(snapshot.channels.front().samples[1].time == 2.0, "合并后应保留第二个同源样本");
    require(snapshot.channels.front().samples[2].time == 3.0, "不同 source 的后续样本仍应保留");
}

void test_application_plot_push_drains_with_budget_and_disconnect_keeps_pending()
{
    auto transportState = std::make_shared<QueuedEventTransport::State>();

    protoscope::app::Application application;
    application.setTransportFactoryForTest([transportState](protoscope::transport::TransportKind kind) {
        static_cast<void>(kind);
        return std::make_unique<QueuedEventTransport>(transportState);
    });
    require(application.initialize(), "应用初始化失败");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/plot_merge", true),
            "plot_merge 协议应可加载");

    auto config = application.captureConfig();
    config.gui.realtimeBacklog.plotAppendsPerPump = 1;
    require(application.applyConfig(config), "plot append 预算配置应可应用");
    require(application.reloadProtocolDirectory("tests/fixtures/protocols/plot_merge", true),
            "plot_merge 协议应可重新加载");

    application.openTransport();
    application.pumpOnce();

    auto snapshot = application.docks().waveState().buffer.snapshot(-std::numeric_limits<double>::infinity(),
                                                                    std::numeric_limits<double>::infinity());
    require(snapshot.channels.size() == 1, "plot_merge 应生成 1 个波形通道");
    require(snapshot.channels.front().totalSamples == 2, "同源 plot append 不应被预算拆开导致旧样本丢失");
    require(application.docks().commState().pendingPlotAppends == 1U, "剩余不同源 plot append 应保留到后续 pump");

    application.closeTransport();
    require(application.docks().commState().pendingPlotAppends == 1U, "responsive 默认断开应保留未提交 plot append");
    snapshot = application.docks().waveState().buffer.snapshot(-std::numeric_limits<double>::infinity(),
                                                               std::numeric_limits<double>::infinity());
    require(snapshot.channels.front().totalSamples == 2, "断开保留 backlog 不应立即提交到 buffer");
}
