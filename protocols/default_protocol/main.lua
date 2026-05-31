-- 核心流程：协议脚本只描述控件、收发和完整应答，发送排队、半双工、超时与弹窗都交给宿主处理。

function ui()
  return {
    {
      id = "protocol",
      title = "默认协议面板",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", default = true },
        { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
        { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
        { type = "input_float", id = "scale", label = "缩放", default = 1.0 },
        { type = "elf_symbol_combo", id = "target_symbol", label = "ELF 变量", debounce_ms = 150, limit = 64 },
      },
      layout = {
        kind = "form",
        items = {
          { collapse = "基础控制", default_open = true, items = {{ controls = { "read_version", "device_id" } }} },
          { collapse = "高级控制", default_open = false, items = {{ controls = { "hex_send", "mode", "timeout_ms", "scale" } }} },
          { collapse = "读取变量地址", default_open = true, items = {{ controls = { "target_symbol" } }} },
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

local function log_symbol_info(symbol)
  if type(symbol) ~= "table" then
    proto.log("info", "尚未选择 ELF 静态变量")
    return
  end

  local label = tostring(symbol.label or "")
  local address = tostring(symbol.value or "")
  local value_type = tostring(symbol.type or "")
  if label == "" or address == "" or value_type == "" then
    proto.log("info", "ELF 静态变量信息不完整")
    return
  end

  proto.log("info", string.format("ELF 静态变量: %s, 地址: %s, 类型: %s", label, address, value_type))
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
  elseif id == "target_symbol" then
    -- 核心流程：示范从新增 elf_symbol_combo 读取 `{ label, value, type }`，并用 info 日志输出变量信息。
    log_symbol_info(proto.get_control("target_symbol") or value)
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
