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

const char* kDefaultProtocolMainLua = R"PROTO(-- 核心流程：协议脚本只描述控件、收发和超时语义，底层 I/O 与 UI 统一由宿主的 proto.* API 承担。

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
        { type = "input_float", id = "scale", label = "缩放", default = 1.0 }
      }
    }
  }
end

local rx_buffer = {}
local next_scope_t = 0.0

local function init_scope(reset_history)
  proto.plot.setup({
    source = "default_protocol",
    reset_history = reset_history,
    time_scale = 0.001,
    time_unit = "s",
    vertical_min = -2.0,
    vertical_max = 2.0,
    vertical_unit = "V",
    history_limit = 20000,
    channels = {
      { label = "CH1", unit = "V" },
      { label = "CH2", unit = "V" }
    }
  })
end

local function push_scope_samples(bytes)
  local ch1 = {}
  local ch2 = {}
  for i = 1, #bytes do
    local value = bytes[i]
    ch1[#ch1 + 1] = { t = next_scope_t, y = (value - 128.0) / 64.0 }
    ch2[#ch2 + 1] = { t = next_scope_t, y = ((value % 32) - 16.0) / 8.0 }
    next_scope_t = next_scope_t + 0.001
  end
  if #ch1 > 0 then
    proto.plot.push(1, { samples = ch1 })
    proto.plot.push(2, { samples = ch2 })
  end
end

local function clear_rx_buffer()
  rx_buffer = {}
end

local function append_bytes(bytes)
  for i = 1, #bytes do
    rx_buffer[#rx_buffer + 1] = bytes[i]
  end
end

local function timeout_ms()
  return proto.get_control("timeout_ms") or 1000
end

local function device_id_byte()
  local device_id = tostring(proto.get_control("device_id") or "01")
  return string.byte(device_id, 1, 1) or 0x30
end

local function build_read_version_frame()
  if proto.get_control("hex_send") then
    return { 0xAA, 0x55, device_id_byte(), 0x01, 0x0D }
  end
  return "52 45 41 44 20 " .. string.format("%02X", device_id_byte())
end

local function parse_frame(bytes)
  if #bytes >= 2 and bytes[1] == string.byte("O") and bytes[2] == string.byte("K") then
    return {
      status = "ok",
      size = #bytes,
      mode = proto.get_control("mode"),
      scale = proto.get_control("scale")
    }
  end
  return nil
end

function on_open(ctx)
  init_scope(true)
  proto.log("info", "连接已打开: " .. ctx.kind .. " -> " .. ctx.endpoint)
end

function on_close(ctx)
  proto.log("info", "连接已关闭: " .. ctx.endpoint)
end

function on_error(ctx, message)
  proto.log("error", "连接错误: " .. message)
end

function on_control(ctx, id, value)
  if id == "read_version" then
    clear_rx_buffer()
    proto.send(build_read_version_frame())
    proto.set_timer("read_version_timeout", timeout_ms())
    proto.emit("request", { action = "read_version", connection_id = ctx.connection_id })
  else
    proto.log("info", "控件更新: " .. id .. "=" .. tostring(value))
  end
end

function on_bytes(ctx, bytes)
  push_scope_samples(bytes)
  append_bytes(bytes)
  local result = parse_frame(rx_buffer)
  if result then
    clear_rx_buffer()
    proto.cancel_timer("read_version_timeout")
    proto.emit("frame", result)
  else
    proto.emit("rx_bytes", { size = #bytes })
  end
end

function on_timer(ctx, name)
  if name == "read_version_timeout" then
    clear_rx_buffer()
    proto.emit("warning", { message = "读取版本超时", connection_id = ctx.connection_id })
  end
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

const char* kDefaultProtocolsReadme = R"README(# ProtoScope Lua 协议脚本指南

`protocols` 是 ProtoScope 的 Lua 协议工作区。每个协议目录至少包含一个 `main.lua`：

```text
protocols/
├── default_protocol/
│   └── main.lua
├── lua_waveform_demo/
│   └── main.lua
├── README.md
└── protoscope_api.lua
```

启动时如果当前工作目录不存在 `protocols` 目录，程序会自动生成以上默认工作区。若目录已存在，程序不会覆盖用户脚本。

## 快速开始

协议脚本由宿主加载执行，不需要 `require` 任何 ProtoScope 模块。脚本通过全局对象 `proto` 与宿主通信：

```lua
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
      }
    }
  }
end

function on_control(ctx, id, value)
  if id == "read_version" then
    proto.send({ 0xAA, 0x55, 0x01, 0x0D })
    proto.set_timer("read_version_timeout", 1000)
  end
end
```

## UI 定义

脚本可定义 `ui()`，返回 Dock 面板数组。每个 Dock 支持：

- `id`：Dock 唯一 ID。
- `title`：面板标题。
- `anchor`：停靠位置，可用值为 `left`、`left_bottom`、`right_top`、`right_mid`、`right_bottom`、`main_bottom`，默认 `left_bottom`。
- `tab_group`：同组 Dock 以标签页形式组织，可省略。
- `controls`：控件数组。

控件类型：

- `button`：按钮，点击后触发 `on_control(ctx, id, true)`。
- `input_text`：文本输入，`default` 为字符串。
- `input_int`：整数输入，`default` 为整数。
- `input_float`：浮点输入，`default` 为数字。
- `checkbox`：复选框，`default` 为布尔值。
- `combo`：下拉框，必须提供 `options` 字符串数组，`default` 为从 1 开始的选项序号。

## 生命周期回调

按需定义以下全局函数即可：

- `on_open(ctx)`：连接打开。
- `on_close(ctx)`：连接关闭。
- `on_error(ctx, message)`：连接或传输错误。
- `on_control(ctx, id, value)`：UI 控件变化或按钮点击。
- `on_bytes(ctx, bytes)`：收到字节数组，`bytes` 为 `number[]`，元素范围 `0..255`。
- `on_timer(ctx, name)`：定时器触发。

`ctx` 包含：

- `connection_id`：连接 ID。
- `kind`：连接类型，例如 `serial`、`tcp_client`、`tcp_server`。
- `endpoint`：端点描述。

## 通信与事件 API

- `proto.send(payload)`：发送数据，`payload` 可为字符串或 `number[]`。
- `proto.log(level, message)`：写日志，`level` 常用 `debug`、`info`、`warn`、`error`。
- `proto.emit(name, payload)`：向宿主发出结构化脚本事件，`payload` 会序列化显示。
- `proto.get_control(id)`：读取控件当前值。
- `proto.set_control(id, value)`：设置控件当前值。
- `proto.set_timer(name, delay_ms)`：设置一次性定时器。
- `proto.cancel_timer(name)`：取消定时器。

CRC 辅助函数：

- `proto.crc16_modbus(payload)`：返回 Modbus CRC16。
- `proto.crc16_ccitt_false(payload)`：返回 CRC16/CCITT-FALSE。
- `proto.crc32_ieee(payload)`：返回 IEEE CRC32。

## 波形绘图 API

调用 `proto.plot.setup(payload)` 配置波形通道：

```lua
proto.plot.setup({
  source = "demo",
  reset_history = true,
  time_scale = 0.001,
  time_unit = "s",
  vertical_min = -2.0,
  vertical_max = 2.0,
  vertical_unit = "V",
  history_limit = 20000,
  channels = {
    { label = "CH1", unit = "V" },
    { label = "CH2", unit = "V", offset = 1.0 },
  }
})
```

调用 `proto.plot.push(channel_index, payload)` 追加采样点。`channel_index` 从 1 开始：

```lua
proto.plot.push(1, {
  samples = {
    { t = 0.000, y = 0.0 },
    { t = 0.001, y = 0.5 },
  }
})
```

## LuaLS 类型提示

`protocols/protoscope_api.lua` 是给 LuaLS 使用的虚拟 API 文件，只包含注解和空实现，不参与运行时协议逻辑。

在 LuaLS 中把 `protocols/protoscope_api.lua` 加入 workspace/library 后，打开 `default_protocol/main.lua` 或 `lua_waveform_demo/main.lua` 即可获得 `proto.*`、`proto.plot.*` 和回调参数的基础类型提示。

业务脚本无需 `require("protoscope_api")`，运行时由 ProtoScope 注入全局 `proto`。
)README";

const char* kDefaultLuaLsApi = R"LUALS(---@meta

---@alias ProtoLogLevel '"debug"'|'"info"'|'"warn"'|'"error"'
---@alias ProtoControlType '"button"'|'"input_text"'|'"input_int"'|'"input_float"'|'"checkbox"'|'"combo"'
---@alias ProtoDockAnchor '"left"'|'"left_bottom"'|'"right_top"'|'"right_mid"'|'"right_bottom"'|'"main_bottom"'
---@alias ProtoControlValue boolean|integer|number|string|nil
---@alias ProtoBytes integer[]
---@alias ProtoPayload string|ProtoBytes

---@class ProtoConnectionContext
---@field connection_id integer 连接 ID。
---@field kind string 连接类型，例如 serial、tcp_client、tcp_server。
---@field endpoint string 端点描述。

---@class ProtoControlDescriptor
---@field type ProtoControlType 控件类型。
---@field id string 控件 ID。
---@field label string 控件标签。
---@field default? ProtoControlValue 默认值；combo 为从 1 开始的选项序号。
---@field options? string[] combo 控件选项。

---@class ProtoDockDescriptor
---@field id string Dock 唯一 ID。
---@field title string Dock 标题。
---@field anchor? ProtoDockAnchor 停靠位置，默认 left_bottom。
---@field tab_group? string 标签页分组。
---@field controls ProtoControlDescriptor[] 控件列表。

---@class ProtoEventPayload
---@field [string] any

---@class ProtoPlotChannel
---@field label string 通道名称。
---@field unit? string 通道单位。
---@field offset? number 通道显示偏移。

---@class ProtoPlotSetup
---@field source? string 数据来源名称。
---@field reset_history? boolean 是否清空历史数据。
---@field time_scale? number 时间缩放。
---@field time_unit? string 时间单位。
---@field vertical_min? number 垂直轴最小值。
---@field vertical_max? number 垂直轴最大值。
---@field vertical_unit? string 垂直轴单位。
---@field history_limit? integer 历史采样保留上限。
---@field channels ProtoPlotChannel[] 通道定义。

---@class ProtoPlotSample
---@field t number 时间戳。
---@field y number 采样值。

---@class ProtoPlotPushPayload
---@field samples ProtoPlotSample[] 采样点数组。

---@class ProtoPlotApi
local plot_api = {}

---配置 Lua 波形通道和显示参数。
---@param payload ProtoPlotSetup
function plot_api.setup(payload) end

---向指定通道追加采样点；channel_index 从 1 开始。
---@param channel_index integer
---@param payload ProtoPlotPushPayload
function plot_api.push(channel_index, payload) end

---@class ProtoApi
---@field plot ProtoPlotApi
proto = {}
proto.plot = plot_api

---写入脚本日志。
---@param level ProtoLogLevel
---@param message string
function proto.log(level, message) end

---发送字节数组或字符串。
---@param payload ProtoPayload
function proto.send(payload) end

---发出结构化脚本事件。
---@param name string
---@param payload any
function proto.emit(name, payload) end

---读取控件当前值。
---@param id string
---@return ProtoControlValue
function proto.get_control(id) return nil end

---设置控件当前值。
---@param id string
---@param value ProtoControlValue
function proto.set_control(id, value) end

---设置一次性定时器。
---@param name string
---@param delay_ms integer
function proto.set_timer(name, delay_ms) end

---取消定时器。
---@param name string
function proto.cancel_timer(name) end

---计算 Modbus CRC16。
---@param payload ProtoPayload
---@return integer
function proto.crc16_modbus(payload) return 0 end

---计算 CRC16/CCITT-FALSE。
---@param payload ProtoPayload
---@return integer
function proto.crc16_ccitt_false(payload) return 0 end

---计算 IEEE CRC32。
---@param payload ProtoPayload
---@return integer
function proto.crc32_ieee(payload) return 0 end

---定义 Dock UI。
---@return ProtoDockDescriptor[]
function ui() return {} end

---连接打开回调。
---@param ctx ProtoConnectionContext
function on_open(ctx) end

---连接关闭回调。
---@param ctx ProtoConnectionContext
function on_close(ctx) end

---连接错误回调。
---@param ctx ProtoConnectionContext
---@param message string
function on_error(ctx, message) end

---控件变化回调。
---@param ctx ProtoConnectionContext
---@param id string
---@param value ProtoControlValue
function on_control(ctx, id, value) end

---收到字节回调。
---@param ctx ProtoConnectionContext
---@param bytes ProtoBytes
function on_bytes(ctx, bytes) end

---定时器触发回调。
---@param ctx ProtoConnectionContext
---@param name string
function on_timer(ctx, name) end
)LUALS";

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
- `proto.send(hexStringOrByteArray)`
- `proto.emit(name, payload)`
- `proto.set_timer(name, delayMs)`
- `proto.cancel_timer(name)`
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
            result.config.gui.wave.overviewMaxSamples =
                readScalar<std::size_t>(wave, "overview_max_samples", result.config.gui.wave.overviewMaxSamples);
            result.config.gui.luaDockLayoutDebug = readScalar<bool>(gui, "lua_dock_layout_debug", result.config.gui.luaDockLayoutDebug);
        }

        const auto protocol = root["protocol"];
        result.config.protocol.rootDir = readScalar<std::string>(protocol, "root_dir", result.config.protocol.rootDir);
        result.config.protocol.selectedDir = readScalar<std::string>(protocol, "selected_dir", result.config.protocol.selectedDir);
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
    root["gui"]["wave"]["overview_max_samples"] = config.gui.wave.overviewMaxSamples;
    root["gui"]["lua_dock_layout_debug"] = config.gui.luaDockLayoutDebug;

    root["protocol"]["root_dir"] = config.protocol.rootDir;
    root["protocol"]["selected_dir"] = config.protocol.selectedDir;

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

        std::filesystem::create_directories(defaultProtocolDir_);
        std::filesystem::create_directories(protocolRoot / "lua_waveform_demo");

        // 核心流程：仅在 protocols 根目录缺失时生成完整默认工作区，避免覆盖用户已有协议资产。
        {
            std::ofstream out(mainLuaPath(defaultProtocolDir_));
            if (!out.good()) {
                error = "无法写入默认协议脚本";
                return false;
            }
            out << kDefaultProtocolMainLua;
        }
        {
            std::ofstream out(protocolRoot / "lua_waveform_demo" / "main.lua");
            if (!out.good()) {
                error = "无法写入 Lua 波形示例脚本";
                return false;
            }
            out << kDefaultWaveformDemoLua;
        }
        {
            std::ofstream out(protocolRoot / "README.md");
            if (!out.good()) {
                error = "无法写入 protocols README";
                return false;
            }
            out << kDefaultProtocolsReadme;
        }
        {
            std::ofstream out(protocolRoot / "protoscope_api.lua");
            if (!out.good()) {
                error = "无法写入 LuaLS API 提示文件";
                return false;
            }
            out << kDefaultLuaLsApi;
        }
        return true;
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
    wave.overviewMaxSamples = config.gui.wave.overviewMaxSamples;
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
    config.gui.wave.overviewMaxSamples = dockStore.waveState().view.overviewMaxSamples;
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
