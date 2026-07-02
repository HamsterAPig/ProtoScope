#include "protoscope/config/config.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include "test_helpers.hpp"
#include "test_registry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using protoscope::tests::makeUniqueTempDir;
using protoscope::tests::require;
using protoscope::tests::ScopedTempPath;

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream input(path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void writeMainLua(const std::filesystem::path& protocolDir, const char* script)
{
    std::ofstream out(protocolDir / "main.lua");
    require(out.good(), "测试协议 main.lua 应可写入");
    out << script;
}

std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

protoscope::transport::ConnectionContext sampleCtx()
{
    return protoscope::transport::ConnectionContext{
        .kind = protoscope::transport::TransportKind::TcpClient,
        .endpoint = "127.0.0.1:9000",
        .connectionId = 42,
        .timestampMs = nowMs(),
    };
}

std::filesystem::path fixtureProtocolDir(const char* name)
{
    return std::filesystem::path("tests/fixtures/protocols") / name;
}

std::filesystem::path templateProtocolDir(const char* name)
{
    return std::filesystem::path("protocols/templates") / name;
}

struct ScopedCurrentPath {
    explicit ScopedCurrentPath(const std::filesystem::path& path) : original_(std::filesystem::current_path())
    {
        std::filesystem::current_path(path);
    }

    ~ScopedCurrentPath() { std::filesystem::current_path(original_); }

    std::filesystem::path original_;
};

std::uint16_t readBe16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return (static_cast<std::uint16_t>(bytes.at(offset)) << 8U) | static_cast<std::uint16_t>(bytes.at(offset + 1));
}

std::int16_t readBeI16(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::int16_t>(readBe16(bytes, offset));
}

std::vector<std::uint8_t> makeStreamFixtureFrame(std::uint8_t value)
{
    std::vector<std::uint8_t> frame{0xAA, 0x55, 0x01, value, 0x00, 0x00};
    const std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto crc = protoscope::protocol_utils::crc16Modbus(payload);
    frame[4] = static_cast<std::uint8_t>(crc & 0xFFU);
    frame[5] = static_cast<std::uint8_t>((crc >> 8U) & 0xFFU);
    return frame;
}

void rewriteModbusCrcLoHi(std::vector<std::uint8_t>& frame)
{
    require(frame.size() >= 2, "CRC 重算时帧长度不足");
    const std::vector<std::uint8_t> payload(frame.begin(), frame.end() - 2);
    const auto crc = protoscope::protocol_utils::crc16Modbus(payload);
    frame[frame.size() - 2] = static_cast<std::uint8_t>(crc & 0xFFU);
    frame[frame.size() - 1] = static_cast<std::uint8_t>((crc >> 8U) & 0xFFU);
}

void appendBytes(std::vector<std::uint8_t>& target, const std::vector<std::uint8_t>& source)
{
    target.insert(target.end(), source.begin(), source.end());
}

void requireProtocolLoaded(protoscope::scripting::ScriptHost& host, const char* directory)
{
    if (!host.loadProtocolDirectory(directory)) {
        throw std::runtime_error(std::string(directory) + " 加载失败: " + host.lastError());
    }
}

std::vector<std::uint8_t> makeSnScopeFc06Ack(std::uint16_t address, std::uint16_t value)
{
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
    rewriteModbusCrcLoHi(frame);
    return frame;
}

std::vector<std::uint8_t> makeSnScopeFc16Ack(std::uint16_t address, std::uint16_t count)
{
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
    rewriteModbusCrcLoHi(frame);
    return frame;
}

std::vector<std::uint8_t> makeSnScopeFc03Response(std::initializer_list<std::uint16_t> values)
{
    std::vector<std::uint8_t> frame{
        0xFF,
        0x03,
        static_cast<std::uint8_t>(values.size() * 2U),
    };
    for (const auto value : values) {
        frame.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
        frame.push_back(static_cast<std::uint8_t>(value & 0xFFU));
    }
    frame.push_back(0x00);
    frame.push_back(0x00);
    rewriteModbusCrcLoHi(frame);
    return frame;
}

std::vector<std::uint8_t> makeSnScopeUploadFrame(
    std::uint16_t sequence, std::int16_t ch1, std::int16_t ch2, std::int16_t ch3, std::int16_t ch4)
{
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
    rewriteModbusCrcLoHi(frame);
    return frame;
}

void completeHalfDuplexStartup(protoscope::scripting::ScriptHost& master,
                               protoscope::scripting::ScriptHost& slave,
                               const protoscope::transport::ConnectionContext& ctx)
{
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
            detail << "每次 SN Scope 请求都应返回 1 个 ACK" << " | request_size=" << request.payload.size()
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

std::vector<std::uint8_t> nextHalfDuplexWaveFrame(protoscope::scripting::ScriptHost& slave)
{
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

void test_script_controls_snapshot()
{
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

void test_script_load_directory_rejected_before_lua_dofile()
{
    const ScopedTempPath scriptDir(makeUniqueTempDir("protoscope-script-load-directory"));
    protoscope::scripting::ScriptHost host;

    require(!host.loadScriptFile(scriptDir.path().generic_string()), "目录路径不应作为 Lua 脚本加载");
    require(host.lastError().find("不是普通文件") != std::string::npos, "目录路径应在文件探测阶段给出明确错误");
}

void test_script_optional_labels_allowed_for_compact_controls()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("optional_label_controls").generic_string()),
            "紧凑控件应允许省略 label");

    const auto controls = host.controlsSnapshot();
    require(controls.size() == 4, "无 label 紧凑控件应全部保留");
    require(controls[0].type == protoscope::scripting::ControlType::Checkbox, "第一个控件应为 checkbox");
    require(controls[0].id == "enabled", "checkbox id 不应改变");
    require(controls[0].label.empty(), "checkbox 应允许空 label");
    require(controls[1].type == protoscope::scripting::ControlType::InputText, "第二个控件应为 input_text");
    require(controls[1].id == "name", "input_text id 不应改变");
    require(controls[1].label.empty(), "input_text 应允许空 label");
    require(controls[2].type == protoscope::scripting::ControlType::InputInt, "第三个控件应为 input_int");
    require(controls[2].id == "count", "input_int id 不应改变");
    require(controls[2].label.empty(), "input_int 应允许空 label");
    require(controls[3].type == protoscope::scripting::ControlType::InputFloat, "第四个控件应为 input_float");
    require(controls[3].id == "scale", "input_float id 不应改变");
    require(controls[3].label.empty(), "input_float 应允许空 label");
}

void test_script_required_labels_still_reject_visual_controls()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_required_label_controls").generic_string()),
            "按钮、下拉和 ELF 选择控件缺少 label 时应加载失败");
    require(host.lastError().find("label") != std::string::npos, "缺少必需 label 应记录明确错误");
}

void test_default_protocol_logs_elf_symbol_info()
{
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
        if (log.level == "info" && log.message.find("global.counter") != std::string::npos &&
            log.message.find("0x20000010") != std::string::npos && log.message.find("uint32_t") != std::string::npos) {
            foundInfoLog = true;
        }
    }
    require(foundInfoLog, "默认协议应把 ELF 变量信息以 info 日志输出");
}

void test_script_elf_symbol_combo_descriptor_defaults()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("elf_symbol_combo").generic_string()),
            "elf_symbol_combo 协议脚本应可加载");

    const auto controls = host.controlsSnapshot();
    require(controls.size() == 2, "elf_symbol_combo fixture 应暴露 2 个控件");
    require(controls[0].type == protoscope::scripting::ControlType::ElfSymbolCombo, "应解析 elf_symbol_combo 控件类型");
    require(controls[0].debounceMs == 150, "debounce_ms 默认值应为 150");
    require(controls[0].limit == 64, "limit 默认值应为 64");
    require(!controls[0].debounceMsConfigured, "未配置 debounce_ms 时应标记为使用全局默认");
    require(!controls[0].limitConfigured, "未配置 limit 时应标记为使用全局默认");
    require(controls[1].debounceMs == 25, "应解析自定义 debounce_ms");
    require(controls[1].limit == 3, "应解析自定义 limit");
    require(controls[1].debounceMsConfigured, "自定义 debounce_ms 应标记为 Lua 覆盖");
    require(controls[1].limitConfigured, "自定义 limit 应标记为 Lua 覆盖");
}

void test_script_elf_symbol_combo_invalid_config_fails()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_elf_symbol_combo").generic_string()),
            "非法 elf_symbol_combo 配置应加载失败");
    require(host.lastError().find("elf_symbol_combo") != std::string::npos, "非法 elf_symbol_combo 应记录明确错误");
}

void test_script_elf_symbol_combo_get_control_returns_table()
{
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
    require(events[0].payload.find("label=global.counter") != std::string::npos, "proto.get_control 应返回 label 字段");
    require(events[0].payload.find("value=0x20000010") != std::string::npos, "proto.get_control 应返回 value 字段");
    require(events[0].payload.find("type=uint32_t") != std::string::npos, "proto.get_control 应返回 type 字段");
}

void test_script_on_open_log()
{
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

void test_script_on_close_log()
{
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

void test_script_on_error_log()
{
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

void test_script_proto_log_trace_level()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-script-log-trace"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_open(ctx)
  proto.log("trace", "trace detail")
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "trace 日志协议应可加载");
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});

    bool foundTraceLog = false;
    for (const auto& log : host.drainLogs()) {
        foundTraceLog = foundTraceLog || (log.level == "trace" && log.message == "trace detail");
    }
    require(foundTraceLog, "proto.log(\"trace\") 应保留 trace 等级");
}

void test_script_multi_dock_snapshot()
{
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

void test_script_dock_layout_fields()
{
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

void test_script_table_layout_snapshot()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("table_layout").generic_string()),
            "table_layout 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "table_layout 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "table_layout 应解析 layout");

    const auto& table = docks[0].descriptor.layout->root;
    require(table.kind == protoscope::scripting::LayoutNodeKind::Table, "layout root 应为 table");
    require(table.columns == 2, "table_layout 列数应为 2");
    require(table.rows.size() == 3, "table_layout 应解析三行");
    require(table.rows[0].size() == 2, "第一行应有两列");
    require(table.rows[0][0].kind == protoscope::scripting::LayoutNodeKind::Control, "第一行第一列应为 control");
    require(table.rows[0][0].controlId == "device_id", "第一行第一列应绑定 device_id");
    require(table.rows[0][0].controlIndex == 0, "第一行第一列应固化 controlIndex");
    require(table.rows[1][1].kind == protoscope::scripting::LayoutNodeKind::Spacer, "第二行第二列应为 spacer");
    require(table.rows[2][0].controlId == "read_version", "第三行第一列应绑定 read_version");
}

void test_script_ui_descriptor_sugar_snapshot()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-ui-descriptor-sugar"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function ui()
  return {
    id = "tools",
    title = "协议工具",
    controls = {
      { "btn", "send_once", "发送一次" },
      { "text", "device_id", "设备 ID", default = "01" },
      { "int", "count", "次数", default = 3 },
      { "float", "scale", "比例", default = 1.5 },
      { "check", "hex_send", "HEX", label_position = "right", default = true },
      { "select", "mode", "模式", options = { "轮询", "单次" }, default = 1 },
      { "symbol", "target", "ELF 变量" },
      {
        "values",
        "status",
        "状态",
        rows = {
          { 0x1010, "电压", "V", note = "母线电压" },
        },
      },
    },
    layout = {
      { "device_id", "hex_send", "send_once" },
      "count",
      { id = "scale", min_width = 120 },
      { text = "说明文字" },
      { separator = true },
      { spacer = true },
      { "mode", "target", "status" },
    },
  }
end
)lua");

    protoscope::scripting::ScriptHost host;
    if (!host.loadProtocolDirectory(protocolDir.path().generic_string())) {
        throw std::runtime_error("语法糖协议应可加载: " + host.lastError());
    }

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "单 dock 返回糖应产出一个 dock");
    const auto& dock = docks[0].descriptor;
    require(dock.id == "tools" && dock.title == "协议工具", "单 dock 字段应正确解析");
    require(dock.controls.size() == 8, "语法糖控件数量不正确");
    require(dock.controls[0].type == protoscope::scripting::ControlType::Button, "btn 应映射为 button");
    require(dock.controls[1].type == protoscope::scripting::ControlType::InputText, "text 应映射为 input_text");
    require(dock.controls[2].type == protoscope::scripting::ControlType::InputInt, "int 应映射为 input_int");
    require(dock.controls[3].type == protoscope::scripting::ControlType::InputFloat, "float 应映射为 input_float");
    require(dock.controls[4].type == protoscope::scripting::ControlType::Checkbox, "check 应映射为 checkbox");
    require(dock.controls[5].type == protoscope::scripting::ControlType::Combo, "select 应映射为 combo");
    require(dock.controls[6].type == protoscope::scripting::ControlType::ElfSymbolCombo,
            "symbol 应映射为 elf_symbol_combo");
    require(dock.controls[7].type == protoscope::scripting::ControlType::ValueTable, "values 应映射为 value_table");
    require(dock.controls[1].id == "device_id" && dock.controls[1].label == "设备 ID", "控件位置参数应展开 id/label");
    require(dock.controls[1].textDefault == "01", "input_text default 应保持可用");
    require(dock.controls[4].boolDefault, "checkbox default 应保持可用");
    require(dock.controls[4].labelPosition == protoscope::scripting::ControlLabelPosition::Right,
            "checkbox label_position 应保持可用");
    require(dock.controls[7].valueRows.size() == 1, "value_table 普通行位置参数应展开一行");
    require(dock.controls[7].valueRows[0].id == 0x1010U, "value_table 行 id 应来自第 1 位置参数");
    require(dock.controls[7].valueRows[0].label == "电压", "value_table 行 label 应来自第 2 位置参数");
    require(dock.controls[7].valueRows[0].unit == "V", "value_table 行 unit 应来自第 3 位置参数");
    require(dock.controls[7].valueRows[0].note == "母线电压", "value_table 行 note 应保持命名字段");

    require(dock.layout.has_value(), "语法糖 layout 应生成显式布局描述");
    const auto& root = dock.layout->root;
    require(root.kind == protoscope::scripting::LayoutNodeKind::Column, "无 type layout root 应默认 column");
    require(root.children.size() == 7, "root column 应包含七个子节点");
    require(root.children[0].kind == protoscope::scripting::LayoutNodeKind::Flow, "字符串数组应展开为 flow");
    require(root.children[0].children.size() == 3, "flow 应包含三个 control 子节点");
    require(root.children[0].children[0].controlId == "device_id", "flow 第一个 control id 不正确");
    require(root.children[1].kind == protoscope::scripting::LayoutNodeKind::Control &&
                root.children[1].controlId == "count",
            "字符串 layout 子项应展开为 control");
    require(root.children[2].kind == protoscope::scripting::LayoutNodeKind::Control &&
                root.children[2].controlId == "scale" && root.children[2].minWidth == 120.0F,
            "id + min_width 糖应展开为 control 节点");
    require(root.children[3].kind == protoscope::scripting::LayoutNodeKind::Text && root.children[3].text == "说明文字",
            "text 糖应展开为 text 节点");
    require(root.children[4].kind == protoscope::scripting::LayoutNodeKind::Separator,
            "separator 糖应展开为 separator 节点");
    require(root.children[5].kind == protoscope::scripting::LayoutNodeKind::Spacer, "spacer 糖应展开为 spacer 节点");
}

void test_script_ui_descriptor_sugar_layout_validation_failures()
{
    {
        const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-ui-sugar-unknown"));
        writeMainLua(protocolDir.path(),
                     R"lua(
function ui()
  return {
    id = "tools",
    title = "Tools",
    controls = {
      { "text", "device_id", "Device" },
    },
    layout = {
      "missing_control",
    },
  }
end
)lua");
        protoscope::scripting::ScriptHost host;
        require(!host.loadProtocolDirectory(protocolDir.path().generic_string()),
                "语法糖 layout 引用未知控件应加载失败");
        require(host.lastError().find("未声明控件") != std::string::npos, "未知控件错误应复用 layout 校验");
    }

    {
        const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-ui-sugar-duplicate"));
        writeMainLua(protocolDir.path(),
                     R"lua(
function ui()
  return {
    id = "tools",
    title = "Tools",
    controls = {
      { "text", "device_id", "Device" },
      { "btn", "send_once", "Send" },
    },
    layout = {
      { "device_id", "device_id", "send_once" },
    },
  }
end
)lua");
        protoscope::scripting::ScriptHost host;
        require(!host.loadProtocolDirectory(protocolDir.path().generic_string()),
                "语法糖 layout 重复引用控件应加载失败");
        require(host.lastError().find("重复引用控件") != std::string::npos, "重复引用错误应复用 layout 校验");
    }

    {
        const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-ui-sugar-missing"));
        writeMainLua(protocolDir.path(),
                     R"lua(
function ui()
  return {
    id = "tools",
    title = "Tools",
    controls = {
      { "text", "device_id", "Device" },
      { "btn", "send_once", "Send" },
    },
    layout = {
      "device_id",
    },
  }
end
)lua");
        protoscope::scripting::ScriptHost host;
        require(!host.loadProtocolDirectory(protocolDir.path().generic_string()), "语法糖 layout 漏布局控件应加载失败");
        require(host.lastError().find("缺少控件") != std::string::npos, "漏布局错误应复用 layout 校验");
    }
}

void test_script_form_layout_snapshot()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("form_layout").generic_string()), "form_layout 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "form_layout 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "form_layout 应解析 layout");

    const auto& root = docks[0].descriptor.layout->root;
    require(root.kind == protoscope::scripting::LayoutNodeKind::Column, "form_layout 根节点应为 column");
    const auto& items = root.children;
    require(items.size() == 5, "form_layout 应解析 5 个顶层布局项");
    require(items[0].kind == protoscope::scripting::LayoutNodeKind::Text, "第一项应为 text");
    require(items[1].kind == protoscope::scripting::LayoutNodeKind::Flow, "第二项应为 flow");
    require(items[1].children.size() == 2, "flow 应包含两个控件");
    require(items[1].children[0].controlId == "read_version", "flow 第一个控件顺序错误");
    require(items[1].children[1].controlId == "device_id", "flow 第二个控件顺序错误");
    require(items[2].kind == protoscope::scripting::LayoutNodeKind::Separator, "第三项应为 separator");
    require(items[3].kind == protoscope::scripting::LayoutNodeKind::Group, "第四项应为 group");
    require(items[3].children.size() == 1, "group 内应有 1 个子项");
    require(items[3].children[0].kind == protoscope::scripting::LayoutNodeKind::Flow, "group 内应嵌套 flow");
    require(items[3].children[0].children[0].controlId == "hex_send", "group 内第一个控件顺序错误");
    require(items[3].children[0].children[1].controlId == "mode", "group 内第二个控件顺序错误");
    require(items[4].kind == protoscope::scripting::LayoutNodeKind::Collapse, "第五项应为 collapse");
    require(!items[4].defaultOpen, "collapse 默认展开状态解析错误");
    require(items[4].children[0].kind == protoscope::scripting::LayoutNodeKind::Flow, "collapse 内应嵌套 flow");
    require(items[4].children[0].children[0].controlId == "timeout_ms", "collapse 内第一个控件顺序错误");
    require(items[4].children[0].children[1].controlId == "scale", "collapse 内第二个控件顺序错误");
}

void test_script_flow_layout_snapshot()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("flow_layout").generic_string()), "flow_layout 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "flow_layout 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "flow_layout 应解析 layout");

    const auto& root = docks[0].descriptor.layout->root;
    require(root.kind == protoscope::scripting::LayoutNodeKind::Column, "flow_layout 根节点应为 column");
    require(root.children.size() == 3, "flow_layout 应包含 flow、collapse、table 三个节点");
    require(root.children[0].kind == protoscope::scripting::LayoutNodeKind::Flow, "第一项应为 flow");
    require(root.children[0].spacing == 6.0F, "flow spacing 应解析");
    require(root.children[0].runSpacing == 5.0F, "flow run_spacing 应解析");
    require(root.children[1].kind == protoscope::scripting::LayoutNodeKind::Collapse, "第二项应为 collapse");
    require(!root.children[1].defaultOpen, "collapse default_open 应解析");
    require(root.children[1].children[0].kind == protoscope::scripting::LayoutNodeKind::Flow,
            "collapse 内应允许嵌入 flow");
    require(root.children[2].kind == protoscope::scripting::LayoutNodeKind::Table, "第三项应为 table");
    require(root.children[2].rows[0][1].kind == protoscope::scripting::LayoutNodeKind::Flow,
            "table cell 内应允许嵌入 flow");

    const auto controls = host.controlsSnapshot();
    require(controls[1].labelPosition == protoscope::scripting::ControlLabelPosition::Left,
            "label_position 缺省应为 left");
    require(controls[2].labelPosition == protoscope::scripting::ControlLabelPosition::Right,
            "label_position 显式 right 应解析");
}

void test_script_layout_width_controls_shorthand_snapshot()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("layout_width_controls_shorthand").generic_string()),
            "layout_width_controls_shorthand 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "layout_width_controls_shorthand 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "layout_width_controls_shorthand 应解析 layout");

    const auto& root = docks[0].descriptor.layout->root;
    require(root.kind == protoscope::scripting::LayoutNodeKind::Column, "根节点应为 column");
    require(root.children.size() == 3, "根节点应包含 flow、column 和 control 三项");

    const auto& flow = root.children[0];
    require(flow.kind == protoscope::scripting::LayoutNodeKind::Flow, "第一项应为 flow");
    require(flow.children.size() == 2, "flow.controls 应展开为两个 control 子节点");
    require(flow.children[0].controlId == "read_version", "flow.controls 第一个控件顺序错误");
    require(flow.children[1].controlId == "device_id", "flow.controls 第二个控件顺序错误");

    const auto& column = root.children[1];
    require(column.kind == protoscope::scripting::LayoutNodeKind::Column, "第二项应为 column");
    require(column.children.size() == 2, "column.controls 应展开为两个 control 子节点");
    require(column.children[0].controlId == "timeout_ms", "column.controls 第一个控件顺序错误");
    require(column.children[1].controlId == "scale", "column.controls 第二个控件顺序错误");

    const auto& constrained = root.children[2];
    require(constrained.kind == protoscope::scripting::LayoutNodeKind::Control, "第三项应为 control");
    require(constrained.controlId == "mode", "第三项应绑定 mode");
    require(constrained.minWidth.has_value() && *constrained.minWidth == 120.0F, "control min_width 应解析");
    require(constrained.maxWidth.has_value() && *constrained.maxWidth == 240.0F, "control max_width 应解析");
    require(constrained.fillWidth, "control fill_width 应解析");
}

void test_script_inline_group_layout_snapshot()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("inline_group_layout").generic_string()),
            "inline_group_layout 协议应可加载");

    const auto controls = host.controlsSnapshot();
    require(controls.size() == 4, "inline_group_layout 应解析 4 个控件");
    require(controls[0].shortLabel == "读", "button short_label 应解析");
    require(controls[0].compactLabelBelow.has_value() && *controls[0].compactLabelBelow == 80.0F,
            "button compact_label_below 应解析");
    require(controls[1].shortLabel == "ID", "input_text short_label 应解析");
    require(controls[1].compactLabelBelow.has_value() && *controls[1].compactLabelBelow == 180.0F,
            "input_text compact_label_below 应解析");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 1, "inline_group_layout 协议应只产出一个 dock");
    require(docks[0].descriptor.layout.has_value(), "inline_group_layout 应解析 layout");

    const auto& flow = docks[0].descriptor.layout->root;
    require(flow.kind == protoscope::scripting::LayoutNodeKind::Flow, "根节点应为 flow");
    require(flow.spacing == 4.0F, "flow spacing 应解析");
    require(flow.runSpacing == 7.0F, "flow run_spacing 应解析");
    require(flow.children.size() == 3, "flow 应包含两个 inline_group 和一个 control");

    const auto& shortcutGroup = flow.children[0];
    require(shortcutGroup.kind == protoscope::scripting::LayoutNodeKind::InlineGroup, "第一项应为 inline_group");
    require(shortcutGroup.spacing == 3.0F, "inline_group spacing 应解析");
    require(shortcutGroup.minWidth.has_value() && *shortcutGroup.minWidth == 200.0F, "inline_group min_width 应解析");
    require(!shortcutGroup.maxWidth.has_value(), "inline_group 不应解析 max_width");
    require(shortcutGroup.fillWidth, "inline_group fill_width 应解析");
    require(shortcutGroup.children.size() == 2, "inline_group.controls 应展开两个 control 子节点");
    require(shortcutGroup.children[0].controlId == "read_version", "inline_group.controls 第一个控件顺序错误");
    require(shortcutGroup.children[1].controlId == "device_id", "inline_group.controls 第二个控件顺序错误");

    const auto& mixedGroup = flow.children[1];
    require(mixedGroup.kind == protoscope::scripting::LayoutNodeKind::InlineGroup, "第二项应为 inline_group");
    require(mixedGroup.children.size() == 2, "inline_group.children 应解析 text 与 control");
    require(mixedGroup.children[0].kind == protoscope::scripting::LayoutNodeKind::Text,
            "inline_group 第一个子项应为 text");
    require(mixedGroup.children[0].text == "发送选项", "inline_group text 内容应保留");
    require(mixedGroup.children[1].controlId == "hex_send", "inline_group 第二个子项应绑定 hex_send");
    require(flow.children[2].controlId == "timeout_ms", "flow 第三个子项应绑定 timeout_ms");
}

void test_script_duplicate_label_controls_allowed()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("duplicate_label_controls").generic_string()),
            "duplicate_label_controls 协议应可加载");

    const auto docks = host.dockSnapshots();
    require(docks.size() == 2, "重复 label 夹具应产出 form 与 flow 两个 dock");

    const auto& formDock = docks[0];
    require(formDock.descriptor.layout.has_value(), "重复 label form 应解析 layout");
    require(formDock.descriptor.layout->root.kind == protoscope::scripting::LayoutNodeKind::Flow,
            "重复 label 夹具首个 dock 应为 flow layout");
    require(formDock.controls.size() == 2, "重复 label form 应保留两个控件");
    require(formDock.controls[0].descriptor.id == "src_addr", "第一个重复 label 控件 id 不应改变");
    require(formDock.controls[1].descriptor.id == "dst_addr", "第二个重复 label 控件 id 不应改变");
    require(formDock.controls[0].descriptor.label == "地址", "第一个重复 label 控件 label 不应改变");
    require(formDock.controls[1].descriptor.label == "地址", "第二个重复 label 控件 label 不应改变");

    const auto& flowDock = docks[1];
    require(!flowDock.descriptor.layout.has_value(), "重复 label flow 不应声明 layout");
    require(flowDock.controls.size() == 2, "重复 label flow 应保留两个控件");
    require(flowDock.controls[0].descriptor.id == "read_src", "第一个重复 label flow 控件 id 不应改变");
    require(flowDock.controls[1].descriptor.id == "read_dst", "第二个重复 label flow 控件 id 不应改变");
    require(flowDock.controls[0].descriptor.label == "读取", "第一个重复 label flow 控件 label 不应改变");
    require(flowDock.controls[1].descriptor.label == "读取", "第二个重复 label flow 控件 label 不应改变");
}

void test_script_crc_bridge()
{
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

void test_script_read_version_flow()
{
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

void test_script_read_version_split_flow()
{
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

void test_script_stream_schema_legacy_on_bytes_still_works()
{
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

void test_script_stream_schema_bypasses_on_bytes_and_calls_on_frame()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("stream_frame_only").generic_string()),
            "stream_frame_only 协议应可加载");

    const auto ctx = sampleCtx();
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, makeStreamFixtureFrame(0x23)});

    bool foundFrame = false;
    bool foundLegacy = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "legacy_bytes") {
            foundLegacy = true;
        }
        if (event.name == "stream_frame" && event.payload.find("stream_sample") != std::string::npos &&
            event.payload.find("value=35") != std::string::npos &&
            event.payload.find("top_value=35") != std::string::npos &&
            event.payload.find("field_values_match=true") != std::string::npos &&
            event.payload.find("crc_ok=true") != std::string::npos) {
            foundFrame = true;
        }
    }
    require(foundFrame, "启用 stream() 后应回调 on_frame");
    require(!foundLegacy, "启用 stream() 后不应继续把每批 bytes 传给 on_bytes");
}

void test_script_stream_schema_prefers_on_batch_over_on_frame()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("stream_batch_only").generic_string()),
            "stream_batch_only 协议应可加载");

    const auto ctx = sampleCtx();
    auto payload = makeStreamFixtureFrame(0x11);
    appendBytes(payload, makeStreamFixtureFrame(0x22));
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, payload});

    bool foundBatch = false;
    bool foundFrame = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "stream_batch" && event.payload.find("count=2") != std::string::npos &&
            event.payload.find("first=17") != std::string::npos &&
            event.payload.find("second=34") != std::string::npos) {
            foundBatch = true;
        }
        if (event.name == "stream_frame") {
            foundFrame = true;
        }
    }
    require(foundBatch, "定义 stream.on_batch 后应批量回调完整帧");
    require(!foundFrame, "stream.on_batch 已定义时不应重复调用 frame.on_frame");
}

void test_script_stream_schema_allows_on_batch_without_on_frame()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("stream_batch_without_on_frame").generic_string()),
            "stream_batch_without_on_frame 协议应可加载");

    const auto ctx = sampleCtx();
    auto payload = makeStreamFixtureFrame(0x01);
    appendBytes(payload, makeStreamFixtureFrame(0x02));
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, payload});

    bool foundBatch = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "stream_batch_only" && event.payload.find("count=2") != std::string::npos &&
            event.payload.find("total=3") != std::string::npos) {
            foundBatch = true;
        }
    }
    require(foundBatch, "仅定义 stream.on_batch 时也应处理完整帧批量");
}

void test_script_stream_schema_reports_overflow_and_crc_error()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("stream_frame_only").generic_string()),
            "stream_frame_only 协议应可加载");

    const auto ctx = sampleCtx();
    host.onTransportBytes(
        protoscope::transport::TransportBytesEvent{ctx, {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18}});

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

void test_script_stream_runtime_profile_set_and_clear()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("runtime_profile_stream").generic_string()),
            "runtime_profile_stream 协议应可加载");

    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table);
    auto setTable = lua.create_table();
    setTable["frame"] = "dynamic_profile";
    setTable["length"] = 8;
    auto map = lua.create_table();
    map[1] = 2;
    map[2] = 1;
    setTable["channel_map"] = map;

    std::string error;
    require(host.setStreamRuntimeProfile(sol::make_object(lua, setTable), error), "set_profile 应成功");
    const auto profileEvents = host.drainStreamRuntimeProfileEvents();
    require(profileEvents.size() == 1 && !profileEvents.front().cleared, "set_profile 应产生 profile_set 事件");

    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});
    std::vector<std::uint8_t> raw{0xFF, 0x26, 0x00, 0x11, 0x00, 0x22};
    const auto crc = protoscope::protocol_utils::crc16Modbus(raw);
    raw.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    raw.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{sampleCtx(), raw});

    bool foundFrame = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "runtime_profile_frame") {
            foundFrame = true;
        }
    }
    require(foundFrame, "set_profile 后应持续按 runtime profile 解析");

    require(host.clearStreamRuntimeProfile(sol::make_object(lua, std::string("dynamic_profile")), error),
            "clear_profile(frame) 应成功");
    const auto clearOneEvents = host.drainStreamRuntimeProfileEvents();
    require(clearOneEvents.size() == 1 && clearOneEvents.front().cleared, "clear_profile(frame) 应产生 clear 事件");

    require(host.clearStreamRuntimeProfile(sol::make_object(lua, sol::lua_nil), error), "clear_profile() 应成功");
    const auto clearAllEvents = host.drainStreamRuntimeProfileEvents();
    require(clearAllEvents.size() == 1 && clearAllEvents.front().cleared, "clear_profile() 应产生 clear-all 事件");
}

void test_script_stream_raw_output_omit_skips_frame_raw()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-stream-raw-omit"));
    {
        std::ofstream script(protocolDir.path() / "main.lua");
        script << R"lua(
local function on_stream_frame(ctx, frame)
    proto.emit("stream_raw_mode", {
        raw_present = frame.raw ~= nil,
        value = frame.fields.value,
    })
end

function stream()
    return {
        raw_output = "omit",
        buffer = {
            capacity = 16,
            overflow = "drop_oldest",
        },
        frames = {
            {
                name = "stream_sample",
                header = { 0xAA, 0x55 },
                len = { offset = 3, type = "u8", means = "payload", extra = 5 },
                crc = { type = "crc16_modbus", order = "lo_hi" },
                fields = {
                    { name = "value", type = "u8", offset = 4 },
                },
                on_frame = on_stream_frame,
            },
        },
    }
end
)lua";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "raw_output omit 协议应可加载");
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{sampleCtx(), makeStreamFixtureFrame(0x34)});

    bool found = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "stream_raw_mode") {
            found = true;
            require(event.payload.find("raw_present=false") != std::string::npos, "raw_output=omit 不应生成 frame.raw");
            require(event.payload.find("value=52") != std::string::npos, "raw_output=omit 不应影响字段解析");
        }
    }
    require(found, "raw_output=omit 应触发 stream frame 回调");
}

void test_script_stream_low_overhead_keeps_fields_and_trims_last_batch()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-stream-low-overhead"));
    {
        std::ofstream script(protocolDir.path() / "main.lua");
        script << R"lua(
local function on_stream_frame(ctx, frame)
    proto.emit("stream_low_overhead", {
        raw_present = frame.raw ~= nil,
        value = frame.fields.value,
        top_value = frame.value,
    })
end

function stream()
    return {
        raw_output = "omit",
        low_overhead = true,
        buffer = {
            capacity = 16,
            overflow = "drop_oldest",
        },
        frames = {
            {
                name = "stream_sample",
                header = { 0xAA, 0x55 },
                len = { offset = 3, type = "u8", means = "payload", extra = 5 },
                crc = { type = "crc16_modbus", order = "lo_hi" },
                fields = {
                    { name = "value", type = "u8", offset = 4 },
                },
                on_frame = on_stream_frame,
            },
        },
    }
end
)lua";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "low_overhead 协议应可加载");
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{sampleCtx(), makeStreamFixtureFrame(0x35)});

    bool found = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "stream_low_overhead") {
            found = true;
            require(event.payload.find("raw_present=false") != std::string::npos, "low_overhead 不应向 Lua 暴露 raw");
            require(event.payload.find("value=53") != std::string::npos, "low_overhead 不应影响 fields 读取");
            require(event.payload.find("top_value=53") != std::string::npos, "默认兼容模式仍应保留顶层字段别名");
        }
    }
    require(found, "low_overhead 应触发 stream frame 回调");

    const auto lastBatch = host.lastStreamParseBatch();
    require(lastBatch.has_value(), "low_overhead 后应保留解析摘要");
    require(lastBatch->frames.empty(), "low_overhead lastBatch 不应保留成功帧字段和 raw");
    require(lastBatch->errors.empty(), "成功解析不应产生错误");
    require(lastBatch->bufferSize == 0, "完整帧解析后缓冲区应清空");
}

void test_script_stream_field_output_fields_only_omits_top_aliases()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-stream-fields-only"));
    {
        std::ofstream script(protocolDir.path() / "main.lua");
        script << R"lua(
local function on_stream_frame(ctx, frame)
    proto.emit("stream_fields_only", {
        field_value = frame.fields.value,
        top_present = frame.value ~= nil,
    })
end

function stream()
    return {
        field_output = "fields_only",
        buffer = {
            capacity = 16,
            overflow = "drop_oldest",
        },
        frames = {
            {
                name = "stream_sample",
                header = { 0xAA, 0x55 },
                len = { offset = 3, type = "u8", means = "payload", extra = 5 },
                crc = { type = "crc16_modbus", order = "lo_hi" },
                fields = {
                    { name = "value", type = "u8", offset = 4 },
                },
                on_frame = on_stream_frame,
            },
        },
    }
end
)lua";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "fields_only 协议应可加载");
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{sampleCtx(), makeStreamFixtureFrame(0x36)});

    bool found = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "stream_fields_only") {
            found = true;
            require(event.payload.find("field_value=54") != std::string::npos, "fields_only 应保留 fields 字段");
            require(event.payload.find("top_present=false") != std::string::npos, "fields_only 不应生成顶层字段别名");
        }
    }
    require(found, "fields_only 应触发 stream frame 回调");
}

void test_script_stream_schema_reload_uses_current_callbacks()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-stream-reload-callbacks"));
    const auto writeProtocol = [&](const std::string& version) {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "stream reload 测试协议应可写入");
        out << "local version = \"" << version << "\"\n";
        out << "function controls() return {} end\n";
        out << "local function on_stream_frame(ctx, frame)\n";
        out << "  proto.emit(\"stream_frame_\" .. version, tostring(frame.fields.value))\n";
        out << "end\n";
        out << "local function on_stream_error(ctx, err)\n";
        out << "  proto.emit(\"stream_error_\" .. version, err.code)\n";
        out << "end\n";
        out << "function stream()\n";
        out << "  return {\n";
        out << "    buffer = { capacity = 8, overflow = \"drop_oldest\" },\n";
        out << "    frames = { {\n";
        out << "      name = \"stream_sample\",\n";
        out << "      header = { 0xAA, 0x55 },\n";
        out << "      len = { offset = 3, type = \"u8\", means = \"payload\", extra = 5 },\n";
        out << "      crc = { type = \"crc16_modbus\", order = \"lo_hi\" },\n";
        out << "      fields = { { name = \"value\", type = \"u8\", offset = 4 } },\n";
        out << "      on_frame = on_stream_frame,\n";
        out << "    } },\n";
        out << "    on_error = on_stream_error,\n";
        out << "  }\n";
        out << "end\n";
        out << "function on_bytes(ctx, bytes)\n";
        out << "  proto.emit(\"legacy_bytes\", tostring(#bytes))\n";
        out << "end\n";
    };
    const auto writeBrokenProtocol = [&]() {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "stream reload 失败协议应可写入");
        out << "function controls() return {} end\n";
        out << "function stream()\n";
        out << "  return {\n";
    };
    const auto hasEvent = [](const std::vector<protoscope::scripting::ScriptEvent>& events, const std::string& name) {
        return std::any_of(events.begin(), events.end(), [&](const auto& event) { return event.name == name; });
    };

    protoscope::scripting::ScriptHost host;
    const auto ctx = sampleCtx();

    writeProtocol("A");
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "版本 A stream 协议应可加载");
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, makeStreamFixtureFrame(0x11)});
    auto events = host.drainEvents();
    require(hasEvent(events, "stream_frame_A"), "版本 A 应触发 A 的 on_frame");
    require(!hasEvent(events, "stream_frame_B"), "版本 A 不应触发 B 的 on_frame");

    writeProtocol("B");
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "版本 B stream 协议应可加载");
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, makeStreamFixtureFrame(0x22)});
    events = host.drainEvents();
    require(!hasEvent(events, "stream_frame_A"), "成功 reload 后不应继续触发 A 的 on_frame");
    require(hasEvent(events, "stream_frame_B"), "成功 reload 后应触发 B 的 on_frame");

    writeBrokenProtocol();
    require(!host.loadProtocolDirectory(protocolDir.path().generic_string()), "损坏 stream 协议应加载失败");
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, makeStreamFixtureFrame(0x33)});
    events = host.drainEvents();
    require(!hasEvent(events, "stream_frame_A"), "失败 reload 后不应回退到 A");
    require(hasEvent(events, "stream_frame_B"), "失败 reload 后应保留 B 的 on_frame");

    writeProtocol("C");
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "版本 C stream 协议应可加载");
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, makeStreamFixtureFrame(0x44)});
    events = host.drainEvents();
    require(!hasEvent(events, "stream_frame_B"), "再次成功 reload 后不应继续触发 B 的 on_frame");
    require(hasEvent(events, "stream_frame_C"), "再次成功 reload 后应触发 C 的 on_frame");

    host.onTransportBytes(
        protoscope::transport::TransportBytesEvent{ctx, {0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18}});
    auto broken = makeStreamFixtureFrame(0x55);
    broken[4] = static_cast<std::uint8_t>(broken[4] ^ 0x01U);
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, broken});
    events = host.drainEvents();
    require(!hasEvent(events, "stream_error_A"), "错误回调不应命中 A runtime");
    require(!hasEvent(events, "stream_error_B"), "错误回调不应命中 B runtime");
    require(hasEvent(events, "stream_error_C"), "错误回调应只命中当前 C runtime");
}

void test_script_stream_schema_rejects_count_function()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-count-function"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "count=function 测试协议应可写入");
        out << "function controls()\n";
        out << "  return {}\n";
        out << "end\n";
        out << "function stream()\n";
        out << "  return { frames = { { name = \"bad\", header = { 0xAA }, size = 2, fields = {\n";
        out << "    { name = \"values\", type = \"u8\", offset = 2, count = function() return 1 end },\n";
        out << "  }, on_frame = function() end } } }\n";
        out << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(protocolDir.path().generic_string()), "count=function 应加载失败");
    require(host.lastError().find("stream.frames[1].fields[1].count") != std::string::npos,
            "错误应包含精确 schema 路径");
    require(host.lastError().find("不再支持 function") != std::string::npos, "错误应明确旧 function 写法已废弃");
    require(host.lastError().find("count = { op = \"div\", field = \"byte_count\", by = 2 }") != std::string::npos,
            "错误应给出 count 表达式迁移示例");
}

void test_script_stream_schema_accepts_count_expression_table()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-count-expression"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "count 表达式测试协议应可写入");
        out << "function controls()\n";
        out << "  return {}\n";
        out << "end\n";
        out << "local function on_frame(ctx, frame)\n";
        out << "  proto.emit(\"count_expression\", { count = #frame.fields.values, first = frame.fields.values[1] })\n";
        out << "end\n";
        out << "function stream()\n";
        out << "  return { frames = { { name = \"expr\", header = { 0xAA, 0x55 }, len = { offset = 3, type = \"u8\", "
               "means = \"payload\", extra = 5 }, crc = { type = \"crc16_modbus\", order = \"lo_hi\" }, fields = {\n";
        out << "    { name = \"byte_count\", type = \"u8\", offset = 4 },\n";
        out << "    { name = \"values\", type = \"u16_be\", offset = 5, count = { op = \"div\", field = "
               "\"byte_count\", by = 2 } },\n";
        out << "  }, on_frame = on_frame } } }\n";
        out << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "count 表达式 table 应可加载");
    auto frame = std::vector<std::uint8_t>{0xAA, 0x55, 0x05, 0x04, 0x00, 0x11, 0x00, 0x22};
    const auto crc = protoscope::protocol_utils::crc16Modbus(frame);
    frame.push_back(static_cast<std::uint8_t>(crc & 0xFFU));
    frame.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xFFU));
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{sampleCtx(), frame});

    bool found = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "count_expression" && event.payload.find("count=2") != std::string::npos &&
            event.payload.find("first=17") != std::string::npos) {
            found = true;
        }
    }
    require(found, "count 表达式 table 应正确解析动态字段数量");
}

void test_script_timeout_flow()
{
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

void test_script_dialog_requests_keep_connection_context()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("dialog_requests").generic_string()),
            "dialog_requests 协议应可加载");

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
    require(dialog.window.resizable, "默认弹窗应允许缩放");
    require(dialog.window.movable, "默认弹窗应允许移动");
    require(!dialog.window.autoResize, "默认弹窗不应强制自动尺寸");
}

void test_script_dialog_requests_detached_without_active_connection()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("dialog_requests").generic_string()),
            "dialog_requests 协议应可加载");

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

void test_script_dialog_requests_parse_window_options()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("dialog_window_options").generic_string()),
            "dialog_window_options 协议应可加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});

    const auto dialogs = host.drainDialogRequests();
    require(dialogs.size() == 1, "on_open 应生成一条带窗口选项的 alert 请求");
    const auto& dialog = dialogs.front();
    require(dialog.window.width.has_value() && *dialog.window.width == 520.0, "window.width 应按 Lua 配置解析");
    require(dialog.window.height.has_value() && *dialog.window.height == 260.0, "window.height 应按 Lua 配置解析");
    require(dialog.window.x.has_value() && *dialog.window.x == 120.0, "window.x 应按 Lua 配置解析");
    require(dialog.window.y.has_value() && *dialog.window.y == 80.0, "window.y 应按 Lua 配置解析");
    require(!dialog.window.resizable, "window.resizable 应按 Lua 配置解析");
    require(!dialog.window.movable, "window.movable 应按 Lua 配置解析");
    require(!dialog.window.autoResize, "window.auto_resize 应按 Lua 配置解析");
}

void test_script_dialog_requests_reject_invalid_window_options()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("invalid_dialog_window").generic_string()),
            "invalid_dialog_window 协议应可加载");

    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});
    require(host.drainDialogRequests().empty(), "非法 window 配置不应生成弹窗请求");
}

void test_luals_api_sync_contains_tx_and_dialog_api()
{
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
    require(text.find("function proto.request(payload, opts) end") != std::string::npos,
            "LuaLS API 应声明 proto.request");
    require(text.find("function proto.request_guarded(payload, opts) end") != std::string::npos,
            "LuaLS API 应声明 proto.request_guarded");
    require(text.find("@field max_attempts? integer") != std::string::npos,
            "LuaLS API 应声明 guarded request 最大尝试次数");
    require(text.find("@field guard_state? ProtoRequestGuardState") != std::string::npos,
            "LuaLS API 应声明 guarded request 状态字段");
    require(text.find("function proto.reset_request_guard() end") != std::string::npos,
            "LuaLS API 应声明 guarded request reset 接口");
    require(text.find("function proto.request_done(result) end") != std::string::npos,
            "LuaLS API 应声明 proto.request_done");
    require(text.find("function proto.status.set(text, opts) end") != std::string::npos,
            "LuaLS API 应声明 proto.status.set");
    require(text.find("function proto.plot.push(channel_index, payload) end") != std::string::npos,
            "LuaLS API 应声明 proto.plot.push");
    require(text.find("function proto.oscilloscope.set_running(running) end") != std::string::npos,
            "LuaLS API 应声明 proto.oscilloscope.set_running");
    require(text.find("function proto.ui.alert(opts) end") != std::string::npos, "LuaLS API 应声明 proto.ui.alert");
    require(text.find("@class ProtoDialogWindowOptions") != std::string::npos,
            "LuaLS API 应声明 ProtoDialogWindowOptions");
    require(text.find("@field window? ProtoDialogWindowOptions") != std::string::npos,
            "LuaLS API 应声明弹窗 window 配置");
    require(text.find("function proto.fs.open(path, opts) end") != std::string::npos, "LuaLS API 应声明 proto.fs.open");
    require(text.find("function proto.fs.read(handle, opts) end") != std::string::npos,
            "LuaLS API 应声明 proto.fs.read");
    require(text.find("@class ProtoBuffer") != std::string::npos, "LuaLS API 应声明 ProtoBuffer");
    require(text.find("function proto.bits.count(value) end") != std::string::npos,
            "LuaLS API 应声明 proto.bits.count");
    require(text.find("'elf_symbol_combo'") != std::string::npos, "LuaLS API 应声明 elf_symbol_combo");
    require(text.find("@class ProtoElfSymbolValue") != std::string::npos, "LuaLS API 应声明 ProtoElfSymbolValue");
    require(text.find("function stream() end") != std::string::npos, "LuaLS API 应声明 stream()");
    require(text.find("function on_tx(ctx, evt) end") != std::string::npos, "LuaLS API 应声明 on_tx");
    require(text.find("function on_oscilloscope_toggle(ctx, current_running, target_running) end") != std::string::npos,
            "LuaLS API 应声明 on_oscilloscope_toggle");
    require(text.find("function on_dialog(ctx, evt) end") != std::string::npos, "LuaLS API 应声明 on_dialog");
    require(text.find("function on_file_dialog(ctx, evt) end") != std::string::npos, "LuaLS API 应声明 on_file_dialog");
    require(text.find("@field color? string") != std::string::npos, "LuaLS API 应声明 ProtoPlotChannel.color");
    require(text.find("@field line_width? number") != std::string::npos,
            "LuaLS API 应声明 ProtoPlotChannel.line_width");
    require(text.find("@class ProtoPlotBitDisplay") != std::string::npos, "LuaLS API 应声明 ProtoPlotBitDisplay");
    require(text.find("@field bit_display? boolean|ProtoPlotBitDisplay") != std::string::npos,
            "LuaLS API 应声明 ProtoPlotChannel.bit_display");
}

void test_script_oscilloscope_toggle_returns_true_and_receives_args()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-toggle-true"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_oscilloscope_toggle(ctx, current_running, target_running)
  proto.emit("oscilloscope_toggle", "endpoint=" .. ctx.endpoint .. ",current=" .. tostring(current_running) .. ",target=" .. tostring(target_running))
  return true
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "示波器切换测试脚本应可加载");

    require(host.requestOscilloscopeToggle(sampleCtx(), false, true), "Lua 返回 true 时应允许切换");
    const auto events = host.drainEvents();
    require(events.size() == 1, "示波器切换回调应收到参数并 emit 事件");
    require(events[0].payload.find("endpoint=127.0.0.1:9000") != std::string::npos, "应传入连接上下文");
    require(events[0].payload.find("current=false") != std::string::npos, "应传入 current_running=false");
    require(events[0].payload.find("target=true") != std::string::npos, "应传入 target_running=true");
}

void test_script_oscilloscope_toggle_returns_false()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-toggle-false"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_oscilloscope_toggle(ctx, current_running, target_running)
  return false
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "示波器切换 false 测试脚本应可加载");
    require(!host.requestOscilloscopeToggle(sampleCtx(), true, false), "Lua 返回 false 时应拒绝切换");
}

void test_script_oscilloscope_set_running_outputs_update()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-set-running"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_open(ctx)
  proto.oscilloscope.set_running(true)
  proto.oscilloscope.set_running(false)
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "set_running 测试脚本应可加载");

    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});
    const auto updates = host.drainOscilloscopeRunningUpdates();
    require(updates.size() == 2, "set_running 应进入示波器状态输出队列");
    require(updates[0].running, "第一条状态应为 running=true");
    require(!updates[1].running, "第二条状态应为 running=false");
}

void test_script_oscilloscope_toggle_true_defaults_target_update()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-toggle-default-update"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_oscilloscope_toggle(ctx, current_running, target_running)
  return true
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "默认状态同步测试脚本应可加载");

    require(host.requestOscilloscopeToggle(sampleCtx(), false, true), "Lua 返回 true 时应保留允许切换语义");
    const auto updates = host.drainOscilloscopeRunningUpdates();
    require(updates.size() == 1, "Lua 返回 true 且未显式 set_running 时应默认同步 target_running");
    require(updates[0].running, "默认同步状态应使用 target_running=true");
}

void test_script_oscilloscope_toggle_explicit_set_running_overrides_default()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-toggle-explicit-update"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_oscilloscope_toggle(ctx, current_running, target_running)
  proto.oscilloscope.set_running(false)
  return true
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "显式状态同步测试脚本应可加载");

    require(host.requestOscilloscopeToggle(sampleCtx(), false, true), "显式 set_running 不应破坏旧 true 语义");
    const auto updates = host.drainOscilloscopeRunningUpdates();
    require(updates.size() == 1, "同一回调显式 set_running 时不应再追加默认 target 状态");
    require(!updates[0].running, "显式 set_running(false) 应优先于 target_running=true");
}

void test_script_oscilloscope_toggle_false_allows_later_set_running()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-toggle-later-update"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_oscilloscope_toggle(ctx, current_running, target_running)
  proto.set_timer("start_ack", 1)
  return false
end

function on_timer(ctx, name)
  if name == "start_ack" then
    proto.oscilloscope.set_running(true)
  end
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "延迟状态同步测试脚本应可加载");

    require(!host.requestOscilloscopeToggle(sampleCtx(), false, true), "Lua 返回 false 时仍应保持旧拒绝语义");
    require(host.drainOscilloscopeRunningUpdates().empty(), "返回 false 本身不应默认同步 target 状态");

    host.tick(nowMs() + 10);
    const auto updates = host.drainOscilloscopeRunningUpdates();
    require(updates.size() == 1 && updates[0].running, "后续 set_running(true) 应可同步运行状态");
}

void test_script_oscilloscope_toggle_missing_callback_rejects()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("missing_callbacks").generic_string()),
            "缺失回调脚本也应允许加载");
    require(!host.requestOscilloscopeToggle(sampleCtx(), false, true), "缺失示波器切换回调应拒绝切换");
}

void test_script_oscilloscope_toggle_non_function_rejects_and_logs()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-toggle-non-function"));
    writeMainLua(protocolDir.path(), "on_oscilloscope_toggle = \"bad\"\n");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "非 function 回调不应阻止脚本加载");
    require(!host.requestOscilloscopeToggle(sampleCtx(), false, true), "非 function 示波器回调应拒绝切换");

    bool foundLog = false;
    for (const auto& log : host.drainLogs()) {
        foundLog = foundLog || log.message.find("on_oscilloscope_toggle 必须是 function") != std::string::npos;
    }
    require(foundLog, "非 function 示波器回调应写错误日志");
}

void test_script_oscilloscope_toggle_runtime_error_rejects_and_logs()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-toggle-error"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_oscilloscope_toggle(ctx, current_running, target_running)
  error("boom")
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "异常回调测试脚本应可加载");
    require(!host.requestOscilloscopeToggle(sampleCtx(), false, true), "示波器回调异常时应拒绝切换");

    bool foundLog = false;
    for (const auto& log : host.drainLogs()) {
        foundLog = foundLog || log.message.find("on_oscilloscope_toggle 执行失败") != std::string::npos;
    }
    require(foundLog, "示波器回调异常应写错误日志");
}

void test_script_oscilloscope_toggle_non_boolean_rejects_and_logs()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-oscilloscope-toggle-non-boolean"));
    writeMainLua(protocolDir.path(),
                 R"lua(
function on_oscilloscope_toggle(ctx, current_running, target_running)
  return "yes"
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "非 boolean 回调测试脚本应可加载");
    require(!host.requestOscilloscopeToggle(sampleCtx(), false, true), "示波器回调返回非 boolean 时应拒绝切换");

    bool foundLog = false;
    for (const auto& log : host.drainLogs()) {
        foundLog = foundLog || log.message.find("on_oscilloscope_toggle 必须返回 boolean") != std::string::npos;
    }
    require(foundLog, "示波器回调返回非 boolean 应写错误日志");
}

void test_script_missing_callbacks_allowed()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(fixtureProtocolDir("missing_callbacks").generic_string()),
            "缺失回调脚本也应允许加载");

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

void test_script_invalid_controls_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_controls").generic_string()),
            "非法 controls() 应加载失败");
    require(!host.lastError().empty(), "非法 controls() 失败时应记录错误");
}

void test_script_invalid_dock_anchor_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_dock_anchor").generic_string()),
            "非法 dock anchor 应加载失败");
    require(host.lastError().find("dock anchor 不支持") != std::string::npos, "非法 dock anchor 应给出清晰错误");
}

void test_script_table_layout_unknown_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_unknown_control").generic_string()),
            "引用未知控件的 table layout 应加载失败");
    require(host.lastError().find("未声明控件") != std::string::npos, "未知控件错误应包含未声明控件提示");
}

void test_script_table_layout_duplicate_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_duplicate_control").generic_string()),
            "重复引用控件的 table layout 应加载失败");
    require(host.lastError().find("重复引用控件") != std::string::npos, "重复引用错误应包含重复控件提示");
}

void test_script_table_layout_missing_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_missing_control").generic_string()),
            "遗漏控件的 table layout 应加载失败");
    require(host.lastError().find("缺少控件") != std::string::npos, "遗漏控件错误应包含缺少控件提示");
}

void test_script_table_layout_row_overflow_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_table_row_overflow").generic_string()),
            "超出列数的 table layout 应加载失败");
    require(host.lastError().find("单元格数量不能超过 columns") != std::string::npos, "超列错误应包含 columns 提示");
}

void test_script_form_layout_unknown_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_unknown_control").generic_string()),
            "引用未知控件的 form layout 应加载失败");
    require(host.lastError().find("未声明控件") != std::string::npos, "未知控件错误应包含未声明控件提示");
}

void test_script_form_layout_duplicate_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_duplicate_control").generic_string()),
            "重复引用控件的 form layout 应加载失败");
    require(host.lastError().find("重复引用控件") != std::string::npos, "重复引用错误应包含重复控件提示");
}

void test_script_form_layout_missing_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_form_missing_control").generic_string()),
            "遗漏控件的 form layout 应加载失败");
    require(host.lastError().find("缺少控件") != std::string::npos, "遗漏控件错误应包含缺少控件提示");
}

void test_script_layout_unknown_type_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_unknown_type").generic_string()),
            "未知 layout type 应加载失败");
    require(host.lastError().find("type 不支持") != std::string::npos, "未知 layout type 错误应包含 type 提示");
}

void test_script_layout_width_range_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_width_range").generic_string()),
            "min_width 大于 max_width 的 layout control 应加载失败");
    require(host.lastError().find("min_width 不能大于 max_width") != std::string::npos,
            "宽度范围错误应包含 min_width 与 max_width 提示");
}

void test_script_layout_width_type_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_width_type").generic_string()),
            "非数字 layout control 宽度应加载失败");
    require(host.lastError().find("min_width 必须是 number") != std::string::npos, "宽度类型错误应包含 number 提示");
}

void test_script_layout_fill_width_type_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_fill_width_type").generic_string()),
            "非 boolean fill_width 应加载失败");
    require(host.lastError().find("fill_width 必须是 boolean") != std::string::npos,
            "fill_width 类型错误应包含 boolean 提示");
}

void test_script_inline_group_fill_width_type_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_inline_group_fill_width_type").generic_string()),
            "非 boolean inline_group fill_width 应加载失败");
    require(host.lastError().find("fill_width 必须是 boolean") != std::string::npos,
            "inline_group fill_width 类型错误应包含 boolean 提示");
}

void test_script_layout_width_non_positive_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_width_non_positive").generic_string()),
            "非正 layout control 宽度应加载失败");
    require(host.lastError().find("min_width 必须是正数") != std::string::npos, "非正宽度错误应包含正数提示");
}

void test_script_layout_children_controls_conflict_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_children_controls").generic_string()),
            "同时声明 children 和 controls 的 layout 容器应加载失败");
    require(host.lastError().find("不能同时声明 children 和 controls") != std::string::npos,
            "children/controls 混用错误应包含互斥提示");
}

void test_script_layout_shortcut_unknown_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_shortcut_unknown_control").generic_string()),
            "controls 简写引用未知控件应加载失败");
    require(host.lastError().find("未声明控件") != std::string::npos, "简写未知控件错误应包含未声明控件提示");
}

void test_script_layout_shortcut_duplicate_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(
        !host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_shortcut_duplicate_control").generic_string()),
        "controls 简写重复引用控件应加载失败");
    require(host.lastError().find("重复引用控件") != std::string::npos, "简写重复控件错误应包含重复控件提示");
}

void test_script_layout_shortcut_missing_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_layout_shortcut_missing_control").generic_string()),
            "controls 简写遗漏控件应加载失败");
    require(host.lastError().find("缺少控件") != std::string::npos, "简写遗漏控件错误应包含缺少控件提示");
}

void test_script_inline_group_children_controls_conflict_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_inline_group_children_controls").generic_string()),
            "inline_group 同时声明 children 和 controls 时应加载失败");
    require(host.lastError().find("不能同时声明 children 和 controls") != std::string::npos,
            "inline_group children/controls 混用错误应包含互斥提示");
}

void test_script_inline_group_unknown_control_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_inline_group_unknown_control").generic_string()),
            "inline_group.controls 引用未知控件应加载失败");
    require(host.lastError().find("未声明控件") != std::string::npos, "inline_group 未知控件错误应包含未声明控件提示");
}

void test_script_inline_group_child_layout_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_inline_group_child_layout").generic_string()),
            "inline_group 嵌套 flow 时应加载失败");
    require(host.lastError().find("inline_group 只允许 control 或 text 子节点") != std::string::npos,
            "inline_group 非法子布局错误应说明允许的子节点类型");
}

void test_script_inline_group_min_width_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_inline_group_min_width").generic_string()),
            "inline_group 非正 min_width 应加载失败");
    require(host.lastError().find("min_width 必须是正数") != std::string::npos,
            "inline_group min_width 错误应包含正数提示");
}

void test_script_short_label_type_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_short_label_type").generic_string()),
            "short_label 非字符串时应加载失败");
    require(host.lastError().find("short_label") != std::string::npos, "short_label 类型错误应包含字段名");
}

void test_script_short_label_empty_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_short_label_empty").generic_string()),
            "short_label 为空时应加载失败");
    require(host.lastError().find("short_label") != std::string::npos, "short_label 空值错误应包含字段名");
}

void test_script_compact_label_below_type_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_compact_label_below_type").generic_string()),
            "compact_label_below 非数字时应加载失败");
    require(host.lastError().find("compact_label_below 必须是 number") != std::string::npos,
            "compact_label_below 类型错误应包含 number 提示");
}

void test_script_compact_label_below_non_positive_fail()
{
    protoscope::scripting::ScriptHost host;
    require(
        !host.loadProtocolDirectory(fixtureProtocolDir("invalid_compact_label_below_non_positive").generic_string()),
        "compact_label_below 非正数时应加载失败");
    require(host.lastError().find("compact_label_below 必须是正数") != std::string::npos,
            "compact_label_below 非正数错误应包含正数提示");
}

void test_script_invalid_label_position_fail()
{
    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(fixtureProtocolDir("invalid_label_position").generic_string()),
            "非法 label_position 应加载失败");
    require(host.lastError().find("label_position") != std::string::npos, "非法 label_position 错误应包含字段名");
}

void test_script_runtime_error_logged()
{
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

void test_script_reload_invalid_types_fail_without_throw()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-invalid-lua-types"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "非法 stream 测试脚本应可写入");
        out << "function stream()\n";
        out << "  return \"bad stream\"\n";
        out << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(!host.loadProtocolDirectory(protocolDir.path().generic_string()), "stream() 返回非 table 应加载失败");
    require(host.lastError().find("stream() 必须返回 table") != std::string::npos, "stream 类型错误应返回清晰错误");

    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "非法 ui 测试脚本应可写入");
        out << "function ui()\n";
        out << "  return \"bad ui\"\n";
        out << "end\n";
    }

    require(!host.loadProtocolDirectory(protocolDir.path().generic_string()), "ui() 返回非 table 应加载失败");
    require(host.lastError().find("ui() 必须返回 table") != std::string::npos, "ui 类型错误应返回清晰错误");
}

void test_script_non_function_callbacks_only_log_errors()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-non-function-callbacks"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "非 function 回调测试脚本应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"safe\", title = \"Safe\", controls = { { type = \"button\", id = \"run\", label = "
               "\"Run\" } } } }\n";
        out << "end\n";
        out << "on_open = {}\n";
        out << "on_control = \"bad\"\n";
        out << "on_tx = {}\n";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "非 function 回调不应阻止脚本加载");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.onControl(ctx, "run", true);
    host.onTxEvent(ctx, protoscope::scripting::TxEvent{});

    bool foundOpen = false;
    bool foundControl = false;
    bool foundTx = false;
    for (const auto& log : host.drainLogs()) {
        foundOpen = foundOpen || log.message.find("on_open 必须是 function") != std::string::npos;
        foundControl = foundControl || log.message.find("on_control 必须是 function") != std::string::npos;
        foundTx = foundTx || log.message.find("on_tx 必须是 function") != std::string::npos;
    }
    require(foundOpen, "on_open 非 function 应只写错误日志");
    require(foundControl, "on_control 非 function 应只写错误日志");
    require(foundTx, "on_tx 非 function 应只写错误日志");
}

void test_script_failed_reload_keeps_previous_runtime()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-script-reload-transaction"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "reload 事务测试脚本应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"safe\", title = \"Safe\", controls = { { type = \"button\", id = \"run\", label = "
               "\"Run\" } } } }\n";
        out << "end\n";
        out << "function on_control(ctx, id, value)\n";
        out << "  proto.emit(\"old_runtime\", id)\n";
        out << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "初始脚本应可加载");
    const auto beforeControls = host.controlsSnapshot();
    require(beforeControls.size() == 1, "初始脚本应提供一个控件");

    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "reload 失败测试脚本应可写入");
        out << "function ui()\n";
        out << "  return \"bad ui\"\n";
        out << "end\n";
    }

    require(!host.loadProtocolDirectory(protocolDir.path().generic_string()), "reload 加载期错误应返回失败");
    require(host.lastError().find("ui() 必须返回 table") != std::string::npos, "reload 失败应记录新错误");
    require(host.controlsSnapshot().size() == beforeControls.size(), "reload 失败应保留旧控件快照");

    host.onControl(sampleCtx(), "run", true);
    bool foundOldRuntime = false;
    for (const auto& event : host.drainEvents()) {
        if (event.name == "old_runtime" && event.payload == "run") {
            foundOldRuntime = true;
        }
    }
    require(foundOldRuntime, "reload 失败后旧回调仍应可用");
}

void test_protocol_directory_reload()
{
    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory("protocols/default_protocol"), "默认协议目录应可加载");
    require(host.protocolDirectory() == "protocols/default_protocol", "协议目录应被记录");
    require(host.scriptPath().find("main.lua") != std::string::npos, "协议入口应固定为 main.lua");
}

void test_protocol_ui_templates_load()
{
    const std::array<const char*, 3> templates{"ui_basic", "ui_layouts", "ui_dialogs"};

    for (const auto* name : templates) {
        protoscope::scripting::ScriptHost host;
        const auto directory = templateProtocolDir(name).generic_string();

        requireProtocolLoaded(host, directory.c_str());
        const auto message = std::string(name) + " 模板应声明可见控件";
        require(!host.controlsSnapshot().empty(), message.c_str());
    }
}

void test_config_default_roundtrip()
{
    protoscope::config::ConfigStore store;
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-config-roundtrip"));
    const auto tempPath = tempRoot.path() / "protoscope.yaml";

    auto config = store.load(tempPath).config;
    require(config.protocol.rootDir.find("protocols/templates") != std::string::npos,
            "默认协议根目录应指向 protocols/templates");
    require(config.protocol.selectedDir.find("protocols/templates/default_protocol") != std::string::npos,
            "默认协议目录应指向 protocols/templates/default_protocol");
    require(config.gui.wave.controlMode == protoscope::plot::WaveControlMode::Oscilloscope,
            "波形控制模式默认值应为 oscilloscope");
    require(config.gui.wave.displayFormula == protoscope::plot::WaveDisplayFormula::OffsetThenScale,
            "波形显示公式默认值应为 offset_then_scale");
    require(config.gui.wave.gridDivisionReadoutMode == protoscope::plot::WaveGridDivisionReadoutMode::DisplayValue,
            "网格每格读数默认值应为 display_value");
    require(config.gui.wave.channelCardWidthMode == protoscope::plot::WaveChannelCardWidthMode::Fixed,
            "CH 卡片宽度模式默认值应为 fixed");
    require(
        config.gui.wave.channelDoubleClickAction == protoscope::plot::WaveChannelDoubleClickAction::ResetScaleOffset,
        "CH 卡片双击默认值应为 reset_scale_offset");
    require(config.gui.wave.xAxisDoubleClickAction == protoscope::plot::WaveXAxisDoubleClickAction::FitFullHistory,
            "X 轴双击默认值应为 fit_full_history");
    require(config.gui.wave.yAxisDoubleClickAction == protoscope::plot::WaveYAxisDoubleClickAction::FitVisibleChannels,
            "Y 轴双击默认值应为 fit_visible_channels");
    require(std::abs(config.gui.wave.channelCardFixedWidth - 128.0) < 1e-12, "CH 卡片固定宽度默认值应为 128");
    require(std::abs(config.gui.wave.channelCardAdaptiveRatio - 0.22) < 1e-12, "CH 卡片自适应比例默认值应为 0.22");
    require(std::abs(config.gui.wave.verticalAutoFitMultiplier - 1.25) < 1e-12, "Y 轴 Auto Fit 系数默认值应为 1.25");
    require(!config.gui.wave.zoomSelectionAutoExit, "框选放大默认不应自动退出");
    require(config.gui.wave.hiddenChannelPolicy == protoscope::plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews,
            "隐藏 CH 策略默认应只让可见通道参与派生视图");
    require(config.gui.wave.cursorExtremeSnapPolicy == protoscope::plot::WaveCursorExtremeSnapPolicy::NearestWaveform,
            "游标极值吸附策略默认应为 nearest_waveform");
    require(config.gui.wave.mouseYOffsetDragMode == protoscope::plot::WaveMouseYOffsetDragMode::Direct,
            "鼠标 Y 偏移拖动模式默认应为 direct");
    require(config.gui.wave.legendOverlayDoubleClickAutoCollapse, "双击图例展开默认应在鼠标离开后自动收起");
    require(config.gui.wave.peakDetectDownsample, "peak-detect 降采样默认应开启");
    require(config.gui.wave.showChannelLegend, "波形图例默认应显示");
    require(config.gui.wave.showFftLegend, "FFT 图例默认应显示");
    require(!config.gui.wave.followMeasurementCursorsOnScroll, "测量游标跟随滚动默认应关闭");
    require(std::abs(config.gui.wave.cursorFftHighlightRgba[0] - 0.20F) < 1e-6F &&
                std::abs(config.gui.wave.cursorFftHighlightRgba[1] - 0.55F) < 1e-6F &&
                std::abs(config.gui.wave.cursorFftHighlightRgba[2] - 1.00F) < 1e-6F &&
                std::abs(config.gui.wave.cursorFftHighlightRgba[3] - 0.16F) < 1e-6F,
            "游标 FFT 高亮色默认值应为建议 RGBA");
    require(config.gui.wave.fullscreenMode == protoscope::config::GuiWaveFullscreenMode::Overlay,
            "波形全屏模式默认应为 overlay");
    require(config.gui.rendererBackend == protoscope::config::GuiRendererBackend::OpenGL,
            "GUI 渲染后端默认应为 opengl");
    require(config.gui.font.chineseGlyphRange == protoscope::config::GuiFontChineseGlyphRange::SimplifiedCommon,
            "中文字体默认应只加载常用简中字形");
    require(config.gui.logHistory.transferRawLimit == 10000, "原始收发历史默认上限应为 10000");
    require(config.gui.logHistory.transferFrameLimit == 120000, "逐帧收发历史默认上限应为 120000");
    require(config.gui.logHistory.hostLimit == 5000, "宿主日志默认上限应为 5000");
    require(config.gui.logHistory.scriptLimit == 5000, "脚本日志默认上限应为 5000");
    require(config.gui.logHistory.requestTraceLimit == 5000, "请求追踪默认上限应为 5000");
    require(config.gui.rawCapture.liveLimitBytes == 64U * 1024U * 1024U, "实时原始缓存默认上限应为 64MiB");
    require(config.gui.rawCapture.recordingQueueLimitBytes == 256U * 1024U * 1024U,
            "完整原始录制队列默认硬上限应为 256MiB");
    require(config.gui.wave.resetHistoryOnTimeReset, "波形时间回绕默认应从零开始新历史");
    require(config.gui.realtimeBacklog.rawFirstBacklogWarnBytes == 32U * 1024U * 1024U,
            "raw-first backlog 默认告警阈值应为 32MiB");
    require(config.gui.realtimeBacklog.derivedBacklogDegradeEnabled, "派生 UI backlog 默认应允许降级");
    require(config.gui.realtimeBacklog.rxChunkBytesPerPump == 4096U, "实时 backlog RX pump 默认应为 4096 字节");
    require(config.gui.realtimeBacklog.plotAppendsPerPump == 128U, "实时 backlog plot append 默认应为 128");
    require(std::abs(config.gui.realtimeBacklog.pumpMinIntervalMs - 1.0) < 1e-12,
            "实时 backlog pump 最小间隔默认应为 1ms");
    require(config.receive.transportReadBufferBytes == 4096U, "transport 读缓冲默认应为 4096 字节");
    require(config.scripting.workerBatchBytes == 8192U, "worker batch 默认应为 8192 字节");
    require(std::abs(config.scripting.workerOutputFlushBudgetMs - 2.0) < 1e-12, "worker 输出刷新时间预算默认应为 2ms");
    require(config.gui.elfSymbolCombo.limit == 10, "ELF 变量候选默认上限应为 10");
    require(config.gui.elfSymbolCombo.debounceMs == 300, "ELF 变量候选默认消抖应为 300ms");
    require(config.gui.elfSymbolCombo.autoRefreshSelectedAddress, "ELF 已选地址默认应自动刷新");
    require(!config.gui.elfSymbolCombo.autoRefreshEmitOnControl, "ELF 已选地址默认应静默刷新");
    require(!config.gui.showAppHeader, "现代应用栏默认应关闭");
    require(config.gui.sendHistoryLimit == 20, "发送历史条数默认值应为 20");
    require(!config.gui.replayRawHistoryOnSchemaSwitch, "raw 切到 schema 默认不应回放旧历史");
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
    config.gui.wave.gridDivisionReadoutMode = protoscope::plot::WaveGridDivisionReadoutMode::RawValue;
    config.gui.wave.channelCardWidthMode = protoscope::plot::WaveChannelCardWidthMode::Adaptive;
    config.gui.wave.channelDoubleClickAction = protoscope::plot::WaveChannelDoubleClickAction::ResetAll;
    config.gui.wave.xAxisDoubleClickAction = protoscope::plot::WaveXAxisDoubleClickAction::FitVisibleWindow;
    config.gui.wave.yAxisDoubleClickAction = protoscope::plot::WaveYAxisDoubleClickAction::FitActiveChannel;
    config.gui.wave.hiddenChannelPolicy = protoscope::plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews;
    config.gui.wave.cursorExtremeSnapPolicy = protoscope::plot::WaveCursorExtremeSnapPolicy::ViewportZone;
    config.gui.wave.mouseYOffsetDragMode = protoscope::plot::WaveMouseYOffsetDragMode::Shift;
    config.gui.wave.legendOverlayDoubleClickAutoCollapse = false;
    config.gui.wave.channelCardFixedWidth = 144.0;
    config.gui.wave.channelCardAdaptiveRatio = 0.3;
    config.gui.wave.verticalAutoFitMultiplier = 1.5;
    config.gui.wave.zoomSelectionAutoExit = true;
    config.gui.wave.peakDetectDownsample = false;
    config.gui.wave.showChannelLegend = false;
    config.gui.wave.showFftLegend = false;
    config.gui.wave.followMeasurementCursorsOnScroll = true;
    config.gui.wave.cursorFftHighlightRgba = {0.10F, 0.20F, 0.30F, 0.40F};
    config.gui.wave.fullscreenMode = protoscope::config::GuiWaveFullscreenMode::Overlay;
    config.gui.rendererBackend = protoscope::config::GuiRendererBackend::D3D11Warp;
    config.gui.font.chineseGlyphRange = protoscope::config::GuiFontChineseGlyphRange::Full;
    config.gui.logHistory.transferRawLimit = 11;
    config.gui.logHistory.transferFrameLimit = 22;
    config.gui.logHistory.hostLimit = 33;
    config.gui.logHistory.scriptLimit = 44;
    config.gui.logHistory.requestTraceLimit = 55;
    config.gui.rawCapture.liveLimitBytes = 123;
    config.gui.rawCapture.recordingQueueLimitBytes = 456;
    config.gui.wave.resetHistoryOnTimeReset = false;
    config.gui.realtimeBacklog.rawFirstBacklogWarnBytes = 789;
    config.gui.realtimeBacklog.derivedBacklogDegradeEnabled = false;
    config.receive.transportReadBufferBytes = 321;
    config.gui.elfSymbolCombo.limit = 12;
    config.gui.elfSymbolCombo.debounceMs = 350;
    config.gui.elfSymbolCombo.autoRefreshSelectedAddress = false;
    config.gui.elfSymbolCombo.autoRefreshEmitOnControl = true;
    config.gui.showAppHeader = true;
    config.gui.sendHistoryLimit = 7;
    config.gui.replayRawHistoryOnSchemaSwitch = true;
    config.scripting.fileIo.enabled = true;
    config.scripting.fileIo.maxOpenFiles = 3;
    config.scripting.fileIo.defaultChunkBytes = 32;
    config.scripting.fileIo.maxChunkBytes = 64;
    config.scripting.fileIo.extraAllowedRoots = {"firmware"};

    std::string error;
    require(store.save(tempPath, config, error), "默认配置写回失败");

    const auto reloaded = store.load(tempPath);
    require(reloaded.config.communication.kind == protoscope::transport::TransportKind::Serial,
            "串口模式 roundtrip 失败");
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
    require(reloaded.config.gui.wave.gridDivisionReadoutMode == protoscope::plot::WaveGridDivisionReadoutMode::RawValue,
            "网格每格读数模式 roundtrip 失败");
    require(reloaded.config.gui.wave.channelCardWidthMode == protoscope::plot::WaveChannelCardWidthMode::Adaptive,
            "CH 卡片宽度模式 roundtrip 失败");
    require(
        reloaded.config.gui.wave.channelDoubleClickAction == protoscope::plot::WaveChannelDoubleClickAction::ResetAll,
        "CH 卡片双击行为 roundtrip 失败");
    require(reloaded.config.gui.wave.xAxisDoubleClickAction ==
                protoscope::plot::WaveXAxisDoubleClickAction::FitVisibleWindow,
            "X 轴双击行为 roundtrip 失败");
    require(reloaded.config.gui.wave.yAxisDoubleClickAction ==
                protoscope::plot::WaveYAxisDoubleClickAction::FitActiveChannel,
            "Y 轴双击行为 roundtrip 失败");
    require(reloaded.config.gui.wave.hiddenChannelPolicy ==
                protoscope::plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews,
            "隐藏 CH 策略 roundtrip 失败");
    require(
        reloaded.config.gui.wave.cursorExtremeSnapPolicy == protoscope::plot::WaveCursorExtremeSnapPolicy::ViewportZone,
        "游标极值吸附策略 roundtrip 失败");
    require(reloaded.config.gui.wave.mouseYOffsetDragMode == protoscope::plot::WaveMouseYOffsetDragMode::Shift,
            "鼠标 Y 偏移拖动模式 roundtrip 失败");
    require(!reloaded.config.gui.wave.legendOverlayDoubleClickAutoCollapse, "双击图例展开自动收起开关 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.channelCardFixedWidth - 144.0) < 1e-12, "CH 卡片固定宽度 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.channelCardAdaptiveRatio - 0.3) < 1e-12,
            "CH 卡片自适应比例 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.verticalAutoFitMultiplier - 1.5) < 1e-12,
            "Y 轴 Auto Fit 系数 roundtrip 失败");
    require(reloaded.config.gui.wave.zoomSelectionAutoExit, "框选放大自动退出开关 roundtrip 失败");
    require(!reloaded.config.gui.wave.peakDetectDownsample, "peak-detect 降采样开关 roundtrip 失败");
    require(!reloaded.config.gui.wave.showChannelLegend, "波形图例显示开关 roundtrip 失败");
    require(!reloaded.config.gui.wave.showFftLegend, "FFT 图例显示开关 roundtrip 失败");
    require(reloaded.config.gui.wave.followMeasurementCursorsOnScroll, "测量游标跟随滚动开关 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.cursorFftHighlightRgba[0] - 0.10F) < 1e-6F &&
                std::abs(reloaded.config.gui.wave.cursorFftHighlightRgba[1] - 0.20F) < 1e-6F &&
                std::abs(reloaded.config.gui.wave.cursorFftHighlightRgba[2] - 0.30F) < 1e-6F &&
                std::abs(reloaded.config.gui.wave.cursorFftHighlightRgba[3] - 0.40F) < 1e-6F,
            "游标 FFT 高亮色 roundtrip 失败");
    require(reloaded.config.gui.wave.fullscreenMode == protoscope::config::GuiWaveFullscreenMode::Overlay,
            "波形全屏模式 roundtrip 失败");
    require(reloaded.config.gui.rendererBackend == protoscope::config::GuiRendererBackend::D3D11Warp,
            "GUI 渲染后端 roundtrip 失败");
    require(reloaded.config.gui.font.chineseGlyphRange == protoscope::config::GuiFontChineseGlyphRange::Full,
            "中文字体字形范围 roundtrip 失败");
    require(reloaded.config.gui.wave.maxRenderPointsPerChannel == 64, "波形每通道渲染点数 roundtrip 失败");
    require(reloaded.config.gui.wave.maxRenderVertices == 4096, "波形顶点预算 roundtrip 失败");
    require(reloaded.config.gui.wave.overviewMaxSamples == 128, "波形概览点数 roundtrip 失败");
    require(std::abs(reloaded.config.gui.wave.minVisibleTimeSpan - 0.0025) < 1e-12, "波形最小可视跨度 roundtrip 失败");
    require(reloaded.config.gui.logHistory.transferRawLimit == 11, "原始收发历史上限 roundtrip 失败");
    require(reloaded.config.gui.logHistory.transferFrameLimit == 22, "逐帧收发历史上限 roundtrip 失败");
    require(reloaded.config.gui.logHistory.hostLimit == 33, "宿主日志历史上限 roundtrip 失败");
    require(reloaded.config.gui.logHistory.scriptLimit == 44, "脚本日志历史上限 roundtrip 失败");
    require(reloaded.config.gui.logHistory.requestTraceLimit == 55, "请求追踪历史上限 roundtrip 失败");
    require(reloaded.config.gui.rawCapture.liveLimitBytes == 123, "实时原始缓存上限 roundtrip 失败");
    require(reloaded.config.gui.rawCapture.recordingQueueLimitBytes == 456, "完整原始录制队列上限 roundtrip 失败");
    require(!reloaded.config.gui.wave.resetHistoryOnTimeReset, "波形时间回绕策略 roundtrip 失败");
    require(reloaded.config.gui.realtimeBacklog.rawFirstBacklogWarnBytes == 789,
            "raw-first backlog 告警阈值 roundtrip 失败");
    require(!reloaded.config.gui.realtimeBacklog.derivedBacklogDegradeEnabled, "派生 UI 降级开关 roundtrip 失败");
    require(reloaded.config.receive.transportReadBufferBytes == 321, "transport 读缓冲大小 roundtrip 失败");
    require(reloaded.config.gui.elfSymbolCombo.limit == 12, "ELF 变量候选上限 roundtrip 失败");
    require(reloaded.config.gui.elfSymbolCombo.debounceMs == 350, "ELF 变量候选消抖 roundtrip 失败");
    require(!reloaded.config.gui.elfSymbolCombo.autoRefreshSelectedAddress, "ELF 已选地址自动刷新开关 roundtrip 失败");
    require(reloaded.config.gui.elfSymbolCombo.autoRefreshEmitOnControl, "ELF 自动刷新回调开关 roundtrip 失败");
    require(reloaded.config.gui.showAppHeader, "现代应用栏开关 roundtrip 失败");
    require(reloaded.config.gui.sendHistoryLimit == 7, "发送历史条数 roundtrip 失败");
    require(reloaded.config.gui.replayRawHistoryOnSchemaSwitch, "raw 切到 schema 回放开关 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.enabled, "Lua 文件 IO 开关 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.maxOpenFiles == 3, "Lua 文件 IO 打开数上限 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.defaultChunkBytes == 32, "Lua 文件 IO 默认分块 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.maxChunkBytes == 64, "Lua 文件 IO 最大分块 roundtrip 失败");
    require(reloaded.config.scripting.fileIo.extraAllowedRoots.size() == 1 &&
                reloaded.config.scripting.fileIo.extraAllowedRoots[0] == "firmware",
            "Lua 文件 IO 额外授权根 roundtrip 失败");

    const auto blockedParent = tempRoot.path() / "blocked-parent";
    {
        std::ofstream blocker(blockedParent, std::ios::binary | std::ios::trunc);
        blocker << "not a directory";
    }
    error.clear();
    require(!store.save(blockedParent / "protoscope.yaml", config, error), "父路径为文件时配置写回应失败");
    require(error.find("创建配置目录失败") != std::string::npos, "配置目录创建失败应返回明确错误");
}

void test_config_wave_mouse_y_offset_drag_mode_apply_capture()
{
    protoscope::config::ConfigStore store;
    protoscope::config::AppConfig config;
    config.gui.wave.mouseYOffsetDragMode = protoscope::plot::WaveMouseYOffsetDragMode::Shift;
    config.gui.wave.gridDivisionReadoutMode = protoscope::plot::WaveGridDivisionReadoutMode::ActualValue;
    config.gui.wave.cursorFftHighlightRgba = {0.30F, 0.40F, 0.50F, 0.60F};
    config.gui.wave.followMeasurementCursorsOnScroll = true;
    config.gui.wave.peakDetectDownsample = false;
    config.gui.wave.legendOverlayDoubleClickAutoCollapse = false;

    protoscope::dock::DockStore dockStore;
    store.applyToDock(config, dockStore);
    require(dockStore.waveState().view.mouseYOffsetDragMode == protoscope::plot::WaveMouseYOffsetDragMode::Shift,
            "applyToDock 应写入鼠标 Y 偏移拖动模式");
    require(dockStore.waveState().view.gridDivisionReadoutMode ==
                protoscope::plot::WaveGridDivisionReadoutMode::ActualValue,
            "applyToDock 应写入网格每格读数模式");
    require(std::abs(dockStore.waveState().view.cursorFftHighlightRgba[3] - 0.60F) < 1e-6F,
            "applyToDock 应写入游标 FFT 高亮色");
    require(dockStore.waveState().view.followMeasurementCursorsOnScroll, "applyToDock 应写入测量游标跟随滚动开关");
    require(!dockStore.waveState().view.peakDetectDownsample, "applyToDock 应写入 peak-detect 降采样开关");
    require(!dockStore.waveState().legendOverlay.doubleClickAutoCollapse, "applyToDock 应写入图例双击展开自动收起开关");

    dockStore.waveState().view.mouseYOffsetDragMode = protoscope::plot::WaveMouseYOffsetDragMode::Disabled;
    dockStore.waveState().view.gridDivisionReadoutMode = protoscope::plot::WaveGridDivisionReadoutMode::RawValue;
    dockStore.waveState().view.cursorFftHighlightRgba = {0.70F, 0.60F, 0.50F, 0.40F};
    dockStore.waveState().view.followMeasurementCursorsOnScroll = false;
    dockStore.waveState().view.peakDetectDownsample = true;
    dockStore.waveState().legendOverlay.doubleClickAutoCollapse = true;
    const auto captured = store.captureFromDock(dockStore);
    require(captured.gui.wave.mouseYOffsetDragMode == protoscope::plot::WaveMouseYOffsetDragMode::Disabled,
            "captureFromDock 应捕获鼠标 Y 偏移拖动模式");
    require(captured.gui.wave.gridDivisionReadoutMode == protoscope::plot::WaveGridDivisionReadoutMode::RawValue,
            "captureFromDock 应捕获网格每格读数模式");
    require(std::abs(captured.gui.wave.cursorFftHighlightRgba[0] - 0.70F) < 1e-6F &&
                std::abs(captured.gui.wave.cursorFftHighlightRgba[3] - 0.40F) < 1e-6F,
            "captureFromDock 应捕获游标 FFT 高亮色");
    require(!captured.gui.wave.followMeasurementCursorsOnScroll, "captureFromDock 应捕获测量游标跟随滚动开关");
    require(captured.gui.wave.peakDetectDownsample, "captureFromDock 应捕获 peak-detect 降采样开关");
    require(captured.gui.wave.legendOverlayDoubleClickAutoCollapse, "captureFromDock 应捕获图例双击展开自动收起开关");
}

void test_config_repo_default_yaml_loads()
{
    protoscope::config::ConfigStore store;
    const std::filesystem::path repoConfigPath = std::filesystem::path{"config"} / "protoscope.yaml";

    require(std::filesystem::exists(repoConfigPath), "源码默认配置文件应存在");
    const auto loaded = store.load(repoConfigPath);

    if (!loaded.error.empty()) {
        throw std::runtime_error("源码默认配置 YAML 读取失败: " + loaded.error);
    }
    require(loaded.loadedFromDisk, "源码默认配置应从磁盘读取");
    require(std::abs(loaded.config.scripting.workerBackpressureHighWatermark - 0.5) < 1e-12,
            "默认配置应读取 worker 高水位");
    require(std::abs(loaded.config.scripting.workerBackpressureLowWatermark - 0.3) < 1e-12,
            "默认配置应读取 worker 低水位");
    require(loaded.config.gui.wave.fullscreenMode == protoscope::config::GuiWaveFullscreenMode::Overlay,
            "源码默认配置应读取 overlay 波形全屏模式");
    require(loaded.config.gui.rendererBackend == protoscope::config::GuiRendererBackend::OpenGL,
            "源码默认配置应读取 opengl 渲染后端");
    require(loaded.config.gui.wave.peakDetectDownsample, "源码默认配置应开启 peak-detect 降采样");
    require(loaded.config.gui.wave.legendOverlayDoubleClickAutoCollapse, "源码默认配置应开启图例双击展开自动收起");
}

void test_script_file_io_proto_buffer_roundtrip()
{
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-script-file-io"));
    const auto protocolDir = tempRoot.path() / "proto";
    std::filesystem::create_directories(protocolDir);
    {
        std::ofstream data(protocolDir / "input.bin", std::ios::binary);
        data << "abcdef";
    }
    {
        std::ofstream script(protocolDir / "main.lua");
        script << "function on_open(ctx)\n";
        script << "  local bad_job, bad_err = proto.fs.send_file('input.bin', { kind = 'invalid' })\n";
        script << "  assert(bad_job == nil)\n";
        script << "  assert(bad_err and bad_err:find('kind'))\n";
        script << "  local stat = assert(proto.fs.stat('input.bin'))\n";
        script << "  assert(stat.is_file and stat.size == 6)\n";
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

void test_config_performance_scale_applies_default_budgets()
{
    protoscope::config::ConfigStore store;
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-config-performance-scale"));
    const auto tempPath = tempRoot.path() / "protoscope.yaml";
    {
        std::ofstream out(tempPath);
        out << R"yaml(
performance:
  scale: 2.0
scripting:
  worker:
    batch_bytes: 123
)yaml";
    }

    const auto loaded = store.load(tempPath).config;

    require(std::abs(loaded.performance.scale - 2.0) < 1e-12, "performance.scale 应读取为 2.0");
    require(loaded.receive.transportReadBufferBytes == 8192U, "transport 读缓冲缺省值应按 scale 放大");
    require(loaded.scripting.workerRxQueueLimitBytes == 128U * 1024U * 1024U, "worker RX 队列缺省值应按 scale 放大");
    require(loaded.scripting.workerMemoryBudgetBytes == 512U * 1024U * 1024U, "worker 内存预算缺省值应按 scale 放大");
    require(loaded.scripting.workerOutputQueueLimit == 131072U, "worker 输出队列缺省值应按 scale 放大");
    require(loaded.scripting.workerBatchBytes == 123U, "显式 batch_bytes 不应被 performance.scale 覆盖");
    require(std::abs(loaded.scripting.workerOutputFlushBudgetMs - 4.0) < 1e-12,
            "worker 输出刷新时间预算缺省值应按 scale 放大");
    require(loaded.gui.realtimeBacklog.rxChunkBytesPerPump == 8192U,
            "实时 backlog RX pump 字节预算缺省值应按 scale 放大");
    require(loaded.gui.realtimeBacklog.transferFrameRowsPerPump == 4000U, "实时 backlog 行预算缺省值应按 scale 放大");
    require(loaded.gui.realtimeBacklog.plotAppendsPerPump == 256U,
            "实时 backlog plot append 预算缺省值应按 scale 放大");
    require(loaded.gui.realtimeBacklog.rawFirstBacklogWarnBytes == 64U * 1024U * 1024U,
            "raw-first backlog 告警阈值缺省值应按 scale 放大");
}

void test_config_performance_save_keeps_scaled_defaults_compact()
{
    protoscope::config::ConfigStore store;
    std::string error;
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-config-performance-save"));
    const auto scaledPath = tempRoot.path() / "scaled.yaml";
    {
        std::ofstream out(scaledPath);
        out << "performance:\n  scale: 2.0\n";
    }

    auto scaledConfig = store.load(scaledPath).config;
    require(store.save(scaledPath, scaledConfig, error), "缩放配置写回失败");
    const auto scaledYaml = readTextFile(scaledPath);

    require(scaledYaml.find("performance:") != std::string::npos, "写回配置应保留 performance 分组");
    require(scaledYaml.find("scale:") != std::string::npos, "写回配置应保留 performance.scale");
    require(scaledYaml.find("transport_read_buffer_bytes") == std::string::npos,
            "未显式覆盖的 transport 读缓冲不应固化到 YAML");
    require(scaledYaml.find("rx_queue_limit_bytes") == std::string::npos, "未显式覆盖的 worker RX 队列不应固化到 YAML");
    require(scaledYaml.find("batch_bytes") == std::string::npos, "未显式覆盖的 worker batch 不应固化到 YAML");
    require(scaledYaml.find("rx_chunk_bytes_per_pump") == std::string::npos,
            "未显式覆盖的实时 backlog 预算不应固化到 YAML");

    const auto explicitPath = tempRoot.path() / "explicit.yaml";
    {
        std::ofstream out(explicitPath);
        out << "performance:\n  scale: 2.0\nscripting:\n  worker:\n    batch_bytes: 123\n";
    }
    auto explicitConfig = store.load(explicitPath).config;
    require(store.save(explicitPath, explicitConfig, error), "显式覆盖配置写回失败");
    const auto explicitYaml = readTextFile(explicitPath);

    require(explicitYaml.find("batch_bytes: 123") != std::string::npos,
            "显式 batch_bytes 应写回并继续覆盖 performance.scale");
}

void test_config_wave_mode_invalid_fallback()
{
    protoscope::config::ConfigStore store;
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-config-wave-invalid"));
    const auto tempPath = tempRoot.path() / "config.yaml";
    std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
    out << "gui:\n"
           "  renderer_backend: weird\n"
           "  font:\n"
           "    chinese_glyph_range: weird\n"
           "  wave:\n"
           "    control_mode: weird\n"
           "    display_formula: wrong\n"
           "    grid_division_readout_mode: weird\n"
           "    channel_card_width_mode: weird\n"
           "    channel_double_click_action: weird\n"
           "    x_axis_double_click_action: weird\n"
           "    y_axis_double_click_action: weird\n"
           "    mouse_y_offset_drag_mode: weird\n"
           "    fullscreen_mode: weird\n"
           "    channel_card_fixed_width: 0\n"
           "    channel_card_adaptive_ratio: -0.5\n"
           "    vertical_auto_fit_multiplier: 0\n";
    out.close();

    const auto loaded = store.load(tempPath).config;
    require(loaded.gui.wave.controlMode == protoscope::plot::WaveControlMode::Oscilloscope,
            "非法 control_mode 应回退到 oscilloscope");
    require(loaded.gui.wave.displayFormula == protoscope::plot::WaveDisplayFormula::OffsetThenScale,
            "非法 display_formula 应回退到 offset_then_scale");
    require(loaded.gui.wave.gridDivisionReadoutMode == protoscope::plot::WaveGridDivisionReadoutMode::DisplayValue,
            "非法 grid_division_readout_mode 应回退到 display_value");
    require(loaded.gui.wave.channelCardWidthMode == protoscope::plot::WaveChannelCardWidthMode::Fixed,
            "非法 channel_card_width_mode 应回退到 fixed");
    require(
        loaded.gui.wave.channelDoubleClickAction == protoscope::plot::WaveChannelDoubleClickAction::ResetScaleOffset,
        "非法 channel_double_click_action 应回退到 reset_scale_offset");
    require(loaded.gui.wave.xAxisDoubleClickAction == protoscope::plot::WaveXAxisDoubleClickAction::FitFullHistory,
            "非法 x_axis_double_click_action 应回退到 fit_full_history");
    require(loaded.gui.wave.yAxisDoubleClickAction == protoscope::plot::WaveYAxisDoubleClickAction::FitVisibleChannels,
            "非法 y_axis_double_click_action 应回退到 fit_visible_channels");
    require(loaded.gui.wave.fullscreenMode == protoscope::config::GuiWaveFullscreenMode::Overlay,
            "非法 fullscreen_mode 应回退到 overlay");
    require(loaded.gui.rendererBackend == protoscope::config::GuiRendererBackend::OpenGL,
            "非法 renderer_backend 应回退到 opengl");
    require(loaded.gui.wave.mouseYOffsetDragMode == protoscope::plot::WaveMouseYOffsetDragMode::Direct,
            "非法 mouse_y_offset_drag_mode 应回退到 direct");
    require(loaded.gui.font.chineseGlyphRange == protoscope::config::GuiFontChineseGlyphRange::SimplifiedCommon,
            "非法 chinese_glyph_range 应回退到 simplified_common");
    require(std::abs(loaded.gui.wave.channelCardFixedWidth - 128.0) < 1e-12, "非正固定宽度应回退到 128");
    require(std::abs(loaded.gui.wave.channelCardAdaptiveRatio - 0.22) < 1e-12, "非正自适应比例应回退到 0.22");
    require(std::abs(loaded.gui.wave.verticalAutoFitMultiplier - 1.25) < 1e-12, "非正 Auto Fit 系数应回退到 1.25");
}

void test_config_logging_roundtrip()
{
    protoscope::config::ConfigStore store;
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-config-logging"));
    const auto tempPath = tempRoot.path() / "logging.yaml";

    auto base = store.load(tempPath).config;
    base.logging.level = protoscope::config::LogLevel::Trace;
    base.logging.filePath = "logs/protoscope.log";
    base.logging.payloadPreview.enabled = true;
    base.logging.payloadPreview.maxBytes = 8;
    base.logging.maxFileSizeBytes = 1024;
    base.logging.maxFiles = 2;

    std::string error;
    require(store.save(tempPath, base, error), "日志配置写回失败");
    auto reloaded = store.load(tempPath).config;
    require(reloaded.logging.level == protoscope::config::LogLevel::Trace, "trace 日志等级 roundtrip 失败");
    require(reloaded.logging.filePath == "logs/protoscope.log", "日志相对路径 roundtrip 失败");
    require(reloaded.logging.payloadPreview.enabled, "payload preview enabled roundtrip 失败");
    require(reloaded.logging.payloadPreview.maxBytes == 8, "payload preview max_bytes roundtrip 失败");
    require(reloaded.logging.maxFileSizeBytes == 1024, "日志轮转 max_file_size_bytes roundtrip 失败");
    require(reloaded.logging.maxFiles == 2, "日志轮转 max_files roundtrip 失败");

    base.logging.filePath.clear();
    require(store.save(tempPath, base, error), "空日志路径写回失败");
    reloaded = store.load(tempPath).config;
    require(reloaded.logging.filePath.empty(), "空日志路径应保持为空");

    const auto missingPath = tempRoot.path() / "missing.yaml";
    const auto missing = store.load(missingPath).config;
    require(missing.logging.filePath.empty(), "缺失日志路径时应默认为空");
    require(missing.logging.level == protoscope::config::LogLevel::Info, "缺失日志等级时应回退到 info");
    require(!missing.logging.payloadPreview.enabled, "payload preview 缺省应关闭");
    require(missing.logging.payloadPreview.maxBytes == 64, "payload preview max_bytes 缺省值错误");
}

void test_config_default_protocol_workspace_initializes_half_duplex_demos()
{
    protoscope::config::ConfigStore store;
    std::string error;

    require(store.ensureDefaultProtocolWorkspace(error), "protocols 工作区初始化失败");

    const auto protocolRoot = store.defaultProtocolDir().parent_path().parent_path();
    const auto templateRoot = protocolRoot / "templates";
    require(std::filesystem::exists(templateRoot / "default_protocol" / "main.lua"), "默认协议模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "lua_waveform_demo" / "main.lua"), "Lua 波形模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "half_duplex_modbus_master" / "main.lua"),
            "半双工主机模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "half_duplex_modbus_slave" / "main.lua"),
            "半双工从机模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "file_dialog" / "main.lua"), "文件对话框模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "send_file" / "main.lua"), "文件发送模板脚本应生成");
    require(std::filesystem::exists(templateRoot / "request_guarded" / "main.lua"), "受保护请求模板脚本应生成");
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
    require(readmeText.find("Lua 协议脚本指南") != std::string::npos, "默认 README 应包含 Lua 协议脚本指南");
}

void test_config_default_protocol_workspace_fills_missing_resources()
{
    protoscope::config::ConfigStore store;
    const auto protocolRoot = store.defaultProtocolDir().parent_path().parent_path();
    std::string error;

    std::filesystem::create_directories(protocolRoot);
    {
        std::ofstream out(protocolRoot / "keep.txt");
        out << "keep";
    }
    const auto defaultProtocolScript = protocolRoot / "templates" / "default_protocol" / "main.lua";
    std::filesystem::create_directories(defaultProtocolScript.parent_path());
    {
        std::ofstream out(defaultProtocolScript);
        out << "function ui()\n"
               "  return {{ id = 'protocol', title = '旧模板', controls = {}, layout = { kind = 'form' } }}\n"
               "end\n";
    }

    require(store.ensureDefaultProtocolWorkspace(error), "已有 protocols 根目录时初始化不应失败");
    require(std::filesystem::exists(protocolRoot / "keep.txt"), "已有 protocols 内容不应丢失");
    require(std::filesystem::exists(defaultProtocolScript), "已有 protocols 根目录时也应补齐默认模板");
    {
        std::ifstream input(defaultProtocolScript);
        std::stringstream buffer;
        buffer << input.rdbuf();
        const auto scriptText = buffer.str();
        require(scriptText.find("type = \"column\"") != std::string::npos, "旧版默认模板应刷新为当前 layout.type 格式");
        require(scriptText.find("kind = 'form'") == std::string::npos, "旧版默认模板内容不应残留");
    }
    require(std::filesystem::exists(protocolRoot / "templates" / "lua_waveform_demo" / "main.lua"),
            "已有 protocols 根目录时也应补齐波形模板");
    require(std::filesystem::exists(protocolRoot / "templates" / "file_dialog" / "main.lua"),
            "已有 protocols 根目录时也应补齐文件对话框模板");
    require(std::filesystem::exists(protocolRoot / "templates" / "send_file" / "main.lua"),
            "已有 protocols 根目录时也应补齐文件发送模板");
    require(std::filesystem::exists(protocolRoot / "templates" / "request_guarded" / "main.lua"),
            "已有 protocols 根目录时也应补齐受保护请求模板");
    require(std::filesystem::exists(protocolRoot / "README.md"), "已有 protocols 根目录时应补齐 README");
    require(std::filesystem::exists(protocolRoot / "protoscope_api.lua"),
            "已有 protocols 根目录时应补齐 LuaLS API 提示文件");
}

void test_protocol_scan_and_root_roundtrip()
{
    protoscope::config::ConfigStore store;
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-protocol-scan"));
    const auto alphaDir = tempRoot.path() / "alpha";
    const auto betaDir = tempRoot.path() / "beta";
    std::string error;

    require(store.ensureDefaultProtocolScript(alphaDir, error), "alpha 协议脚本补建失败");
    require(store.ensureDefaultProtocolScript(betaDir, error), "beta 协议脚本补建失败");

    const auto scanned = store.scanProtocolDirectories(tempRoot.path());
    require(scanned.size() == 2, "协议目录扫描数量不正确");
    require(scanned[0].find("alpha") != std::string::npos, "扫描结果应包含 alpha");
    require(scanned[1].find("beta") != std::string::npos, "扫描结果应包含 beta");

    const auto tempPath = tempRoot.path() / "protoscope-protocol-root-roundtrip.yaml";
    const auto missingLoad = store.load(tempPath);
    require(!missingLoad.loadedFromDisk, "缺失配置文件应使用默认配置");
    require(missingLoad.error.empty(), "普通缺失配置文件不应记录错误");
    const auto missingSnapshot = store.snapshot(tempPath);
    require(!missingSnapshot.exists && missingSnapshot.timestampMs == 0, "缺失配置文件快照应保持空状态");
    require(!store.hasChanged(missingSnapshot), "缺失配置文件重复快照不应误报变化");
    require(!store.protocolEntryExists(tempRoot.path() / "missing_protocol"), "缺失协议入口应返回 false");

    auto config = missingLoad.config;
    config.protocol.rootDir = tempRoot.path().generic_string();
    config.protocol.selectedDir = betaDir.generic_string();

    require(store.save(tempPath, config, error), "协议根目录保存失败");
    const auto reloaded = store.load(tempPath);
    require(reloaded.config.protocol.rootDir == tempRoot.path().generic_string(), "协议根目录 roundtrip 失败");
    require(reloaded.config.protocol.selectedDir == betaDir.generic_string(), "协议目录 roundtrip 失败");

    const auto normalized = store.normalizeProtocolDir(tempRoot.path(), tempRoot.path() / "missing");
    require(normalized == alphaDir, "root-aware 协议目录归一化应优先回退到当前 root 下的有效目录");

    const auto rootPrefixedRelative = std::filesystem::path{"protocols"} / "templates" / "beta";
    const auto normalizedRelative = store.normalizeProtocolDir(tempRoot.path(), rootPrefixedRelative);
    require(normalizedRelative == betaDir, "root-aware 协议目录归一化应按协议名解析 root 前缀相对路径");
}

void test_script_plot_api_snapshot()
{
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
    require(setups[0].channels[0].lineWidth.has_value(), "CH1 line_width 应解析为显式线宽");
    require(std::abs(*setups[0].channels[0].lineWidth - 2.5F) < 1e-6F, "CH1 line_width 解析错误");
    require(!setups[0].channels[1].lineWidth.has_value(), "CH2 未配置 line_width 时应保留默认样式");
    require(appends.size() == 2, "打开连接后应推送 2 组通道数据");
    require(appends[0].second.samples.size() == 3, "通道采样点数量不正确");
}

void test_script_plot_bit_display_config()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-bit-plot"));
    writeMainLua(protocolDir.path(), R"lua(
function on_open(ctx)
  proto.plot.setup({
    channels = {
      { label = "CH1", unit = "raw", bit_display = true },
      { label = "CH2", unit = "raw", bit_display = { first_bit = 4, bit_count = 12, y_offset = 2.5 } },
      { label = "CH3", unit = "raw", bit_display = { enabled = false, first_bit = 2, bit_count = 3 } },
    }
  })
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "bit_display 协议应可加载");
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{.context = sampleCtx()});

    const auto setups = host.drainPlotSetups();
    require(setups.size() == 1, "bit_display setup 应生成 1 次配置");
    require(setups[0].channels.size() == 3, "bit_display setup 应保留所有通道");
    require(setups[0].channels[0].bitDisplay.enabled, "bit_display=true 应启用 bit 显示");
    require(setups[0].channels[0].bitDisplay.firstBit == 0, "bit_display=true first_bit 默认值错误");
    require(setups[0].channels[0].bitDisplay.bitCount == 8, "bit_display=true bit_count 默认值错误");
    require(std::abs(setups[0].channels[0].bitDisplay.yOffset) < 1e-12, "bit_display=true y_offset 默认值错误");
    require(setups[0].channels[1].bitDisplay.enabled, "bit_display table 省略 enabled 时应默认启用");
    require(setups[0].channels[1].bitDisplay.firstBit == 4, "bit_display first_bit 解析错误");
    require(setups[0].channels[1].bitDisplay.bitCount == 12, "bit_display bit_count 解析错误");
    require(std::abs(setups[0].channels[1].bitDisplay.yOffset - 2.5) < 1e-12, "bit_display y_offset 解析错误");
    require(!setups[0].channels[2].bitDisplay.enabled, "bit_display.enabled=false 应关闭 bit 显示");
    require(setups[0].channels[2].bitDisplay.firstBit == 2, "关闭状态仍应保留 first_bit 配置");
    require(setups[0].channels[2].bitDisplay.bitCount == 3, "关闭状态仍应保留 bit_count 配置");
}

void test_script_plot_bit_display_rejects_invalid_config()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-bit-plot-invalid"));
    writeMainLua(protocolDir.path(), R"lua(
function on_open(ctx)
  proto.plot.setup({
    channels = {
      { label = "CH1", bit_display = { first_bit = 60, bit_count = 8 } },
    }
  })
end
)lua");

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "非法 bit_display 脚本本身应可加载");
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{.context = sampleCtx()});
    require(host.drainPlotSetups().empty(), "非法 bit_display 不应生成 plot.setup");

    bool hasBitDisplayError = false;
    for (const auto& log : host.drainLogs()) {
        if (log.message.find("bit_display.first_bit + bit_count 不允许超过 64") != std::string::npos) {
            hasBitDisplayError = true;
        }
    }
    require(hasBitDisplayError, "非法 bit_display 应记录明确字段错误");
}

void test_script_plot_push_accepts_compact_series()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-compact-plot"));
    {
        std::ofstream script(protocolDir.path() / "main.lua");
        script << R"lua(
function on_open(ctx)
    proto.plot.setup({
        channels = {
            { label = "CH1", unit = "V" }
        }
    })
    proto.plot.push(1, {
        source = "compact",
        t0 = 0.25,
        dt = 0.5,
        values = { 1.0, 2.0, 3.0 }
    })
end
)lua";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "compact plot 协议应可加载");
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{.context = sampleCtx()});

    const auto appends = host.drainPlotAppends();
    require(appends.size() == 1, "compact plot 应生成 1 组追加请求");
    require(appends[0].first == 0, "Lua 1-based 通道应转换为 C++ 0-based");
    require(appends[0].second.source == "compact", "compact plot 应保留 source");
    require(appends[0].second.samples.size() == 3, "compact plot 应展开 3 个采样");
    require(std::abs(appends[0].second.samples[0].time - 0.25) < 1e-12, "compact plot 起始时间错误");
    require(std::abs(appends[0].second.samples[1].time - 0.75) < 1e-12, "compact plot dt 展开错误");
    require(std::abs(appends[0].second.samples[2].value - 3.0) < 1e-12, "compact plot 数值展开错误");
}

void test_half_duplex_modbus_request_batches()
{
    protoscope::scripting::ScriptHost host;
    requireProtocolLoaded(host, "protocols/half_duplex_modbus_master");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    const auto setups = host.drainPlotSetups();
    require(setups.size() == 1, "半双工主站打开连接后应配置波形");
    require(setups[0].view.historyLimit == 30000U, "高速上传波形应限制实时显示历史");
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

void test_half_duplex_modbus_ack_and_plot_flow()
{
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
    require(appends.size() == 4, "120 帧上传批应按 4 个通道聚合推送");
    std::array<std::size_t, 4> perChannel{};
    std::size_t totalSamples = 0;
    for (const auto& append : appends) {
        require(append.first < perChannel.size(), "通道编号应落在 0~3");
        perChannel[append.first] += append.second.samples.size();
        totalSamples += append.second.samples.size();
    }
    require(totalSamples == 480, "总样本数应为 480");
    require(perChannel[0] == 120 && perChannel[1] == 120 && perChannel[2] == 120 && perChannel[3] == 120,
            "四个通道都应各收到 120 个样本");
    for (const auto& append : appends) {
        require(append.second.source == "sn_scope_upload", "上传波形应保留 source");
        require(std::abs(append.second.samples[0].time - 0.0) < 1e-12, "首批上传波形应从 0 秒开始");
        require(std::abs(append.second.samples[1].time - append.second.samples[0].time - (0.01 / 120.0)) < 1e-12,
                "compact 上传波形展开后应保持固定 dt");
    }
}

void test_half_duplex_modbus_ch3_uses_third_harmonic()
{
    protoscope::scripting::ScriptHost master;
    protoscope::scripting::ScriptHost slave;
    requireProtocolLoaded(master, "protocols/half_duplex_modbus_master");
    requireProtocolLoaded(slave, "protocols/half_duplex_modbus_slave");

    const auto ctx = sampleCtx();
    completeHalfDuplexStartup(master, slave, ctx);
    const auto frame = nextHalfDuplexWaveFrame(slave);

    constexpr double sampleRateHz = 12000.0;
    constexpr double fundamentalHz = 50.0;
    constexpr double thirdHarmonicRatio = 0.5;
    constexpr double channelScale = 1000.0;
    constexpr double phaseStep = 2.0 * 3.14159265358979323846 * fundamentalHz / sampleRateHz;

    for (std::size_t frameIndex = 0; frameIndex < 120; ++frameIndex) {
        const double phase = static_cast<double>(frameIndex + 1) * phaseStep;
        const auto expected =
            static_cast<int>(std::floor((std::sin(phase) + thirdHarmonicRatio * std::sin(3.0 * phase)) * channelScale));
        const auto actual = static_cast<int>(readBeI16(frame, frameIndex * 14 + 8));
        const auto diff = actual >= expected ? actual - expected : expected - actual;
        require(diff <= 1, "CH3 默认应输出 50Hz 基波叠加 150Hz 三次谐波");
    }
}

void test_half_duplex_modbus_loss_status_keeps_valid_frame()
{
    protoscope::scripting::ScriptHost master;
    requireProtocolLoaded(master, "protocols/half_duplex_modbus_master");

    const auto ctx = sampleCtx();
    master.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    master.drainPlotSetups();

    master.onTransportBytes(
        protoscope::transport::TransportBytesEvent{ctx, makeSnScopeUploadFrame(1, 100, 200, 300, 400)});
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

void test_half_duplex_modbus_ack_matching_rules()
{
    const auto runRequest =
        [](const char* controlId, const std::vector<std::uint8_t>& ack, bool expectedOk, const char* expectedStatus) {
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
    runRequest("read_gain", makeSnScopeFc03Response({1000U, 1000U, 1000U, 1000U}), true, "读取应答");

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

void test_half_duplex_modbus_stop_drains_late_upload_before_ack()
{
    protoscope::scripting::ScriptHost host;
    requireProtocolLoaded(host, "protocols/half_duplex_modbus_master");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.drainPlotSetups();

    host.onControl(ctx, "stop_stream", true);
    const auto requests = host.drainTxRequests();
    require(requests.size() == 1, "停止按钮应生成 1 条 stop_stream request");
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

    auto tailUploadAndAck = makeSnScopeUploadFrame(1, 100, 200, 300, 400);
    appendBytes(tailUploadAndAck, makeSnScopeFc06Ack(0x8888U, 0x0000U));
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, tailUploadAndAck});

    const auto appends = host.drainPlotAppends();
    require(appends.size() == 4, "停止阶段迟到上传帧仍应推送 4 路波形");
    const auto results = host.drainRequestDoneResults();
    require(results.size() == 1, "停止 ACK 应完成 stop_stream request");
    require(results[0].ok, "停止 ACK 匹配结果应为成功");
}

void test_half_duplex_modbus_crc_error_finishes_request()
{
    protoscope::scripting::ScriptHost host;
    requireProtocolLoaded(host, "protocols/half_duplex_modbus_master");

    const auto ctx = sampleCtx();
    host.onTransportOpen(protoscope::transport::TransportOpenEvent{ctx});
    host.drainPlotSetups();
    host.onControl(ctx, "start_stream", true);

    const auto requests = host.drainTxRequests();
    require(requests.size() == 1, "启动按钮应生成 1 条 request");
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

    auto brokenAck = makeSnScopeFc06Ack(0x8888U, 0x0001U);
    brokenAck.back() = static_cast<std::uint8_t>(brokenAck.back() ^ 0x01U);
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{ctx, brokenAck});

    const auto results = host.drainRequestDoneResults();
    require(results.size() == 1, "活动 request 遇到 CRC 错误应产生 request_done");
    require(!results[0].ok, "CRC 错误应结束为失败 request");

    bool foundWarn = false;
    for (const auto& update : host.drainStatusUpdates()) {
        if (update.level == "warn" && update.text.find("CRC 校验失败") != std::string::npos) {
            foundWarn = true;
        }
    }
    require(foundWarn, "CRC 错误应以 warn 状态提示");
}

void test_half_duplex_modbus_sticky_frames()
{
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
    require(appends.size() == 4, "两批粘包输入后应按通道聚合推送");
    std::size_t totalSamples = 0;
    for (const auto& append : appends) {
        totalSamples += append.second.samples.size();
    }
    require(totalSamples == 960, "两批粘包输入后样本总数应保持 960");
}

void test_half_duplex_modbus_noise_prefix_ignored()
{
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
    require(appends.size() == 4, "噪声前缀后仍应按通道聚合完整上传批");
}

void test_half_duplex_modbus_crc_resync_keeps_following_frame()
{
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
        if (update.level == "warn" && update.text.find("CRC") != std::string::npos) {
            foundCrcError = true;
        }
    }
    require(foundCrcError, "CRC 校验失败后应上报解析错误");
}

void test_half_duplex_modbus_multi_schema_candidates()
{
    const ScopedTempPath tempRoot(makeUniqueTempDir("protoscope-half-duplex-multi-schema"));
    const auto protocolDir = tempRoot.path() / "multi_schema";
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
        out << "        [FUNC_ALPHA] = { name = \"alpha\", fields = { { name = \"value\", type = \"u8\", offset = 0 } "
               "} },\n";
        out << "      },\n";
        out << "    },\n";
        out << "    {\n";
        out << "      id = FRAME_BETA,\n";
        out << "      header = { 0xB2, 0x2B },\n";
        out << "      sequence = { type = \"u8\", bits = 8 },\n";
        out << "      length = { type = \"u16\", endian = \"le\", unit = \"bytes\" },\n";
        out << "      messages = {\n";
        out << "        [FUNC_BETA] = { name = \"beta\", fields = { { name = \"value\", type = \"u16\", endian = "
               "\"le\", offset = 0 } } },\n";
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
        if (event.payload.find("alpha_frame") != std::string::npos &&
            event.payload.find("value=7") != std::string::npos) {
            foundAlpha = true;
        }
        if (event.payload.find("beta_frame") != std::string::npos && event.payload.find("4660") != std::string::npos) {
            foundBeta = true;
        }
    }
    require(foundAlpha, "应按首个候选 schema 解析 alpha_frame");
    require(foundBeta, "应按第二个候选 schema 解析 beta_frame");
}

void test_script_value_table_parse_and_update()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-value-table-parse"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "value_table 测试脚本应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"safe\", title = \"Safe\", controls = {\n";
        out << "    { type = \"value_table\", id = \"regs\", label = \"寄存器\", rows = {\n";
        out << "      { id = 0x1010, label = \"电压\", unit = \"V\", note = \"母线\" },\n";
        out << "      { id = 0x1020, label = \"状态字\",\n";
        out << "        bits = {\n";
        out << "          { bit = 0, label = \"运行\", values = { [0] = \"停\", [1] = \"转\" } },\n";
        out << "          { bit = 31, label = \"高位\", values = { [0] = \"低\", [1] = \"高\" } },\n";
        out << "        },\n";
        out << "      },\n";
        out << "      { start_id = 0x1030, len = 2, labels = { \"温度\", \"湿度\" }, units = { \"C\", \"%\" } },\n";
        out << "    } },\n";
        out << "  } } }\n";
        out << "end\n";
        out << "function on_open(ctx)\n";
        out << "  proto.set_control(\"regs\", { [0x1020] = 0x80000000 })\n";
        out << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "value_table 协议应可加载");

    auto controls = host.controlsSnapshot();
    const protoscope::scripting::ControlDescriptor* desc = nullptr;
    for (const auto& c : controls) {
        if (c.id == "regs") {
            desc = &c;
            break;
        }
    }
    require(desc != nullptr, "应能找到 regs 控件");
    require(desc->type == protoscope::scripting::ControlType::ValueTable, "regs 应为 value_table 类型");
    require(desc->valueRows.size() == 5, "应展开为 5 行: 1 普通 + 2 bit + 2 range");
    require(desc->valueRows[0].id == 0x1010U, "第一行 id 应为 0x1010");
    require(desc->valueRows[0].label == "电压", "第一行 label 应为 电压");
    require(desc->valueRows[0].note == "母线", "第一行 note 应为 母线");
    require(desc->valueRows[1].bit.has_value() && *desc->valueRows[1].bit == 0, "bit0 行");
    require(desc->valueRows[2].bit.has_value() && *desc->valueRows[2].bit == 31, "bit31 行");
    require(desc->valueRows[3].id == 0x1030U, "range 首行 id 应为 0x1030");
    require(desc->valueRows[4].id == 0x1031U, "range 第二行 id 应为 0x1031");
    require(desc->valueRowById.size() == 3, "普通+range 行共计 3 个 row id 索引");
    require(desc->valueBitRowsBySourceId.size() == 1, "应有一个 bit 源 id 索引");

    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});

    auto controlsAfterOpen = host.controlStatesSnapshot();
    const protoscope::scripting::ControlSnapshot* opened = nullptr;
    for (const auto& s : controlsAfterOpen) {
        if (s.descriptor.id == "regs") {
            opened = &s;
        }
    }
    require(opened != nullptr, "regs 应在打开后快照中");
    const auto* openedTable = std::get_if<protoscope::scripting::ValueTableValue>(&opened->value);
    require(openedTable != nullptr, "打开后值应为 ValueTableValue");
    require(openedTable->rows[1].set && openedTable->rows[1].value == "停", "bit0 应展开为 停");
    require(openedTable->rows[2].set && openedTable->rows[2].value == "高", "bit31 应展开为 高");

    {
        using namespace protoscope::scripting;
        ValueTableValue patch;
        patch.rows.resize(desc->valueRows.size());
        patch.rows[0].value = "220.1";
        patch.rows[0].set = true;
        host.onControl(sampleCtx(), "regs", patch);
    }

    const auto snapshot = host.controlStatesSnapshot();
    const protoscope::scripting::ControlSnapshot* cs = nullptr;
    for (const auto& s : snapshot) {
        if (s.descriptor.id == "regs") {
            cs = &s;
        }
    }
    require(cs != nullptr, "regs 应在快照中");
    const auto* tv = std::get_if<protoscope::scripting::ValueTableValue>(&cs->value);
    require(tv != nullptr, "值应为 ValueTableValue");
    require(tv->rows[0].set && tv->rows[0].value == "220.1", "电压应为 220.1");

    {
        using namespace protoscope::scripting;
        ValueTableValue patch;
        patch.rows.resize(desc->valueRows.size());
        patch.rows[3].value = "25.3";
        patch.rows[3].set = true;
        patch.rows[4].value = "68.1";
        patch.rows[4].set = true;
        host.onControl(sampleCtx(), "regs", patch);
    }

    const auto snapshot2 = host.controlStatesSnapshot();
    const protoscope::scripting::ControlSnapshot* cs2 = nullptr;
    for (const auto& s : snapshot2) {
        if (s.descriptor.id == "regs") {
            cs2 = &s;
        }
    }
    require(cs2 != nullptr, "regs 应在快照中");
    const auto* tvFinal = std::get_if<protoscope::scripting::ValueTableValue>(&cs2->value);
    require(tvFinal != nullptr && tvFinal->rows[3].value == "25.3" && tvFinal->rows[4].value == "68.1",
            "range 更新应正确写入");
    require(tvFinal->rows[0].value == "220.1", "之前的普通行更新应保留");
    require(tvFinal->rows[1].value == "停", "bit0 更新应保留");
    require(tvFinal->rows[2].value == "高", "bit31 更新应保留");
}

void test_script_value_table_bit_integer_and_bytes_sources()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-value-table-bit-sources"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "value_table bit source 测试脚本应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"safe\", title = \"Safe\", controls = {\n";
        out << "    { type = \"value_table\", id = \"regs\", label = \"寄存器\", rows = {\n";
        out << "      { id = 0x2000, label = \"大整数\",\n";
        out << "        bits = {\n";
        out << "          { bit = 32, label = \"bit32\" },\n";
        out << "          { bit = 63, label = \"bit63\" },\n";
        out << "          { bit = 64, label = \"bit64\" },\n";
        out << "        },\n";
        out << "      },\n";
        out << "      { id = 0x2001, label = \"字节数组\",\n";
        out << "        bits = {\n";
        out << "          { bit = 0, label = \"arr_bit0\" },\n";
        out << "          { bit = 7, label = \"arr_bit7\" },\n";
        out << "          { bit = 8, label = \"arr_bit8\" },\n";
        out << "          { bit = 15, label = \"arr_bit15\" },\n";
        out << "        },\n";
        out << "      },\n";
        out << "      { id = 0x2002, label = \"HEX 字符串\",\n";
        out << "        bits = {\n";
        out << "          { bit = 0, label = \"hex_bit0\" },\n";
        out << "          { bit = 7, label = \"hex_bit7\" },\n";
        out << "          { bit = 8, label = \"hex_bit8\" },\n";
        out << "          { bit = 15, label = \"hex_bit15\" },\n";
        out << "        },\n";
        out << "      },\n";
        out << "    } },\n";
        out << "  } } }\n";
        out << "end\n";
        out << "function on_open(ctx)\n";
        out << "  proto.set_control(\"regs\", {\n";
        out << "    [0x2000] = \"0x8000000100000000\",\n";
        out << "    [0x2001] = { 0x81, 0x80 },\n";
        out << "    [0x2002] = \"81 80\",\n";
        out << "  })\n";
        out << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "value_table bit source 协议应可加载");

    const auto controls = host.controlsSnapshot();
    const protoscope::scripting::ControlDescriptor* desc = nullptr;
    for (const auto& c : controls) {
        if (c.id == "regs") {
            desc = &c;
            break;
        }
    }
    require(desc != nullptr, "应能找到 regs 控件");
    require(desc->valueRows.size() == 11, "应展开 11 个 bit 行");
    require(desc->valueRows[0].bit.has_value() && *desc->valueRows[0].bit == 32U, "bit32 不应被配置拒绝");
    require(desc->valueRows[1].bit.has_value() && *desc->valueRows[1].bit == 63U, "bit63 不应被配置拒绝");
    require(desc->valueRows[2].bit.has_value() && *desc->valueRows[2].bit == 64U, "bit64 不应被配置拒绝");

    host.onTransportOpen(protoscope::transport::TransportOpenEvent{sampleCtx()});

    const auto snapshot = host.controlStatesSnapshot();
    const protoscope::scripting::ControlSnapshot* control = nullptr;
    for (const auto& item : snapshot) {
        if (item.descriptor.id == "regs") {
            control = &item;
            break;
        }
    }
    require(control != nullptr, "regs 应在打开后快照中");
    const auto* table = std::get_if<protoscope::scripting::ValueTableValue>(&control->value);
    require(table != nullptr, "打开后值应为 ValueTableValue");
    require(table->rows[0].set && table->rows[0].value == "1", "0x 字符串整数应展开 bit32");
    require(table->rows[1].set && table->rows[1].value == "1", "0x 字符串整数应展开 bit63");
    require(table->rows[2].set && table->rows[2].value == "0", "整数源超过 bit63 时应按 0 展开");
    require(table->rows[3].value == "1" && table->rows[4].value == "1", "number[] 应展开第 1 字节 bit0/bit7");
    require(table->rows[5].value == "0" && table->rows[6].value == "1", "number[] 应按第 2 字节低位优先展开");
    require(table->rows[7].value == table->rows[3].value && table->rows[8].value == table->rows[4].value &&
                table->rows[9].value == table->rows[5].value && table->rows[10].value == table->rows[6].value,
            "HEX 字符串 bytes 应与 number[] 展开结果一致");
}

void test_script_value_table_stream_value_targets_bit_sources()
{
    const ScopedTempPath protocolDir(makeUniqueTempDir("protoscope-value-table-stream-targets"));
    {
        std::ofstream out(protocolDir.path() / "main.lua");
        require(out.good(), "value_table stream target 测试脚本应可写入");
        out << "function ui()\n";
        out << "  return { { id = \"safe\", title = \"Safe\", controls = {\n";
        out << "    { type = \"value_table\", id = \"regs\", label = \"寄存器\", rows = {\n";
        out << "      { id = 0x3000, label = \"状态值\", bits = {\n";
        out << "        { bit = 31, label = \"u32_bit31\" },\n";
        out << "        { bit = 32, label = \"u32_bit32\" },\n";
        out << "      } },\n";
        out << "      { id = 0x3001, label = \"字节位\", bits = {\n";
        out << "        { bit = 0, label = \"byte_bit0\" },\n";
        out << "        { bit = 7, label = \"byte_bit7\" },\n";
        out << "        { bit = 8, label = \"byte_bit8\" },\n";
        out << "        { bit = 15, label = \"byte_bit15\" },\n";
        out << "      } },\n";
        out << "    } },\n";
        out << "  } } }\n";
        out << "end\n";
        out << "local function on_stream_frame(ctx, frame)\n";
        out << "  proto.emit(\"stream_value_targets\", \"ok\")\n";
        out << "end\n";
        out << "function stream()\n";
        out << "  return { frames = { {\n";
        out << "    name = \"sample\",\n";
        out << "    header = { 0xAA, 0x55 },\n";
        out << "    size = 8,\n";
        out << "    fields = {\n";
        out << "      { name = \"status\", type = \"u32_le\", offset = 3 },\n";
        out << "      { name = \"flags\", type = \"bytes\", offset = 7, count = 2 },\n";
        out << "    },\n";
        out << "    value_targets = { controls = {\n";
        out << "      { id = \"regs\", start_id = 0x3000, values_field = \"status\" },\n";
        out << "      { id = \"regs\", start_id = 0x3001, values_field = \"flags\" },\n";
        out << "    } },\n";
        out << "    on_frame = on_stream_frame,\n";
        out << "  } } }\n";
        out << "end\n";
    }

    protoscope::scripting::ScriptHost host;
    require(host.loadProtocolDirectory(protocolDir.path().generic_string()), "value_table stream target 协议应可加载");
    host.onTransportBytes(protoscope::transport::TransportBytesEvent{
        sampleCtx(), std::vector<std::uint8_t>{0xAA, 0x55, 0x00, 0x00, 0x00, 0x80, 0x81, 0x80}});

    const auto snapshot = host.controlStatesSnapshot();
    const protoscope::scripting::ControlSnapshot* control = nullptr;
    for (const auto& item : snapshot) {
        if (item.descriptor.id == "regs") {
            control = &item;
            break;
        }
    }
    require(control != nullptr, "regs 应在 stream 后快照中");
    const auto* table = std::get_if<protoscope::scripting::ValueTableValue>(&control->value);
    require(table != nullptr, "stream 后值应为 ValueTableValue");
    require(table->rows[0].set && table->rows[0].value == "1", "u32 value_targets 应展开 bit31");
    require(table->rows[1].set && table->rows[1].value == "0", "u32 value_targets 应允许声明 bit32 并展开为 0");
    require(table->rows[2].value == "1" && table->rows[3].value == "1", "bytes value_targets 应展开第 1 字节");
    require(table->rows[4].value == "0" && table->rows[5].value == "1",
            "bytes value_targets 不应按 range 拆分，应作为单个低位优先 bit 源");
}

namespace {

static const TestCase kAllTests[] = {
    {"hex_roundtrip", &test_hex_roundtrip},
    {"hex_invalid_input", &test_hex_invalid_input},
    {"hex_normalize_input", &test_hex_normalize_input},
    {"hex_editor_cursor_normalize", &test_hex_editor_cursor_normalize},
    {"crc_known_vectors", &test_crc_known_vectors},
    {"keyboard_shortcut_table_has_no_scope_duplicates", &test_keyboard_shortcut_table_has_no_scope_duplicates},
    {"keyboard_shortcut_labels_match_plan", &test_keyboard_shortcut_labels_match_plan},
    {"algorithm_help_search_empty_query_has_no_match", &test_algorithm_help_search_empty_query_has_no_match},
    {"algorithm_help_search_chinese_keyword", &test_algorithm_help_search_chinese_keyword},
    {"algorithm_help_search_english_case_insensitive", &test_algorithm_help_search_english_case_insensitive},
    {"algorithm_help_search_multiple_terms_require_all", &test_algorithm_help_search_multiple_terms_require_all},
    {"algorithm_help_search_no_match", &test_algorithm_help_search_no_match},
    {"algorithm_help_search_navigation_wraps", &test_algorithm_help_search_navigation_wraps},
    {"algorithm_help_search_fft_unit_keywords", &test_algorithm_help_search_fft_unit_keywords},
    {"startup_diagnostics_parse_diagnose_arg", &test_startup_diagnostics_parse_diagnose_arg},
    {"startup_diagnostics_parse_default_off", &test_startup_diagnostics_parse_default_off},
#if defined(_WIN32)
    {"startup_windows_arg_to_utf8_ascii_exe", &test_startup_windows_arg_to_utf8_ascii_exe},
    {"startup_windows_arg_to_utf8_renderer_equals", &test_startup_windows_arg_to_utf8_renderer_equals},
    {"startup_windows_arg_to_utf8_chinese_and_empty", &test_startup_windows_arg_to_utf8_chinese_and_empty},
#endif
    {"startup_diagnostics_parse_renderer_equals", &test_startup_diagnostics_parse_renderer_equals},
    {"startup_diagnostics_parse_renderer_space", &test_startup_diagnostics_parse_renderer_space},
    {"startup_diagnostics_parse_renderer_missing_value", &test_startup_diagnostics_parse_renderer_missing_value},
    {"startup_diagnostics_parse_renderer_invalid_value", &test_startup_diagnostics_parse_renderer_invalid_value},
    {"startup_diagnostics_parse_renderer_with_diagnose", &test_startup_diagnostics_parse_renderer_with_diagnose},
    {"startup_diagnostics_parse_renderer_probe_enables_diagnose",
     &test_startup_diagnostics_parse_renderer_probe_enables_diagnose},
    {"startup_renderer_backend_priority", &test_startup_renderer_backend_priority},
    {"startup_diagnostics_log_path_fallback", &test_startup_diagnostics_log_path_fallback},
    {"startup_diagnostics_construct_with_diagnose_writes_process_start",
     &test_startup_diagnostics_construct_with_diagnose_writes_process_start},
    {"startup_diagnostics_header_writes_selected_path_and_attempts",
     &test_startup_diagnostics_header_writes_selected_path_and_attempts},
    {"startup_diagnostics_log_failure_no_diagnose_does_not_create_log",
     &test_startup_diagnostics_log_failure_no_diagnose_does_not_create_log},
    {"startup_early_diagnostics_default_stage_does_not_create_log",
     &test_startup_early_diagnostics_default_stage_does_not_create_log},
    {"startup_early_diagnostics_diagnose_writes_fixed_log", &test_startup_early_diagnostics_diagnose_writes_fixed_log},
    {"startup_early_diagnostics_crash_without_diagnose_creates_log",
     &test_startup_early_diagnostics_crash_without_diagnose_creates_log},
    {"startup_early_diagnostics_crash_falls_back_to_temp_log",
     &test_startup_early_diagnostics_crash_falls_back_to_temp_log},
    {"startup_diagnostics_write_crash_no_diagnose_does_not_create_log",
     &test_startup_diagnostics_write_crash_no_diagnose_does_not_create_log},
    {"startup_diagnostics_set_stage_with_diagnose_appends_stage",
     &test_startup_diagnostics_set_stage_with_diagnose_appends_stage},
    {"startup_diagnostics_command_line_parsed_logs_renderer_cli",
     &test_startup_diagnostics_command_line_parsed_logs_renderer_cli},
    {"startup_diagnostics_append_failure_state_records_stage",
     &test_startup_diagnostics_append_failure_state_records_stage},
    {"startup_diagnostics_log_failure_with_diagnose_writes_stage_reason",
     &test_startup_diagnostics_log_failure_with_diagnose_writes_stage_reason},
    {"startup_diagnostics_report_omits_config_body", &test_startup_diagnostics_report_omits_config_body},
    {"startup_diagnostics_fatal_message_includes_stage_reason_log",
     &test_startup_diagnostics_fatal_message_includes_stage_reason_log},
    {"startup_diagnostics_fatal_message_includes_diagnostics_state",
     &test_startup_diagnostics_fatal_message_includes_diagnostics_state},
    {"startup_diagnostics_fatal_message_omits_empty_log_path",
     &test_startup_diagnostics_fatal_message_omits_empty_log_path},
    {"config_external_reload_state", &test_config_external_reload_state},
    {"request_trace_filter_and_clear", &test_request_trace_filter_and_clear},
    {"request_trace_csv_export_format", &test_request_trace_csv_export_format},
    {"script_controls_snapshot", &test_script_controls_snapshot},
    {"script_load_directory_rejected_before_lua_dofile", &test_script_load_directory_rejected_before_lua_dofile},
    {"script_optional_labels_allowed_for_compact_controls", &test_script_optional_labels_allowed_for_compact_controls},
    {"script_required_labels_still_reject_visual_controls", &test_script_required_labels_still_reject_visual_controls},
    {"default_protocol_logs_elf_symbol_info", &test_default_protocol_logs_elf_symbol_info},
    {"script_elf_symbol_combo_descriptor_defaults", &test_script_elf_symbol_combo_descriptor_defaults},
    {"script_elf_symbol_combo_invalid_config_fails", &test_script_elf_symbol_combo_invalid_config_fails},
    {"script_elf_symbol_combo_get_control_returns_table", &test_script_elf_symbol_combo_get_control_returns_table},
    {"script_value_table_parse_and_update", &test_script_value_table_parse_and_update},
    {"script_value_table_bit_integer_and_bytes_sources", &test_script_value_table_bit_integer_and_bytes_sources},
    {"script_value_table_stream_value_targets_bit_sources", &test_script_value_table_stream_value_targets_bit_sources},
    {"script_on_open_log", &test_script_on_open_log},
    {"script_on_close_log", &test_script_on_close_log},
    {"script_on_error_log", &test_script_on_error_log},
    {"script_proto_log_trace_level", &test_script_proto_log_trace_level},
    {"script_multi_dock_snapshot", &test_script_multi_dock_snapshot},
    {"script_dock_layout_fields", &test_script_dock_layout_fields},
    {"script_table_layout_snapshot", &test_script_table_layout_snapshot},
    {"script_ui_descriptor_sugar_snapshot", &test_script_ui_descriptor_sugar_snapshot},
    {"script_ui_descriptor_sugar_layout_validation_failures",
     &test_script_ui_descriptor_sugar_layout_validation_failures},
    {"script_form_layout_snapshot", &test_script_form_layout_snapshot},
    {"script_flow_layout_snapshot", &test_script_flow_layout_snapshot},
    {"script_layout_width_controls_shorthand_snapshot", &test_script_layout_width_controls_shorthand_snapshot},
    {"script_inline_group_layout_snapshot", &test_script_inline_group_layout_snapshot},
    {"script_duplicate_label_controls_allowed", &test_script_duplicate_label_controls_allowed},
    {"script_crc_bridge", &test_script_crc_bridge},
    {"script_read_version_flow", &test_script_read_version_flow},
    {"script_read_version_split_flow", &test_script_read_version_split_flow},
    {"script_stream_schema_legacy_on_bytes_still_works", &test_script_stream_schema_legacy_on_bytes_still_works},
    {"script_stream_schema_bypasses_on_bytes_and_calls_on_frame",
     &test_script_stream_schema_bypasses_on_bytes_and_calls_on_frame},
    {"script_stream_schema_prefers_on_batch_over_on_frame", &test_script_stream_schema_prefers_on_batch_over_on_frame},
    {"script_stream_schema_allows_on_batch_without_on_frame",
     &test_script_stream_schema_allows_on_batch_without_on_frame},
    {"script_stream_schema_reports_overflow_and_crc_error", &test_script_stream_schema_reports_overflow_and_crc_error},
    {"script_stream_runtime_profile_set_and_clear", &test_script_stream_runtime_profile_set_and_clear},
    {"script_stream_raw_output_omit_skips_frame_raw", &test_script_stream_raw_output_omit_skips_frame_raw},
    {"script_stream_low_overhead_keeps_fields_and_trims_last_batch",
     &test_script_stream_low_overhead_keeps_fields_and_trims_last_batch},
    {"script_stream_field_output_fields_only_omits_top_aliases",
     &test_script_stream_field_output_fields_only_omits_top_aliases},
    {"script_stream_schema_reload_uses_current_callbacks", &test_script_stream_schema_reload_uses_current_callbacks},
    {"script_stream_schema_rejects_count_function", &test_script_stream_schema_rejects_count_function},
    {"script_stream_schema_accepts_count_expression_table", &test_script_stream_schema_accepts_count_expression_table},
    {"script_timeout_flow", &test_script_timeout_flow},
    {"frame_stream_parser_waits_for_full_frame", &test_frame_stream_parser_waits_for_full_frame},
    {"frame_stream_parser_handles_sticky_frames_and_noise_prefix",
     &test_frame_stream_parser_handles_sticky_frames_and_noise_prefix},
    {"frame_stream_parser_crc_resync_keeps_following_frame",
     &test_frame_stream_parser_crc_resync_keeps_following_frame},
    {"frame_stream_parser_reports_overflow_drop_oldest", &test_frame_stream_parser_reports_overflow_drop_oldest},
    {"frame_stream_parser_default_grows_without_drop_oldest",
     &test_frame_stream_parser_default_grows_without_drop_oldest},
    {"frame_stream_parser_near_overflow_threshold", &test_frame_stream_parser_near_overflow_threshold},
    {"frame_stream_parser_large_chunk_keeps_latest_window", &test_frame_stream_parser_large_chunk_keeps_latest_window},
    {"frame_stream_parser_supports_fixed_size_raw_frame", &test_frame_stream_parser_supports_fixed_size_raw_frame},
    {"frame_stream_parser_prefers_longer_same_header_candidate",
     &test_frame_stream_parser_prefers_longer_same_header_candidate},
    {"frame_stream_parser_count_expression_arithmetic", &test_frame_stream_parser_count_expression_arithmetic},
    {"frame_stream_parser_count_expression_remaining_if_flag_and_case",
     &test_frame_stream_parser_count_expression_remaining_if_flag_and_case},
    {"frame_stream_parser_multi_schema_large_chunk_throughput",
     &test_frame_stream_parser_multi_schema_large_chunk_throughput},
    {"frame_stream_parser_crc_frame_across_chunks", &test_frame_stream_parser_crc_frame_across_chunks},
    {"frame_stream_parser_can_omit_success_frame_raw", &test_frame_stream_parser_can_omit_success_frame_raw},
    {"frame_stream_parser_overflow_keeps_latest_crc_window",
     &test_frame_stream_parser_overflow_keeps_latest_crc_window},
    {"frame_stream_parser_runtime_profile_length_and_channel_map",
     &test_frame_stream_parser_runtime_profile_length_and_channel_map},
    {"frame_stream_parser_runtime_profile_errors", &test_frame_stream_parser_runtime_profile_errors},
    {"frame_stream_parser_rejects_unsafe_count_bounds", &test_frame_stream_parser_rejects_unsafe_count_bounds},
    {"frame_stream_parser_runtime_profile_truncated_fields",
     &test_frame_stream_parser_runtime_profile_truncated_fields},
    {"luals_api_sync_contains_tx_and_dialog_api", &test_luals_api_sync_contains_tx_and_dialog_api},
    {"script_oscilloscope_toggle_returns_true_and_receives_args",
     &test_script_oscilloscope_toggle_returns_true_and_receives_args},
    {"script_oscilloscope_toggle_returns_false", &test_script_oscilloscope_toggle_returns_false},
    {"script_oscilloscope_set_running_outputs_update", &test_script_oscilloscope_set_running_outputs_update},
    {"script_oscilloscope_toggle_true_defaults_target_update",
     &test_script_oscilloscope_toggle_true_defaults_target_update},
    {"script_oscilloscope_toggle_explicit_set_running_overrides_default",
     &test_script_oscilloscope_toggle_explicit_set_running_overrides_default},
    {"script_oscilloscope_toggle_false_allows_later_set_running",
     &test_script_oscilloscope_toggle_false_allows_later_set_running},
    {"script_oscilloscope_toggle_missing_callback_rejects", &test_script_oscilloscope_toggle_missing_callback_rejects},
    {"script_oscilloscope_toggle_non_function_rejects_and_logs",
     &test_script_oscilloscope_toggle_non_function_rejects_and_logs},
    {"script_oscilloscope_toggle_runtime_error_rejects_and_logs",
     &test_script_oscilloscope_toggle_runtime_error_rejects_and_logs},
    {"script_oscilloscope_toggle_non_boolean_rejects_and_logs",
     &test_script_oscilloscope_toggle_non_boolean_rejects_and_logs},
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
    {"script_layout_unknown_type_fail", &test_script_layout_unknown_type_fail},
    {"script_layout_width_range_fail", &test_script_layout_width_range_fail},
    {"script_layout_width_type_fail", &test_script_layout_width_type_fail},
    {"script_layout_fill_width_type_fail", &test_script_layout_fill_width_type_fail},
    {"script_inline_group_fill_width_type_fail", &test_script_inline_group_fill_width_type_fail},
    {"script_layout_width_non_positive_fail", &test_script_layout_width_non_positive_fail},
    {"script_layout_children_controls_conflict_fail", &test_script_layout_children_controls_conflict_fail},
    {"script_layout_shortcut_unknown_control_fail", &test_script_layout_shortcut_unknown_control_fail},
    {"script_layout_shortcut_duplicate_control_fail", &test_script_layout_shortcut_duplicate_control_fail},
    {"script_layout_shortcut_missing_control_fail", &test_script_layout_shortcut_missing_control_fail},
    {"script_inline_group_children_controls_conflict_fail", &test_script_inline_group_children_controls_conflict_fail},
    {"script_inline_group_unknown_control_fail", &test_script_inline_group_unknown_control_fail},
    {"script_inline_group_child_layout_fail", &test_script_inline_group_child_layout_fail},
    {"script_inline_group_min_width_fail", &test_script_inline_group_min_width_fail},
    {"script_short_label_type_fail", &test_script_short_label_type_fail},
    {"script_short_label_empty_fail", &test_script_short_label_empty_fail},
    {"script_compact_label_below_type_fail", &test_script_compact_label_below_type_fail},
    {"script_compact_label_below_non_positive_fail", &test_script_compact_label_below_non_positive_fail},
    {"script_invalid_label_position_fail", &test_script_invalid_label_position_fail},
    {"script_runtime_error_logged", &test_script_runtime_error_logged},
    {"script_reload_invalid_types_fail_without_throw", &test_script_reload_invalid_types_fail_without_throw},
    {"script_non_function_callbacks_only_log_errors", &test_script_non_function_callbacks_only_log_errors},
    {"script_failed_reload_keeps_previous_runtime", &test_script_failed_reload_keeps_previous_runtime},
    {"gui_runtime_version_utils", &test_gui_runtime_version_utils},
    {"update_check_evaluates_newer_version", &test_update_check_evaluates_newer_version},
    {"update_check_reports_up_to_date_for_exact_tag", &test_update_check_reports_up_to_date_for_exact_tag},
    {"update_check_reports_development_build", &test_update_check_reports_development_build},
    {"update_check_rejects_response_without_semantic_tags", &test_update_check_rejects_response_without_semantic_tags},
    {"protocol_directory_reload", &test_protocol_directory_reload},
    {"protocol_ui_templates_load", &test_protocol_ui_templates_load},
    {"config_default_roundtrip", &test_config_default_roundtrip},
    {"config_wave_mouse_y_offset_drag_mode_apply_capture", &test_config_wave_mouse_y_offset_drag_mode_apply_capture},
    {"config_repo_default_yaml_loads", &test_config_repo_default_yaml_loads},
    {"config_performance_scale_applies_default_budgets", &test_config_performance_scale_applies_default_budgets},
    {"config_performance_save_keeps_scaled_defaults_compact",
     &test_config_performance_save_keeps_scaled_defaults_compact},
    {"config_wave_mode_invalid_fallback", &test_config_wave_mode_invalid_fallback},
    {"config_logging_roundtrip", &test_config_logging_roundtrip},
    {"config_default_protocol_workspace_initializes_half_duplex_demos",
     &test_config_default_protocol_workspace_initializes_half_duplex_demos},
    {"config_default_protocol_workspace_fills_missing_resources",
     &test_config_default_protocol_workspace_fills_missing_resources},
    {"script_file_io_proto_buffer_roundtrip", &test_script_file_io_proto_buffer_roundtrip},
    {"protocol_scan_and_root_roundtrip", &test_protocol_scan_and_root_roundtrip},
    {"protocol_state_file_backs_up_corrupt_yaml", &test_protocol_state_file_backs_up_corrupt_yaml},
    {"protocol_state_file_atomic_write_replaces_valid_yaml",
     &test_protocol_state_file_atomic_write_replaces_valid_yaml},
    {"protocol_state_file_preserves_other_protocol_nodes", &test_protocol_state_file_preserves_other_protocol_nodes},
    {"protocol_state_file_roundtrips_elf_path_per_protocol",
     &test_protocol_state_file_roundtrips_elf_path_per_protocol},
    {"protocol_state_file_replace_failure_keeps_target", &test_protocol_state_file_replace_failure_keeps_target},
    {"script_plot_api_snapshot", &test_script_plot_api_snapshot},
    {"script_plot_bit_display_config", &test_script_plot_bit_display_config},
    {"script_plot_bit_display_rejects_invalid_config", &test_script_plot_bit_display_rejects_invalid_config},
    {"script_plot_push_accepts_compact_series", &test_script_plot_push_accepts_compact_series},
    {"half_duplex_modbus_request_batches", &test_half_duplex_modbus_request_batches},
    {"half_duplex_modbus_ack_and_plot_flow", &test_half_duplex_modbus_ack_and_plot_flow},
    {"half_duplex_modbus_ch3_uses_third_harmonic", &test_half_duplex_modbus_ch3_uses_third_harmonic},
    {"half_duplex_modbus_loss_status_keeps_valid_frame", &test_half_duplex_modbus_loss_status_keeps_valid_frame},
    {"half_duplex_modbus_ack_matching_rules", &test_half_duplex_modbus_ack_matching_rules},
    {"half_duplex_modbus_stop_drains_late_upload_before_ack",
     &test_half_duplex_modbus_stop_drains_late_upload_before_ack},
    {"half_duplex_modbus_crc_error_finishes_request", &test_half_duplex_modbus_crc_error_finishes_request},
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
    {"wave_protocol_state_missing_wave_node_clears_analysis_markers",
     &test_wave_protocol_state_missing_wave_node_clears_analysis_markers},
    {"wave_protocol_state_hidden_channel_indices_roundtrip_and_legacy_labels",
     &test_wave_protocol_state_hidden_channel_indices_roundtrip_and_legacy_labels},
    {"wave_protocol_state_cursor_extreme_snap_policy", &test_wave_protocol_state_cursor_extreme_snap_policy},
    {"wave_protocol_state_prefer_waveform_hover_readout_defaults_true",
     &test_wave_protocol_state_prefer_waveform_hover_readout_defaults_true},
    {"wave_protocol_state_view_mode_legend_overlay_and_color_override",
     &test_wave_protocol_state_view_mode_legend_overlay_and_color_override},
    {"wave_protocol_state_glow_phosphor_roundtrip", &test_wave_protocol_state_glow_phosphor_roundtrip},
    {"wave_protocol_state_legacy_phosphor_glow_only_migrates_to_glow",
     &test_wave_protocol_state_legacy_phosphor_glow_only_migrates_to_glow},
    {"dock_visibility_state_isolated_by_protocol_key", &test_dock_visibility_state_isolated_by_protocol_key},
    {"dock_visibility_state_decode_missing_fields_defaults",
     &test_dock_visibility_state_decode_missing_fields_defaults},
    {"lua_dock_layout_key_uses_protocol_and_script", &test_lua_dock_layout_key_uses_protocol_and_script},
    {"lua_dock_layout_key_falls_back_to_script_directory", &test_lua_dock_layout_key_falls_back_to_script_directory},
    {"lua_dock_layout_paths_prefer_user_layout", &test_lua_dock_layout_paths_prefer_user_layout},
    {"lua_dock_layout_paths_detect_legacy_source", &test_lua_dock_layout_paths_detect_legacy_source},
    {"lua_dock_layout_meta_path_is_sibling_yaml", &test_lua_dock_layout_meta_path_is_sibling_yaml},
    {"lua_dock_layout_meta_schema_v3_marks_modern_layout", &test_lua_dock_layout_meta_schema_v3_marks_modern_layout},
    {"lua_dock_layout_meta_schema_v2_marks_modern_layout", &test_lua_dock_layout_meta_schema_v2_marks_modern_layout},
    {"lua_dock_layout_meta_read_failure_falls_back_to_legacy",
     &test_lua_dock_layout_meta_read_failure_falls_back_to_legacy},
    {"lua_dock_layout_dock_id_sharing_does_not_mark_modern_legacy",
     &test_lua_dock_layout_dock_id_sharing_does_not_mark_modern_legacy},
    {"lua_dock_window_name_keeps_stable_id", &test_lua_dock_window_name_keeps_stable_id},
    {"lua_dock_layout_requests_group_tabs", &test_lua_dock_layout_requests_group_tabs},
    {"lua_dock_layout_requests_default_anchor_falls_back_left_bottom",
     &test_lua_dock_layout_requests_default_anchor_falls_back_left_bottom},
    {"dock_layout_ini_requires_exactly_one_central_node", &test_dock_layout_ini_requires_exactly_one_central_node},
    {"dock_layout_ini_rebuilds_legacy_left_central_node", &test_dock_layout_ini_rebuilds_legacy_left_central_node},
    {"lua_dock_layout_requests_preserve_supported_anchors", &test_lua_dock_layout_requests_preserve_supported_anchors},
    {"lua_dock_settings_filter_keeps_current_protocol_windows",
     &test_lua_dock_settings_filter_keeps_current_protocol_windows},
    {"lua_dock_settings_filter_keeps_current_windows_without_active_docks",
     &test_lua_dock_settings_filter_keeps_current_windows_without_active_docks},
    {"lua_dock_settings_filter_keeps_same_dock_id_tab_stack",
     &test_lua_dock_settings_filter_keeps_same_dock_id_tab_stack},
    {"workspace_layout_mode_after_load_prefers_default_build_only_when_missing",
     &test_workspace_layout_mode_after_load_prefers_default_build_only_when_missing},
    {"protocol_workspace_switch_decision_uses_draft_only_until_reload",
     &test_protocol_workspace_switch_decision_uses_draft_only_until_reload},
    {"protocol_workspace_switch_decision_reloads_draft_when_clicked",
     &test_protocol_workspace_switch_decision_reloads_draft_when_clicked},
    {"protocol_switch_resets_lua_default_dock_state_only_when_changed",
     &test_protocol_switch_resets_lua_default_dock_state_only_when_changed},
    {"lua_default_dock_layout_runs_only_during_default_build",
     &test_lua_default_dock_layout_runs_only_during_default_build},
    {"protocol_workspace_layout_reset_requires_loaded_protocol",
     &test_protocol_workspace_layout_reset_requires_loaded_protocol},
    {"plot_cursor_snap_by_time_and_measurement", &test_plot_cursor_snap_by_time_and_measurement},
    {"plot_measurement_dispersion_metrics", &test_plot_measurement_dispersion_metrics},
    {"plot_measurement_error_metrics", &test_plot_measurement_error_metrics},
    {"wave_cursor_smart_snap_edge", &test_wave_cursor_smart_snap_edge},
    {"wave_cursor_smart_snap_extreme", &test_wave_cursor_smart_snap_extreme},
    {"wave_cursor_nearest_waveform_extreme_policy_snaps_to_local_trough",
     &test_wave_cursor_nearest_waveform_extreme_policy_snaps_to_local_trough},
    {"wave_cursor_viewport_zone_extreme_policy_keeps_bottom_zone_behavior",
     &test_wave_cursor_viewport_zone_extreme_policy_keeps_bottom_zone_behavior},
    {"wave_cursor_extreme_snap_falls_back_to_window_peak_with_transforms",
     &test_wave_cursor_extreme_snap_falls_back_to_window_peak_with_transforms},
    {"wave_cursor_extreme_snap_falls_back_to_window_trough",
     &test_wave_cursor_extreme_snap_falls_back_to_window_trough},
    {"wave_cursor_smart_snap_fallback_to_nearest", &test_wave_cursor_smart_snap_fallback_to_nearest},
    {"wave_cursor_drag_time_uses_smart_snap", &test_wave_cursor_drag_time_uses_smart_snap},
    {"split_cursor_drag_id_namespace_is_unique", &test_split_cursor_drag_id_namespace_is_unique},
    {"split_cursor_snap_forced_channel_ignores_other_rows", &test_split_cursor_snap_forced_channel_ignores_other_rows},
    {"split_cursor_smart_snap_forced_channel_ignores_hidden_row_candidate",
     &test_split_cursor_smart_snap_forced_channel_ignores_hidden_row_candidate},
    {"tcp_transport_roundtrip", &test_tcp_transport_roundtrip},
    {"transport_enqueue_send_async_roundtrip", &test_transport_enqueue_send_async_roundtrip},
    {"tcp_server_connection_takeover_replaces_active_client",
     &test_tcp_server_connection_takeover_replaces_active_client},
    {"serial_transport_error_path", &test_serial_transport_error_path},
    {"udp_peer_transport_roundtrip", &test_udp_peer_transport_roundtrip},
    {"udp_peer_failed_open_releases_bound_socket", &test_udp_peer_failed_open_releases_bound_socket},
    {"serial_port_name_normalization", &test_serial_port_name_normalization},
    {"script_dialog_requests_keep_connection_context", &test_script_dialog_requests_keep_connection_context},
    {"script_dialog_requests_detached_without_active_connection",
     &test_script_dialog_requests_detached_without_active_connection},
    {"script_dialog_requests_parse_window_options", &test_script_dialog_requests_parse_window_options},
    {"script_dialog_requests_reject_invalid_window_options",
     &test_script_dialog_requests_reject_invalid_window_options},
    {"application_tcp_lua_read_version_roundtrip", &test_application_tcp_lua_read_version_roundtrip},
    {"application_lua_controls_without_connection", &test_application_lua_controls_without_connection},
    {"application_oscilloscope_toggle_syncs_running_from_script_outputs",
     &test_application_oscilloscope_toggle_syncs_running_from_script_outputs},
    {"application_tx_overflow_popup_keeps_dialog_payload", &test_application_tx_overflow_popup_keeps_dialog_payload},
    {"application_failed_protocol_reload_keeps_previous_runtime",
     &test_application_failed_protocol_reload_keeps_previous_runtime},
    {"application_same_protocol_reload_keeps_runtime_stable",
     &test_application_same_protocol_reload_keeps_runtime_stable},
    {"application_same_protocol_reload_without_force_preserves_runtime_state",
     &test_application_same_protocol_reload_without_force_preserves_runtime_state},
    {"application_failed_reload_keeps_old_callbacks_alive", &test_application_failed_reload_keeps_old_callbacks_alive},
    {"application_forced_reload_discards_old_tx_callback_outputs",
     &test_application_forced_reload_discards_old_tx_callback_outputs},
    {"application_request_done_success_does_not_set_comm_error",
     &test_application_request_done_success_does_not_set_comm_error},
    {"application_request_timeout_drains_pending_rx_before_timeout",
     &test_application_request_timeout_drains_pending_rx_before_timeout},
    {"application_guarded_request_timeout_retry_then_success_keeps_guard_active",
     &test_application_guarded_request_timeout_retry_then_success_keeps_guard_active},
    {"application_guarded_request_final_timeout_halts_followup_guarded",
     &test_application_guarded_request_final_timeout_halts_followup_guarded},
    {"application_guarded_requests_count_attempts_independently",
     &test_application_guarded_requests_count_attempts_independently},
    {"application_guarded_request_reset_allows_new_attempts",
     &test_application_guarded_request_reset_allows_new_attempts},
    {"application_request_done_failure_sets_comm_error", &test_application_request_done_failure_sets_comm_error},
    {"application_open_transport_uses_serial_runtime_config",
     &test_application_open_transport_uses_serial_runtime_config},
    {"application_open_transport_uses_udp_peer_runtime_config",
     &test_application_open_transport_uses_udp_peer_runtime_config},
    {"application_set_log_level_updates_runtime_config", &test_application_set_log_level_updates_runtime_config},
    {"application_capture_config_preserves_protocol_tx_runtime_config",
     &test_application_capture_config_preserves_protocol_tx_runtime_config},
    {"application_wave_legend_visibility_config_roundtrip", &test_application_wave_legend_visibility_config_roundtrip},
    {"application_wave_zoom_selection_auto_exit_config_roundtrip",
     &test_application_wave_zoom_selection_auto_exit_config_roundtrip},
    {"application_reset_wave_history_restores_default_viewport",
     &test_application_reset_wave_history_restores_default_viewport},
    {"application_plot_setup_reset_history_preserves_paused_follow",
     &test_application_plot_setup_reset_history_preserves_paused_follow},
    {"application_plot_setup_reset_history_preserves_active_follow",
     &test_application_plot_setup_reset_history_preserves_active_follow},
    {"application_refreshes_selected_elf_symbol_controls_silently",
     &test_application_refreshes_selected_elf_symbol_controls_silently},
    {"application_refreshes_selected_elf_symbol_controls_with_on_control",
     &test_application_refreshes_selected_elf_symbol_controls_with_on_control},
    {"application_clear_elf_static_address_file_resets_queries",
     &test_application_clear_elf_static_address_file_resets_queries},
    {"application_logging_filters_script_and_host", &test_application_logging_filters_script_and_host},
    {"application_raw_capture_export_import_roundtrip", &test_application_raw_capture_export_import_roundtrip},
    {"application_session_package_export_contains_replay_assets",
     &test_application_session_package_export_contains_replay_assets},
    {"application_session_package_exports_protocol_directory_and_import_requires_helper",
     &test_application_session_package_exports_protocol_directory_and_import_requires_helper},
    {"application_session_package_import_rejects_unsafe_entries",
     &test_application_session_package_import_rejects_unsafe_entries},
    {"application_wave_analysis_report_exports_summary_and_markers",
     &test_application_wave_analysis_report_exports_summary_and_markers},
    {"application_session_package_import_without_markers_clears_existing_state",
     &test_application_session_package_import_without_markers_clears_existing_state},
    {"application_session_package_import_invalid_protocol_rolls_back_runtime",
     &test_application_session_package_import_invalid_protocol_rolls_back_runtime},
    {"application_raw_capture_replay_timeline_steps_events",
     &test_application_raw_capture_replay_timeline_steps_events},
    {"application_loads_protocol_action_templates", &test_application_loads_protocol_action_templates},
    {"application_initialize_prepares_default_config_and_protocol_dirs",
     &test_application_initialize_prepares_default_config_and_protocol_dirs},
    {"application_live_raw_capture_trims_to_limit", &test_application_live_raw_capture_trims_to_limit},
    {"application_live_raw_capture_trim_keeps_runtime_profile_event",
     &test_application_live_raw_capture_trim_keeps_runtime_profile_event},
    {"application_session_package_export_trims_raw_capture_window",
     &test_application_session_package_export_trims_raw_capture_window},
    {"application_raw_capture_recording_preserves_full_rx_when_live_buffer_trims",
     &test_application_raw_capture_recording_preserves_full_rx_when_live_buffer_trims},
    {"application_raw_capture_import_preserves_full_history",
     &test_application_raw_capture_import_preserves_full_history},
    {"application_raw_capture_import_replays_runtime_profile_events",
     &test_application_raw_capture_import_replays_runtime_profile_events},
    {"application_raw_capture_import_replays_plot_setup_snapshot",
     &test_application_raw_capture_import_replays_plot_setup_snapshot},
    {"application_raw_capture_import_skips_duplicate_plot_setup_reset",
     &test_application_raw_capture_import_skips_duplicate_plot_setup_reset},
    {"application_raw_capture_import_replays_stream_in_chunks",
     &test_application_raw_capture_import_replays_stream_in_chunks},
    {"application_reload_rebuilds_frame_rows_with_count_expression",
     &test_application_reload_rebuilds_frame_rows_with_count_expression},
    {"application_transfer_log_frame_view_waits_for_rx_full_frame",
     &test_application_transfer_log_frame_view_waits_for_rx_full_frame},
    {"application_transfer_log_frame_view_keeps_unmatched_tx_raw",
     &test_application_transfer_log_frame_view_keeps_unmatched_tx_raw},
    {"application_switching_to_parsed_view_defaults_to_new_stream_only",
     &test_application_switching_to_parsed_view_defaults_to_new_stream_only},
    {"application_switching_to_parsed_view_can_replay_old_raw_history",
     &test_application_switching_to_parsed_view_can_replay_old_raw_history},
    {"application_rx_events_are_processed_with_budget", &test_application_rx_events_are_processed_with_budget},
    {"application_large_rx_event_drains_by_byte_budget", &test_application_large_rx_event_drains_by_byte_budget},
    {"application_comm_pressure_debug_log_respects_log_level",
     &test_application_comm_pressure_debug_log_respects_log_level},
    {"application_responsive_disconnect_discards_realtime_backlog",
     &test_application_responsive_disconnect_discards_realtime_backlog},
    {"application_complete_disconnect_keeps_realtime_backlog",
     &test_application_complete_disconnect_keeps_realtime_backlog},
    {"application_transfer_frame_rows_drain_after_input_stops",
     &test_application_transfer_frame_rows_drain_after_input_stops},
    {"application_plot_push_merges_same_channel_source", &test_application_plot_push_merges_same_channel_source},
    {"application_plot_push_drains_with_budget_and_disconnect_keeps_pending",
     &test_application_plot_push_drains_with_budget_and_disconnect_keeps_pending},
    {"runtime_scheduler_limits_busy_render_frames", &test_runtime_scheduler_limits_busy_render_frames},
    {"script_runtime_worker_disabled_mode_waits_for_rx_idle",
     &test_script_runtime_worker_disabled_mode_waits_for_rx_idle},
    {"script_runtime_worker_rx_limit_keeps_all_queued_bytes",
     &test_script_runtime_worker_rx_limit_keeps_all_queued_bytes},
    {"script_runtime_worker_batch_bytes_merges_adjacent_rx_events",
     &test_script_runtime_worker_batch_bytes_merges_adjacent_rx_events},
    {"script_runtime_worker_oscilloscope_toggle_sync_returns_lua_result",
     &test_script_runtime_worker_oscilloscope_toggle_sync_returns_lua_result},
    {"pipeline_worker_threads_resolve_from_hardware_limit", &test_pipeline_worker_threads_resolve_from_hardware_limit},
    {"plot_history_trim_and_envelope", &test_plot_history_trim_and_envelope},
    {"plot_history_limit_zero_keeps_all_samples", &test_plot_history_limit_zero_keeps_all_samples},
    {"plot_set_view_config_applies_history_limit_immediately",
     &test_plot_set_view_config_applies_history_limit_immediately},
    {"plot_time_reset_clears_history_by_default", &test_plot_time_reset_clears_history_by_default},
    {"plot_time_reset_can_continue_history", &test_plot_time_reset_can_continue_history},
    {"wave_sample_frequency_visible_range_filters_by_sample_index",
     &test_wave_sample_frequency_visible_range_filters_by_sample_index},
    {"wave_snapshot_visible_range_keeps_adjacent_samples", &test_wave_snapshot_visible_range_keeps_adjacent_samples},
    {"wave_visible_range_adjacent_samples_clamp_at_edges", &test_wave_visible_range_adjacent_samples_clamp_at_edges},
    {"wave_sample_frequency_preserves_trimmed_sample_offset",
     &test_wave_sample_frequency_preserves_trimmed_sample_offset},
    {"wave_max_total_samples_trim_refreshes_cached_frame", &test_wave_max_total_samples_trim_refreshes_cached_frame},
    {"wave_max_total_samples_noop_preserves_revision", &test_wave_max_total_samples_noop_preserves_revision},
    {"wave_layout_solver_clamps_without_overflow", &test_wave_layout_solver_clamps_without_overflow},
    {"measurement_overlay_safe_right_collapsed_uses_content_right",
     &test_measurement_overlay_safe_right_collapsed_uses_content_right},
    {"measurement_overlay_safe_right_open_drawer_reserves_drawer_width",
     &test_measurement_overlay_safe_right_open_drawer_reserves_drawer_width},
    {"measurement_overlay_placement_clamps_to_safe_right", &test_measurement_overlay_placement_clamps_to_safe_right},
    {"measurement_overlay_placement_skips_when_safe_area_too_narrow",
     &test_measurement_overlay_placement_skips_when_safe_area_too_narrow},
    {"plot_limited_envelope_preserves_spikes", &test_plot_limited_envelope_preserves_spikes},
    {"plot_low_density_envelope_keeps_single_value_line", &test_plot_low_density_envelope_keeps_single_value_line},
    {"plot_cursor_snap_and_delta", &test_plot_cursor_snap_and_delta},
    {"plot_channel_scale_and_offset_apply_to_display_only", &test_plot_channel_scale_and_offset_apply_to_display_only},
    {"plot_channel_bit_display_reaches_snapshot", &test_plot_channel_bit_display_reaches_snapshot},
    {"bit_lane_double_click_reset_selects_without_active_lane",
     &test_bit_lane_double_click_reset_selects_without_active_lane},
    {"bit_render_lane_downsample_keeps_orthogonal_segments",
     &test_bit_render_lane_downsample_keeps_orthogonal_segments},
    {"bit_render_lane_low_density_keeps_exact_steps", &test_bit_render_lane_low_density_keeps_exact_steps},
    {"wave_bit_display_bounds_and_hidden_policy", &test_wave_bit_display_bounds_and_hidden_policy},
    {"bit_cursor_only_snaps_to_transitions", &test_bit_cursor_only_snaps_to_transitions},
    {"hidden_bit_lane_excluded_from_layout_hit_and_snap", &test_hidden_bit_lane_excluded_from_layout_hit_and_snap},
    {"split_bit_cursor_forced_channel_still_snaps_to_transition",
     &test_split_bit_cursor_forced_channel_still_snaps_to_transition},
    {"bit_measurement_cross_lane_still_outputs_dt_f", &test_bit_measurement_cross_lane_still_outputs_dt_f},
    {"bit_cursor_cross_lane_refresh_uses_own_y_anchor", &test_bit_cursor_cross_lane_refresh_uses_own_y_anchor},
    {"bit_active_switches_measurement_mode", &test_bit_active_switches_measurement_mode},
    {"invisible_active_bit_lane_falls_back_to_waveform_cursor",
     &test_invisible_active_bit_lane_falls_back_to_waveform_cursor},
    {"active_channel_cursor_time_fallback_rebinds_after_channel_switch",
     &test_active_channel_cursor_time_fallback_rebinds_after_channel_switch},
    {"active_channel_cursor_time_fallback_restores_pair_measurement",
     &test_active_channel_cursor_time_fallback_restores_pair_measurement},
    {"active_bit_lane_cursor_can_return_nearby_waveform", &test_active_bit_lane_cursor_can_return_nearby_waveform},
    {"bit_display_cursor_excludes_same_channel_raw_waveform",
     &test_bit_display_cursor_excludes_same_channel_raw_waveform},
    {"explicit_bit_readout_policy_keeps_waveform_when_bit_not_active",
     &test_explicit_bit_readout_policy_keeps_waveform_when_bit_not_active},
    {"bit_layout_independent_of_axis_range", &test_bit_layout_independent_of_axis_range},
    {"bit_layout_multiple_channels_with_y_offset", &test_bit_layout_multiple_channels_with_y_offset},
    {"bit_lane_display_label_uses_simplified_bit_text", &test_bit_lane_display_label_uses_simplified_bit_text},
    {"bit_layout_16_and_8_bit_channels_share_absolute_rows",
     &test_bit_layout_16_and_8_bit_channels_share_absolute_rows},
    {"plot_snapshot_without_stats_keeps_ranges_and_samples",
     &test_plot_snapshot_without_stats_keeps_ranges_and_samples},
    {"plot_build_display_data_into_reuses_storage_and_matches_output",
     &test_plot_build_display_data_into_reuses_storage_and_matches_output},
    {"plot_channel_ratio_and_formula_modes", &test_plot_channel_ratio_and_formula_modes},
    {"plot_channel_transform_updates_are_isolated", &test_plot_channel_transform_updates_are_isolated},
    {"plot_cursor_snap_scope_selection", &test_plot_cursor_snap_scope_selection},
    {"plot_hover_readout_ignores_hidden_channels", &test_plot_hover_readout_ignores_hidden_channels},
    {"bit_hover_readout_tracks_steady_level", &test_bit_hover_readout_tracks_steady_level},
    {"bit_hover_readout_uses_sample_frequency_time_axis", &test_bit_hover_readout_uses_sample_frequency_time_axis},
    {"bit_hover_readout_excludes_same_channel_raw_waveform",
     &test_bit_hover_readout_excludes_same_channel_raw_waveform},
    {"bit_hover_readout_uses_nearest_display_shape_in_mixed_view",
     &test_bit_hover_readout_uses_nearest_display_shape_in_mixed_view},
    {"bit_hover_readout_falls_back_to_waveform_outside_lane",
     &test_bit_hover_readout_falls_back_to_waveform_outside_lane},
    {"plot_limited_envelope_edges", &test_plot_limited_envelope_edges},
    {"wave_frequency_parse_and_axis_mapping", &test_wave_frequency_parse_and_axis_mapping},
    {"wave_display_data_uses_visible_window_only", &test_wave_display_data_uses_visible_window_only},
    {"wave_main_render_data_uses_viewport_window", &test_wave_main_render_data_uses_viewport_window},
    {"wave_peak_detect_downsample_orders_bucket_points", &test_wave_peak_detect_downsample_orders_bucket_points},
    {"wave_peak_detect_downsample_respects_point_budget", &test_wave_peak_detect_downsample_respects_point_budget},
    {"wave_render_mode_label_reports_each_path", &test_wave_render_mode_label_reports_each_path},
    {"wave_main_render_data_uses_sample_frequency_viewport",
     &test_wave_main_render_data_uses_sample_frequency_viewport},
    {"wave_default_viewport_without_frequency_uses_time_scale",
     &test_wave_default_viewport_without_frequency_uses_time_scale},
    {"wave_default_viewport_uses_sample_frequency_budget", &test_wave_default_viewport_uses_sample_frequency_budget},
    {"wave_default_viewport_duration_tracks_render_budget", &test_wave_default_viewport_duration_tracks_render_budget},
    {"wave_default_viewport_preserves_configured_y_range", &test_wave_default_viewport_preserves_configured_y_range},
    {"wave_sample_frequency_auto_follow_preserves_trimmed_offset",
     &test_wave_sample_frequency_auto_follow_preserves_trimmed_offset},
    {"wave_oscilloscope_toggle_deferred", &test_wave_oscilloscope_toggle_deferred},
    {"wave_overview_display_data_is_budgeted", &test_wave_overview_display_data_is_budgeted},
    {"wave_overview_bounds_use_full_history_window", &test_wave_overview_bounds_use_full_history_window},
    {"wave_fit_visible_waveforms_uses_full_history_time_bounds",
     &test_wave_fit_visible_waveforms_uses_full_history_time_bounds},
    {"wave_fit_visible_waveforms_uses_sample_frequency_full_history",
     &test_wave_fit_visible_waveforms_uses_sample_frequency_full_history},
    {"wave_fit_visible_waveforms_ignores_hidden_channels", &test_wave_fit_visible_waveforms_ignores_hidden_channels},
    {"wave_x_axis_double_click_bounds_selects_full_history",
     &test_wave_x_axis_double_click_bounds_selects_full_history},
    {"wave_fft_detects_50hz_and_150hz_components", &test_wave_fft_detects_50hz_and_150hz_components},
    {"wave_fft_fundamental_percent_uses_each_channel_fundamental",
     &test_wave_fft_fundamental_percent_uses_each_channel_fundamental},
    {"wave_fft_fundamental_percent_requires_valid_fundamental_magnitude",
     &test_wave_fft_fundamental_percent_requires_valid_fundamental_magnitude},
    {"wave_fft_visible_samples_supports_non_power_of_two", &test_wave_fft_visible_samples_supports_non_power_of_two},
    {"wave_fft_manual_point_count_supports_non_power_of_two",
     &test_wave_fft_manual_point_count_supports_non_power_of_two},
    {"wave_fft_cursor_window_resolves_point_counts_and_duration",
     &test_wave_fft_cursor_window_resolves_point_counts_and_duration},
    {"wave_fft_fit_viewport_resets_frequency_and_value_ranges",
     &test_wave_fft_fit_viewport_resets_frequency_and_value_ranges},
    {"wave_fft_fit_viewport_can_ignore_channel_fundamental_bins",
     &test_wave_fft_fit_viewport_can_ignore_channel_fundamental_bins},
    {"wave_fft_fit_viewport_ignore_fundamental_falls_back_when_empty",
     &test_wave_fft_fit_viewport_ignore_fundamental_falls_back_when_empty},
    {"wave_fft_fit_viewport_offsets_are_visual_only", &test_wave_fft_fit_viewport_offsets_are_visual_only},
    {"wave_fft_x_axis_mode_conversions", &test_wave_fft_x_axis_mode_conversions},
    {"wave_fft_cursor_window_resolves_point_counts_and_duration",
     &test_wave_fft_cursor_window_resolves_point_counts_and_duration},
    {"wave_viewport_zoom_modes_and_clamp", &test_wave_viewport_zoom_modes_and_clamp},
    {"wave_overview_viewport_normalize", &test_wave_overview_viewport_normalize},
    {"wave_cursor_position_in_viewport", &test_wave_cursor_position_in_viewport},
    {"wave_measurement_cursors_follow_equal_width_scroll", &test_wave_measurement_cursors_follow_equal_width_scroll},
    {"wave_measurement_cursor_scroll_refresh_rebinds_by_time",
     &test_wave_measurement_cursor_scroll_refresh_rebinds_by_time},
    {"wave_cursor_interval_text_by_axis", &test_wave_cursor_interval_text_by_axis},
    {"wave_cursor_interval_lock", &test_wave_cursor_interval_lock},
    {"cursor_intersection_readouts_default_disabled", &test_cursor_intersection_readouts_default_disabled},
    {"cursor_intersection_readouts_respect_enabled_cursors",
     &test_cursor_intersection_readouts_respect_enabled_cursors},
    {"cursor_intersection_readouts_use_visible_waveform_channels_only",
     &test_cursor_intersection_readouts_use_visible_waveform_channels_only},
    {"cursor_intersection_readouts_skip_bit_lane_channels", &test_cursor_intersection_readouts_skip_bit_lane_channels},
    {"cursor_intersection_readouts_respect_max_time_distance",
     &test_cursor_intersection_readouts_respect_max_time_distance},
    {"cursor_readout_annotation_requires_hold_or_pin", &test_cursor_readout_annotation_requires_hold_or_pin},
    {"wave_channel_card_width_modes", &test_wave_channel_card_width_modes},
    {"wave_vertical_auto_fit_multiplier", &test_wave_vertical_auto_fit_multiplier},
    {"wave_y_axis_double_click_bounds_selects_visible_or_active",
     &test_wave_y_axis_double_click_bounds_selects_visible_or_active},
    {"wave_visible_channel_bounds_ignore_hidden_channels", &test_wave_visible_channel_bounds_ignore_hidden_channels},
    {"wave_hidden_channel_policy_defaults_to_visible_only", &test_wave_hidden_channel_policy_defaults_to_visible_only},
    {"wave_hidden_channel_indices_allow_duplicate_labels", &test_wave_hidden_channel_indices_allow_duplicate_labels},
    {"wave_hidden_channel_indices_survive_duplicate_rename",
     &test_wave_hidden_channel_indices_survive_duplicate_rename},
    {"wave_grid_division_readout_conversions", &test_wave_grid_division_readout_conversions},
    {"wave_grid_division_readout_formula_offset_cancels", &test_wave_grid_division_readout_formula_offset_cancels},
    {"wave_status_overlay_items_only_show_non_default_states",
     &test_wave_status_overlay_items_only_show_non_default_states},
    {"wave_auto_follow_pause_policy_respects_interaction_setting",
     &test_wave_auto_follow_pause_policy_respects_interaction_setting},
    {"wave_phosphor_stroke_style_uses_channel_style", &test_wave_phosphor_stroke_style_uses_channel_style},
    {"wave_phosphor_trigger_detection_interpolates_edges", &test_wave_phosphor_trigger_detection_interpolates_edges},
    {"wave_phosphor_trigger_window_aligns_to_fixed_x", &test_wave_phosphor_trigger_window_aligns_to_fixed_x},
    {"wave_phosphor_non_follow_mode_freezes", &test_wave_phosphor_non_follow_mode_freezes},
    {"wave_channel_reset_all_uses_protocol_default", &test_wave_channel_reset_all_uses_protocol_default},
    {"wave_channel_reset_scale_offset_preserves_label_and_ratio",
     &test_wave_channel_reset_scale_offset_preserves_label_and_ratio},
    {"wave_channel_reset_scale_preserves_offset", &test_wave_channel_reset_scale_preserves_offset},
    {"wave_offset_reset_uses_protocol_default_only", &test_wave_offset_reset_uses_protocol_default_only},
    {"wave_reset_one_channel_view_settings_only_resets_target",
     &test_wave_reset_one_channel_view_settings_only_resets_target},
    {"wave_reset_all_channel_view_settings_preserves_samples",
     &test_wave_reset_all_channel_view_settings_preserves_samples},
    {"wave_mouse_y_offset_drag_mode_gate", &test_wave_mouse_y_offset_drag_mode_gate},
    {"raw_capture_file_roundtrip", &test_raw_capture_file_roundtrip},
    {"wave_csv_wide_roundtrip", &test_wave_csv_wide_roundtrip},
    {"wave_csv_long_import_rejects_non_finite", &test_wave_csv_long_import_rejects_non_finite},
    {"wave_csv_long_import", &test_wave_csv_long_import},
    {"raw_capture_csv_roundtrip", &test_raw_capture_csv_roundtrip},
    {"raw_capture_csv_rejects_bad_hex", &test_raw_capture_csv_rejects_bad_hex},
    {"csv_export_range_filters_wave_and_raw_events", &test_csv_export_range_filters_wave_and_raw_events},
    {"raw_capture_file_plot_setup_roundtrip", &test_raw_capture_file_plot_setup_roundtrip},
    {"raw_capture_file_plot_setup_rejects_bad_fields", &test_raw_capture_file_plot_setup_rejects_bad_fields},
    {"raw_capture_file_v2_event_stream_still_reads", &test_raw_capture_file_v2_event_stream_still_reads},
    {"raw_capture_file_rejects_size_mismatch", &test_raw_capture_file_rejects_size_mismatch},
    {"raw_capture_file_requires_protocol_fields", &test_raw_capture_file_requires_protocol_fields},
    {"application_raw_capture_import_updates_last_pump_diagnostics",
     &test_application_raw_capture_import_updates_last_pump_diagnostics},
    {"raw_capture_file_rejects_trailing_bytes", &test_raw_capture_file_rejects_trailing_bytes},
    {"raw_capture_file_rejects_profile_set_without_length", &test_raw_capture_file_rejects_profile_set_without_length},
    {"session_package_roundtrip_preserves_binary_entries", &test_session_package_roundtrip_preserves_binary_entries},
    {"session_package_rejects_truncated_entry", &test_session_package_rejects_truncated_entry},
    {"session_package_rejects_excessive_entry_count", &test_session_package_rejects_excessive_entry_count},
    {"elf_static_view_bridge_loads_dump_json_and_queries_symbols",
     &test_elf_static_view_bridge_loads_dump_json_and_queries_symbols},
    {"elf_static_view_bridge_finds_exact_label_only", &test_elf_static_view_bridge_finds_exact_label_only},
    {"elf_static_view_bridge_queries_flattened_composite_members",
     &test_elf_static_view_bridge_queries_flattened_composite_members},
    {"elf_static_view_bridge_loads_private_binary_without_extension",
     &test_elf_static_view_bridge_loads_private_binary_without_extension},
    {"elf_static_view_bridge_loads_variable_summary_export",
     &test_elf_static_view_bridge_loads_variable_summary_export},
    {"elf_static_view_bridge_keeps_old_model_on_load_failure",
     &test_elf_static_view_bridge_keeps_old_model_on_load_failure},
    {"elf_static_view_bridge_clear_resets_loaded_model", &test_elf_static_view_bridge_clear_resets_loaded_model},
    {"elf_static_address_file_watch_detects_changes_and_delete_recreate_reload",
     &test_elf_static_address_file_watch_detects_changes_and_delete_recreate_reload},
};

} // namespace

const TestCase* allTests()
{
    return kAllTests;
}

int testCount()
{
    return static_cast<int>(sizeof(kAllTests) / sizeof(kAllTests[0]));
}
