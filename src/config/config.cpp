#include "protoscope/config/config.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

namespace protoscope::config {

namespace {

template <typename T>
T readScalar(const YAML::Node& node, const char* key, T fallback) {
    if (!node || !node[key]) {
        return fallback;
    }
    return node[key].as<T>();
}

std::string normalizeTextPath(std::filesystem::path path) {
    path.make_preferred();
    return path.generic_string();
}

LogLevel parseLogLevel(const std::string& value) {
    if (value == "debug") {
        return LogLevel::Debug;
    }
    if (value == "warn" || value == "warning") {
        return LogLevel::Warn;
    }
    if (value == "error") {
        return LogLevel::Error;
    }
    return LogLevel::Info;
}

std::string toLogLevelText(const LogLevel level) {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warn:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

transport::TransportKind parseTransportKind(const std::string& value) {
    if (value == "tcp_server") {
        return transport::TransportKind::TcpServer;
    }
    if (value == "serial") {
        return transport::TransportKind::Serial;
    }
    return transport::TransportKind::TcpClient;
}

std::string toTransportKindText(transport::TransportKind kind) {
    switch (kind) {
    case transport::TransportKind::TcpClient:
        return "tcp_client";
    case transport::TransportKind::TcpServer:
        return "tcp_server";
    case transport::TransportKind::Serial:
        return "serial";
    }
    return "tcp_client";
}

const std::vector<std::string> kDefaultSerialPorts = {"COM1", "COM2", "COM3", "COM4"};

const char* kDefaultProtocolMainLua = R"PROTO(-- 核心流程：协议脚本只描述控件、收发和完整应答，发送排队、半双工、超时与弹窗都交给宿主处理。

function ui()
  return {
    {
      id = "protocol",
      title = "协议动作",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
      },
      layout = {
        kind = "table",
        columns = 2,
        borders = false,
        resizable = true,
        row_bg = false,
        sizing = "stretch",
        rows = {
          {
            { control = "read_version" },
            { control = "device_id" },
          },
        }
      }
    },
    {
      id = "advanced",
      title = "高级参数",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "checkbox", id = "hex_send", label = "HEX 发送", default = true },
        { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
        { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
        { type = "input_float", id = "scale", label = "缩放", default = 1.0 },
      },
      layout = {
        kind = "form",
        items = {
          { text = "超时由宿主 request 队列管理；脚本只在拿到完整应答后调用 proto.request_done。" },
          { controls = { "hex_send", "mode" } },
          { separator = true },
          {
            group = "采样参数",
            items = {
              { controls = { "timeout_ms", "scale" } },
            }
          }
        }
      }
    }
  }
end

local rx_buffer = {}
local next_scope_t = 0.0
local current_request_id = nil
local current_request_tag = nil

proto.plot.setup({
  source = "default_protocol",
  channels = {
    { label = "CH1", unit = "V", color = "#47C971" },
    { label = "CH2", unit = "V", color = "#5B8FF9" },
  },
  view = {
    time_scale = 0.2,
    time_unit = "s",
    vertical_min = -1.5,
    vertical_max = 1.5,
    vertical_unit = "V",
    history_limit = 2000,
  }
})

local function clear_rx_buffer()
  rx_buffer = {}
end

local function append_bytes(bytes)
  for _, value in ipairs(bytes) do
    rx_buffer[#rx_buffer + 1] = value
  end
end

local function timeout_ms()
  return proto.get_control("timeout_ms") or 1000
end

local function current_scale()
  local scale = proto.get_control("scale") or 1.0
  if scale == 0 then
    return 1.0
  end
  return scale
end

local function build_read_version_frame()
  local text = tostring(proto.get_control("device_id") or "01")
  local device = tonumber(text, 16) or 0x01
  return { 0xAA, 0x55, device & 0xFF, 0x0D }
end

local function parse_frame(buffer)
  if #buffer < 4 then
    return nil
  end
  if buffer[#buffer - 1] ~= 0x0D or buffer[#buffer] ~= 0x0A then
    return nil
  end
  local chars = {}
  for i = 1, #buffer - 2 do
    chars[i] = string.char(buffer[i])
  end
  return {
    raw = table.concat(chars),
    text = table.concat(chars),
    size = #buffer,
  }
end

local function push_scope_samples(bytes)
  local scale = current_scale()
  local samples = {}
  for _, value in ipairs(bytes) do
    local normalized = (value - 128.0) / 128.0
    samples[#samples + 1] = { t = next_scope_t, y = normalized * scale }
    next_scope_t = next_scope_t + 0.001
  end
  if #samples > 0 then
    proto.plot.push(1, { source = "rx", samples = samples })
  end
end

local function clear_pending_request()
  current_request_id = nil
  current_request_tag = nil
end

function on_open(ctx)
  proto.log("info", "连接已打开: " .. ctx.kind .. " -> " .. ctx.endpoint)
  proto.status.set("连接已打开，等待发送请求", { level = "info" })
end

function on_close(ctx)
  proto.log("info", "连接已关闭: " .. ctx.endpoint)
  proto.status.clear()
  clear_pending_request()
end

function on_error(ctx, message)
  proto.log("error", "连接错误: " .. message)
  proto.status.set("连接错误: " .. message, { level = "error" })
  clear_pending_request()
end

function on_control(ctx, id, value)
  if id == "read_version" then
    clear_rx_buffer()
    local request_id, err = proto.request(build_read_version_frame(), {
      timeout_ms = timeout_ms(),
      tag = "read_version",
    })
    if not request_id then
      proto.status.set("读取版本请求入队失败: " .. tostring(err), { level = "error" })
      proto.ui.alert({
        title = "请求失败",
        message = "读取版本请求入队失败: " .. tostring(err),
        level = "error",
        dedupe_key = "read_version_request_failed",
      })
      return
    end
    current_request_id = request_id
    current_request_tag = "read_version"
    proto.status.set("读取版本请求已入队", { level = "info" })
    proto.emit("request", { action = "read_version", request_id = request_id, connection_id = ctx.connection_id })
  end
end

function on_bytes(ctx, bytes)
  push_scope_samples(bytes)
  append_bytes(bytes)
  local result = parse_frame(rx_buffer)
  if result then
    clear_rx_buffer()
    proto.emit("frame", result)
    proto.status.set("已收到完整版本帧", { level = "info" })
    if current_request_id then
      proto.request_done({ ok = true, message = "版本应答完成" })
      clear_pending_request()
    end
  else
    proto.emit("rx_bytes", { size = #bytes })
  end
end

function on_tx(ctx, evt)
  if evt.kind == "request" and evt.tag == "read_version" then
    if evt.state == "sent" then
      proto.status.set("读取版本请求已写出，等待完整应答", { level = "info" })
    elseif evt.state == "completed" then
      proto.status.set("读取版本流程完成", { level = "info" })
      clear_pending_request()
    elseif evt.state == "timeout" then
      clear_rx_buffer()
      clear_pending_request()
      proto.status.set("读取版本超时", { level = "warn" })
      proto.ui.alert({
        title = "请求超时",
        message = "读取版本请求在宿主等待 request_done 时超时。",
        level = "warn",
        dedupe_key = "read_version_timeout",
      })
    elseif evt.state == "rejected" or evt.state == "dropped" or evt.state == "canceled" then
      clear_rx_buffer()
      clear_pending_request()
      proto.status.set("读取版本请求失败: " .. tostring(evt.error or evt.state), { level = "error" })
    end
  elseif evt.state == "rejected" then
    proto.status.set("发送失败: " .. tostring(evt.error or "unknown"), { level = "error" })
  end
end

function on_dialog(ctx, evt)
  proto.emit("dialog", evt)
end
)PROTO";

const char* kDefaultWaveformDemoLua = R"WAVE(-- 核心流程：本脚本不依赖串口输入，Lua 定时生成采样点并推送到波形面板。

local timer_name = "lua_waveform_tick"
local running = true
local time_cursor = 0.0
local plot_ready = false

local defaults = {
  frequency_hz = 1.0,
  amplitude = 1.0,
  offset = 0.0,
  phase_deg = 0.0,
  sample_rate_hz = 200,
  points_per_tick = 8,
  timer_ms = 40,
  show_sine = true,
  show_triangle = true,
  show_square = true,
  show_saw = false
}

local function read_number(id, fallback)
  local value = proto.get_control(id)
  if type(value) ~= "number" then
    return fallback
  end
  return value
end

local function read_bool(id, fallback)
  local value = proto.get_control(id)
  if type(value) ~= "boolean" then
    return fallback
  end
  return value
end

local function read_positive_number(id, fallback, minimum)
  local value = read_number(id, fallback)
  if value < minimum then
    return minimum
  end
  return value
end

local function phase_ratio()
  return read_number("phase_deg", defaults.phase_deg) / 360.0
end

local function wrap01(value)
  return value - math.floor(value)
end

local function sine_wave(cycle)
  return math.sin(cycle * 2.0 * math.pi)
end

local function triangle_wave(cycle)
  local position = wrap01(cycle)
  return 1.0 - 4.0 * math.abs(position - 0.5)
end

local function square_wave(cycle)
  return wrap01(cycle) < 0.5 and 1.0 or -1.0
end

local function saw_wave(cycle)
  return wrap01(cycle) * 2.0 - 1.0
end

local function scaled(value)
  local amplitude = read_number("amplitude", defaults.amplitude)
  local offset = read_number("offset", defaults.offset)
  return offset + amplitude * value
end

local function channel_enabled(channel_id)
  return read_bool(channel_id, defaults[channel_id])
end

local function setup_plot(reset_history)
  local vertical_range = math.abs(read_number("amplitude", defaults.amplitude)) + math.abs(read_number("offset", defaults.offset)) + 0.5
  proto.plot.setup({
    source = "lua_waveform_demo",
    reset_history = reset_history,
    time_scale = 1.0,
    time_unit = "s",
    vertical_min = -vertical_range,
    vertical_max = vertical_range,
    vertical_unit = "V",
    history_limit = 12000,
    channels = {
      { label = "正弦", unit = "V", offset = 0.0 },
      { label = "三角", unit = "V", offset = 0.0 },
      { label = "方波", unit = "V", offset = 0.0 },
      { label = "锯齿", unit = "V", offset = 0.0 }
    }
  })
  plot_ready = true
end

local function push_channel(channel_index, samples)
  if #samples > 0 then
    proto.plot.push(channel_index, {
      source = "lua_waveform_demo",
      samples = samples
    })
  end
end

local function emit_samples()
  if not plot_ready then
    setup_plot(true)
  end

  local frequency = read_positive_number("frequency_hz", defaults.frequency_hz, 0.01)
  local sample_rate = read_positive_number("sample_rate_hz", defaults.sample_rate_hz, 1.0)
  local points_per_tick = math.floor(read_positive_number("points_per_tick", defaults.points_per_tick, 1.0))
  local time_step = 1.0 / sample_rate
  local phase = phase_ratio()
  local sine_samples = {}
  local triangle_samples = {}
  local square_samples = {}
  local saw_samples = {}

  -- 核心流程：固定步长推进逻辑时间，避免串口状态或系统刷新抖动影响演示曲线连续性。
  for _ = 1, points_per_tick do
    local time_value = time_cursor
    local cycle = time_value * frequency + phase
    if channel_enabled("show_sine") then
      sine_samples[#sine_samples + 1] = { t = time_value, y = scaled(sine_wave(cycle)) }
    end
    if channel_enabled("show_triangle") then
      triangle_samples[#triangle_samples + 1] = { t = time_value, y = scaled(triangle_wave(cycle)) }
    end
    if channel_enabled("show_square") then
      square_samples[#square_samples + 1] = { t = time_value, y = scaled(square_wave(cycle)) }
    end
    if channel_enabled("show_saw") then
      saw_samples[#saw_samples + 1] = { t = time_value, y = scaled(saw_wave(cycle)) }
    end
    time_cursor = time_cursor + time_step
  end

  push_channel(1, sine_samples)
  push_channel(2, triangle_samples)
  push_channel(3, square_samples)
  push_channel(4, saw_samples)
end

local function schedule_next_tick()
  proto.set_timer(timer_name, math.floor(read_positive_number("timer_ms", defaults.timer_ms, 10.0)))
end

local function restart(reset_history)
  setup_plot(reset_history)
  schedule_next_tick()
end

function ui()
  return {
    {
      id = "wave_run",
      title = "Lua 波形演示 / 运行控制",
      anchor = "left_bottom",
      tab_group = "wave_tools",
      controls = {
        { type = "button", id = "start", label = "开始" },
        { type = "button", id = "pause", label = "暂停" },
        { type = "button", id = "resume", label = "恢复" },
        { type = "button", id = "clear_history", label = "清空历史" }
      }
    },
    {
      id = "wave_params",
      title = "Lua 波形演示 / 参数",
      anchor = "left_bottom",
      tab_group = "wave_tools",
      controls = {
        { type = "input_float", id = "frequency_hz", label = "频率(Hz)", default = defaults.frequency_hz },
        { type = "input_float", id = "amplitude", label = "幅值", default = defaults.amplitude },
        { type = "input_float", id = "offset", label = "偏置", default = defaults.offset },
        { type = "input_float", id = "phase_deg", label = "相位(度)", default = defaults.phase_deg },
        { type = "input_float", id = "sample_rate_hz", label = "采样率(Hz)", default = defaults.sample_rate_hz },
        { type = "input_int", id = "points_per_tick", label = "每次点数", default = defaults.points_per_tick },
        { type = "input_int", id = "timer_ms", label = "刷新间隔(ms)", default = defaults.timer_ms }
      }
    },
    {
      id = "wave_channels",
      title = "Lua 波形演示 / 通道",
      anchor = "left_bottom",
      tab_group = "wave_tools",
      controls = {
        { type = "checkbox", id = "show_sine", label = "显示正弦", default = defaults.show_sine },
        { type = "checkbox", id = "show_triangle", label = "显示三角", default = defaults.show_triangle },
        { type = "checkbox", id = "show_square", label = "显示方波", default = defaults.show_square },
        { type = "checkbox", id = "show_saw", label = "显示锯齿", default = defaults.show_saw }
      }
    }
  }
end

function on_control(ctx, id, value)
  if id == "start" then
    running = true
    time_cursor = 0.0
    restart(true)
  elseif id == "pause" then
    running = false
  elseif id == "resume" then
    running = true
    schedule_next_tick()
  elseif id == "clear_history" then
    time_cursor = 0.0
    restart(true)
  elseif id == "frequency_hz" or id == "amplitude" or id == "offset" or id == "phase_deg"
      or id == "sample_rate_hz" or id == "points_per_tick" or id == "timer_ms"
      or id == "show_sine" or id == "show_triangle" or id == "show_square" or id == "show_saw" then
    setup_plot(true)
  end
end

function on_timer(ctx, name)
  if name ~= timer_name then
    return
  end
  if running then
    emit_samples()
    schedule_next_tick()
  end
end

-- 加载后自动启动，让用户无需串口连接即可立即看到波形。
restart(true)
)WAVE";
const char* kHalfDuplexModbusCommonLua = R"MODBUS_COMMON(-- 核心流程：这里是通用半双工帧 codec，只负责按调用方传入的 schema 组帧、解帧和流式解析。

local M = {}

-- 默认帧头和长度字段配置；具体协议可以在自己的 protocol 表中覆盖。
M.DEFAULT_HEADER = { 0xA5, 0x5A }
M.LENGTH_SPEC = { type = "u16", endian = "le", unit = "bytes" }
M.SEQUENCE_BITS = 8

local TYPE_WIDTH = {
  u8 = 1,
  i8 = 1,
  u16 = 2,
  i16 = 2,
  u32 = 4,
  i32 = 4,
  bytes = 1,
}

local UNIT_WIDTH = {
  bytes = 1,
  u16 = 2,
  u32 = 4,
}

local function append_bytes(target, source)
  for index = 1, #source do
    target[#target + 1] = source[index]
  end
end

local function copy_bytes(source, first_index, last_index)
  local bytes = {}
  for index = first_index or 1, last_index or #source do
    bytes[#bytes + 1] = source[index]
  end
  return bytes
end

local function clamp_byte(value)
  return math.floor(value) & 0xFF
end

local function scalar_width(type_name)
  return TYPE_WIDTH[type_name]
end

local function unit_width(unit_name)
  return UNIT_WIDTH[unit_name or "bytes"] or 1
end

local function read_unsigned(bytes, offset, width, endian)
  local value = 0
  if endian == "be" then
    for step = 0, width - 1 do
      value = (value << 8) | (bytes[offset + step + 1] or 0)
    end
  else
    for step = width - 1, 0, -1 do
      value = (value << 8) | (bytes[offset + step + 1] or 0)
    end
  end
  return value
end

local function write_unsigned(value, width, endian)
  local unsigned = math.floor(value)
  local bytes = {}
  if endian == "be" then
    for step = width - 1, 0, -1 do
      bytes[#bytes + 1] = clamp_byte(unsigned >> (step * 8))
    end
  else
    for step = 0, width - 1 do
      bytes[#bytes + 1] = clamp_byte(unsigned >> (step * 8))
    end
  end
  return bytes
end

local function signed_from_unsigned(value, width)
  local bits = width * 8
  local sign_bit = 1 << (bits - 1)
  local modulus = 1 << bits
  if value >= sign_bit then
    return value - modulus
  end
  return value
end

local function unsigned_from_signed(value, width)
  local bits = width * 8
  local modulus = 1 << bits
  if value < 0 then
    return value + modulus
  end
  return value
end

local function resolve_reference(ref, values, bytes_remaining, field)
  if type(ref) == "function" then
    return ref(values, bytes_remaining, field)
  end
  if type(ref) == "string" then
    return values[ref]
  end
  return ref
end

local function resolve_repeat_count(field, values, bytes_remaining, item_width)
  if field.count ~= nil then
    return resolve_reference(field.count, values, bytes_remaining, field), nil
  end
  if field.count_from ~= nil then
    return resolve_reference(field.count_from, values, bytes_remaining, field), nil
  end
  if field.length_from ~= nil then
    local byte_length = resolve_reference(field.length_from, values, bytes_remaining, field) or 0
    if (byte_length % item_width) ~= 0 then
      return nil, string.format("字段 %s 长度 %d 无法按 %d 字节对齐", field.name, byte_length, item_width)
    end
    return math.floor(byte_length / item_width), nil
  end
  if field.until_end then
    if (bytes_remaining % item_width) ~= 0 then
      return nil, string.format("字段 %s 剩余长度 %d 无法按 %d 字节对齐", field.name, bytes_remaining, item_width)
    end
    return math.floor(bytes_remaining / item_width), nil
  end
  return 1, nil
end

local function is_array_field(field)
  return field.count ~= nil
      or field.count_from ~= nil
      or field.length_from ~= nil
      or field.until_end == true
      or field.is_array == true
end

local function encode_scalar(type_name, value, endian)
  local width = scalar_width(type_name)
  if not width then
    return nil, "不支持的字段类型: " .. tostring(type_name)
  end
  local unsigned = type_name:sub(1, 1) == "i" and unsigned_from_signed(value, width) or math.floor(value)
  return write_unsigned(unsigned, width, endian or "le"), nil
end

local function decode_scalar(type_name, bytes, offset, endian)
  local width = scalar_width(type_name)
  if not width then
    return nil, "不支持的字段类型: " .. tostring(type_name)
  end
  local value = read_unsigned(bytes, offset, width, endian or "le")
  if type_name:sub(1, 1) == "i" then
    value = signed_from_unsigned(value, width)
  end
  return value, nil
end

-- 按 schema.fields 把 Lua table 编码成 payload 字节数组。
-- 支持标量字段、定长/动态数组字段和 bytes 字段；offset 可用于跳过保留字节。
function M.encode_fields(fields, values)
  local buffer = {}
  local cursor = 0
  for _, field in ipairs(fields) do
    local offset = field.offset ~= nil and field.offset or cursor
    while #buffer < offset do
      buffer[#buffer + 1] = 0
    end

    local field_bytes = {}
    if field.type == "bytes" then
      local source = values[field.name] or {}
      if type(source) ~= "table" then
        return nil, "字段 " .. field.name .. " 必须提供 bytes table"
      end
      field_bytes = copy_bytes(source)
    elseif is_array_field(field) then
      local items = values[field.name] or {}
      if type(items) ~= "table" then
        return nil, "字段 " .. field.name .. " 必须提供数组"
      end
      local width = scalar_width(field.type)
      local expected_count, count_error = resolve_repeat_count(field, values, #items * width, width)
      if not expected_count then
        return nil, count_error
      end
      if expected_count ~= #items then
        return nil, string.format("字段 %s 期望 %d 项，实际 %d 项", field.name, expected_count, #items)
      end
      for item_index = 1, #items do
        local encoded, encode_error = encode_scalar(field.type, items[item_index], field.endian)
        if not encoded then
          return nil, encode_error
        end
        append_bytes(field_bytes, encoded)
      end
    else
      local encoded, encode_error = encode_scalar(field.type, values[field.name] or 0, field.endian)
      if not encoded then
        return nil, encode_error
      end
      field_bytes = encoded
    end

    append_bytes(buffer, field_bytes)
    cursor = math.max(cursor, offset + #field_bytes)
  end
  return buffer, nil
end

-- 按 schema.fields 从 payload 字节数组解出 Lua table。
-- 数组字段可通过 count/count_from/length_from/until_end 描述长度来源。
function M.decode_fields(fields, bytes)
  local values = {}
  local cursor = 0
  for _, field in ipairs(fields) do
    local offset = field.offset ~= nil and field.offset or cursor
    if offset > #bytes then
      return nil, string.format("字段 %s 偏移 %d 超出负载长度 %d", field.name, offset, #bytes)
    end

    if field.type == "bytes" then
      local byte_length, count_error = resolve_repeat_count(field, values, #bytes - offset, 1)
      if not byte_length then
        return nil, count_error
      end
      if offset + byte_length > #bytes then
        return nil, string.format("字段 %s 长度不足", field.name)
      end
      values[field.name] = copy_bytes(bytes, offset + 1, offset + byte_length)
      cursor = math.max(cursor, offset + byte_length)
    elseif is_array_field(field) then
      local width = scalar_width(field.type)
      local item_count, count_error = resolve_repeat_count(field, values, #bytes - offset, width)
      if not item_count then
        return nil, count_error
      end
      local total_width = item_count * width
      if offset + total_width > #bytes then
        return nil, string.format("字段 %s 长度不足，期望 %d 字节，实际剩余 %d", field.name, total_width, #bytes - offset)
      end
      local decoded = {}
      for item_index = 0, item_count - 1 do
        local value, decode_error = decode_scalar(field.type, bytes, offset + item_index * width, field.endian)
        if decode_error then
          return nil, decode_error
        end
        decoded[#decoded + 1] = value
      end
      values[field.name] = decoded
      cursor = math.max(cursor, offset + total_width)
    else
      local width = scalar_width(field.type)
      if offset + width > #bytes then
        return nil, string.format("字段 %s 长度不足", field.name)
      end
      local value, decode_error = decode_scalar(field.type, bytes, offset, field.endian)
      if decode_error then
        return nil, decode_error
      end
      values[field.name] = value
      cursor = math.max(cursor, offset + width)
    end
  end
  return values, nil
end

local function encode_length(payload_size, length_spec)
  local unit = unit_width(length_spec.unit)
  if (payload_size % unit) ~= 0 then
    return nil, string.format("长度 %d 不能按 %s 单位编码", payload_size, length_spec.unit or "bytes")
  end
  local length_value = math.floor(payload_size / unit)
  return encode_scalar(length_spec.type or "u16", length_value, length_spec.endian or "le")
end

local function decode_length(bytes, offset, length_spec)
  local length_value, decode_error = decode_scalar(length_spec.type or "u16", bytes, offset, length_spec.endian or "le")
  if decode_error then
    return nil, decode_error
  end
  return length_value * unit_width(length_spec.unit), nil
end

local function find_header(buffer, header)
  for start = 1, #buffer - #header + 1 do
    local matched = true
    for offset = 1, #header do
      if buffer[start + offset - 1] ~= header[offset] then
        matched = false
        break
      end
    end
    if matched then
      return start
    end
  end
  return nil
end

local function popcount(mask)
  local count = 0
  local current = mask or 0
  while current > 0 do
    count = count + (current & 1)
    current = current >> 1
  end
  return count
end

M.popcount = popcount

-- 统计 bitmask 中置 1 的位数，常用于按通道掩码推导数组长度。
function M.popcount(mask)
  return popcount(mask)
end

-- 把 1-based 通道开关数组转换成 bitmask；channel_count 缺省时按 values 长度计算。
function M.channel_mask_from_values(values, channel_count)
  local mask = 0
  local count = channel_count or #values
  for channel_index = 1, count do
    if values[channel_index] and values[channel_index] ~= 0 then
      mask = mask | (1 << (channel_index - 1))
    end
  end
  return mask
end

-- 把 bitmask 转回 1-based 通道序号数组；channel_count 用于限制最高检查位。
function M.enabled_channels_from_mask(mask, channel_count)
  local channels = {}
  local count = channel_count or 32
  for channel_index = 1, count do
    if (mask & (1 << (channel_index - 1))) ~= 0 then
      channels[#channels + 1] = channel_index
    end
  end
  return channels
end

-- 追踪每个 key 的连续序号，返回本帧是否初始化、是否丢帧以及累计丢帧数。
function M.track_sequence(tracker, key, sequence, bits)
  local modulus = 1 << (bits or M.SEQUENCE_BITS)
  local slot = tracker[key]
  if not slot then
    slot = {
      initialized = false,
      expected = 0,
      lost_total = 0,
    }
    tracker[key] = slot
  end

  local result = {
    expected = slot.expected,
    current = sequence,
    lost = 0,
    lost_total = slot.lost_total,
  }

  if not slot.initialized then
    slot.initialized = true
    slot.expected = (sequence + 1) % modulus
    result.expected = sequence
    return result
  end

  local lost = (sequence - slot.expected) % modulus
  if lost > 0 then
    slot.lost_total = slot.lost_total + lost
  end
  slot.expected = (sequence + 1) % modulus
  result.lost = lost
  result.expected = slot.expected
  result.lost_total = slot.lost_total
  return result
end

local function message_schema(protocol, func)
  return protocol.messages[func]
end

local function remove_prefix(buffer, count)
  for _ = 1, count do
    table.remove(buffer, 1)
  end
end

-- 创建流式解析器状态。调用方必须传入具体 protocol schema。
function M.new_parser(protocol)
  return {
    protocol = protocol,
    buffer = {},
    sequence = {},
  }
end

-- 按具体 protocol schema 组一帧：header + func + seq + len + payload + crc16_modbus。
function M.build_frame(protocol, func, sequence, payload_values)
  local codec = protocol
  if not codec then
    return nil, "必须提供 protocol schema"
  end
  local schema = message_schema(codec, func)
  if not schema then
    return nil, string.format("未定义的功能码: 0x%02X", func)
  end

  local payload, payload_error = M.encode_fields(schema.fields or {}, payload_values or {})
  if not payload then
    return nil, payload_error
  end

  local length_bytes, length_error = encode_length(#payload, codec.length or M.LENGTH_SPEC)
  if not length_bytes then
    return nil, length_error
  end

  local frame = copy_bytes(codec.header or M.DEFAULT_HEADER)
  frame[#frame + 1] = clamp_byte(func)
  frame[#frame + 1] = clamp_byte(sequence or 0)
  append_bytes(frame, length_bytes)
  append_bytes(frame, payload)

  local crc = proto.crc16_modbus(frame)
  frame[#frame + 1] = clamp_byte(crc)
  frame[#frame + 1] = clamp_byte(crc >> 8)
  return frame, nil
end

-- 追加输入字节并尽可能解析完整帧；半包保留在 state.buffer，坏帧以 errors 返回。
function M.feed_parser(state, incoming_bytes)
  append_bytes(state.buffer, incoming_bytes)

  local frames = {}
  local errors = {}
  local header = state.protocol.header or M.DEFAULT_HEADER
  local length_width = scalar_width((state.protocol.length or M.LENGTH_SPEC).type or "u16")
  local frame_prefix_size = #header + 1 + 1 + length_width
  local frame_min_size = frame_prefix_size + 2

  while true do
    local start = find_header(state.buffer, header)
    if not start then
      if #state.buffer > (#header - 1) then
        state.buffer = copy_bytes(state.buffer, #state.buffer - #header + 2, #state.buffer)
      end
      break
    end

    if start > 1 then
      remove_prefix(state.buffer, start - 1)
    end

    if #state.buffer < frame_min_size then
      break
    end

    local payload_size, length_error = decode_length(state.buffer, #header + 2, state.protocol.length or M.LENGTH_SPEC)
    if not payload_size then
      errors[#errors + 1] = { kind = "length", message = length_error }
      remove_prefix(state.buffer, 1)
      goto continue
    end

    local total_size = frame_prefix_size + payload_size + 2
    if #state.buffer < total_size then
      break
    end

    local frame_bytes = copy_bytes(state.buffer, 1, total_size)
    remove_prefix(state.buffer, total_size)

    local crc_expected = proto.crc16_modbus(copy_bytes(frame_bytes, 1, #frame_bytes - 2))
    local crc_low = frame_bytes[#frame_bytes - 1]
    local crc_high = frame_bytes[#frame_bytes]
    local crc_actual = crc_low | (crc_high << 8)
    if crc_expected ~= crc_actual then
      errors[#errors + 1] = {
        kind = "crc",
        message = string.format("CRC 不匹配，期望 0x%04X，实际 0x%04X", crc_expected, crc_actual),
      }
      goto continue
    end

    local func = frame_bytes[#header + 1]
    local sequence = frame_bytes[#header + 2]
    local payload = copy_bytes(frame_bytes, frame_prefix_size + 1, frame_prefix_size + payload_size)
    local schema = message_schema(state.protocol, func)
    if not schema then
      errors[#errors + 1] = {
        kind = "func",
        func = func,
        sequence = sequence,
        message = string.format("未知功能码 0x%02X", func),
      }
      goto continue
    end

    local decoded, decode_error = M.decode_fields(schema.fields or {}, payload)
    if not decoded then
      errors[#errors + 1] = {
        kind = "schema",
        func = func,
        sequence = sequence,
        message = decode_error,
      }
      goto continue
    end

    local frame = {
      func = func,
      name = schema.name,
      sequence = sequence,
      payload_size = payload_size,
      payload = decoded,
      payload_bytes = payload,
      raw = frame_bytes,
      sequence_state = M.track_sequence(state.sequence, func, sequence, state.protocol.sequence_bits or M.SEQUENCE_BITS),
    }
    frames[#frames + 1] = frame

    ::continue::
  end

  return frames, errors
end

return M)MODBUS_COMMON";

const char* kHalfDuplexModbusMasterLua = R"MODBUS_MASTER(-- 核心流程：上位机只做三件事——按寄存器分批入队 request、在 ACK 到达时 request_done、把主动上报帧推成波形。

local modbus = require("half_duplex_modbus_common")

local FUNC_WRITE_REGISTERS = 0x10
local FUNC_WRITE_ACK = 0x90
local FUNC_STREAM_DATA = 0x91

local REG_CH1 = 0x5AA5
local REG_CH2 = 0x5AA6
local REG_CH3 = 0x5AA7
local REG_CH4 = 0x5AA8
local REG_START = 0x5AA9

local STATUS_OK = 0
local STATUS_ERROR = 1
local CHANNEL_COUNT = 4
local CHANNEL_SCALE = 0.001
local DEFAULT_INTERVAL_MS = 100
local DEFAULT_SAMPLE_COUNT = 16

-- 协议 schema 放在具体协议脚本里；common 只按这里的定义做编解码。
local protocol = {
  header = modbus.DEFAULT_HEADER,
  length = modbus.LENGTH_SPEC,
  sequence_bits = modbus.SEQUENCE_BITS,
  messages = {
    [FUNC_WRITE_REGISTERS] = {
      name = "write_registers",
      fields = {
        { name = "start_address", type = "u16", endian = "le", offset = 0 },
        { name = "register_count", type = "u8", offset = 2 },
        { name = "values", type = "u16", endian = "le", count_from = "register_count" },
      },
    },
    [FUNC_WRITE_ACK] = {
      name = "write_ack",
      fields = {
        { name = "status", type = "u8", offset = 0 },
        { name = "start_address", type = "u16", endian = "le", offset = 1 },
        { name = "register_count", type = "u8", offset = 3 },
      },
    },
    [FUNC_STREAM_DATA] = {
      name = "stream_data",
      fields = {
        { name = "timestamp_ms", type = "u32", endian = "le", offset = 0 },
        { name = "channel_mask", type = "u8" },
        { name = "sample_count", type = "u8" },
        {
          name = "samples",
          type = "i16",
          endian = "le",
          count_from = function(values)
            return (values.sample_count or 0) * modbus.popcount(values.channel_mask or 0)
          end,
        },
      },
    },
  },
}

local controls = {
  ch1 = true,
  ch2 = true,
  ch3 = true,
  ch4 = true,
  interval_ms = DEFAULT_INTERVAL_MS,
  samples_per_frame = DEFAULT_SAMPLE_COUNT,
}

local parser = modbus.new_parser(protocol)
local next_request_sequence = 0

local function next_sequence()
  local value = next_request_sequence
  next_request_sequence = (next_request_sequence + 1) % 256
  return value
end

local function read_checkbox(id)
  return controls[id] and 1 or 0
end

local function read_positive_int(id, fallback)
  local value = tonumber(controls[id]) or fallback
  value = math.floor(value)
  if value < 1 then
    return fallback
  end
  return value
end

local function configured_channel_mask()
  return modbus.channel_mask_from_values({
    read_checkbox("ch1"),
    read_checkbox("ch2"),
    read_checkbox("ch3"),
    read_checkbox("ch4"),
  }, CHANNEL_COUNT)
end

local function setup_plot(reset_history)
  proto.plot.setup({
    source = "half_duplex_modbus_master",
    reset_history = reset_history,
    channels = {
      { label = "CH1 正弦", unit = "V", scale = 1.0, color = "#4FC3F7" },
      { label = "CH2 偏置正弦", unit = "V", scale = 1.0, color = "#81C784" },
      { label = "CH3 高斯噪声", unit = "V", scale = 1.0, color = "#FFB74D" },
      { label = "CH4 三角波", unit = "V", scale = 1.0, color = "#E57373" },
    },
  })
end

local function request_frame(frame, tag)
  local request_id, err = proto.request(frame, {
    timeout_ms = 1000,
    tag = tag,
  })
  if not request_id then
    proto.status.set("请求入队失败: " .. tostring(err), { level = "error" })
    return false
  end
  return true
end

local function queue_write(start_address, values, tag)
  local frame, err = modbus.build_frame(protocol, FUNC_WRITE_REGISTERS, next_sequence(), {
    start_address = start_address,
    register_count = #values,
    values = values,
  })
  if not frame then
    proto.status.set("组帧失败: " .. tostring(err), { level = "error" })
    return false
  end
  return request_frame(frame, tag)
end

local function auto_configure_and_start()
  local batches = {
    { start = REG_CH1, values = { read_checkbox("ch1"), read_checkbox("ch2") }, tag = "cfg_ch12" },
    { start = REG_CH3, values = { read_checkbox("ch3"), read_checkbox("ch4") }, tag = "cfg_ch34" },
    { start = REG_START, values = { 1 }, tag = "cfg_start" },
  }

  for _, batch in ipairs(batches) do
    if not queue_write(batch.start, batch.values, batch.tag) then
      return
    end
  end

  proto.status.set(string.format("已入队 3 组半双工请求，目标通道掩码 0x%02X", configured_channel_mask()), {
    level = "info",
  })
end

local function send_start_stop(start_value)
  local tag = start_value == 1 and "start_stream" or "stop_stream"
  queue_write(REG_START, { start_value }, tag)
end

local function handle_ack(frame)
  local ok = (frame.payload.status or STATUS_ERROR) == STATUS_OK
  local message = string.format("ACK 0x%04X x %d", frame.payload.start_address or 0, frame.payload.register_count or 0)
  proto.request_done({
    ok = ok,
    message = message,
  })
  if ok then
    proto.status.set(message, { level = "info" })
  else
    proto.status.set("从机 ACK 报错: " .. message, { level = "error" })
  end
end

local function handle_stream(frame)
  local enabled_channels = modbus.enabled_channels_from_mask(frame.payload.channel_mask or 0, CHANNEL_COUNT)
  local sample_count = frame.payload.sample_count or 0
  local flat_samples = frame.payload.samples or {}
  local interval_ms = read_positive_int("interval_ms", DEFAULT_INTERVAL_MS)
  local dt = (interval_ms / 1000.0) / math.max(sample_count, 1)
  local base_t = (frame.payload.timestamp_ms or 0) / 1000.0

  local sequence_state = frame.sequence_state
  if sequence_state and sequence_state.lost > 0 then
    proto.status.set(string.format("丢帧: %d", sequence_state.lost_total), { level = "warn" })
  end

  for channel_slot, channel_index in ipairs(enabled_channels) do
    local samples = {}
    for point_index = 1, sample_count do
      -- 从机按“采样点 -> 通道”的交错顺序发送，主机必须按同样布局拆回各通道。
      local sample_index = (point_index - 1) * #enabled_channels + channel_slot
      local raw = flat_samples[sample_index] or 0
      samples[#samples + 1] = {
        t = base_t + (point_index - 1) * dt,
        y = raw * CHANNEL_SCALE,
      }
    end
    proto.plot.push(channel_index, {
      source = "half_duplex_modbus_stream",
      samples = samples,
    })
  end
end

local function report_parse_errors(errors)
  for _, item in ipairs(errors) do
    proto.status.set("解析失败: " .. tostring(item.message), { level = "error" })
  end
end

function ui()
  return {
    {
      id = "half_duplex_master",
      title = "半双工 Modbus Master",
      anchor = "left_bottom",
      controls = {
        { type = "checkbox", id = "ch1", label = "CH1 正弦", default = true },
        { type = "checkbox", id = "ch2", label = "CH2 偏置正弦", default = true },
        { type = "checkbox", id = "ch3", label = "CH3 高斯噪声", default = true },
        { type = "checkbox", id = "ch4", label = "CH4 三角波", default = true },
        { type = "input_int", id = "interval_ms", label = "发送间隔(ms)", default = DEFAULT_INTERVAL_MS },
        { type = "input_int", id = "samples_per_frame", label = "每帧点数", default = DEFAULT_SAMPLE_COUNT },
        { type = "button", id = "auto_start", label = "自动配置并启动" },
        { type = "button", id = "start_stream", label = "仅启动" },
        { type = "button", id = "stop_stream", label = "停止" },
        { type = "button", id = "clear_plot", label = "清空波形" },
      },
    },
  }
end

function on_open(ctx)
  parser = modbus.new_parser(protocol)
  proto.status.clear()
  setup_plot(true)
end

function on_close(ctx)
  proto.status.clear()
end

function on_error(ctx, message)
  proto.status.set("连接错误: " .. tostring(message), { level = "error" })
end

function on_tx(ctx, evt)
  if evt.kind == "request" and evt.state == "timeout" then
    proto.status.set("请求超时: " .. tostring(evt.tag), { level = "warn" })
  elseif evt.kind == "request" and evt.state == "rejected" then
    proto.status.set("请求被拒绝: " .. tostring(evt.tag), { level = "error" })
  end
end

function on_control(ctx, id, value)
  controls[id] = value

  if id == "auto_start" then
    auto_configure_and_start()
  elseif id == "start_stream" then
    send_start_stop(1)
  elseif id == "stop_stream" then
    send_start_stop(0)
  elseif id == "clear_plot" then
    setup_plot(true)
    proto.status.set("波形已清空", { level = "info" })
  end
end

function on_bytes(ctx, bytes)
  local frames, errors = modbus.feed_parser(parser, bytes)
  if #errors > 0 then
    report_parse_errors(errors)
  end

  for _, frame in ipairs(frames) do
    if frame.func == FUNC_WRITE_ACK then
      handle_ack(frame)
    elseif frame.func == FUNC_STREAM_DATA then
      handle_stream(frame)
    end
  end
end)MODBUS_MASTER";

const char* kHalfDuplexModbusSlaveLua = R"MODBUS_SLAVE(-- 核心流程：从机只接写寄存器请求，回 ACK；当 start 寄存器置 1 后，由定时器主动发波形帧。

local modbus = require("half_duplex_modbus_common")

local FUNC_WRITE_REGISTERS = 0x10
local FUNC_WRITE_ACK = 0x90
local FUNC_STREAM_DATA = 0x91

local REG_CH1 = 0x5AA5
local REG_CH2 = 0x5AA6
local REG_CH3 = 0x5AA7
local REG_CH4 = 0x5AA8
local REG_START = 0x5AA9

local STATUS_OK = 0
local CHANNEL_COUNT = 4
local CHANNEL_SCALE = 0.001
local DEFAULT_INTERVAL_MS = 100
local DEFAULT_SAMPLE_COUNT = 16

-- 协议 schema 放在具体协议脚本里；common 只按这里的定义做编解码。
local protocol = {
  header = modbus.DEFAULT_HEADER,
  length = modbus.LENGTH_SPEC,
  sequence_bits = modbus.SEQUENCE_BITS,
  messages = {
    [FUNC_WRITE_REGISTERS] = {
      name = "write_registers",
      fields = {
        { name = "start_address", type = "u16", endian = "le", offset = 0 },
        { name = "register_count", type = "u8", offset = 2 },
        { name = "values", type = "u16", endian = "le", count_from = "register_count" },
      },
    },
    [FUNC_WRITE_ACK] = {
      name = "write_ack",
      fields = {
        { name = "status", type = "u8", offset = 0 },
        { name = "start_address", type = "u16", endian = "le", offset = 1 },
        { name = "register_count", type = "u8", offset = 3 },
      },
    },
    [FUNC_STREAM_DATA] = {
      name = "stream_data",
      fields = {
        { name = "timestamp_ms", type = "u32", endian = "le", offset = 0 },
        { name = "channel_mask", type = "u8" },
        { name = "sample_count", type = "u8" },
        {
          name = "samples",
          type = "i16",
          endian = "le",
          count_from = function(values)
            return (values.sample_count or 0) * modbus.popcount(values.channel_mask or 0)
          end,
        },
      },
    },
  },
}

local parser = modbus.new_parser(protocol)
local registers = {
  [REG_CH1] = 1,
  [REG_CH2] = 1,
  [REG_CH3] = 1,
  [REG_CH4] = 1,
  [REG_START] = 0,
}

local wave_sequence = 0
local timestamp_ms = 0
local sample_cursor = 0

local function clamp_i16(value)
  if value > 32767 then
    return 32767
  end
  if value < -32768 then
    return -32768
  end
  return math.floor(value)
end

local function gaussian_noise(seed)
  local x1 = math.abs(math.sin(seed * 12.9898))
  local x2 = math.abs(math.sin((seed + 17.0) * 78.233))
  local u1 = math.max(x1 % 1.0, 1e-6)
  local u2 = x2 % 1.0
  return math.sqrt(-2.0 * math.log(u1)) * math.cos(2.0 * math.pi * u2)
end

local function triangle_wave(phase)
  local cycle = (phase / (2.0 * math.pi)) % 1.0
  return 4.0 * math.abs(cycle - 0.5) - 1.0
end

local function channel_value(channel_index, sample_index)
  local phase = sample_index * 0.15
  if channel_index == 1 then
    return math.sin(phase)
  elseif channel_index == 2 then
    return 0.35 + 0.65 * math.sin(phase * 0.8 + 0.6)
  elseif channel_index == 3 then
    return gaussian_noise(sample_index) * 0.2
  end
  return triangle_wave(phase * 0.6)
end

local function build_samples(channel_mask, sample_count)
  local samples = {}
  local channels = modbus.enabled_channels_from_mask(channel_mask, CHANNEL_COUNT)
  for point_index = 1, sample_count do
    for _, channel_index in ipairs(channels) do
      local raw = channel_value(channel_index, sample_cursor + point_index)
      samples[#samples + 1] = clamp_i16(raw / CHANNEL_SCALE)
    end
  end
  sample_cursor = sample_cursor + sample_count
  return samples
end

local function start_streaming()
  if registers[REG_START] ~= 1 then
    registers[REG_START] = 1
  end
  proto.set_timer("stream_tick", DEFAULT_INTERVAL_MS)
end

local function stop_streaming()
  registers[REG_START] = 0
  proto.cancel_timer("stream_tick")
end

local function apply_register_write(start_address, values)
  for offset = 1, #values do
    registers[start_address + offset - 1] = values[offset]
  end
  if (registers[REG_START] or 0) == 1 then
    start_streaming()
  else
    stop_streaming()
  end
end

local function send_ack(sequence, start_address, register_count)
  local frame, err = modbus.build_frame(protocol, FUNC_WRITE_ACK, sequence, {
    status = STATUS_OK,
    start_address = start_address,
    register_count = register_count,
  })
  if not frame then
    proto.status.set("ACK 组帧失败: " .. tostring(err), { level = "error" })
    return
  end
  proto.send(frame, { tag = "write_ack" })
end

local function handle_write_request(frame)
  apply_register_write(frame.payload.start_address, frame.payload.values or {})
  send_ack(frame.sequence, frame.payload.start_address, frame.payload.register_count or 0)
end

local function report_parse_errors(errors)
  for _, item in ipairs(errors) do
    proto.status.set("请求解析失败: " .. tostring(item.message), { level = "error" })
  end
end

function ui()
  return {
    {
      id = "half_duplex_slave",
      title = "半双工 Modbus Slave",
      anchor = "left_bottom",
      controls = {
        { type = "button", id = "noop", label = "从机自动模式" },
      },
    },
  }
end

function on_open(ctx)
  parser = modbus.new_parser(protocol)
  timestamp_ms = 0
  wave_sequence = 0
  sample_cursor = 0
  stop_streaming()
  proto.status.set("从机已就绪，等待写寄存器请求", { level = "info" })
end

function on_close(ctx)
  stop_streaming()
  proto.status.clear()
end

function on_error(ctx, message)
  proto.status.set("从机连接错误: " .. tostring(message), { level = "error" })
end

function on_control(ctx, id, value)
end

function on_bytes(ctx, bytes)
  local frames, errors = modbus.feed_parser(parser, bytes)
  if #errors > 0 then
    report_parse_errors(errors)
  end

  for _, frame in ipairs(frames) do
    if frame.func == FUNC_WRITE_REGISTERS then
      handle_write_request(frame)
    end
  end
end

function on_timer(ctx, timer_name)
  if timer_name ~= "stream_tick" then
    return
  end
  if (registers[REG_START] or 0) ~= 1 then
    return
  end

  local channel_mask = modbus.channel_mask_from_values({
    registers[REG_CH1] or 0,
    registers[REG_CH2] or 0,
    registers[REG_CH3] or 0,
    registers[REG_CH4] or 0,
  }, CHANNEL_COUNT)
  if channel_mask == 0 then
    proto.set_timer("stream_tick", DEFAULT_INTERVAL_MS)
    return
  end

  local sample_count = DEFAULT_SAMPLE_COUNT
  local samples = build_samples(channel_mask, sample_count)
  local frame, err = modbus.build_frame(protocol, FUNC_STREAM_DATA, wave_sequence, {
    timestamp_ms = timestamp_ms,
    channel_mask = channel_mask,
    sample_count = sample_count,
    samples = samples,
  })
  if not frame then
    proto.status.set("波形组帧失败: " .. tostring(err), { level = "error" })
    return
  end

  proto.send(frame, { tag = "stream_data" })
  wave_sequence = (wave_sequence + 1) % 256
  timestamp_ms = timestamp_ms + DEFAULT_INTERVAL_MS
  proto.set_timer("stream_tick", DEFAULT_INTERVAL_MS)
end)MODBUS_SLAVE";

const char* kDefaultProtocolsReadme = R"README(# ProtoScope Lua 协议脚本指南

`protocols` 是 ProtoScope 的 Lua 协议工作区。宿主会自动注入全局 `proto`，脚本只负责描述 UI、协议解析和在“完整应答完成”时调用 `proto.request_done()`。

## 发送模型

新的发送 API 分成两类：

- `proto.send(payload, opts?)`：普通异步发送，写出成功后会收到一次 `on_tx(..., { state = "sent" })`。
- `proto.request(payload, opts?)`：半双工请求。宿主负责排队、串行下发、超时、取消和下一条自动推进。
- `proto.request_done(result?)`：脚本在**确认收到完整应答**后调用；不需要传 `request_id`。

示例：

```lua
local request_id, err = proto.request({ 0xAA, 0x55, 0x01, 0x0D }, {
  timeout_ms = 1000,
  tag = "read_version",
})

function on_bytes(ctx, bytes)
  if has_full_response(bytes) then
    proto.request_done({ ok = true, message = "版本应答完成" })
  end
end
```

## `on_tx` 回调

宿主只在主线程回调 `on_tx(ctx, evt)`：

```lua
---@class ProtoTxEvent
---@field id integer
---@field kind 'send'|'request'
---@field state 'sent'|'completed'|'timeout'|'rejected'|'dropped'|'canceled'
---@field tag string
---@field bytes integer
---@field queued_ms integer
---@field finished_ms integer
---@field error? string
```

## 半双工 Modbus Schema Demo

仓库内新增了一组双端 demo：

- `protocols/half_duplex_modbus_master`：上位机脚本。用 `proto.request()` 分 3 组写寄存器：`0x5AA5~0x5AA6`、`0x5AA7~0x5AA8`、`0x5AA9`。
- `protocols/half_duplex_modbus_slave`：从机脚本。收到写寄存器请求后回复 ACK；当 `0x5AA9 = 1` 时，通过 `proto.set_timer()` 主动上报波形。
- `protocols/half_duplex_modbus_common.lua`：通用流式 codec，只负责按各协议脚本传入的 schema 组帧/解帧，默认帧格式为 `header + func + seq + len + payload + crc16_modbus`。

### 固定寄存器语义

- `0x5AA5~0x5AA8`：4 个通道开关，写 `1` 开启，其他值关闭。
- `0x5AA9`：启动位，写 `1` 开始传输，写 `0` 停止。

### 主机端行为

- “自动配置并启动”会一次入队 3 条半双工请求，由宿主 request 队列串行下发。
- 主机不主动轮询；只在收到完整 ACK 时调用 `proto.request_done()`，并在收到主动上报帧后推送 `proto.plot.push()`。
- 当流式数据序列号跳号时，主机会累计丢帧数，并通过 `proto.status.set(string.format("丢帧: %d", lost_total), { level = "warn" })` 提示。

### 从机端行为

- ACK 与主动上报共用同一套 schema，长度字段默认按字节计数，CRC 为 `proto.crc16_modbus()`。
- 波形帧载荷固定包含 `timestamp_ms`、`channel_mask`、`sample_count` 和按启用通道展开的 `i16` 采样值。
- 4 个通道分别输出正弦、带偏置正弦、高斯噪声、三角波；只对开启的通道生成数据。

其中：

- `sent`：字节已经写出。
- `completed`：仅 `request` 在 `proto.request_done()` 后触发。
- `timeout` / `rejected` / `dropped` / `canceled`：都属于宿主终态。

## 状态栏与弹窗 API

```lua
proto.status.set("读取版本请求已写出", { level = "info" })
proto.status.clear()

proto.ui.alert({
  title = "请求超时",
  message = "读取版本请求超时",
  level = "warn",
  dedupe_key = "read_version_timeout",
})
```

- `proto.status.set(text, opts?)`：写入底部状态栏，`opts.level` 当前只做轻量级语义标记。
- `proto.ui.alert(opts)`：宿主管理的模态框，固定“关闭”按钮。
- `proto.ui.confirm(opts)`：宿主管理的确认框，固定“确认 / 取消”。
- `dedupe_key`：同一个 key 在未关闭前只保留一个弹窗。
- 用户关闭/确认后会回调 `on_dialog(ctx, evt)`。

## 波形颜色

`proto.plot.setup()` 的 `channels[]` 现在支持颜色：

```lua
proto.plot.setup({
  source = "default_protocol",
  channels = {
    { label = "CH1", unit = "V", color = "#47C971" },
    { label = "CH2", unit = "V", color = "#5B8FF9AA" },
  }
})
```

支持：

- `#RRGGBB`
- `#RRGGBBAA`

只改颜色不会清空历史波形；宿主只刷新显示配置。

## LuaLS 类型提示

`protocols/protoscope_api.lua` 仅供 LuaLS 使用，不参与运行时。

把 `protocols/protoscope_api.lua` 加入 LuaLS 的 `workspace.library` 后，就能获得：

- `proto.send / proto.request / proto.request_done`
- `proto.status.* / proto.ui.*`
- `on_tx / on_dialog`
- `ProtoPlotChannel.color`

## 默认协议示例

`protocols/default_protocol/main.lua` 已经示范新的半双工语义：

- “读取版本”使用 `proto.request(...)`
- 收到完整帧后调用 `proto.request_done(...)`
- 超时由宿主 request timeout 主导
- 状态栏和弹窗都走新的 `proto.status.* / proto.ui.*`)README";

const char* kDefaultLuaLsApi = R"LUALS(---@meta

--[[
ProtoScope 脚本 API 定义文件。
这里只提供 LuaLS / EmmyLua 需要的类型、结构和回调签名，
不包含任何实际运行逻辑。
]]

-- 基础枚举：覆盖日志、控件、停靠、传输和弹窗状态。
---@alias ProtoLogLevel 'debug'|'info'|'warn'|'error'
---@alias ProtoControlType 'button'|'input_text'|'input_int'|'input_float'|'checkbox'|'combo'
---@alias ProtoDockAnchor 'left'|'left_bottom'|'right_top'|'right_mid'|'right_bottom'|'main_bottom'
---@alias ProtoControlValue boolean|integer|number|string|nil
---@alias ProtoBytes integer[]
---@alias ProtoPayload string|ProtoBytes
---@alias ProtoFormLayoutItem ProtoFormControlItem|ProtoFormControlsItem|ProtoFormGroupItem|ProtoFormCollapseItem|ProtoFormSeparatorItem|ProtoFormTextItem
---@alias ProtoTxKind 'send'|'request'
---@alias ProtoTxState 'sent'|'completed'|'timeout'|'rejected'|'dropped'|'canceled'
---@alias ProtoDialogKind 'alert'|'confirm'
---@alias ProtoDialogState 'closed'|'confirmed'|'canceled'

-- 表格布局：用于以表格方式组织停靠面板中的控件。
---@class ProtoTableCell
---@field control? string
---@field spacer? boolean

---@class ProtoTableLayout
---@field kind 'table'
---@field columns integer
---@field borders? boolean
---@field resizable? boolean
---@field row_bg? boolean
---@field sizing? 'stretch'
---@field rows ProtoTableCell[][]

-- 表单布局：用于把控件按分组、折叠、文本说明等结构组合起来。
---@class ProtoFormControlItem
---@field control string

---@class ProtoFormControlsItem
---@field controls string[]

---@class ProtoFormGroupItem
---@field group string
---@field items ProtoFormLayoutItem[]

---@class ProtoFormCollapseItem
---@field collapse string
---@field default_open? boolean
---@field items ProtoFormLayoutItem[]

---@class ProtoFormSeparatorItem
---@field separator true

---@class ProtoFormTextItem
---@field text string

---@class ProtoFormLayout
---@field kind 'form'
---@field items ProtoFormLayoutItem[]

---@alias ProtoDockLayout ProtoTableLayout|ProtoFormLayout

-- 连接上下文：宿主在打开、关闭、收发数据时传入的基础运行信息。
---@class ProtoConnectionContext
---@field connection_id integer
---@field kind string
---@field endpoint string
---@field timestamp_ms integer
---@field ready_for_io boolean

-- 控件描述：声明一个可交互控件及其默认值、枚举选项。
---@class ProtoControlDescriptor
---@field type ProtoControlType
---@field id string
---@field label string
---@field default? ProtoControlValue
---@field options? string[]

-- 停靠面板描述：定义一个脚本 UI 面板的标题、锚点和控件布局。
---@class ProtoDockDescriptor
---@field id string
---@field title string
---@field anchor? ProtoDockAnchor
---@field tab_group? string
---@field controls ProtoControlDescriptor[]
---@field layout? ProtoDockLayout

-- 波形通道描述：定义曲线显示名称、单位、缩放和颜色。
---@class ProtoPlotChannel
---@field label string
---@field unit? string
---@field scale? number
---@field offset? number
---@field color? string @支持 '#RRGGBB' 或 '#RRGGBBAA'。

-- 波形初始化参数：用于一次性配置波形来源、通道和视图范围。
---@class ProtoPlotSetup
---@field source? string
---@field channels ProtoPlotChannel[]
---@field reset_history? boolean
---@field view? { time_scale?: number, time_unit?: string, vertical_min?: number, vertical_max?: number, vertical_unit?: string, history_limit?: integer }

-- 单个波形采样点：t 是横轴时间，y 是纵轴数值。
---@class ProtoPlotSample
---@field t number
---@field y number

-- 波形追加请求：向已有通道持续推送采样点。
---@class ProtoPlotAppendRequest
---@field source? string
---@field samples ProtoPlotSample[]

-- 发送类操作的附加参数：常用于超时和业务标签标记。
---@class ProtoSendOptions
---@field timeout_ms? integer
---@field tag? string

-- 请求类操作的附加参数：和发送类似，但语义上表示等待响应。
---@class ProtoRequestOptions
---@field timeout_ms? integer
---@field tag? string

-- 请求完成回传结果：脚本在收到响应后可上报最终处理状态。
---@class ProtoRequestDoneResult
---@field ok? boolean
---@field message? string

-- 传输事件：描述一次发送/请求在宿主侧的生命周期变化。
---@class ProtoTxEvent
---@field id integer
---@field kind ProtoTxKind
---@field state ProtoTxState
---@field tag string
---@field bytes integer
---@field queued_ms integer
---@field finished_ms integer
---@field error? string

-- 状态栏配置：控制状态文本的提示级别。
---@class ProtoStatusOptions
---@field level? ProtoLogLevel

-- 弹窗参数：用于 alert / confirm 的标题、内容和去重键。
---@class ProtoDialogOptions
---@field title string
---@field message string
---@field level? ProtoLogLevel
---@field dedupe_key? string

-- 弹窗事件：回传弹窗的最终状态与确认结果。
---@class ProtoDialogEvent
---@field id integer
---@field kind ProtoDialogKind
---@field state ProtoDialogState
---@field confirmed? boolean
---@field title string
---@field message string
---@field level ProtoLogLevel
---@field dedupe_key string
---@field timestamp_ms integer

proto = proto or {}
proto.plot = proto.plot or {}
proto.status = proto.status or {}
proto.ui = proto.ui or {}

-- 记录脚本日志，便于调试协议、状态流转和异常定位。
---@param level ProtoLogLevel
---@param message string
function proto.log(level, message) end

-- 发送一段原始载荷到宿主，通常用于下行串口数据或主动写入。
---@param payload ProtoPayload
---@param opts? ProtoSendOptions
---@return integer|nil request_id
---@return string|nil error
function proto.send(payload, opts) end

-- 发起一次带响应语义的请求，适合需要等待对端应答的场景。
---@param payload ProtoPayload
---@param opts? ProtoRequestOptions
---@return integer|nil request_id
---@return string|nil error
function proto.request(payload, opts) end

-- 标记当前请求已经完成，宿主可据此结束等待并释放关联状态。
---@param result? ProtoRequestDoneResult
---@return boolean ok
---@return string|nil error
function proto.request_done(result) end

-- 向脚本内部广播一个自定义事件，供同脚本的其他逻辑分发处理。
---@param name string
---@param payload any
function proto.emit(name, payload) end

-- 创建或刷新一次定时器，适合轮询、延迟重试或 UI 延迟刷新。
---@param name string
---@param delay_ms integer
function proto.set_timer(name, delay_ms) end

-- 取消指定定时器，避免超时回调在脚本不需要时继续触发。
---@param name string
function proto.cancel_timer(name) end

-- 设置状态栏文本，适合反馈当前处理进度、错误或提示信息。
---@param text string
---@param opts? ProtoStatusOptions
function proto.status.set(text, opts) end

-- 清空状态栏文本，恢复为无状态提示。
function proto.status.clear() end

-- 弹出提示型对话框，适合告警、信息确认前的提示展示。
---@param opts ProtoDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.ui.alert(opts) end

-- 弹出确认型对话框，常用于需要用户二次确认的操作。
---@param opts ProtoDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.ui.confirm(opts) end

-- 配置波形视图，通常在连接建立后先调用一次完成通道初始化。
---@param payload ProtoPlotSetup
function proto.plot.setup(payload) end

-- 追加波形采样点，适合持续推送实时数据。
---@param channel_index integer
---@param payload ProtoPlotAppendRequest
function proto.plot.push(channel_index, payload) end

-- 读取某个控件的当前值，常用于界面联动或提交前取回最新状态。
---@param id string
---@return ProtoControlValue
function proto.get_control(id) end

-- 设置某个控件的值，适合脚本根据协议结果反向驱动界面状态。
---@param id string
---@param value ProtoControlValue
function proto.set_control(id, value) end

-- 计算 Modbus CRC16，适合构造或校验常见串口帧尾。
---@param payload ProtoPayload
---@return integer
function proto.crc16_modbus(payload) end

-- 计算 CCITT-FALSE CRC16，适合部分自定义协议或设备协议。
---@param payload ProtoPayload
---@return integer
function proto.crc16_ccitt_false(payload) end

-- 计算 IEEE CRC32，适合需要 32 位校验的协议场景。
---@param payload ProtoPayload
---@return integer
function proto.crc32_ieee(payload) end

-- 返回当前脚本要展示的 Dock 面板描述列表。
---@return ProtoDockDescriptor[]
function ui() end

-- 连接打开回调：宿主建立连接后触发，用于初始化状态、定时器或 UI。
---@param ctx ProtoConnectionContext
function on_open(ctx) end

-- 连接关闭回调：连接断开时触发，适合清理缓存、定时器和临时状态。
---@param ctx ProtoConnectionContext
function on_close(ctx) end

-- 错误回调：脚本或宿主运行时发生异常时触发，用于记录和提示。
---@param ctx ProtoConnectionContext
---@param message string
function on_error(ctx, message) end

-- 控件变化回调：当 UI 控件被用户修改时触发。
---@param ctx ProtoConnectionContext
---@param id string
---@param value ProtoControlValue
function on_control(ctx, id, value) end

-- 原始字节回调：宿主收到串口/输入字节流后调用脚本解析。
---@param ctx ProtoConnectionContext
---@param bytes ProtoBytes
function on_bytes(ctx, bytes) end

-- 定时器回调：由 proto.set_timer 创建的定时器到期后触发。
---@param ctx ProtoConnectionContext
---@param name string
function on_timer(ctx, name) end

-- 传输事件回调：用于跟踪 send/request 的发送结果、超时或失败。
---@param ctx ProtoConnectionContext
---@param evt ProtoTxEvent
function on_tx(ctx, evt) end

-- 弹窗事件回调：用于接收 alert / confirm 的最终状态。
---@param ctx ProtoConnectionContext
---@param evt ProtoDialogEvent
function on_dialog(ctx, evt) end)LUALS";

const char* kDefaultGuide = R"(# ProtoScope Lua Host Guide

## 生命周期
- `on_open(ctx)`：连接打开后触发
- `on_close(ctx)`：连接关闭后触发
- `on_error(ctx, message)`：通讯层错误
- `on_bytes(ctx, bytes)`：收到原始字节
- `on_timer(ctx, name)`：定时器触发
- `on_control(ctx, id, value)`：宿主控件变化或按钮触发

## UI 描述
优先使用 `ui()` 返回多个 Dock：

```lua
function ui()
  return {
    {
      id = "protocol",
      title = "协议动作",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
      }
    }
  }
end
```

兼容旧脚本时也可保留 `controls()`。

## proto API
- `proto.log(level, message)`
- `proto.send(payload, opts?)`
- `proto.request(payload, opts?)`
- `proto.request_done(result?)`
- `proto.emit(name, payload)`
- `proto.set_timer(name, delayMs)`
- `proto.cancel_timer(name)`
- `proto.status.set(text, opts?)`
- `proto.status.clear()`
- `proto.ui.alert(opts)`
- `proto.ui.confirm(opts)`
- `proto.get_control(id)`
- `proto.set_control(id, value)`
- `proto.crc16_modbus(payload)`
- `proto.crc16_ccitt_false(payload)`
- `proto.crc32_ieee(payload)`
)";

} // namespace

ConfigStore::ConfigStore()
    : defaultConfigPath_("config/protoscope.yaml"),
      defaultProtocolDir_("protocols/default_protocol") {}

AppConfig ConfigStore::withDefaults() const {
    AppConfig config;
    config.protocol.rootDir = normalizeTextPath(defaultProtocolDir_.parent_path());
    config.protocol.selectedDir = normalizeTextPath(defaultProtocolDir_);
    config.configPath = normalizeTextPath(defaultConfigPath_);
    config.communication.serialPortOptions = kDefaultSerialPorts;
    return config;
}

ConfigLoadResult ConfigStore::load(const std::filesystem::path& path) const {
    ConfigLoadResult result;
    result.config = withDefaults();
    result.resolvedPath = path.empty() ? defaultConfigPath_ : path;

    if (!std::filesystem::exists(result.resolvedPath)) {
        result.config.configPath = normalizeTextPath(result.resolvedPath);
        return result;
    }

    try {
        const YAML::Node root = YAML::LoadFile(result.resolvedPath.string());
        result.loadedFromDisk = true;

        const auto app = root["app"];
        result.config.app.language = readScalar<std::string>(app, "language", result.config.app.language);
        result.config.app.fpsLimit = readScalar<std::uint32_t>(app, "fps_limit", result.config.app.fpsLimit);
        result.config.app.idleRender = readScalar<std::string>(app, "idle_render", result.config.app.idleRender);
        if (const auto autoSave = app["auto_save"]) {
            result.config.app.autoSave.enabled = readScalar<bool>(autoSave, "enabled", result.config.app.autoSave.enabled);
            result.config.app.autoSave.intervalMs = readScalar<std::uint64_t>(autoSave, "interval_ms", result.config.app.autoSave.intervalMs);
        }
        if (const auto configHotReload = app["config_hot_reload"]) {
            result.config.app.configHotReload.enabled =
                readScalar<bool>(configHotReload, "enabled", result.config.app.configHotReload.enabled);
        }

        const auto gui = root["gui"];
        if (const auto window = gui["window"]) {
            result.config.gui.window.title = readScalar<std::string>(window, "title", result.config.gui.window.title);
            result.config.gui.window.width = readScalar<int>(window, "width", result.config.gui.window.width);
            result.config.gui.window.height = readScalar<int>(window, "height", result.config.gui.window.height);
            result.config.gui.window.maximized = readScalar<bool>(window, "maximized", result.config.gui.window.maximized);
        }
        if (const auto wave = gui["wave"]) {
            result.config.gui.wave.maxRenderPointsPerChannel =
                readScalar<std::size_t>(wave, "max_render_points_per_channel", result.config.gui.wave.maxRenderPointsPerChannel);
            result.config.gui.wave.maxRenderVertices =
                readScalar<std::size_t>(wave, "max_render_vertices", result.config.gui.wave.maxRenderVertices);
            result.config.gui.wave.downsampleStartMultiplier =
                readScalar<double>(wave, "downsample_start_multiplier", result.config.gui.wave.downsampleStartMultiplier);
            result.config.gui.wave.overviewMaxSamples =
                readScalar<std::size_t>(wave, "overview_max_samples", result.config.gui.wave.overviewMaxSamples);
            result.config.gui.wave.minVisibleTimeSpan =
                readScalar<double>(wave, "min_visible_time_span", result.config.gui.wave.minVisibleTimeSpan);
            result.config.gui.wave.showAxisLabels =
                readScalar<bool>(wave, "show_axis_labels", result.config.gui.wave.showAxisLabels);
            result.config.gui.luaDockLayoutDebug = readScalar<bool>(gui, "lua_dock_layout_debug", result.config.gui.luaDockLayoutDebug);
        }

        const auto protocol = root["protocol"];
        result.config.protocol.rootDir = readScalar<std::string>(protocol, "root_dir", result.config.protocol.rootDir);
        result.config.protocol.selectedDir = readScalar<std::string>(protocol, "selected_dir", result.config.protocol.selectedDir);
        if (const auto tx = protocol["tx"]) {
            result.config.protocol.tx.sendTimeoutMs =
                readScalar<std::uint64_t>(tx, "send_timeout_ms", result.config.protocol.tx.sendTimeoutMs);
            result.config.protocol.tx.requestTimeoutMs =
                readScalar<std::uint64_t>(tx, "request_timeout_ms", result.config.protocol.tx.requestTimeoutMs);
            result.config.protocol.tx.maxPending =
                readScalar<std::size_t>(tx, "max_pending", result.config.protocol.tx.maxPending);
            result.config.protocol.tx.overflowPolicy =
                readScalar<std::string>(tx, "overflow_policy", result.config.protocol.tx.overflowPolicy);
            result.config.protocol.tx.overflowNotify =
                readScalar<std::string>(tx, "overflow_notify", result.config.protocol.tx.overflowNotify);
        }
        const auto logging = root["logging"];
        result.config.logging.level =
            parseLogLevel(readScalar<std::string>(logging, "level", toLogLevelText(result.config.logging.level)));
        result.config.logging.filePath = readScalar<std::string>(logging, "file_path", result.config.logging.filePath);
        result.config.configPath = normalizeTextPath(result.resolvedPath);

        const auto communication = root["communication"];
        result.config.communication.kind =
            parseTransportKind(readScalar<std::string>(communication, "kind", toTransportKindText(result.config.communication.kind)));

        if (const auto tcpClient = communication["tcp_client"]) {
            result.config.communication.tcpClient.host =
                readScalar<std::string>(tcpClient, "host", result.config.communication.tcpClient.host);
            result.config.communication.tcpClient.port =
                readScalar<std::uint16_t>(tcpClient, "port", result.config.communication.tcpClient.port);
        }

        if (const auto tcpServer = communication["tcp_server"]) {
            result.config.communication.tcpServer.bindAddress =
                readScalar<std::string>(tcpServer, "bind_address", result.config.communication.tcpServer.bindAddress);
            result.config.communication.tcpServer.port =
                readScalar<std::uint16_t>(tcpServer, "port", result.config.communication.tcpServer.port);
            result.config.communication.tcpServer.rejectNewConnection =
                readScalar<bool>(tcpServer, "reject_new_connection", result.config.communication.tcpServer.rejectNewConnection);
        }

        if (const auto serial = communication["serial"]) {
            result.config.communication.serial.portName =
                readScalar<std::string>(serial, "port_name", result.config.communication.serial.portName);
            result.config.communication.serial.baudRate =
                readScalar<std::uint32_t>(serial, "baud_rate", result.config.communication.serial.baudRate);
            result.config.communication.serial.dataBits =
                readScalar<std::uint32_t>(serial, "data_bits", result.config.communication.serial.dataBits);
            result.config.communication.serial.parity =
                readScalar<std::string>(serial, "parity", result.config.communication.serial.parity);
            result.config.communication.serial.stopBits =
                readScalar<std::string>(serial, "stop_bits", result.config.communication.serial.stopBits);
            result.config.communication.serial.flowControl =
                readScalar<std::string>(serial, "flow_control", result.config.communication.serial.flowControl);
        }
        if (const auto receive = root["receive"]) {
            result.config.communication.serialPortOptions = kDefaultSerialPorts;
            result.config.communication.reconnectRequired = false;
            result.config.communication.lastError.clear();
            result.config.communication.txCount = 0;
            result.config.communication.rxCount = 0;
            (void)receive;
        }
    } catch (const std::exception& ex) {
        result.error = std::string("读取 YAML 失败: ") + ex.what();
        result.loadedFromDisk = false;
    }

    return result;
}

bool ConfigStore::save(const std::filesystem::path& path, const AppConfig& config, std::string& error) const {
    YAML::Node root;

    root["app"]["language"] = config.app.language;
    root["app"]["fps_limit"] = config.app.fpsLimit;
    root["app"]["idle_render"] = config.app.idleRender;
    root["app"]["auto_save"]["enabled"] = config.app.autoSave.enabled;
    root["app"]["auto_save"]["interval_ms"] = config.app.autoSave.intervalMs;
    root["app"]["config_hot_reload"]["enabled"] = config.app.configHotReload.enabled;

    root["gui"]["window"]["title"] = config.gui.window.title;
    root["gui"]["window"]["width"] = config.gui.window.width;
    root["gui"]["window"]["height"] = config.gui.window.height;
    root["gui"]["window"]["maximized"] = config.gui.window.maximized;
    root["gui"]["wave"]["max_render_points_per_channel"] = config.gui.wave.maxRenderPointsPerChannel;
    root["gui"]["wave"]["max_render_vertices"] = config.gui.wave.maxRenderVertices;
    root["gui"]["wave"]["downsample_start_multiplier"] = config.gui.wave.downsampleStartMultiplier;
    root["gui"]["wave"]["overview_max_samples"] = config.gui.wave.overviewMaxSamples;
    root["gui"]["wave"]["min_visible_time_span"] = config.gui.wave.minVisibleTimeSpan;
    root["gui"]["wave"]["show_axis_labels"] = config.gui.wave.showAxisLabels;
    root["gui"]["lua_dock_layout_debug"] = config.gui.luaDockLayoutDebug;

    root["protocol"]["root_dir"] = config.protocol.rootDir;
    root["protocol"]["selected_dir"] = config.protocol.selectedDir;
    root["protocol"]["tx"]["send_timeout_ms"] = config.protocol.tx.sendTimeoutMs;
    root["protocol"]["tx"]["request_timeout_ms"] = config.protocol.tx.requestTimeoutMs;
    root["protocol"]["tx"]["max_pending"] = config.protocol.tx.maxPending;
    root["protocol"]["tx"]["overflow_policy"] = config.protocol.tx.overflowPolicy;
    root["protocol"]["tx"]["overflow_notify"] = config.protocol.tx.overflowNotify;
    root["logging"]["level"] = toLogLevelText(config.logging.level);
    if (!config.logging.filePath.empty()) {
        root["logging"]["file_path"] = config.logging.filePath;
    }

    root["communication"]["kind"] = toTransportKindText(config.communication.kind);
    root["communication"]["tcp_client"]["host"] = config.communication.tcpClient.host;
    root["communication"]["tcp_client"]["port"] = config.communication.tcpClient.port;
    root["communication"]["tcp_server"]["bind_address"] = config.communication.tcpServer.bindAddress;
    root["communication"]["tcp_server"]["port"] = config.communication.tcpServer.port;
    root["communication"]["tcp_server"]["reject_new_connection"] = config.communication.tcpServer.rejectNewConnection;
    root["communication"]["serial"]["port_name"] = config.communication.serial.portName;
    root["communication"]["serial"]["baud_rate"] = config.communication.serial.baudRate;
    root["communication"]["serial"]["data_bits"] = config.communication.serial.dataBits;
    root["communication"]["serial"]["parity"] = config.communication.serial.parity;
    root["communication"]["serial"]["stop_bits"] = config.communication.serial.stopBits;
    root["communication"]["serial"]["flow_control"] = config.communication.serial.flowControl;

    try {
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream out(path);
        if (!out.good()) {
            error = "无法写入配置文件";
            return false;
        }
        out << root;
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& dir) const {
    return normalizeProtocolDir(defaultProtocolDir_.parent_path(), dir);
}

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& rootDir, const std::filesystem::path& dir) const {
    std::filesystem::path candidate = dir.empty() ? defaultProtocolDir_ : dir;
    if (protocolEntryExists(candidate)) {
        return candidate;
    }

    const auto scanned = scanProtocolDirectories(rootDir);
    if (!scanned.empty()) {
        return std::filesystem::path(scanned.front());
    }
    return defaultProtocolDir_;
}

std::filesystem::path ConfigStore::mainLuaPath(const std::filesystem::path& protocolDir) const {
    return protocolDir / "main.lua";
}

std::string ConfigStore::protocolName(const std::filesystem::path& protocolDir) const {
    const auto filename = protocolDir.filename().string();
    return filename.empty() ? std::string("default_protocol") : filename;
}

bool ConfigStore::protocolEntryExists(const std::filesystem::path& protocolDir) const {
    return std::filesystem::exists(mainLuaPath(protocolDir));
}

std::vector<std::string> ConfigStore::scanProtocolDirectories(const std::filesystem::path& rootDir) const {
    std::vector<std::string> results;
    std::error_code ec;
    if (!std::filesystem::exists(rootDir, ec) || ec) {
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(rootDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory()) {
            continue;
        }
        if (!protocolEntryExists(entry.path())) {
            continue;
        }
        results.push_back(normalizeTextPath(entry.path()));
    }

    std::sort(results.begin(), results.end());
    return results;
}

std::filesystem::path ConfigStore::defaultScriptWorkspaceDir() const {
    return "scripts";
}

std::filesystem::path ConfigStore::defaultScriptHelpPath() const {
    return defaultScriptWorkspaceDir() / "README.txt";
}

bool ConfigStore::ensureDefaultProtocolScript(const std::filesystem::path& protocolDir, std::string& error) const {
    try {
        std::filesystem::create_directories(protocolDir);
        const auto scriptPath = mainLuaPath(protocolDir);
        if (!std::filesystem::exists(scriptPath)) {
            std::ofstream out(scriptPath);
            if (!out.good()) {
                error = "无法写入默认协议脚本";
                return false;
            }
            out << kDefaultProtocolMainLua;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool ConfigStore::ensureDefaultProtocolWorkspace(std::string& error) const {
    try {
        const auto protocolRoot = defaultProtocolDir_.parent_path();
        if (std::filesystem::exists(protocolRoot)) {
            return true;
        }

        const auto luaWaveformDir = protocolRoot / "lua_waveform_demo";
        const auto halfDuplexMasterDir = protocolRoot / "half_duplex_modbus_master";
        const auto halfDuplexSlaveDir = protocolRoot / "half_duplex_modbus_slave";

        std::filesystem::create_directories(defaultProtocolDir_);
        std::filesystem::create_directories(luaWaveformDir);
        std::filesystem::create_directories(halfDuplexMasterDir);
        std::filesystem::create_directories(halfDuplexSlaveDir);

        const auto writeTextFile = [&](const std::filesystem::path& path,
                                       const char* text,
                                       const char* failureMessage) -> bool {
            std::ofstream out(path);
            if (!out.good()) {
                error = failureMessage;
                return false;
            }
            out << text;
            return true;
        };

        // 核心流程：仅在 protocols 根目录缺失时生成完整默认工作区，避免覆盖用户已有协议资产。
        return writeTextFile(mainLuaPath(defaultProtocolDir_), kDefaultProtocolMainLua, "无法写入默认协议脚本")
            && writeTextFile(luaWaveformDir / "main.lua", kDefaultWaveformDemoLua, "无法写入 Lua 波形示例脚本")
            && writeTextFile(halfDuplexMasterDir / "main.lua", kHalfDuplexModbusMasterLua, "无法写入半双工 Modbus 主机示例脚本")
            && writeTextFile(halfDuplexSlaveDir / "main.lua", kHalfDuplexModbusSlaveLua, "无法写入半双工 Modbus 从机示例脚本")
            && writeTextFile(protocolRoot / "half_duplex_modbus_common.lua", kHalfDuplexModbusCommonLua, "无法写入半双工 Modbus 共享脚本")
            && writeTextFile(protocolRoot / "README.md", kDefaultProtocolsReadme, "无法写入 protocols README")
            && writeTextFile(protocolRoot / "protoscope_api.lua", kDefaultLuaLsApi, "无法写入 LuaLS API 提示文件");
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}
bool ConfigStore::ensureDefaultScriptWorkspace(std::string& error) const {
    try {
        std::filesystem::create_directories(defaultScriptWorkspaceDir());
        if (!std::filesystem::exists(defaultScriptHelpPath())) {
            std::ofstream out(defaultScriptHelpPath());
            out << kDefaultGuide;
        }
        const auto sampleScript = defaultScriptWorkspaceDir() / "main.lua";
        if (!std::filesystem::exists(sampleScript) && std::filesystem::exists(mainLuaPath(defaultProtocolDir_))) {
            std::filesystem::copy_file(mainLuaPath(defaultProtocolDir_), sampleScript, std::filesystem::copy_options::skip_existing);
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

FileSnapshot ConfigStore::snapshot(const std::filesystem::path& path) const {
    FileSnapshot result;
    result.path = path;
    result.exists = std::filesystem::exists(path);
    if (result.exists) {
        result.timestampMs = toTimestampMs(std::filesystem::last_write_time(path));
    }
    return result;
}

bool ConfigStore::hasChanged(const FileSnapshot& previous) const {
    const auto current = snapshot(previous.path);
    return current.exists != previous.exists || current.timestampMs != previous.timestampMs;
}

void ConfigStore::applyToDock(const AppConfig& config, dock::DockStore& dockStore) const {
    auto& comm = dockStore.commState();
    comm.kind = config.communication.kind;
    comm.tcpClient = config.communication.tcpClient;
    comm.tcpServer = config.communication.tcpServer;
    comm.serial = config.communication.serial;
    if (comm.serialPortOptions.empty()) {
        comm.serialPortOptions = kDefaultSerialPorts;
    }

    const auto protocolDir = normalizeProtocolDir(config.protocol.rootDir, config.protocol.selectedDir);
    auto& lua = dockStore.luaState();
    lua.protocolRootDir = config.protocol.rootDir;
    lua.protocolDirOptions = scanProtocolDirectories(lua.protocolRootDir);
    lua.protocolDir = normalizeTextPath(protocolDir);
    lua.protocolName = protocolName(protocolDir);
    lua.scriptPath = normalizeTextPath(mainLuaPath(protocolDir));

    auto& configState = dockStore.configState();
    configState.autoSaveEnabled = config.app.autoSave.enabled;
    configState.autoSaveIntervalMs = config.app.autoSave.intervalMs;
    configState.configHotReloadEnabled = config.app.configHotReload.enabled;
    configState.fpsLimit = config.app.fpsLimit;
    configState.idleRender = config.app.idleRender;
    configState.luaDockLayoutDebug = config.gui.luaDockLayoutDebug;
    configState.loadedFromPath = config.configPath.empty() ? normalizeTextPath(defaultConfigPath_) : config.configPath;

    auto& wave = dockStore.waveState().view;
    wave.maxRenderPointsPerChannel = config.gui.wave.maxRenderPointsPerChannel;
    wave.maxRenderVertices = config.gui.wave.maxRenderVertices;
    wave.downsampleStartMultiplier = (std::max)(config.gui.wave.downsampleStartMultiplier, 1.0);
    wave.overviewMaxSamples = config.gui.wave.overviewMaxSamples;
    wave.minVisibleTimeSpan = config.gui.wave.minVisibleTimeSpan;
    wave.showAxisLabels = config.gui.wave.showAxisLabels;
}

AppConfig ConfigStore::captureFromDock(const dock::DockStore& dockStore) const {
    AppConfig config = withDefaults();

    config.communication = dockStore.commState();
    config.protocol.rootDir = dockStore.luaState().protocolRootDir;
    config.protocol.selectedDir = dockStore.luaState().protocolDir;
    config.app.autoSave.enabled = dockStore.configState().autoSaveEnabled;
    config.app.autoSave.intervalMs = dockStore.configState().autoSaveIntervalMs;
    config.app.configHotReload.enabled = dockStore.configState().configHotReloadEnabled;
    config.app.fpsLimit = dockStore.configState().fpsLimit;
    config.app.idleRender = dockStore.configState().idleRender;
    config.gui.luaDockLayoutDebug = dockStore.configState().luaDockLayoutDebug;
    config.gui.wave.maxRenderPointsPerChannel = dockStore.waveState().view.maxRenderPointsPerChannel;
    config.gui.wave.maxRenderVertices = dockStore.waveState().view.maxRenderVertices;
    config.gui.wave.downsampleStartMultiplier = dockStore.waveState().view.downsampleStartMultiplier;
    config.gui.wave.overviewMaxSamples = dockStore.waveState().view.overviewMaxSamples;
    config.gui.wave.minVisibleTimeSpan = dockStore.waveState().view.minVisibleTimeSpan;
    config.gui.wave.showAxisLabels = dockStore.waveState().view.showAxisLabels;
    config.configPath = dockStore.configState().loadedFromPath;

    return config;
}

const std::filesystem::path& ConfigStore::defaultConfigPath() const {
    return defaultConfigPath_;
}

const std::filesystem::path& ConfigStore::defaultProtocolDir() const {
    return defaultProtocolDir_;
}

std::uint64_t ConfigStore::toTimestampMs(const std::filesystem::file_time_type& fileTime) {
    const auto normalized = fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(normalized.time_since_epoch()).count());
}

} // namespace protoscope::config
