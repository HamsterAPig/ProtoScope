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
- 状态栏和弹窗都走新的 `proto.status.* / proto.ui.*`
)README";

const char* kDefaultLuaLsApi = R"LUALS(---@meta

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

---@class ProtoConnectionContext
---@field connection_id integer
---@field kind string
---@field endpoint string
---@field timestamp_ms integer
---@field ready_for_io boolean

---@class ProtoControlDescriptor
---@field type ProtoControlType
---@field id string
---@field label string
---@field default? ProtoControlValue
---@field options? string[]

---@class ProtoDockDescriptor
---@field id string
---@field title string
---@field anchor? ProtoDockAnchor
---@field tab_group? string
---@field controls ProtoControlDescriptor[]
---@field layout? ProtoDockLayout

---@class ProtoPlotChannel
---@field label string
---@field unit? string
---@field scale? number
---@field offset? number
---@field color? string @支持 '#RRGGBB' 或 '#RRGGBBAA'。

---@class ProtoPlotSetup
---@field source? string
---@field channels ProtoPlotChannel[]
---@field reset_history? boolean
---@field view? { time_scale?: number, time_unit?: string, vertical_min?: number, vertical_max?: number, vertical_unit?: string, history_limit?: integer }

---@class ProtoPlotSample
---@field t number
---@field y number

---@class ProtoPlotAppendRequest
---@field source? string
---@field samples ProtoPlotSample[]

---@class ProtoSendOptions
---@field timeout_ms? integer
---@field tag? string

---@class ProtoRequestOptions
---@field timeout_ms? integer
---@field tag? string

---@class ProtoRequestDoneResult
---@field ok? boolean
---@field message? string

---@class ProtoTxEvent
---@field id integer
---@field kind ProtoTxKind
---@field state ProtoTxState
---@field tag string
---@field bytes integer
---@field queued_ms integer
---@field finished_ms integer
---@field error? string

---@class ProtoStatusOptions
---@field level? ProtoLogLevel

---@class ProtoDialogOptions
---@field title string
---@field message string
---@field level? ProtoLogLevel
---@field dedupe_key? string

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

---@param level ProtoLogLevel
---@param message string
function proto.log(level, message) end

---@param payload ProtoPayload
---@param opts? ProtoSendOptions
---@return integer|nil request_id
---@return string|nil error
function proto.send(payload, opts) end

---@param payload ProtoPayload
---@param opts? ProtoRequestOptions
---@return integer|nil request_id
---@return string|nil error
function proto.request(payload, opts) end

---@param result? ProtoRequestDoneResult
---@return boolean ok
---@return string|nil error
function proto.request_done(result) end

---@param name string
---@param payload any
function proto.emit(name, payload) end

---@param name string
---@param delay_ms integer
function proto.set_timer(name, delay_ms) end

---@param name string
function proto.cancel_timer(name) end

---@param text string
---@param opts? ProtoStatusOptions
function proto.status.set(text, opts) end

function proto.status.clear() end

---@param opts ProtoDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.ui.alert(opts) end

---@param opts ProtoDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.ui.confirm(opts) end

---@param payload ProtoPlotSetup
function proto.plot.setup(payload) end

---@param channel_index integer
---@param payload ProtoPlotAppendRequest
function proto.plot.push(channel_index, payload) end

---@param id string
---@return ProtoControlValue
function proto.get_control(id) end

---@param id string
---@param value ProtoControlValue
function proto.set_control(id, value) end

---@param payload ProtoPayload
---@return integer
function proto.crc16_modbus(payload) end

---@param payload ProtoPayload
---@return integer
function proto.crc16_ccitt_false(payload) end

---@param payload ProtoPayload
---@return integer
function proto.crc32_ieee(payload) end

---@return ProtoDockDescriptor[]
function ui() end

---@param ctx ProtoConnectionContext
function on_open(ctx) end

---@param ctx ProtoConnectionContext
function on_close(ctx) end

---@param ctx ProtoConnectionContext
---@param message string
function on_error(ctx, message) end

---@param ctx ProtoConnectionContext
---@param id string
---@param value ProtoControlValue
function on_control(ctx, id, value) end

---@param ctx ProtoConnectionContext
---@param bytes ProtoBytes
function on_bytes(ctx, bytes) end

---@param ctx ProtoConnectionContext
---@param name string
function on_timer(ctx, name) end

---@param ctx ProtoConnectionContext
---@param evt ProtoTxEvent
function on_tx(ctx, evt) end

---@param ctx ProtoConnectionContext
---@param evt ProtoDialogEvent
function on_dialog(ctx, evt) end
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
