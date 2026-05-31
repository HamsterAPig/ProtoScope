-- 核心流程：从机用 stream() 接请求、按旧协议回 ACK/异常帧；当 0x8888=1 时，每 10ms 打包 120 帧上传后只调一次 proto.send。

local FUNC_READ_HOLDING = 0x03
local FUNC_WRITE_SINGLE = 0x06
local FUNC_WRITE_MULTI = 0x10
local FUNC_UPLOAD_CH4 = 0x26

local FUNC_EXCEPTION_READ = 0x83
local FUNC_EXCEPTION_WRITE_SINGLE = 0x86
local FUNC_EXCEPTION_WRITE_MULTI = 0x90

local REG_SELECT_CH12 = 0x1010
local REG_SELECT_CH34 = 0x1012
local REG_GAIN_CH12 = 0x5A5A
local REG_GAIN_CH34 = 0x5A5C
local REG_STREAM_SWITCH = 0x8888

local CHANNEL_COUNT = 4
local UPLOAD_TICK_MS = 10
local UPLOAD_BATCH_FRAMES = 120
local UPLOAD_SAMPLE_RATE_HZ = UPLOAD_BATCH_FRAMES * 1000.0 / UPLOAD_TICK_MS
local FUNDAMENTAL_HZ = 50.0
local FUNDAMENTAL_PHASE_STEP = 2.0 * math.pi * FUNDAMENTAL_HZ / UPLOAD_SAMPLE_RATE_HZ
local TIMER_NAME = "sn_scope_upload_tick"
local CHANNEL_SCALE = 1000

local registers = {}
local upload_sequence = 0
local upload_sample_cursor = 0

local function append_bytes(target, source)
  for index = 1, #source do
    target[#target + 1] = source[index]
  end
end

local function push_u16_be(target, value)
  local clamped = math.floor(tonumber(value) or 0) & 0xFFFF
  target[#target + 1] = (clamped >> 8) & 0xFF
  target[#target + 1] = clamped & 0xFF
end

local function push_i16_be(target, value)
  local raw = math.floor(tonumber(value) or 0)
  if raw < -32768 then
    raw = -32768
  elseif raw > 32767 then
    raw = 32767
  end
  push_u16_be(target, raw & 0xFFFF)
end

local function rewrite_crc_hi_lo(frame)
  local payload = {}
  for index = 1, #frame - 2 do
    payload[#payload + 1] = frame[index]
  end
  local crc = proto.crc16_modbus(payload)
  frame[#frame - 1] = (crc >> 8) & 0xFF
  frame[#frame] = crc & 0xFF
end

local function build_fc03_response(values)
  local frame = { 0xFF, FUNC_READ_HOLDING, #values * 2 }
  for _, value in ipairs(values) do
    push_u16_be(frame, value)
  end
  frame[#frame + 1] = 0x00
  frame[#frame + 1] = 0x00
  rewrite_crc_hi_lo(frame)
  return frame
end

local function build_fc06_ack(address, value)
  local frame = { 0xFF, FUNC_WRITE_SINGLE }
  push_u16_be(frame, address)
  push_u16_be(frame, value)
  frame[#frame + 1] = 0x00
  frame[#frame + 1] = 0x00
  rewrite_crc_hi_lo(frame)
  return frame
end

local function build_fc16_ack(address, register_count)
  local frame = { 0xFF, FUNC_WRITE_MULTI }
  push_u16_be(frame, address)
  push_u16_be(frame, register_count)
  frame[#frame + 1] = 0x00
  frame[#frame + 1] = 0x00
  rewrite_crc_hi_lo(frame)
  return frame
end

local function build_exception_frame(func, code)
  local frame = { 0xFF, func, code or 0x03, 0x00, 0x00 }
  rewrite_crc_hi_lo(frame)
  return frame
end

local function build_upload_frame(sequence, ch1, ch2, ch3, ch4)
  local frame = { 0xFF, FUNC_UPLOAD_CH4 }
  push_u16_be(frame, sequence)
  push_i16_be(frame, ch1)
  push_i16_be(frame, ch2)
  push_i16_be(frame, ch3)
  push_i16_be(frame, ch4)
  frame[#frame + 1] = 0x00
  frame[#frame + 1] = 0x00
  rewrite_crc_hi_lo(frame)
  return frame
end

local function reset_registers()
  registers = {
    [REG_SELECT_CH12] = 1,
    [REG_SELECT_CH12 + 1] = 2,
    [REG_SELECT_CH34] = 3,
    [REG_SELECT_CH34 + 1] = 4,
    [REG_GAIN_CH12] = 1000,
    [REG_GAIN_CH12 + 1] = 1000,
    [REG_GAIN_CH34] = 1000,
    [REG_GAIN_CH34 + 1] = 1000,
    [REG_STREAM_SWITCH] = 0,
  }
end

local function start_streaming()
  registers[REG_STREAM_SWITCH] = 1
  proto.set_timer(TIMER_NAME, UPLOAD_TICK_MS)
end

local function stop_streaming()
  registers[REG_STREAM_SWITCH] = 0
  proto.cancel_timer(TIMER_NAME)
end

local function selector_register_for_channel(channel_index)
  if channel_index <= 2 then
    return REG_SELECT_CH12 + channel_index - 1
  end
  return REG_SELECT_CH34 + channel_index - 3
end

local function gain_register_for_channel(channel_index)
  if channel_index <= 2 then
    return REG_GAIN_CH12 + channel_index - 1
  end
  return REG_GAIN_CH34 + channel_index - 3
end

local function waveform_value(selector, sample_index)
  local mode = (math.floor(tonumber(selector) or 1) - 1) % 4
  -- 120 帧 / 10ms 等价于 12kHz，每 240 个采样点形成一个 50Hz 基波周期。
  local phase = sample_index * FUNDAMENTAL_PHASE_STEP
  if mode == 0 then
    return math.sin(phase)
  elseif mode == 1 then
    return 0.35 + 0.65 * math.sin(phase * 0.7 + 0.4)
  elseif mode == 2 then
    return math.sin(phase * 0.5) * math.cos(phase * 0.11 + 0.7)
  end
  local cycle = (phase * 0.35) % 1.0
  return 4.0 * math.abs(cycle - 0.5) - 1.0
end

local function channel_sample(channel_index, sample_index)
  local selector = registers[selector_register_for_channel(channel_index)] or channel_index
  local gain = (registers[gain_register_for_channel(channel_index)] or CHANNEL_SCALE) / CHANNEL_SCALE
  return waveform_value(selector, sample_index) * gain * CHANNEL_SCALE
end

local function build_upload_payload()
  local payload = {}
  for frame_index = 1, UPLOAD_BATCH_FRAMES do
    local sample_index = upload_sample_cursor + frame_index
    local frame = build_upload_frame(
      upload_sequence,
      channel_sample(1, sample_index),
      channel_sample(2, sample_index),
      channel_sample(3, sample_index),
      channel_sample(4, sample_index)
    )
    append_bytes(payload, frame)
    upload_sequence = (upload_sequence + 1) % 0x10000
  end
  upload_sample_cursor = upload_sample_cursor + UPLOAD_BATCH_FRAMES
  return payload
end

local function send_frame(frame, tag)
  proto.send(frame, { tag = tag })
end

local function handle_fc03_request(ctx, frame)
  local fields = frame.fields or {}
  local start_address = fields.address or 0
  local register_count = fields.count or 0
  local values = {}
  for offset = 0, register_count - 1 do
    values[#values + 1] = registers[start_address + offset] or 0
  end
  send_frame(build_fc03_response(values), "fc03_response")
end

local function handle_fc06_request(ctx, frame)
  local fields = frame.fields or {}
  local address = fields.address or 0
  local value = fields.value or 0
  registers[address] = value
  if address == REG_STREAM_SWITCH then
    if value == 1 then
      start_streaming()
    else
      stop_streaming()
    end
  end
  send_frame(build_fc06_ack(address, value), "fc06_ack")
end

local function handle_fc16_request(ctx, frame)
  local fields = frame.fields or {}
  if (fields.count or 0) ~= 2 or (fields.byte_count or 0) ~= 4 then
    send_frame(build_exception_frame(FUNC_EXCEPTION_WRITE_MULTI, 0x03), "fc16_exception")
    return
  end

  local address = fields.address or 0
  registers[address] = fields.value1 or 0
  registers[address + 1] = fields.value2 or 0
  send_frame(build_fc16_ack(address, 2), "fc16_ack")
end

local function handle_stream_error(ctx, err)
  if err.code == "crc_mismatch" then
    proto.status.set("请求 CRC 校验失败: " .. tostring(err.message), { level = "warn" })
  elseif err.code == "noise_discarded" then
    proto.status.set("已丢弃请求噪声: " .. tostring(err.message), { level = "warn" })
  elseif err.code == "invalid_length" then
    proto.status.set("请求长度非法: " .. tostring(err.message), { level = "error" })
  else
    proto.status.set("请求解析失败: " .. tostring(err.message), { level = "error" })
  end
end

function stream()
  ---@type ProtoStreamSchema
  local schema = {
    buffer = {
      capacity = 4096,
      overflow = "drop_oldest",
    },
    frames = {
      {
        name = "fc03_request",
        header = { 0xFF, FUNC_READ_HOLDING },
        size = 8,
        crc = { type = "crc16_modbus", order = "hi_lo" },
        fields = {
          { name = "address", type = "u16_be", offset = 3 },
          { name = "count", type = "u16_be", offset = 5 },
        },
        on_frame = handle_fc03_request,
      },
      {
        name = "fc06_request",
        header = { 0xFF, FUNC_WRITE_SINGLE },
        size = 8,
        crc = { type = "crc16_modbus", order = "hi_lo" },
        fields = {
          { name = "address", type = "u16_be", offset = 3 },
          { name = "value", type = "u16_be", offset = 5 },
        },
        on_frame = handle_fc06_request,
      },
      {
        name = "fc16_request",
        header = { 0xFF, FUNC_WRITE_MULTI },
        size = 13,
        crc = { type = "crc16_modbus", order = "hi_lo" },
        fields = {
          { name = "address", type = "u16_be", offset = 3 },
          { name = "count", type = "u16_be", offset = 5 },
          { name = "byte_count", type = "u8", offset = 7 },
          { name = "value1", type = "u16_be", offset = 8 },
          { name = "value2", type = "u16_be", offset = 10 },
        },
        on_frame = handle_fc16_request,
      },
    },
    on_error = handle_stream_error,
  }
  return schema
end

function ui()
  return {
    {
      id = "sn_scope_slave",
      title = "SN Scope Slave",
      anchor = "left_bottom",
      controls = {
        { type = "button", id = "noop", label = "从机自动模式" },
      },
    },
  }
end

function on_open(ctx)
  reset_registers()
  upload_sequence = 0
  upload_sample_cursor = 0
  proto.status.clear()
end

function on_close(ctx)
  proto.cancel_timer(TIMER_NAME)
  proto.status.clear()
end

function on_error(ctx, message)
  proto.status.set("连接错误: " .. tostring(message), { level = "error" })
end

function on_timer(ctx, timer_name)
  if timer_name ~= TIMER_NAME then
    return
  end
  if (registers[REG_STREAM_SWITCH] or 0) ~= 1 then
    return
  end

  local payload = build_upload_payload()
  send_frame(payload, "upload_ch4_batch")
  proto.set_timer(TIMER_NAME, UPLOAD_TICK_MS)
end

function on_control(ctx, id, value)
  if id == "noop" then
    proto.status.set("从机按请求自动应答", { level = "info" })
  end
end
