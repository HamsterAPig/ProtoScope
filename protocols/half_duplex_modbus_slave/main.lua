-- 核心流程：从机只接写寄存器请求，回 ACK；当 start 寄存器置 1 后，由定时器主动发波形帧。

local modbus = require("half_duplex_modbus_common")

local parser = modbus.new_parser(modbus.protocol)
local registers = {
  [modbus.REG_CH1] = 1,
  [modbus.REG_CH2] = 1,
  [modbus.REG_CH3] = 1,
  [modbus.REG_CH4] = 1,
  [modbus.REG_START] = 0,
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
  local channels = modbus.enabled_channels_from_mask(channel_mask)
  for point_index = 1, sample_count do
    for _, channel_index in ipairs(channels) do
      local raw = channel_value(channel_index, sample_cursor + point_index)
      samples[#samples + 1] = clamp_i16(raw / modbus.CHANNEL_SCALE)
    end
  end
  sample_cursor = sample_cursor + sample_count
  return samples
end

local function start_streaming()
  if registers[modbus.REG_START] ~= 1 then
    registers[modbus.REG_START] = 1
  end
  proto.set_timer("stream_tick", modbus.DEFAULT_INTERVAL_MS)
end

local function stop_streaming()
  registers[modbus.REG_START] = 0
  proto.cancel_timer("stream_tick")
end

local function apply_register_write(start_address, values)
  for offset = 1, #values do
    registers[start_address + offset - 1] = values[offset]
  end
  if (registers[modbus.REG_START] or 0) == 1 then
    start_streaming()
  else
    stop_streaming()
  end
end

local function send_ack(sequence, start_address, register_count)
  local frame, err = modbus.build_ack_frame(sequence, start_address, register_count, modbus.STATUS_OK)
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
  parser = modbus.new_parser(modbus.protocol)
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
    if frame.func == modbus.FUNC_WRITE_REGISTERS then
      handle_write_request(frame)
    end
  end
end

function on_timer(ctx, timer_name)
  if timer_name ~= "stream_tick" then
    return
  end
  if (registers[modbus.REG_START] or 0) ~= 1 then
    return
  end

  local channel_mask = modbus.register_mask(registers)
  if channel_mask == 0 then
    proto.set_timer("stream_tick", modbus.DEFAULT_INTERVAL_MS)
    return
  end

  local sample_count = modbus.DEFAULT_SAMPLE_COUNT
  local samples = build_samples(channel_mask, sample_count)
  local frame, err = modbus.build_stream_frame(wave_sequence, timestamp_ms, channel_mask, sample_count, samples)
  if not frame then
    proto.status.set("波形组帧失败: " .. tostring(err), { level = "error" })
    return
  end

  proto.send(frame, { tag = "stream_data" })
  wave_sequence = (wave_sequence + 1) % 256
  timestamp_ms = timestamp_ms + modbus.DEFAULT_INTERVAL_MS
  proto.set_timer("stream_tick", modbus.DEFAULT_INTERVAL_MS)
end
