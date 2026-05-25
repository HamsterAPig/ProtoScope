-- 核心流程：协议脚本只描述控件、收发和超时语义，底层 I/O 与 UI 统一由宿主的 proto.* API 承担。

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
