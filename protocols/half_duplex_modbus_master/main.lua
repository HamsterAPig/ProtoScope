-- 核心流程：上位机只做三件事——按寄存器分批入队 request、在 ACK 到达时 request_done、把主动上报帧推成波形。

local modbus = require("half_duplex_modbus_common")

local FUNC_WRITE_REGISTERS = 0x10
local FUNC_WRITE_ACK = 0x90
local FUNC_STREAM_DATA = 0x91
local FRAME_ID = "half_duplex_modbus"

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
  frames = {
    {
      id = FRAME_ID,
      header = modbus.DEFAULT_HEADER,
      func = modbus.DEFAULT_FUNC,
      sequence = { type = "u8", bits = modbus.SEQUENCE_BITS },
      length = modbus.LENGTH_SPEC,
      crc = modbus.CRC_SPEC,
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

local sequence_tracker = {}
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
  local frame, err = modbus.build_frame(protocol, FRAME_ID, FUNC_WRITE_REGISTERS, next_sequence(), {
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

local function make_legacy_frame(frame)
  local fields = frame.fields or {}
  if frame.name == "stream_data" and fields.samples ~= nil and type(fields.samples) ~= "table" then
    fields.samples = { fields.samples }
  end
  local sequence = fields.sequence
  local sequence_state = nil
  if sequence ~= nil then
    sequence_state = modbus.track_sequence(sequence_tracker, frame.name, sequence, modbus.SEQUENCE_BITS)
  end
  return {
    name = frame.name,
    raw = frame.raw,
    payload = fields,
    sequence = sequence,
    sequence_state = sequence_state,
    crc_ok = frame.crc_ok,
  }
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

local function handle_stream_frame(ctx, frame)
  local legacy = make_legacy_frame(frame)
  if frame.name == "write_ack" then
    handle_ack(legacy)
  elseif frame.name == "stream_data" then
    handle_stream(legacy)
  end
end

local function handle_stream_error(ctx, err)
  local level = err.code == "crc_mismatch" and "warn" or "error"
  proto.status.set("解析失败: " .. tostring(err.message), { level = level })
end

function stream()
  return {
    buffer = {
      capacity = 4096,
      overflow = "drop_oldest",
    },
    frames = {
      {
        name = "write_ack",
        header = { 0xA5, 0x5A, FUNC_WRITE_ACK },
        len = { offset = 5, type = "u16_le", means = "payload", extra = 8 },
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "sequence", type = "u8", offset = 4 },
          { name = "status", type = "u8", offset = 7 },
          { name = "start_address", type = "u16_le", offset = 8 },
          { name = "register_count", type = "u8", offset = 10 },
        },
        on_frame = handle_stream_frame,
      },
      {
        name = "stream_data",
        header = { 0xA5, 0x5A, FUNC_STREAM_DATA },
        len = { offset = 5, type = "u16_le", means = "payload", extra = 8 },
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "sequence", type = "u8", offset = 4 },
          { name = "timestamp_ms", type = "u32_le", offset = 7 },
          { name = "channel_mask", type = "u8" },
          { name = "sample_count", type = "u8" },
          {
            name = "samples",
            type = "i16_le",
            count = function(parsed)
              return (parsed.sample_count or 0) * proto.bits.count(parsed.channel_mask or 0)
            end,
          },
        },
        on_frame = handle_stream_frame,
      },
    },
    on_error = handle_stream_error,
  }
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
  sequence_tracker = {}
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
