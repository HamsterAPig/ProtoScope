#include "test_registry.hpp"

#include "protoscope/config/config.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

protoscope::transport::ConnectionContext sampleCtx() {
    return protoscope::transport::ConnectionContext{
        .kind = protoscope::transport::TransportKind::TcpClient,
        .endpoint = "127.0.0.1:9000",
        .connectionId = 42,
        .timestampMs = nowMs(),
    };
}

std::filesystem::path fixtureProtocolDir(const char* name) {
    return std::filesystem::path("tests/fixtures/protocols") / name;
}

struct ScopedCurrentPath {
    explicit ScopedCurrentPath(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() {
        std::filesystem::current_path(original_);
    }

    std::filesystem::path original_;
};

std::filesystem::path makeUniqueTempDir(const char* prefix) {
    const auto path = std::filesystem::temp_directory_path()
                    / (std::string(prefix) + "-" + std::to_string(nowMs()));
    std::filesystem::create_directories(path);
    return path;
}

std::uint16_t readBe16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return (static_cast<std::uint16_t>(bytes.at(offset)) << 8U)
         | static_cast<std::uint16_t>(bytes.at(offset + 1));
}

std::vector<std::uint8_t> makeStreamFixtureFrame(std::uint8_t value) {
    std::vector<std::uint8_t> frame{0xAA, 0x55, 0x01, value, 0x00, 0x00};
    const std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto crc = protoscope::protocol_utils::crc16Modbus(payload);
    frame[4] = static_cast<std::uint8_t>(crc & 0xFFU);
    frame[5] = static_cast<std::uint8_t>((crc >> 8U) & 0xFFU);
    return frame;
}

void rewriteModbusCrcHiLo(std::vector<std::uint8_t>& frame) {
    require(frame.size() >= 2, "CRC 重算时帧长度不足");
    const std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto crc = protoscope::protocol_utils::crc16Modbus(payload);
    frame[frame.size() - 2] = static_cast<std::uint8_t>((crc >> 8U) & 0xFFU);
    frame[frame.size() - 1] = static_cast<std::uint8_t>(crc & 0xFFU);
}

void appendBytes(std::vector<std::uint8_t>& target, const std::vector<std::uint8_t>& source) {
    target.insert(target.end(), source.begin(), source.end());
}

void requireProtocolLoaded(protoscope::scripting::ScriptHost& host, const char* directory) {
    if (!host.loadProtocolDirectory(directory)) {
        throw std::runtime_error(std::string(directory) + " 加载失败: " + host.lastError());
    }
}

std::vector<std::uint8_t> makeSnScopeFc06Ack(std::uint16_t address, std::uint16_t value) {
    std::vector<std::uint8_t> frame{
        0xFF,
        0x06,
        static_cast<std::uint8_t>((address >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(address & 0xFFU),
        static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(value & 0xFFU),
        0x00,
        0x00,
    };
    rewriteModbusCrcHiLo(frame);
    return frame;
}

std::vector<std::uint8_t> makeSnScopeFc16Ack(std::uint16_t address, std::uint16_t count) {
    std::vector<std::uint8_t> frame{
        0xFF,
        0x10,
        static_cast<std::uint8_t>((address >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(address & 0xFFU),
        static_cast<std::uint8_t>((count >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(count & 0xFFU),
        0x00,
        0x00,
    };
    rewriteModbusCrcHiLo(frame);
    return frame;
}

std::vector<std::uint8_t> makeSnScopeUploadFrame(std::uint16_t sequence,
                                                 std::int16_t ch1,
                                                 std::int16_t ch2,
                                                 std::int16_t ch3,
                                                 std::int16_t ch4) {
    std::vector<std::uint8_t> frame{
        0xFF,
        0x26,
        static_cast<std::uint8_t>((sequence >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(sequence & 0xFFU),
        static_cast<std::uint8_t>((static_cast<std::uint16_t>(ch1) >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(static_cast<std::uint16_t>(ch1) & 0xFFU),
        static_cast<std::uint8_t>((static_cast<std::uint16_t>(ch2) >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(static_cast<std::uint16_t>(ch2) & 0xFFU),
        static_cast<std::uint8_t>((static_cast<std::uint16_t>(ch3) >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(static_cast<std::uint16_t>(ch3) & 0xFFU),
        static_cast<std::uint8_t>((static_cast<std::uint16_t>(ch4) >> 8U) & 0xFFU),
        static_cast<std::uint8_t>(static_cast<std::uint16_t>(ch4) & 0xFFU),
        0x00,
        0x00,
    };
    rewriteModbusCrcHiLo(frame);
    return frame;
}

void completeHalfDuplexStartup(protoscope::scripting::ScriptHost& master,
                               protoscope::scripting::ScriptHost& slave,
                               const protoscope::transport::ConnectionContext& ctx) {
    master.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    slave.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    master.drainPlotSetups();
    master.onControl(ctx, "auto_start", true);

    const auto requests = master.drainTxRequests();
    require(requests.size() == 5, "应先生成 5 条配置/启动请求");
    for (const auto& request : requests) {
        master.onTxEvent(ctx,
                         protoscope::scripting::TxEvent{
                             .id = request.id,
                             .kind = protoscope::scripting::TxRequestKind::Request,
                             .state = protoscope::scripting::TxEventState::Sent,
                             .tag = request.tag,
                             .bytes = request.payload.size(),
                             .queuedMs = nowMs(),
                             .finishedMs = nowMs(),
                             .error = std::nullopt,
                         });
        slave.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, request.payload});
        const auto replies = slave.drainTxRequests();
        if (replies.size() != 1) {
            std::ostringstream detail;
            detail << "每次 SN Scope 请求都应返回 1 个 ACK"
                   << " | request_size=" << request.payload.size()
                   << " | func=0x" << std::hex << static_cast<int>(request.payload[1]);
            for (const auto& update : slave.drainStatusUpdates()) {
                detail << " | slave_status=" << update.text;
            }
            throw std::runtime_error(detail.str());
        }
        require(replies[0].kind == protoscope::scripting::TxRequestKind::Send, "从机 ACK 应走 proto.send");

        master.setRequestAwaitingCompletion(true);
        master.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, replies[0].payload});
        const auto results = master.drainRequestDoneResults();
        require(results.size() == 1, "主机收到 ACK 后应调用一次 proto.request_done");
        require(results[0].ok, "自动配置阶段 ACK 应全部匹配成功");
        master.onTxEvent(ctx,
                         protoscope::scripting::TxEvent{
                             .id = request.id,
                             .kind = protoscope::scripting::TxRequestKind::Request,
                             .state = protoscope::scripting::TxEventState::Completed,
                             .tag = request.tag,
                             .bytes = request.payload.size(),
                             .queuedMs = nowMs(),
                             .finishedMs = nowMs(),
                             .error = std::nullopt,
                         });
    }
}

std::vector<std::uint8_t> nextHalfDuplexWaveFrame(protoscope::scripting::ScriptHost& slave) {
    const auto wakeup = slave.nextWakeupAtMs();
    require(wakeup.has_value(), "从机启动后应注册定时器");
    slave.tick(*wakeup);

    const auto waveRequests = slave.drainTxRequests();
    require(waveRequests.size() == 1, "定时器触发后应主动发送 1 帧波形");
    require(waveRequests[0].kind == protoscope::scripting::TxRequestKind::Send, "主动上报应走 proto.send");
    require(waveRequests[0].payload.size() == 1680, "SN Scope 每次 tick 应拼成 1680 字节上传批");
    return waveRequests[0].payload;
}

} // namespace

void test_script_controls_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto controls = host.controlsSnapshot();
    require(!controls.empty(), "controls 不能为空");
    require(controls[0].id == "read_version", "第一个控件应为 read_version");
    require(controls.size() >= 6, "默认协议脚本应暴露完整控件集合");
    bool foundElfSymbolCombo = false;
    for (const auto& control : controls) {
        if (control.id == "target_symbol") {
            foundElfSymbolCombo = control.type == protoscope::scripting::ControlType::ElfSymbolCombo;
        }
    }
    require(foundElfSymbolCombo, "默认协议应示范 elf_symbol_combo 控件");
}

void test_default_protocol_logs_elf_symbol_info() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    host.onControl(sampleCtx(),
                   "target_symbol",
                   protoscope::scripting::ElfSymbolValue{
                       .label = "global.counter",
                       .value = "0x20000010",
                       .type = "uint32_t",
                   });

    bool foundInfoLog = false;
    for (const auto& log : host.drainLogs()) {
        if (log.level == "info" && log.message.find("global.counter") != std::string::npos
            && log.message.find("0x20000010") != std::string::npos
            && log.message.find("uint32_t") != std::string::npos) {
            foundInfoLog = true;
        }
    }
    require(foundInfoLog, "默认协议应把 ELF 变量信息以 info 日志输出");
}

void test_script_elf_symbol_combo_descriptor_defaults() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("elf_symbol_combo").generic_string()),
            "elf_symbol_combo 协议脚本应可加载");

    const auto controls = host.controlsSnapshot();
    require(controls.size() == 2, "elf_symbol_combo fixture 应暴露 2 个控件");
    require(controls[0].type == protoscope::scripting::ControlType::ElfSymbolCombo,
            "应解析 elf_symbol_combo 控件类型");
    require(controls[0].debounceMs == 150, "debounce_ms 默认值应为 150");
    require(controls[0].limit == 64, "limit 默认值应为 64");
    require(controls[1].debounceMs == 25, "应解析自定义 debounce_ms");
    require(controls[1].limit == 3, "应解析自定义 limit");
}

void test_script_elf_symbol_combo_invalid_config_fails() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_elf_symbol_combo").generic_string()),
            "非法 elf_symbol_combo 配置应加载失败");
    require(host.lastError().find("elf_symbol_combo") != std::string::npos,
            "非法 elf_symbol_combo 应记录明确错误");
}

void test_script_elf_symbol_combo_get_control_returns_table() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("elf_symbol_combo").generic_string()),
            "elf_symbol_combo 协议脚本应可加载");

    host.onControl(sampleCtx(),
                   "target",
                   protoscope::scripting::ElfSymbolValue{
                       .label = "global.counter",
                       .value = "0x20000010",
                       .type = "uint32_t",
                   });

    const auto events = host.drainEvents();
    require(events.size() == 1, "on_control 应 emit 一条事件");
    require(events[0].payload.find("label=global.counter") != std::string::npos,
            "proto.get_control 应返回 label 字段");
    require(events[0].payload.find("value=0x20000010") != std::string::npos,
            "proto.get_control 应返回 value 字段");
    require(events[0].payload.find("type=uint32_t") != std::string::npos,
            "proto.get_control 应返回 type 字段");
}

void test_script_on_open_log() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});
    bool foundOpenLog = false;
    for (const auto& log : host.drainLogs()) {
        if (log.message.find("连接已打开") != std::string::npos) {
            foundOpenLog = true;
        }
    }
    require(foundOpenLog, "on_open 应写入打开日志");
}

void test_script_on_close_log() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportClose(protoscope::transport::TransportCloseEvent{ctx, "manual_close"});
    bool foundCloseLog = false;
    for (const auto& log : host.drainLogs()) {
        if (log.message.find("连接已关闭") != std::string::npos) {
            foundCloseLog = true;
        }
    }
    require(foundCloseLog, "on_close 应写入关闭日志");
}

void test_script_on_error_log() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportError(protoscope::transport::TransportErrorEvent{ctx, "socket_failure"});
    bool foundErrorLog = false;
    const auto logs = host.drainLogs();
    for (const auto& log : logs) {
        if (log.message.find("连接错误") != std::string::npos) {
            foundErrorLog = true;
        }
    }
    if (!foundErrorLog) {
        std::ostringstream message;
        message << "on_error 应写入异常日志，实际日志条数=" << logs.size();
        for (const auto& log : logs) {
            message << " [" << log.level << "] " << log.message;
        }
        throw std::runtime_error(message.str());
    }
}

void test_script_multi_dock_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("multi_dock").generic_string()), "multi_dock 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 2, "多 Dock 协议应产出两个 dock");
    require(docks[0].descriptor.id == "protocol", "第一个 dock id 不正确");
    require(docks[1].descriptor.title == "高级参数", "第二个 dock 标题不正确");

    const auto controls = host.controlsSnapshot();
    bool foundReadVersionButton = false;
    for (const auto& control : controls) {
        if (control.type == protoscope::scripting::ControlType::Button && control.id == "read_version") {
            foundReadVersionButton = true;
            break;
        }
    }
    require(foundReadVersionButton, "应存在 read_version 按钮控件");
}

void test_script_dock_layout_fields() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("multi_dock").generic_string()), "multi_dock 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 2, "多 Dock 协议应产出两个 dock");
    require(docks[0].descriptor.anchor == "left_bottom", "第一个 dock 应解析 anchor");
    require(docks[1].descriptor.anchor == "left_bottom", "第二个 dock 应解析 anchor");
    require(docks[0].descriptor.tabGroup == "protocol_tools", "第一个 dock 应解析 tab_group");
    require(docks[1].descriptor.tabGroup == "protocol_tools", "第二个 dock 应解析 tab_group");
    require(!docks[0].descriptor.layout.has_value(), "未声明 layout 的 dock 应保持旧 flow 行为");
    require(!docks[1].descriptor.layout.has_value(), "未声明 layout 的 dock 应保持旧 flow 行为");
}

void test_script_table_layout_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("table_layout").generic_string()), "table_layout 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "table_layout 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "table_layout 应解析 layout");
    require(docks[0].descriptor.layout->kind == protoscope::scripting::DockLayoutKind::Table, "layout.kind 应为 table");

    const auto& table = docks[0].descriptor.layout->table;
    require(table.columns == 2, "table_layout 列数应为 2");
    require(table.rows.size() == 3, "table_layout 应解析三行");
    require(table.rows[0].cells.size() == 2, "第一行应有两列");
    require(table.rows[0].cells[0].controlId == "device_id", "第一行第一列应绑定 device_id");
    require(table.rows[1].cells[1].spacer, "第二行第二列应为 spacer");
    require(table.rows[2].cells[0].controlId == "read_version", "第三行第一列应绑定 read_version");
}

void test_script_form_layout_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("form_layout").generic_string()), "form_layout 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "form_layout 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "form_layout 应解析 layout");
    require(docks[0].descriptor.layout->kind == protoscope::scripting::DockLayoutKind::Form, "layout.kind 应为 form");

    const auto& items = docks[0].descriptor.layout->form.items;
    require(items.size() == 5, "form_layout 应解析 5 个顶层布局项");
    require(items[0].kind == protoscope::scripting::FormLayoutItemKind::Text, "第一项应为 text");
    require(items[1].kind == protoscope::scripting::FormLayoutItemKind::Controls, "第二项应为 controls");
    require(items[1].controls.controlIds.size() == 2, "controls 应包含两个控件");
    require(items[1].controls.controlIds[0] == "read_version", "同排第一个控件顺序错误");
    require(items[1].controls.controlIds[1] == "device_id", "同排第二个控件顺序错误");
    require(items[2].kind == protoscope::scripting::FormLayoutItemKind::Separator, "第三项应为 separator");
    require(items[3].kind == protoscope::scripting::FormLayoutItemKind::Group, "第四项应为 group");
    require(items[3].group && items[3].group->items.size() == 1, "group 内应有 1 个子项");
    require(items[3].group->items[0].controls.controlIds[0] == "hex_send", "group 内第一个控件顺序错误");
    require(items[3].group->items[0].controls.controlIds[1] == "mode", "group 内第二个控件顺序错误");
    require(items[4].kind == protoscope::scripting::FormLayoutItemKind::Collapse, "第五项应为 collapse");
    require(items[4].collapse && !items[4].collapse->defaultOpen, "collapse 默认展开状态解析错误");
    require(items[4].collapse->items[0].controls.controlIds[0] == "timeout_ms", "collapse 内第一个控件顺序错误");
    require(items[4].collapse->items[0].controls.controlIds[1] == "scale", "collapse 内第二个控件顺序错误");
}

void test_script_crc_bridge() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("crc_probe").generic_string()), "crc_probe 协议应可加载");

    const auto ctx = sampleCtx();
    host.onControl(ctx, "probe_crc", true);

    bool found = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "crc") {
            if (event.payload.empty()) {
                throw std::runtime_error("crc payload 为空");
            }
            found = event.payload.find("modbus=") != std::string::npos &&
                    event.payload.find("ccitt=") != std::string::npos &&
                    event.payload.find("ieee=") != std::string::npos;
            if (!found) {
                throw std::runtime_error("crc payload=" + event.payload);
            }
        }
    }
    require(found, "CRC 桥接结果不正确");
}

void test_script_read_version_flow() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "device_id", std::string("01"));
    host.onControl(ctx, "read_version", true);

    const auto requests = host.drainTxRequests();
    require(requests.size() == 1, "read_version 应产生一次请求");
    require(requests[0].kind == protoscope::scripting::TxRequestKind::Request, "read_version 应走 proto.request");
    require(requests[0].payload.size() >= 4, "发送帧长度不正确");

    const std::vector<std::uint8_t> response{'O', 'K', '\r', '\n'};
    host.setRequestAwaitingCompletion(true);
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, response});

    bool foundFrame = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "frame") {
            foundFrame = true;
        }
    }
    require(foundFrame, "收到 OK 响应后应产生 frame 事件");
    require(!host.drainRequestDoneResults().empty(), "收到完整帧后应调用 proto.request_done");
}

void test_script_read_version_split_flow() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "read_version", true);

    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, {'O'}});
    bool foundFrameEarly = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "frame") {
            foundFrameEarly = true;
        }
    }
    require(!foundFrameEarly, "首包只有 O 时不应提前产出 frame");

    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, {'K', '\r', '\n'}});
    bool foundFrame = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "frame") {
            foundFrame = true;
        }
    }
    require(foundFrame, "分包收到 OK 后仍应产出 frame");
}

void test_script_stream_schema_legacy_on_bytes_still_works() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("bytes_only").generic_string()), "bytes_only 协议应可加载");

    const auto ctx = sampleCtx();
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, {0x01, 0x02, 0x03}});

    bool foundLegacy = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "legacy_bytes" && event.payload.find("size=3") != std::string::npos) {
            foundLegacy = true;
        }
    }
    require(foundLegacy, "未启用 stream() 时仍应走 on_bytes");
}

void test_script_stream_schema_bypasses_on_bytes_and_calls_on_frame() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("stream_frame_only").generic_string()), "stream_frame_only 协议应可加载");

    const auto ctx = sampleCtx();
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, makeStreamFixtureFrame(0x23)});

    bool foundFrame = false;
    bool foundLegacy = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "legacy_bytes") {
            foundLegacy = true;
        }
        if (event.name == "stream_frame"
            && event.payload.find("stream_sample") != std::string::npos
            && event.payload.find("value=35") != std::string::npos
            && event.payload.find("crc_ok=true") != std::string::npos) {
            foundFrame = true;
        }
    }
    require(foundFrame, "启用 stream() 后应回调 on_frame");
    require(!foundLegacy, "启用 stream() 后不应继续把每批 bytes 传给 on_bytes");
}

void test_script_stream_schema_reports_overflow_and_crc_error() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("stream_frame_only").generic_string()), "stream_frame_only 协议应可加载");

    const auto ctx = sampleCtx();
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18}});

    auto broken = makeStreamFixtureFrame(0x45);
    broken[4] = static_cast<std::uint8_t>(broken[4] ^ 0x01U);
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, broken});

    bool foundOverflow = false;
    bool foundCrc = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name != "stream_error") {
            continue;
        }
        if (event.payload.find("overflow") != std::string::npos) {
            foundOverflow = true;
        }
        if (event.payload.find("crc_mismatch") != std::string::npos) {
            foundCrc = true;
        }
    }
    require(foundOverflow, "buffer overflow 时应回调 stream.on_error");
    require(foundCrc, "CRC 校验失败时应回调 stream.on_error");
}

void test_script_timeout_flow() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "read_version", true);
    host.onTxEvent(ctx,
                   protoscope::scripting::TxEvent{
                       .id = 1,
                       .kind = protoscope::scripting::TxRequestKind::Request,
                       .state = protoscope::scripting::TxEventState::Timeout,
                       .tag = "read_version",
                       .bytes = 4,
                       .queuedMs = nowMs(),
                       .finishedMs = nowMs(),
                       .error = std::string("timeout"),
                   });

    bool foundWarnStatus = false;
    for (const auto& update : host.drainStatusUpdates()) {
        if (update.text.find("超时") != std::string::npos) {
            foundWarnStatus = true;
        }
    }
    require(foundWarnStatus, "超时应更新状态栏");
    require(!host.drainDialogRequests().empty(), "超时应弹出 alert");
}

void test_script_dialog_requests_keep_connection_context() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("dialog_requests").generic_string()), "dialog_requests 协议应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});

    const auto dialogs = host.drainDialogRequests();
    require(dialogs.size() == 1, "on_open 应生成一条 alert 请求");
    const auto& dialog = dialogs.front();
    require(dialog.kind == protoscope::scripting::DialogKind::Alert, "on_open 应生成 alert");
    require(dialog.connection.endpoint == ctx.endpoint, "alert 应保留活动连接 endpoint");
    require(dialog.connection.connectionId == ctx.connectionId, "alert 应保留活动连接 ID");
    require(dialog.title == "连接弹窗", "alert title 不应改变");
    require(dialog.message == ctx.endpoint, "alert message 不应改变");
    require(dialog.level == "warn", "alert level 不应改变");
    require(dialog.dedupeKey == "dialog-open", "alert dedupe_key 不应改变");
}

void test_script_dialog_requests_detached_without_active_connection() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("dialog_requests").generic_string()), "dialog_requests 协议应可加载");

    host.onControl(sampleCtx(), "detached_dialog", true);

    const auto dialogs = host.drainDialogRequests();
    require(dialogs.size() == 1, "未建立活动连接时仍应允许脚本弹窗");
    const auto& dialog = dialogs.front();
    require(dialog.kind == protoscope::scripting::DialogKind::Confirm, "detached_dialog 应生成 confirm");
    require(dialog.connection.endpoint == "detached", "无活动连接时 endpoint 应为 detached");
    require(dialog.connection.connectionId == 0, "无活动连接时 connectionId 应为 0");
    require(!dialog.connection.readyForIo, "无活动连接时 readyForIo 应为 false");
    require(dialog.connection.timestampMs == dialog.createdAtMs, "detached 连接时间戳应来自请求创建时间");
    require(dialog.title == "离线确认", "confirm title 不应改变");
    require(dialog.message == "detached path", "confirm message 不应改变");
    require(dialog.dedupeKey == "dialog-detached", "confirm dedupe_key 不应改变");
}

void test_luals_api_sync_contains_tx_and_dialog_api() {
    std::ifstream input("protocols/protoscope_api.lua");
    require(input.good(), "应能读取 protoscope_api.lua");
    std::stringstream buffer;
    buffer << input.rdbuf();
    const std::string text = buffer.str();

    require(text.find("本文件由 protocols/protoscope_api_manifest.json 生成") != std::string::npos,
            "LuaLS API 应声明生成来源");
    require(text.find("ProtoTransportKind 'tcp_client'|'tcp_server'|'serial'|'udp_peer'") != std::string::npos,
            "LuaLS API 应声明 UDP Peer transport kind");
    require(text.find("@field kind ProtoTransportKind") != std::string::npos, "LuaLS API 应收紧 ctx.kind 类型");
    require(text.find("function proto.request(payload, opts) end") != std::string::npos, "LuaLS API 应声明 proto.request");
    require(text.find("function proto.request_done(result) end") != std::string::npos, "LuaLS API 应声明 proto.request_done");
    require(text.find("function proto.status.set(text, opts) end") != std::string::npos, "LuaLS API 应声明 proto.status.set");
    require(text.find("function proto.plot.push(channel_index, payload) end") != std::string::npos, "LuaLS API 应声明 proto.plot.push");
    require(text.find("function proto.ui.alert(opts) end") != std::string::npos, "LuaLS API 应声明 proto.ui.alert");
    require(text.find("function proto.fs.open(path, opts) end") != std::string::npos, "LuaLS API 应声明 proto.fs.open");
    require(text.find("function proto.fs.read(handle, opts) end") != std::string::npos, "LuaLS API 应声明 proto.fs.read");
    require(text.find("@class ProtoBuffer") != std::string::npos, "LuaLS API 应声明 ProtoBuffer");
    require(text.find("function proto.bits.count(value) end") != std::string::npos, "LuaLS API 应声明 proto.bits.count");
    require(text.find("'elf_symbol_combo'") != std::string::npos, "LuaLS API 应声明 elf_symbol_combo");
    require(text.find("@class ProtoElfSymbolValue") != std::string::npos, "LuaLS API 应声明 ProtoElfSymbolValue");
    require(text.find("function stream() end") != std::string::npos, "LuaLS API 应声明 stream()");
    require(text.find("function on_tx(ctx, evt) end") != std::string::npos, "LuaLS API 应声明 on_tx");
    require(text.find("function on_dialog(ctx, evt) end") != std::string::npos, "LuaLS API 应声明 on_dialog");
    require(text.find("function on_file_dialog(ctx, evt) end") != std::string::npos, "LuaLS API 应声明 on_file_dialog");
    require(text.find("@field color? string") != std::string::npos, "LuaLS API 应声明 ProtoPlotChannel.color");
}

void test_script_missing_callbacks_allowed() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("missing_callbacks").generic_string()), "缺失回调脚本也应允许加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "ping", true);

    bool foundEvent = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "ping") {
            foundEvent = true;
        }
    }
    require(foundEvent, "缺失非必要回调时 on_control 仍应可执行");
}

void test_script_invalid_controls_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_controls").generic_string()), "非法 controls() 应加载失败");
    require(!host.lastError().empty(), "非法 controls() 失败时应记录错误");
}

void test_script_invalid_dock_anchor_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_dock_anchor").generic_string()), "非法 dock anchor 应加载失败");
    require(host.lastError().find("dock anchor 不支持") != std::string::npos, "非法 dock anchor 应给出清晰错误");
}

void test_script_table_layout_unknown_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_unknown_control").generic_string()), "引用未知控件的 table layout 应加载失败");
    require(host.lastError().find("未声明控件") != std::string::npos, "未知控件错误应包含未声明控件提示");
}

void test_script_table_layout_duplicate_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_duplicate_control").generic_string()), "重复引用控件的 table layout 应加载失败");
    require(host.lastError().find("重复引用控件") != std::string::npos, "重复引用错误应包含重复控件提示");
}

void test_script_table_layout_missing_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_missing_control").generic_string()), "遗漏控件的 table layout 应加载失败");
    require(host.lastError().find("缺少控件") != std::string::npos, "遗漏控件错误应包含缺少控件提示");
}

void test_script_table_layout_row_overflow_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_row_overflow").generic_string()), "超出列数的 table layout 应加载失败");
    require(host.lastError().find("单元格数量不能超过 columns") != std::string::npos, "超列错误应包含 columns 提示");
}

void test_script_form_layout_unknown_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_unknown_control").generic_string()), "引用未知控件的 form layout 应加载失败");
    require(host.lastError().find("未声明控件") != std::string::npos, "未知控件错误应包含未声明控件提示");
}

void test_script_form_layout_duplicate_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_duplicate_control").generic_string()), "重复引用控件的 form layout 应加载失败");
    require(host.lastError().find("重复引用控件") != std::string::npos, "重复引用错误应包含重复控件提示");
}

void test_script_form_layout_missing_control_fail() {
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_missing_control").generic_string()), "遗漏控件的 form layout 应加载失败");
    require(host.lastError().find("缺少控件") != std::string::npos, "遗漏控件错误应包含缺少控件提示");
}

void test_script_runtime_error_logged() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("runtime_error").generic_string()), "运行时错误脚本应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "explode", true);

    bool foundErrorLog = false;
    for (const auto& log : host.drainLogs()) {
        if (log.message.find("on_control 执行失败") != std::string::npos) {
            foundErrorLog = true;
        }
    }
    require(foundErrorLog, "运行时报错应写入错误日志");

    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, {0x01, 0x02}});
    bool foundEvent = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "after_error") {
            foundEvent = true;
        }
    }
    require(foundEvent, "脚本回调报错后宿主仍应继续运行");
}

void test_protocol_directory_reload() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议目录应可加载");
    require(host.protocolDirectory() == "protocols/default_protocol", "协议目录应被记录");
    require(host.scriptPath().find("main.lua") != std::string::npos, "协议入口应固定为 main.lua");
}

void test_config_default_roundtrip() {
    protoscope::config::ConfigStore store;
    const auto tempRoot = makeUniqueTempDir("protoscope-config-roundtrip");
    const auto tempPath = tempRoot / "protoscope.yaml";

    auto config = store.load(tempPath).config;
    require(config.protocol.rootDir.find("protocols/templates") != std::string::npos,
            "默认协议根目录应指向 protocols/templates");
    require(config.protocol.selectedDir.find("protocols/templates/default_protocol") != std::string::npos,
            "默认协议目录应指向 protocols/templates/default_protocol");
    require(config.gui.wave.controlMode == protoscope::plot::WaveControlMode::Oscilloscope,
            "波形控制模式默认值应为 oscilloscope");
    require(config.gui.wave.displayFormula == protoscope::plot::WaveDisplayFormula::OffsetThenScale,
            "波形显示公式默认值应为 offset_then_scale");
    require(config.gui.wave.channelCardWidthMode == protoscope::plot::WaveChannelCardWidthMode::Fixed,
            "CH 卡片宽度模式默认值应为 fixed");
    require(std::abs(config.gui.wave.channelCardFixedWidth - 128.0) < 1e-12, "CH 卡片固定宽度默认值应为 128");
    require(std::abs(config.gui.wave.channelCardAdaptiveRatio - 0.22) < 1e-12, "CH 卡片自适应比例默认值应为 0.22");
    require(std::abs(config.gui.wave.verticalAutoFitMultiplier - 1.2) < 1e-12, "Y 轴 Auto Fit 系数默认值应为 1.2");
    require(config.gui.logHistory.transferRawLimit == 10000, "原始收发历史默认上限应为 10000");
    require(config.gui.logHistory.transferFrameLimit == 120000, "逐帧收发历史默认上限应为 120000");
    require(config.gui.logHistory.hostLimit == 5000, "宿主日志默认上限应为 5000");
    require(config.gui.logHistory.scriptLimit == 5000, "脚本日志默认上限应为 5000");
    require(config.gui.sendHistoryLimit == 20, "发送历史条数默认值应为 20");
    config.communication.kind = protoscope::transport::TransportKind::Serial;
    config.communication.serial.portName = "COM9";
    config.communication.serial.dataBits = 7;
    config.communication.serial.parity = "even";
    config.communication.serial.stopBits = "two";
    config.communication.serial.flowControl = "hardware";
    config.communication.udpPeer.bindAddress = "127.0.0.1";
    config.communication.udpPeer.bindPort = 19001;
    config.communication.udpPeer.remoteHost = "192.0.2.10";
    config.communication.udpPeer.remotePort = 19002;
    config.app.configHotReload.enabled = true;
    config.gui.wave.maxRenderPointsPerChannel = 64;
    config.gui.wave.maxRenderVertices = 4096;
    config.gui.wave.overviewMaxSamples = 128;
    config.gui.wave.minVisibleTimeSpan = 0.0025;
    config.gui.wave.controlMode = protoscope::plot::WaveControlMode::LegacyGlobal;
    config.gui.wave.displayFormula = protoscope::plot::WaveDisplayFormula::ScaleThenOffset;
    config.gui.wave.channelCardWidthMode = protoscope::plot::WaveChannelCardWidthMode::Adaptive;
    config.gui.wave.channelCardFixedWidth = 144.0;
    config.gui.wave.channelCardAdaptiveRatio = 0.3;
    config.gui.wave.verticalAutoFitMultiplier = 1.5;
    config.gui.logHistory.transferRawLimit = 11;
    config.gui.logHistory.transferFrameLimit = 22;
    config.gui.logHistory.hostLimit = 33;
    config.gui.logHistory.scriptLimit = 44;
    config.gui.sendHistoryLimit = 7;
    config.scripting.fileIo.enabled = true;
    config.scripting.fileIo.maxOpenFiles = 3;
    config.scripting.fileIo.defaultChunkBytes = 32;
    config.scripting.fileIo.maxChunkBytes = 64;
    config.scripting.fileIo.extraAllowedRoots = {"firmware"};

    std::string error;
    require(store.save(tempPath, config, error), "默认配置写回失败");

    const auto reloaded = store.load(tempPath);
    require(reloaded.config.communication.kind == protoscope::transport::TransportKind::Serial, "串口模式 roundtrip 失败");
    require(reloaded.config.communication.serial.portName == "COM9", "串口端口 roundtrip 失败");
    require(reloaded.config.communication.serial.dataBits == 7, "串口数据位 roundtrip 失败");
    require(reloaded.config.communication.serial.parity == "even", "串口奇偶校验 roundtrip 失败");
    require(reloaded.config.communication.serial.stopBits == "two", "串口停止位 roundtrip 失败");
    require(reloaded.config.communication.serial.flowControl == "hardware", "串口流控 roundtrip 失败");
    require(reloaded.config.communication.udpPeer.bindAddress == "127.0.0.1", "UDP Peer 本地地址 roundtrip 失败");
    require(reloaded.config.communication.udpPeer.bindPort == 19001, "UDP Peer 本地端口 roundtrip 失败");
    require(reloaded.config.communication.udpPeer.remoteHost == "192.0.2.10", "UDP Peer 远端地址 roundtrip 失败");
    require(reloaded.config.communication.udpPeer.remotePort == 19002, "UDP Peer 远端端口 roundtrip 失败");
    require(reloaded.config.app.configHotReload.enabled, "配置热重载开关 roundtrip 失败");
    require(reloaded.config.gui.wave.controlMode == protoscope::plot::WaveControlMode::LegacyGlobal,
            "波形控制模式 roundtrip 失败");
    require(reloaded.config.gui.wave.displayFormula == protoscope::plot::WaveDisplayFormula::ScaleThenOffset,
            "波形显示公式 roundtrip 失败");
    require(reloaded.config.gui.wave.channelCardWidthMode == protoscope::plot::WaveChannelCardWidthMode::Adaptive,
            "CH 卡片宽度模式 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.channelCardFixedWidth - 144.0) < 1e-12, "CH 卡片固定宽度 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.channelCardAdaptiveRatio - 0.3) < 1e-12, "CH 卡片自适应比例 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.verticalAutoFitMultiplier - 1.5) < 1e-12, "Y 轴 Auto Fit 系数 roundtrip 失败");
    require(reloaded.config.gui.wave.maxRenderPointsPerChannel == 64, "波形每通道渲染点数 roundtrip 失败");
    require(reloaded.config.gui.wave.maxRenderVertices == 4096, "波形顶点预算 roundtrip 失败");
    require(reloaded.config.gui.wave.overviewMaxSamples == 128, "波形概览点数 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.minVisibleTimeSpan - 0.0025) < 1e-12, "波形最小可视跨度 roundtrip 失败");
    require(reloaded.config.gui.logHistory.transferRawLimit == 11, "原始收发历史上限 roundtrip 失败");
    require(reloaded.config.gui.logHistory.transferFrameLimit == 22, "逐帧收发历史上限 roundtrip 失败");
    require(reloaded.config.gui.logHistory.hostLimit == 33, "宿主日志历史上限 roundtrip 失败");
    require(reloaded.config.gui.logHistory.scriptLimit == 44, "脚本日志历史上限 roundtrip 失败");
    require(reloaded.config.gui.sendHistoryLimit == 7, "发送历史条数 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.enabled, "Lua 文件 IO 开关 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.maxOpenFiles == 3, "Lua 文件 IO 打开数上限 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.defaultChunkBytes == 32, "Lua 文件 IO 默认分块 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.maxChunkBytes == 64, "Lua 文件 IO 最大分块 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.extraAllowedRoots.size() == 1
                && reloaded.config.scripting.fileIo.extraAllowedRoots[0] == "firmware",
            "Lua 文件 IO 额外授权根 roundtrip 失败");
}

void test_script_file_io_proto_buffer_roundtrip() {
    const auto tempRoot = makeUniqueTempDir("protoscope-script-file-io");
    const auto protocolDir = tempRoot / "proto";
    std::filesystem::create_directories(protocolDir);
    {
        std::ofstream data(protocolDir / "input.bin", std::ios::binary);
        data << "abcdef";
    }
    {
        std::ofstream script(protocolDir / "main.lua");
        script << "function on_open(ctx)\n";
        script << "  local h = assert(proto.fs.open('input.bin', { mode = 'read' }))\n";
        script << "  local chunk, err = proto.fs.read(h, { max_bytes = 3 })\n";
        script << "  assert(chunk, err)\n";
        script << "  assert(chunk:size() == 3)\n";
        script << "  proto.fs.close(h)\n";
        script << "  local out = assert(proto.fs.open('out.bin', { mode = 'write', overwrite = true }))\n";
        script << "  assert(proto.fs.write(out, chunk))\n";
        script << "  proto.fs.close(out)\n";
        script << "  proto.send(chunk, { tag = 'file-chunk' })\n";
        script << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.generic_string()), "文件 IO 协议应可加载");
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});
    const auto requests = host.drainTxRequests();
    require(requests.size() == 1, "ProtoBuffer 应可直接传给 proto.send");
    require(requests[0].payload == std::vector<std::uint8_t>{'a', 'b', 'c'}, "发送 payload 应来自文件分块");
    require(requests[0].tag == "file-chunk", "发送 tag 应保留");

    std::ifstream out(protocolDir / "out.bin", std::ios::binary);
    std::string text;
    out >> text;
    require(text == "abc", "ProtoBuffer 写入文件后内容应一致");
}

void test_config_wave_mode_invalid_fallback() {
    protoscope::config::ConfigStore store;
    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-config-wave-invalid.yaml";
    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    out << "gui:\n"
           "  wave:\n"
           "    control_mode: weird\n"
           "    display_formula: wrong\n"
           "    channel_card_width_mode: weird\n"
           "    channel_card_fixed_width: 0\n"
           "    channel_card_adaptive_ratio: -0.5\n"
           "    vertical_auto_fit_multiplier: 0\n";
    out.close();

    const auto loaded = store.load(tempPath).config;
    require(loaded.gui.wave.controlMode == protoscope::plot::WaveControlMode::Oscilloscope,
            "非法 control_mode 应回退到 oscilloscope");
    require(loaded.gui.wave.displayFormula == protoscope::plot::WaveDisplayFormula::OffsetThenScale,
            "非法 display_formula 应回退到 offset_then_scale");
    require(loaded.gui.wave.channelCardWidthMode == protoscope::plot::WaveChannelCardWidthMode::Fixed,
            "非法 channel_card_width_mode 应回退到 fixed");
    require(std::abs(loaded.gui.wave.channelCardFixedWidth - 128.0) < 1e-12, "非正固定宽度应回退到 128");
    require(std::abs(loaded.gui.wave.channelCardAdaptiveRatio - 0.22) < 1e-12, "非正自适应比例应回退到 0.22");
    require(std::abs(loaded.gui.wave.verticalAutoFitMultiplier - 1.2) < 1e-12, "非正 Auto Fit 系数应回退到 1.2");
}

void test_config_logging_roundtrip() {
    protoscope::config::ConfigStore store;
    const auto tempRoot = std::filesystem::temp_directory_path() / "protoscope-config-logging";
    const auto tempPath = tempRoot / "logging.yaml";
    std::filesystem::create_directories(tempRoot);

    auto base = store.load(tempPath).config;
    base.logging.level = protoscope::config::LogLevel::Warn;
    base.logging.filePath = "logs/protoscope.log";

    std::string error;
    require(store.save(tempPath, base, error), "日志配置写回失败");
    auto reloaded = store.load(tempPath).config;
    require(reloaded.logging.level == protoscope::config::LogLevel::Warn, "日志等级 roundtrip 失败");
    require(reloaded.logging.filePath == "logs/protoscope.log", "日志相对路径 roundtrip 失败");

    base.logging.filePath.clear();
    require(store.save(tempPath, base, error), "空日志路径写回失败");
    reloaded = store.load(tempPath).config;
    require(reloaded.logging.filePath.empty(), "空日志路径应保持为空");

    const auto missingPath = tempRoot / "missing.yaml";
    const auto missing = store.load(missingPath).config;
    require(missing.logging.filePath.empty(), "缺失日志路径时应默认为空");
    require(missing.logging.level == protoscope::config::LogLevel::Info, "缺失日志等级时应回退到 info");
}

void test_config_default_protocol_workspace_initializes_half_duplex_demos() {
    protoscope::config::ConfigStore store;
    std::string error;

    require(store.ensureDefaultProtocolWorkspace(error), "protocols 工作区初始化失败");

    const auto protocolRoot = store.defaultProtocolDir().parent_path().parent_path();
    const auto templateRoot = protocolRoot / "templates";
    require(std::filesystem::exists(templateRoot / "default_protocol" / "main.lua"), "默认协议模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "lua_waveform_demo" / "main.lua"), "Lua 波形模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "half_duplex_modbus_master" / "main.lua"), "半双工主机模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "half_duplex_modbus_slave" / "main.lua"), "半双工从机模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "README.md"), "模板 README 应生成");
    require(std::filesystem::exists(protocolRoot / "stream_types.lua"), "stream schema 类型提示文件应生成");
    require(std::filesystem::exists(protocolRoot / "README.md"), "protocols README 应生成");
    require(std::filesystem::exists(protocolRoot / "protoscope_api.lua"), "LuaLS API 提示文件应生成");

    std::ifstream masterScript(templateRoot / "half_duplex_modbus_master" / "main.lua");
    require(masterScript.good(), "半双工主机示例脚本应可读取");
    std::ifstream slaveScript(templateRoot / "half_duplex_modbus_slave" / "main.lua");
    require(slaveScript.good(), "半双工从机示例脚本应可读取");
    std::ifstream readmeInput(protocolRoot / "README.md");
    std::stringstream readmeBuffer;
    readmeBuffer << readmeInput.rdbuf();
    const auto readmeText = readmeBuffer.str();
    require(readmeText.find("Lua 协议脚本指南") != std::string::npos,
            "默认 README 应包含 Lua 协议脚本指南");
}

void test_config_default_protocol_workspace_fills_missing_resources() {
    protoscope::config::ConfigStore store;
    const auto protocolRoot = store.defaultProtocolDir().parent_path().parent_path();
    std::string error;

    std::filesystem::create_directories(protocolRoot);
    {
        std::ofstream out(protocolRoot / "keep.txt");
        out << "keep";
    }

    require(store.ensureDefaultProtocolWorkspace(error), "已有 protocols 根目录时初始化不应失败");
    require(std::filesystem::exists(protocolRoot / "keep.txt"), "已有 protocols 内容不应丢失");
    require(std::filesystem::exists(protocolRoot / "templates" / "default_protocol" / "main.lua"),
            "已有 protocols 根目录时也应补齐默认模板");
    require(std::filesystem::exists(protocolRoot / "templates" / "lua_waveform_demo" / "main.lua"),
            "已有 protocols 根目录时也应补齐波形模板");
    require(std::filesystem::exists(protocolRoot / "README.md"), "已有 protocols 根目录时应补齐 README");
    require(std::filesystem::exists(protocolRoot / "protoscope_api.lua"), "已有 protocols 根目录时应补齐 LuaLS API 提示文件");
}

void test_protocol_scan_and_root_roundtrip() {
    protoscope::config::ConfigStore store;
    const auto tempRoot = std::filesystem::temp_directory_path() / "protoscope-protocol-scan";
    const auto alphaDir = tempRoot / "alpha";
    const auto betaDir = tempRoot / "beta";
    std::string error;

    require(store.ensureDefaultProtocolScript(alphaDir, error), "alpha 协议脚本补建失败");
    require(store.ensureDefaultProtocolScript(betaDir, error), "beta 协议脚本补建失败");

    const auto scanned = store.scanProtocolDirectories(tempRoot);
    require(scanned.size() == 2, "协议目录扫描数量不正确");
    require(scanned[0].find("alpha") != std::string::npos, "扫描结果应包含 alpha");
    require(scanned[1].find("beta") != std::string::npos, "扫描结果应包含 beta");

    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-protocol-root-roundtrip.yaml";
    auto config = store.load(tempPath).config;
    config.protocol.rootDir = tempRoot.generic_string();
    config.protocol.selectedDir = betaDir.generic_string();

    require(store.save(tempPath, config, error), "协议根目录保存失败");
    const auto reloaded = store.load(tempPath);
    require(reloaded.config.protocol.rootDir == tempRoot.generic_string(), "协议根目录 roundtrip 失败");
    require(reloaded.config.protocol.selectedDir == betaDir.generic_string(), "协议目录 roundtrip 失败");

    const auto normalized = store.normalizeProtocolDir(tempRoot, tempRoot / "missing");
    require(normalized == alphaDir, "root-aware 协议目录归一化应优先回退到当前 root 下的有效目录");

}

void test_script_plot_api_snapshot() {
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("plot_stream").generic_string()), "plot_stream 协议应可加载");

    protoscope::transport::TransportOpenEvent openEvent{.context = sampleCtx()};
    host.onTransportOpen(openEvent);

    auto setups = host.drainPlotSetups();
    auto appends = host.drainPlotAppends();
    require(setups.size() == 1, "打开连接后应生成 1 次 plot.setup");
    require(setups[0].channels.size() == 2, "plot.setup 应声明 2 个通道");
    require(std::abs(setups[0].channels[0].ratio - 0.5) < 1e-12, "CH1 ratio 解析错误");
    require(std::abs(setups[0].channels[1].ratio - 1.0) < 1e-12, "CH2 ratio 默认值错误");
    require(std::abs(setups[0].channels[0].scale - 2.0) < 1e-12, "CH1 scale 解析错误");
    require(std::abs(setups[0].channels[1].scale - 1.0) < 1e-12, "CH2 scale 默认值错误");
    require(std::abs(setups[0].channels[0].offset - 0.0) < 1e-12, "CH1 offset 解析错误");
    require(std::abs(setups[0].channels[1].offset - 1.0) < 1e-12, "CH2 offset 解析错误");
    require(appends.size() == 2, "打开连接后应推送 2 组通道数据");
    require(appends[0].second.samples.size() == 3, "通道采样点数量不正确");
}

void test_half_duplex_modbus_request_batches() {
    protoscope::scripting::ScriptHost host;
    requireProtocolLoaded(host, "protocols/half_duplex_modbus_master");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.drainPlotSetups();
    host.onControl(ctx, "auto_start", true);

    const auto requests = host.drainTxRequests();
    require(requests.size() == 5, "自动配置并启动应入队 5 条请求");
    for (std::size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        require(request.kind == protoscope::scripting::TxRequestKind::Request, "主机寄存器写入应全部走 proto.request");
        require(request.payload[0] == 0xFF, "请求帧头不正确");
        if (index < 4) {
            require(request.payload.size() == 13, "前 4 条请求都应是 13 字节 FC16");
            require(request.payload[1] == 0x10, "前 4 条请求应为 FC16");
            require(readBe16(request.payload, 4) == 2U, "FC16 请求应固定写 2 个寄存器");
            require(request.payload[6] == 4U, "FC16 请求 byte_count 应为 4");
        } else {
            require(request.payload.size() == 8, "最后 1 条请求应是 8 字节 FC06");
            require(request.payload[1] == 0x06, "最后 1 条请求应为 FC06");
        }
    }

    require(readBe16(requests[0].payload, 2) == 0x1010U, "第一批请求起始地址错误");
    require(readBe16(requests[1].payload, 2) == 0x1012U, "第二批请求起始地址错误");
    require(readBe16(requests[2].payload, 2) == 0x5A5AU, "第三批请求起始地址错误");
    require(readBe16(requests[3].payload, 2) == 0x5A5CU, "第四批请求起始地址错误");
    require(readBe16(requests[4].payload, 2) == 0x8888U, "第五批请求起始地址错误");
    require(readBe16(requests[4].payload, 4) == 0x0001U, "启动请求应写 0x8888=0x0001");
}

void test_half_duplex_modbus_ack_and_plot_flow() {
    protoscope::scripting::ScriptHost master;
    protoscope::scripting::ScriptHost slave;
    requireProtocolLoaded(master, "protocols/half_duplex_modbus_master");
    requireProtocolLoaded(slave, "protocols/half_duplex_modbus_slave");

    const auto ctx = sampleCtx();
    completeHalfDuplexStartup(master, slave, ctx);
    const auto frame = nextHalfDuplexWaveFrame(slave);
    require(frame.size() == 1680, "波形批长度应为 1680");
    const auto splitAt = static_cast<std::size_t>(7);
    const std::vector<std::uint8_t> part1(frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(splitAt));
    const std::vector<std::uint8_t> part2(frame.begin() + static_cast<std::ptrdiff_t>(splitAt), frame.end());

    master.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, part1});
    require(master.drainPlotAppends().empty(), "半包波形不应提前推送 plot");
    master.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, part2});

    const auto appends = master.drainPlotAppends();
    require(appends.size() == 480, "120 帧 x 4 通道后应推送 480 组单点波形");
    std::array<std::size_t, 4> perChannel{};
    std::size_t totalSamples = 0;
    for (const auto& append : appends) {
        require(append.second.samples.size() == 1, "SN Scope 上传帧应按单点推送");
        require(append.first < perChannel.size(), "通道编号应落在 0~3");
        perChannel[append.first] += append.second.samples.size();
        totalSamples += append.second.samples.size();
    }
    require(totalSamples == 480, "总样本数应为 480");
    require(perChannel[0] == 120 && perChannel[1] == 120 && perChannel[2] == 120 && perChannel[3] == 120,
            "四个通道都应各收到 120 个样本");
}

void test_half_duplex_modbus_loss_status_keeps_valid_frame() {
    protoscope::scripting::ScriptHost master;
    requireProtocolLoaded(master, "protocols/half_duplex_modbus_master");

    const auto ctx = sampleCtx();
    master.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    master.drainPlotSetups();

    master.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, makeSnScopeUploadFrame(1, 100, 200, 300, 400)});
    master.drainPlotAppends();
    master.drainStatusUpdates();

    const auto skippedFrame = makeSnScopeUploadFrame(3, 110, 210, 310, 410);

    master.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, skippedFrame});
    const auto appends = master.drainPlotAppends();
    require(appends.size() == 4, "跳号后的有效波形帧仍应继续推送");

    bool foundLostWarn = false;
    for (const auto& update : master.drainStatusUpdates()) {
        if (update.text.find("丢帧: 1") != std::string::npos) {
            foundLostWarn = true;
        }
    }
    require(foundLostWarn, "序列号跳号后应提示累计丢帧数");
}

void test_half_duplex_modbus_ack_matching_rules() {
    const auto runRequest = [](const char* controlId,
                               const std::vector<std::uint8_t>& ack,
                               bool expectedOk,
                               const char* expectedStatus) {
        protoscope::scripting::ScriptHost host;
        requireProtocolLoaded(host, "protocols/half_duplex_modbus_master");

        const auto ctx = sampleCtx();
        host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
        host.drainPlotSetups();
        host.onControl(ctx, controlId, true);

        const auto requests = host.drainTxRequests();
        require(requests.size() == 1, "单次按钮操作应只生成 1 条 request");
        const auto& request = requests.front();
        host.onTxEvent(ctx,
                       protoscope::scripting::TxEvent{
                           .id = request.id,
                           .kind = protoscope::scripting::TxRequestKind::Request,
                           .state = protoscope::scripting::TxEventState::Sent,
                           .tag = request.tag,
                           .bytes = request.payload.size(),
                           .queuedMs = nowMs(),
                           .finishedMs = nowMs(),
                           .error = std::nullopt,
                       });
        host.setRequestAwaitingCompletion(true);
        host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, ack});

        const auto results = host.drainRequestDoneResults();
        require(results.size() == 1, "收到 ACK 后应产生 1 条 request_done 结果");
        require(results[0].ok == expectedOk, "ACK 匹配结果不符合预期");

        bool foundStatus = false;
        for (const auto& update : host.drainStatusUpdates()) {
            if (update.text.find(expectedStatus) != std::string::npos) {
                foundStatus = true;
            }
        }
        require(foundStatus, "状态栏应包含 ACK 匹配结果");
    };

    runRequest("start_stream", makeSnScopeFc06Ack(0x8888U, 0x0001U), true, "FC06 ACK 匹配");
    runRequest("start_stream", makeSnScopeFc06Ack(0x8888U, 0x0000U), false, "期望");
    runRequest("stop_stream", makeSnScopeFc06Ack(0x8888U, 0x0000U), true, "FC06 ACK 匹配");

    protoscope::scripting::ScriptHost host;
    requireProtocolLoaded(host, "protocols/half_duplex_modbus_master");
    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.drainPlotSetups();
    host.onControl(ctx, "auto_start", true);

    const auto requests = host.drainTxRequests();
    require(!requests.empty(), "自动启动应生成 FC16 请求");
    const auto& fc16 = requests.front();
    host.onTxEvent(ctx,
                   protoscope::scripting::TxEvent{
                       .id = fc16.id,
                       .kind = protoscope::scripting::TxRequestKind::Request,
                       .state = protoscope::scripting::TxEventState::Sent,
                       .tag = fc16.tag,
                       .bytes = fc16.payload.size(),
                       .queuedMs = nowMs(),
                       .finishedMs = nowMs(),
                       .error = std::nullopt,
                   });
    host.setRequestAwaitingCompletion(true);
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, makeSnScopeFc16Ack(0x1010U, 1U)});

    const auto results = host.drainRequestDoneResults();
    require(results.size() == 1, "FC16 ACK 返回后应调用 request_done");
    require(!results[0].ok, "FC16 ACK 数量不匹配时应失败");
    bool foundExpected = false;
    for (const auto& update : host.drainStatusUpdates()) {
        if (update.text.find("期望") != std::string::npos) {
            foundExpected = true;
        }
    }
    require(foundExpected, "FC16 ACK 不匹配时应提示收到/期望信息");
}

void test_half_duplex_modbus_sticky_frames() {
    protoscope::scripting::ScriptHost master;
    protoscope::scripting::ScriptHost slave;
    requireProtocolLoaded(master, "protocols/half_duplex_modbus_master");
    requireProtocolLoaded(slave, "protocols/half_duplex_modbus_slave");

    const auto ctx = sampleCtx();
    completeHalfDuplexStartup(master, slave, ctx);
    master.drainStatusUpdates();

    const auto firstWave = nextHalfDuplexWaveFrame(slave);
    const auto secondWave = nextHalfDuplexWaveFrame(slave);
    std::vector<std::uint8_t> combined;
    appendBytes(combined, firstWave);
    appendBytes(combined, secondWave);

    master.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, combined});
    const auto appends = master.drainPlotAppends();
    require(appends.size() == 960, "两批粘包输入后应推送 960 组单点波形");
}

void test_half_duplex_modbus_noise_prefix_ignored() {
    protoscope::scripting::ScriptHost master;
    protoscope::scripting::ScriptHost slave;
    requireProtocolLoaded(master, "protocols/half_duplex_modbus_master");
    requireProtocolLoaded(slave, "protocols/half_duplex_modbus_slave");

    const auto ctx = sampleCtx();
    completeHalfDuplexStartup(master, slave, ctx);
    master.drainStatusUpdates();

    std::vector<std::uint8_t> noisy = {0x00U, 0x7EU, 0x11U, 0x22U, 0x33U};
    appendBytes(noisy, nextHalfDuplexWaveFrame(slave));

    master.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, noisy});
    const auto appends = master.drainPlotAppends();
    require(appends.size() == 480, "噪声前缀后仍应解析出完整上传批");
}

void test_half_duplex_modbus_crc_resync_keeps_following_frame() {
    protoscope::scripting::ScriptHost master;
    requireProtocolLoaded(master, "protocols/half_duplex_modbus_master");

    const auto ctx = sampleCtx();
    master.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    master.drainPlotSetups();
    master.drainStatusUpdates();

    auto broken = makeSnScopeUploadFrame(1, 100, 200, 300, 400);
    broken[12] = static_cast<std::uint8_t>(broken[12] ^ 0x01U);
    const auto good = makeSnScopeUploadFrame(2, 101, 201, 301, 401);

    std::vector<std::uint8_t> combined;
    appendBytes(combined, broken);
    appendBytes(combined, good);

    master.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, combined});
    const auto appends = master.drainPlotAppends();
    require(appends.size() == 4, "CRC 坏帧后仍应继续解析后续好帧");

    bool foundCrcError = false;
    for (const auto& update : master.drainStatusUpdates()) {
        if (update.text.find("CRC") != std::string::npos) {
            foundCrcError = true;
        }
    }
    require(foundCrcError, "CRC 校验失败后应上报解析错误");
}

void test_half_duplex_modbus_multi_schema_candidates() {
    const auto tempRoot = makeUniqueTempDir("protoscope-half-duplex-multi-schema");
    const auto protocolDir = tempRoot / "multi_schema";
    std::filesystem::create_directories(protocolDir);

    {
        std::ofstream out(protocolDir / "main.lua");
        require(out.good(), "多 schema 测试脚本应可写入");
        out << "local FRAME_ALPHA = \"alpha_frame\"\n";
        out << "local FRAME_BETA = \"beta_frame\"\n";
        out << "local FUNC_ALPHA = 0x31\n";
        out << "local FUNC_BETA = 0x41\n";
        out << "local function append_bytes(target, source)\n";
        out << "  for index = 1, #source do\n";
        out << "    target[#target + 1] = source[index]\n";
        out << "  end\n";
        out << "end\n";
        out << "local function push_u16_le(target, value)\n";
        out << "  target[#target + 1] = value & 0xFF\n";
        out << "  target[#target + 1] = (value >> 8) & 0xFF\n";
        out << "end\n";
        out << "local function build_frame(header, func, sequence, length_type, payload)\n";
        out << "  local frame = { header[1], header[2], func, sequence }\n";
        out << "  if length_type == \"u8\" then\n";
        out << "    frame[#frame + 1] = #payload\n";
        out << "  else\n";
        out << "    push_u16_le(frame, #payload)\n";
        out << "  end\n";
        out << "  append_bytes(frame, payload)\n";
        out << "  local crc = proto.crc16_modbus(frame)\n";
        out << "  push_u16_le(frame, crc)\n";
        out << "  return frame\n";
        out << "end\n";
        out << "local protocol = {\n";
        out << "  frames = {\n";
        out << "    {\n";
        out << "      id = FRAME_ALPHA,\n";
        out << "      header = { 0xA1, 0x1A },\n";
        out << "      sequence = { type = \"u8\", bits = 8 },\n";
        out << "      length = { type = \"u8\", unit = \"bytes\" },\n";
        out << "      messages = {\n";
        out << "        [FUNC_ALPHA] = { name = \"alpha\", fields = { { name = \"value\", type = \"u8\", offset = 0 } } },\n";
        out << "      },\n";
        out << "    },\n";
        out << "    {\n";
        out << "      id = FRAME_BETA,\n";
        out << "      header = { 0xB2, 0x2B },\n";
        out << "      sequence = { type = \"u8\", bits = 8 },\n";
        out << "      length = { type = \"u16\", endian = \"le\", unit = \"bytes\" },\n";
        out << "      messages = {\n";
        out << "        [FUNC_BETA] = { name = \"beta\", fields = { { name = \"value\", type = \"u16\", endian = \"le\", offset = 0 } } },\n";
        out << "      },\n";
        out << "    },\n";
        out << "  },\n";
        out << "}\n";
        out << "function describe()\n";
        out << "  return {\n";
        out << "    {\n";
        out << "      id = \"multi_schema\",\n";
        out << "      title = \"Multi Schema\",\n";
        out << "      controls = {},\n";
        out << "    },\n";
        out << "  }\n";
        out << "end\n";
        out << "function on_open(ctx)\n";
        out << "  proto.status.set(\"multi schema on_open\", { level = \"info\" })\n";
        out << "  local alpha = build_frame({ 0xA1, 0x1A }, FUNC_ALPHA, 1, \"u8\", { 7 })\n";
        out << "  local beta = build_frame({ 0xB2, 0x2B }, FUNC_BETA, 2, \"u16\", { 0x34, 0x12 })\n";
        out << "  local alpha_id, alpha_send_err = proto.send(alpha, { tag = \"alpha\" })\n";
        out << "  if not alpha_id then\n";
        out << "    proto.status.set(\"alpha 发送失败: \" .. tostring(alpha_send_err), { level = \"error\" })\n";
        out << "    return\n";
        out << "  end\n";
        out << "  local beta_id, beta_send_err = proto.send(beta, { tag = \"beta\" })\n";
        out << "  if not beta_id then\n";
        out << "    proto.status.set(\"beta 发送失败: \" .. tostring(beta_send_err), { level = \"error\" })\n";
        out << "    return\n";
        out << "  end\n";
        out << "end\n";
        out << "local function on_multi_frame(ctx, frame)\n";
        out << "  proto.emit(\"frame\", { frame_id = frame.name, value = frame.fields.value })\n";
        out << "end\n";
        out << "function stream()\n";
        out << "  return {\n";
        out << "    buffer = {\n";
        out << "      capacity = 64,\n";
        out << "      overflow = \"drop_oldest\",\n";
        out << "    },\n";
        out << "    frames = {\n";
        out << "      {\n";
        out << "        name = FRAME_ALPHA,\n";
        out << "        header = { 0xA1, 0x1A, FUNC_ALPHA },\n";
        out << "        len = { offset = 5, type = \"u8\", means = \"payload\", extra = 7 },\n";
        out << "        crc = { type = \"crc16_modbus\", order = \"lo_hi\" },\n";
        out << "        fields = {\n";
        out << "          { name = \"sequence\", type = \"u8\", offset = 4 },\n";
        out << "          { name = \"value\", type = \"u8\", offset = 6 },\n";
        out << "        },\n";
        out << "        on_frame = on_multi_frame,\n";
        out << "      },\n";
        out << "      {\n";
        out << "        name = FRAME_BETA,\n";
        out << "        header = { 0xB2, 0x2B, FUNC_BETA },\n";
        out << "        len = { offset = 5, type = \"u16_le\", means = \"payload\", extra = 8 },\n";
        out << "        crc = { type = \"crc16_modbus\", order = \"lo_hi\" },\n";
        out << "        fields = {\n";
        out << "          { name = \"sequence\", type = \"u8\", offset = 4 },\n";
        out << "          { name = \"value\", type = \"u16_le\", offset = 7 },\n";
        out << "        },\n";
        out << "        on_frame = on_multi_frame,\n";
        out << "      },\n";
        out << "    },\n";
        out << "  }\n";
        out << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.generic_string()), "多 schema 测试协议应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});

    const auto requests = host.drainTxRequests();
    if (requests.size() != 2) {
        std::ostringstream detail;
        detail << "多 schema 测试应发送两帧不同 schema 的数据";
        for (const auto& update : host.drainStatusUpdates()) {
            detail << " | status=" << update.text;
        }
        throw std::runtime_error(detail.str());
    }

    std::vector<std::uint8_t> combined;
    appendBytes(combined, requests[0].payload);
    appendBytes(combined, requests[1].payload);
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, combined});

    bool foundAlpha = false;
    bool foundBeta = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name != "frame") {
            continue;
        }
        if (event.payload.find("alpha_frame") != std::string::npos && event.payload.find("value=7") != std::string::npos) {
            foundAlpha = true;
        }
        if (event.payload.find("beta_frame") != std::string::npos && event.payload.find("4660") != std::string::npos) {
            foundBeta = true;
        }
    }
    require(foundAlpha, "应按首个候选 schema 解析 alpha_frame");
    require(foundBeta, "应按第二个候选 schema 解析 beta_frame");
}

namespace {

static const TestCase kAllTests[] = {
    {"hex_roundtrip", &test_hex_roundtrip},
    {"hex_invalid_input", &test_hex_invalid_input},
    {"hex_normalize_input", &test_hex_normalize_input},
    {"hex_editor_cursor_normalize", &test_hex_editor_cursor_normalize},
    {"crc_known_vectors", &test_crc_known_vectors},
    {"config_external_reload_state", &test_config_external_reload_state},
    {"script_controls_snapshot", &test_script_controls_snapshot},
    {"default_protocol_logs_elf_symbol_info", &test_default_protocol_logs_elf_symbol_info},
    {"script_elf_symbol_combo_descriptor_defaults", &test_script_elf_symbol_combo_descriptor_defaults},
    {"script_elf_symbol_combo_invalid_config_fails", &test_script_elf_symbol_combo_invalid_config_fails},
    {"script_elf_symbol_combo_get_control_returns_table", &test_script_elf_symbol_combo_get_control_returns_table},
    {"script_on_open_log", &test_script_on_open_log},
    {"script_on_close_log", &test_script_on_close_log},
    {"script_on_error_log", &test_script_on_error_log},
    {"script_multi_dock_snapshot", &test_script_multi_dock_snapshot},
    {"script_dock_layout_fields", &test_script_dock_layout_fields},
    {"script_table_layout_snapshot", &test_script_table_layout_snapshot},
    {"script_form_layout_snapshot", &test_script_form_layout_snapshot},
    {"script_crc_bridge", &test_script_crc_bridge},
    {"script_read_version_flow", &test_script_read_version_flow},
    {"script_read_version_split_flow", &test_script_read_version_split_flow},
    {"script_stream_schema_legacy_on_bytes_still_works", &test_script_stream_schema_legacy_on_bytes_still_works},
    {"script_stream_schema_bypasses_on_bytes_and_calls_on_frame", &test_script_stream_schema_bypasses_on_bytes_and_calls_on_frame},
    {"script_stream_schema_reports_overflow_and_crc_error", &test_script_stream_schema_reports_overflow_and_crc_error},
    {"script_timeout_flow", &test_script_timeout_flow},
    {"frame_stream_parser_waits_for_full_frame", &test_frame_stream_parser_waits_for_full_frame},
    {"frame_stream_parser_handles_sticky_frames_and_noise_prefix", &test_frame_stream_parser_handles_sticky_frames_and_noise_prefix},
    {"frame_stream_parser_crc_resync_keeps_following_frame", &test_frame_stream_parser_crc_resync_keeps_following_frame},
    {"frame_stream_parser_reports_overflow_drop_oldest", &test_frame_stream_parser_reports_overflow_drop_oldest},
    {"frame_stream_parser_supports_fixed_size_raw_frame", &test_frame_stream_parser_supports_fixed_size_raw_frame},
    {"luals_api_sync_contains_tx_and_dialog_api", &test_luals_api_sync_contains_tx_and_dialog_api},
    {"script_missing_callbacks_allowed", &test_script_missing_callbacks_allowed},
    {"script_invalid_controls_fail", &test_script_invalid_controls_fail},
    {"script_invalid_dock_anchor_fail", &test_script_invalid_dock_anchor_fail},
    {"script_table_layout_unknown_control_fail", &test_script_table_layout_unknown_control_fail},
    {"script_table_layout_duplicate_control_fail", &test_script_table_layout_duplicate_control_fail},
    {"script_table_layout_missing_control_fail", &test_script_table_layout_missing_control_fail},
    {"script_table_layout_row_overflow_fail", &test_script_table_layout_row_overflow_fail},
    {"script_form_layout_unknown_control_fail", &test_script_form_layout_unknown_control_fail},
    {"script_form_layout_duplicate_control_fail", &test_script_form_layout_duplicate_control_fail},
    {"script_form_layout_missing_control_fail", &test_script_form_layout_missing_control_fail},
    {"script_runtime_error_logged", &test_script_runtime_error_logged},
    {"gui_runtime_version_utils", &test_gui_runtime_version_utils},
    {"update_check_evaluates_newer_version", &test_update_check_evaluates_newer_version},
    {"update_check_reports_up_to_date_for_exact_tag", &test_update_check_reports_up_to_date_for_exact_tag},
    {"update_check_reports_development_build", &test_update_check_reports_development_build},
    {"update_check_rejects_response_without_semantic_tags", &test_update_check_rejects_response_without_semantic_tags},
    {"protocol_directory_reload", &test_protocol_directory_reload},
    {"config_default_roundtrip", &test_config_default_roundtrip},
    {"config_wave_mode_invalid_fallback", &test_config_wave_mode_invalid_fallback},
    {"config_logging_roundtrip", &test_config_logging_roundtrip},
    {"config_default_protocol_workspace_initializes_half_duplex_demos", &test_config_default_protocol_workspace_initializes_half_duplex_demos},
    {"config_default_protocol_workspace_fills_missing_resources", &test_config_default_protocol_workspace_fills_missing_resources},
    {"script_file_io_proto_buffer_roundtrip", &test_script_file_io_proto_buffer_roundtrip},
    {"protocol_scan_and_root_roundtrip", &test_protocol_scan_and_root_roundtrip},
    {"script_plot_api_snapshot", &test_script_plot_api_snapshot},
    {"half_duplex_modbus_request_batches", &test_half_duplex_modbus_request_batches},
    {"half_duplex_modbus_ack_and_plot_flow", &test_half_duplex_modbus_ack_and_plot_flow},
    {"half_duplex_modbus_loss_status_keeps_valid_frame", &test_half_duplex_modbus_loss_status_keeps_valid_frame},
    {"half_duplex_modbus_ack_matching_rules", &test_half_duplex_modbus_ack_matching_rules},
    {"half_duplex_modbus_sticky_frames", &test_half_duplex_modbus_sticky_frames},
    {"half_duplex_modbus_noise_prefix_ignored", &test_half_duplex_modbus_noise_prefix_ignored},
    {"half_duplex_modbus_crc_resync_keeps_following_frame", &test_half_duplex_modbus_crc_resync_keeps_following_frame},
    {"half_duplex_modbus_multi_schema_candidates", &test_half_duplex_modbus_multi_schema_candidates},
    {"dock_log_and_script_split", &test_dock_log_and_script_split},
    {"dock_history_limits_trim_all_log_types", &test_dock_history_limits_trim_all_log_types},
    {"dock_receive_row_single_line_hex_and_ascii", &test_dock_receive_row_single_line_hex_and_ascii},
    {"dock_receive_row_single_line_message_and_timestamp", &test_dock_receive_row_single_line_message_and_timestamp},
    {"dock_receive_rows_text_export_format", &test_dock_receive_rows_text_export_format},
    {"dock_receive_row_visual_kind_classification", &test_dock_receive_row_visual_kind_classification},
    {"dock_send_history_deduplicates_and_trims", &test_dock_send_history_deduplicates_and_trims},
    {"log_filter_keeps_order_and_matches_status", &test_log_filter_keeps_order_and_matches_status},
    {"log_filter_keyword_matches_metadata_and_bytes", &test_log_filter_keyword_matches_metadata_and_bytes},
    {"log_filter_combines_status_and_keyword", &test_log_filter_combines_status_and_keyword},
    {"wave_protocol_state_isolated_by_protocol_key", &test_wave_protocol_state_isolated_by_protocol_key},
    {"dock_visibility_state_isolated_by_protocol_key", &test_dock_visibility_state_isolated_by_protocol_key},
    {"dock_visibility_state_decode_missing_fields_defaults", &test_dock_visibility_state_decode_missing_fields_defaults},
    {"lua_dock_layout_key_uses_protocol_and_script", &test_lua_dock_layout_key_uses_protocol_and_script},
    {"lua_dock_layout_key_falls_back_to_script_directory", &test_lua_dock_layout_key_falls_back_to_script_directory},
    {"lua_dock_layout_paths_prefer_user_layout", &test_lua_dock_layout_paths_prefer_user_layout},
    {"lua_dock_layout_paths_detect_legacy_source", &test_lua_dock_layout_paths_detect_legacy_source},
    {"lua_dock_layout_meta_path_is_sibling_yaml", &test_lua_dock_layout_meta_path_is_sibling_yaml},
    {"lua_dock_layout_meta_schema_v3_marks_modern_layout", &test_lua_dock_layout_meta_schema_v3_marks_modern_layout},
    {"lua_dock_layout_meta_schema_v2_marks_modern_layout", &test_lua_dock_layout_meta_schema_v2_marks_modern_layout},
    {"lua_dock_layout_meta_read_failure_falls_back_to_legacy", &test_lua_dock_layout_meta_read_failure_falls_back_to_legacy},
    {"lua_dock_layout_dock_id_sharing_does_not_mark_modern_legacy", &test_lua_dock_layout_dock_id_sharing_does_not_mark_modern_legacy},
    {"lua_dock_window_name_keeps_stable_id", &test_lua_dock_window_name_keeps_stable_id},
    {"lua_dock_layout_requests_group_tabs", &test_lua_dock_layout_requests_group_tabs},
    {"dock_layout_ini_requires_exactly_one_central_node", &test_dock_layout_ini_requires_exactly_one_central_node},
    {"dock_layout_ini_rebuilds_legacy_left_central_node", &test_dock_layout_ini_rebuilds_legacy_left_central_node},
    {"lua_dock_layout_requests_preserve_supported_anchors", &test_lua_dock_layout_requests_preserve_supported_anchors},
    {"lua_dock_settings_filter_keeps_current_protocol_windows", &test_lua_dock_settings_filter_keeps_current_protocol_windows},
    {"lua_dock_settings_filter_keeps_current_windows_without_active_docks", &test_lua_dock_settings_filter_keeps_current_windows_without_active_docks},
    {"lua_dock_settings_filter_keeps_same_dock_id_tab_stack", &test_lua_dock_settings_filter_keeps_same_dock_id_tab_stack},
    {"workspace_layout_mode_after_load_prefers_default_build_only_when_missing", &test_workspace_layout_mode_after_load_prefers_default_build_only_when_missing},
    {"protocol_workspace_switch_decision_uses_draft_only_until_reload", &test_protocol_workspace_switch_decision_uses_draft_only_until_reload},
    {"protocol_workspace_switch_decision_reloads_draft_when_clicked", &test_protocol_workspace_switch_decision_reloads_draft_when_clicked},
    {"protocol_switch_resets_lua_default_dock_state_only_when_changed", &test_protocol_switch_resets_lua_default_dock_state_only_when_changed},
    {"lua_default_dock_layout_runs_only_during_default_build", &test_lua_default_dock_layout_runs_only_during_default_build},
    {"protocol_workspace_layout_reset_requires_loaded_protocol", &test_protocol_workspace_layout_reset_requires_loaded_protocol},
    {"plot_cursor_snap_by_time_and_measurement", &test_plot_cursor_snap_by_time_and_measurement},
    {"wave_cursor_smart_snap_edge", &test_wave_cursor_smart_snap_edge},
    {"wave_cursor_smart_snap_extreme", &test_wave_cursor_smart_snap_extreme},
    {"wave_cursor_extreme_snap_falls_back_to_window_peak_with_transforms", &test_wave_cursor_extreme_snap_falls_back_to_window_peak_with_transforms},
    {"wave_cursor_extreme_snap_falls_back_to_window_trough", &test_wave_cursor_extreme_snap_falls_back_to_window_trough},
    {"wave_cursor_smart_snap_fallback_to_nearest", &test_wave_cursor_smart_snap_fallback_to_nearest},
    {"wave_cursor_drag_time_uses_smart_snap", &test_wave_cursor_drag_time_uses_smart_snap},
    {"tcp_transport_roundtrip", &test_tcp_transport_roundtrip},
    {"transport_enqueue_send_async_roundtrip", &test_transport_enqueue_send_async_roundtrip},
    {"tcp_server_connection_takeover_replaces_active_client", &test_tcp_server_connection_takeover_replaces_active_client},
    {"serial_transport_error_path", &test_serial_transport_error_path},
    {"udp_peer_transport_roundtrip", &test_udp_peer_transport_roundtrip},
    {"serial_port_name_normalization", &test_serial_port_name_normalization},
    {"script_dialog_requests_keep_connection_context", &test_script_dialog_requests_keep_connection_context},
    {"script_dialog_requests_detached_without_active_connection", &test_script_dialog_requests_detached_without_active_connection},
    {"application_tcp_lua_read_version_roundtrip", &test_application_tcp_lua_read_version_roundtrip},
    {"application_lua_controls_without_connection", &test_application_lua_controls_without_connection},
    {"application_tx_overflow_popup_keeps_dialog_payload", &test_application_tx_overflow_popup_keeps_dialog_payload},
    {"application_failed_protocol_reload_keeps_previous_runtime", &test_application_failed_protocol_reload_keeps_previous_runtime},
    {"application_open_transport_uses_serial_runtime_config", &test_application_open_transport_uses_serial_runtime_config},
    {"application_open_transport_uses_udp_peer_runtime_config", &test_application_open_transport_uses_udp_peer_runtime_config},
    {"application_set_log_level_updates_runtime_config", &test_application_set_log_level_updates_runtime_config},
    {"application_logging_filters_script_and_host", &test_application_logging_filters_script_and_host},
    {"application_raw_capture_export_import_roundtrip", &test_application_raw_capture_export_import_roundtrip},
    {"application_raw_capture_import_preserves_full_history", &test_application_raw_capture_import_preserves_full_history},
    {"application_raw_capture_import_replays_stream_in_chunks", &test_application_raw_capture_import_replays_stream_in_chunks},
    {"application_transfer_log_frame_view_waits_for_rx_full_frame", &test_application_transfer_log_frame_view_waits_for_rx_full_frame},
    {"application_transfer_log_frame_view_keeps_unmatched_tx_raw", &test_application_transfer_log_frame_view_keeps_unmatched_tx_raw},
    {"plot_history_trim_and_envelope", &test_plot_history_trim_and_envelope},
    {"wave_layout_solver_clamps_without_overflow", &test_wave_layout_solver_clamps_without_overflow},
    {"plot_limited_envelope_preserves_spikes", &test_plot_limited_envelope_preserves_spikes},
    {"plot_low_density_envelope_keeps_single_value_line", &test_plot_low_density_envelope_keeps_single_value_line},
    {"plot_cursor_snap_and_delta", &test_plot_cursor_snap_and_delta},
    {"plot_channel_scale_and_offset_apply_to_display_only", &test_plot_channel_scale_and_offset_apply_to_display_only},
    {"plot_channel_ratio_and_formula_modes", &test_plot_channel_ratio_and_formula_modes},
    {"plot_channel_transform_updates_are_isolated", &test_plot_channel_transform_updates_are_isolated},
    {"plot_cursor_snap_scope_selection", &test_plot_cursor_snap_scope_selection},
    {"plot_hover_readout_ignores_hidden_channels", &test_plot_hover_readout_ignores_hidden_channels},
    {"plot_limited_envelope_edges", &test_plot_limited_envelope_edges},
    {"wave_frequency_parse_and_axis_mapping", &test_wave_frequency_parse_and_axis_mapping},
    {"wave_fft_detects_50hz_and_150hz_components", &test_wave_fft_detects_50hz_and_150hz_components},
    {"wave_viewport_zoom_modes_and_clamp", &test_wave_viewport_zoom_modes_and_clamp},
    {"wave_overview_viewport_normalize", &test_wave_overview_viewport_normalize},
    {"wave_cursor_position_in_viewport", &test_wave_cursor_position_in_viewport},
    {"wave_cursor_interval_text_by_axis", &test_wave_cursor_interval_text_by_axis},
    {"wave_cursor_interval_lock", &test_wave_cursor_interval_lock},
    {"wave_channel_card_width_modes", &test_wave_channel_card_width_modes},
    {"wave_vertical_auto_fit_multiplier", &test_wave_vertical_auto_fit_multiplier},
    {"wave_visible_channel_bounds_ignore_hidden_channels", &test_wave_visible_channel_bounds_ignore_hidden_channels},
    {"wave_offset_reset_uses_protocol_default_only", &test_wave_offset_reset_uses_protocol_default_only},
    {"raw_capture_file_roundtrip", &test_raw_capture_file_roundtrip},
    {"raw_capture_file_rejects_size_mismatch", &test_raw_capture_file_rejects_size_mismatch},
    {"raw_capture_file_requires_protocol_fields", &test_raw_capture_file_requires_protocol_fields},
    {"elf_static_view_bridge_loads_dump_json_and_queries_symbols", &test_elf_static_view_bridge_loads_dump_json_and_queries_symbols},
    {"elf_static_view_bridge_keeps_old_model_on_load_failure", &test_elf_static_view_bridge_keeps_old_model_on_load_failure},
};

} // namespace

const TestCase* allTests() {
    return kAllTests;
}

int testCount() {
    return static_cast<int>(sizeof(kAllTests) / sizeof(kAllTests[0]));
}
