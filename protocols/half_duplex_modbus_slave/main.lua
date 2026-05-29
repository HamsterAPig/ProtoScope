-- 核心流程：从机只接写寄存器请求，回 ACK；当 start 寄存器置 1 后，由定时器主动发波形帧。

local DEFAULT_HEADER = { 0xA5, 0x5A }
local DEFAULT_FUNC = { type = "u8" }
local LENGTH_SPEC = { type = "u16", endian = "le", unit = "bytes" }
local CRC_SPEC = { type = "crc16_modbus", endian = "le" }
local SEQUENCE_BITS = 8

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
local CHANNEL_COUNT = 4
local CHANNEL_SCALE = 0.001
local DEFAULT_INTERVAL_MS = 100
local DEFAULT_SAMPLE_COUNT = 16

-- 协议 schema 放在具体协议脚本里；common 只按这里的定义做编解码。
local protocol = {
  frames = {
    {
      id = FRAME_ID,
      header = DEFAULT_HEADER,
      func = DEFAULT_FUNC,
      sequence = { type = "u8", bits = SEQUENCE_BITS },
      length = LENGTH_SPEC,
      crc = CRC_SPEC,
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
                return (values.sample_count or 0) * popcount(values.channel_mask or 0)
              end,
            },
          },
        },
      },
    },
  },
}

local TYPE_WIDTH = {
  u8 = 1,
  i8 = 1,
  u16 = 2,
  i16 = 2,
  u32 = 4,
  i32 = 4,
}

local function append_bytes(target, source)
  for index = 1, #source do
    target[#target + 1] = source[index]
  end
end

local function copy_bytes(source)
  local bytes = {}
  append_bytes(bytes, source)
  return bytes
end

local function clamp_byte(value)
  local current = math.floor(tonumber(value) or 0)
  if current < 0 then
    current = current % 256
  end
  return current & 0xFF
end

local function type_width(type_name)
  local width = TYPE_WIDTH[type_name]
  if not width then
    error("不支持的字段类型: " .. tostring(type_name))
  end
  return width
end

local function unit_width(unit_name)
  if unit_name == nil or unit_name == "bytes" then
    return 1
  end
  if unit_name == "u16" then
    return 2
  end
  if unit_name == "u32" then
    return 4
  end
  error("不支持的长度单位: " .. tostring(unit_name))
end

local function encode_scalar(type_name, value, endian)
  local current = math.floor(tonumber(value) or 0)
  if type_name == "u8" or type_name == "i8" then
    return { clamp_byte(current) }
  end

  local width = type_width(type_name)
  local bytes = {}
  for index = 1, width do
    local shift = (index - 1) * 8
    bytes[index] = clamp_byte(current >> shift)
  end
  if endian == "be" then
    local reversed = {}
    for index = width, 1, -1 do
      reversed[#reversed + 1] = bytes[index]
    end
    return reversed
  end
  return bytes
end

local function field_is_array(field)
  return field.count ~= nil or field.count_from ~= nil
end

local function resolve_count(field, values)
  if field.count ~= nil then
    return field.count
  end
  if type(field.count_from) == "string" then
    return tonumber(values[field.count_from]) or 0
  end
  if type(field.count_from) == "function" then
    return tonumber(field.count_from(values)) or 0
  end
  return 1
end

local function encode_fields(fields, values)
  local payload = {}
  for _, field in ipairs(fields or {}) do
    local width = type_width(field.type)
    local count = resolve_count(field, values)
    local offset = field.offset and (field.offset + 1) or (#payload + 1)
    while #payload < (offset - 1) do
      payload[#payload + 1] = 0
    end

    if field_is_array(field) then
      local items = values[field.name] or {}
      for index = 1, count do
        local encoded = encode_scalar(field.type, items[index], field.endian or "le")
        for byte_index = 1, width do
          payload[offset + (index - 1) * width + byte_index - 1] = encoded[byte_index]
        end
      end
    else
      local encoded = encode_scalar(field.type, values[field.name], field.endian or "le")
      for byte_index = 1, width do
        payload[offset + byte_index - 1] = encoded[byte_index]
      end
    end
  end
  return payload
end

local function encode_length(length_value, spec)
  local unit = unit_width(spec.unit)
  return encode_scalar(spec.type or "u16", math.floor((length_value or 0) / unit), spec.endian or "le")
end

local function crc16_modbus(bytes)
  local crc = 0xFFFF
  for index = 1, #bytes do
    crc = crc ~ clamp_byte(bytes[index])
    for _ = 1, 8 do
      local lsb = crc & 0x0001
      crc = crc >> 1
      if lsb ~= 0 then
        crc = crc ~ 0xA001
      end
    end
  end
  return crc & 0xFFFF
end

local function encode_crc(bytes, spec)
  if (spec.type or "crc16_modbus") ~= "crc16_modbus" then
    return nil, "不支持的 CRC 类型: " .. tostring(spec.type)
  end
  return encode_scalar("u16", crc16_modbus(bytes), spec.endian or "le")
end

local function frame_messages(frame_schema)
  return frame_schema.messages or {}
end

local function find_frame_schema(protocol_schema, frame_id)
  local frames = protocol_schema.frames or {}
  if frame_id == nil then
    if #frames == 1 then
      return frames[1]
    end
    return nil, "存在多个 frame schema，必须指定 frame_id"
  end

  for _, frame_schema in ipairs(frames) do
    if frame_schema.id == frame_id then
      return frame_schema
    end
  end
  return nil, "未找到 frame schema: " .. tostring(frame_id)
end

local function build_frame(protocol_schema, frame_id, func, sequence, payload_values)
  local frame_schema, frame_error = find_frame_schema(protocol_schema, frame_id)
  if not frame_schema then
    return nil, frame_error
  end

  local message_schema = frame_messages(frame_schema)[func]
  if not message_schema then
    return nil, string.format("未定义的功能码: 0x%02X", func)
  end

  local payload = encode_fields(message_schema.fields or {}, payload_values or {})
  local func_spec = frame_schema.func or DEFAULT_FUNC
  local sequence_spec = frame_schema.sequence or { type = "u8", bits = SEQUENCE_BITS }
  local frame = copy_bytes(frame_schema.header or DEFAULT_HEADER)
  append_bytes(frame, encode_scalar(func_spec.type or "u8", func, func_spec.endian or "le"))
  append_bytes(frame, encode_scalar(sequence_spec.type or "u8", sequence or 0, sequence_spec.endian or "le"))
  append_bytes(frame, encode_length(#payload, frame_schema.length or LENGTH_SPEC))
  append_bytes(frame, payload)
  local crc_bytes, crc_error = encode_crc(frame, frame_schema.crc or CRC_SPEC)
  if not crc_bytes then
    return nil, crc_error
  end
  append_bytes(frame, crc_bytes)
  return frame, nil
end

function popcount(mask)
  local count = 0
  local current = mask or 0
  while current > 0 do
    count = count + (current & 1)
    current = current >> 1
  end
  return count
end

local function channel_mask_from_values(values, channel_count)
  local mask = 0
  local count = channel_count or #values
  for channel_index = 1, count do
    if values[channel_index] and values[channel_index] ~= 0 then
      mask = mask | (1 << (channel_index - 1))
    end
  end
  return mask
end

local function enabled_channels_from_mask(mask, channel_count)
  local channels = {}
  local count = channel_count or 32
  for channel_index = 1, count do
    if (mask & (1 << (channel_index - 1))) ~= 0 then
      channels[#channels + 1] = channel_index
    end
  end
  return channels
end

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
  local channels = enabled_channels_from_mask(channel_mask, CHANNEL_COUNT)
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
  local frame, err = build_frame(protocol, FRAME_ID, FUNC_WRITE_ACK, sequence, {
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

local function make_legacy_frame(frame)
  local fields = frame.fields or {}
  if frame.name == "write_registers" and fields.values ~= nil and type(fields.values) ~= "table" then
    fields.values = { fields.values }
  end
  return {
    name = frame.name,
    raw = frame.raw,
    payload = fields,
    sequence = fields.sequence,
    crc_ok = frame.crc_ok,
  }
end

local function handle_stream_frame(ctx, frame)
  if frame.name == "write_registers" then
    handle_write_request(make_legacy_frame(frame))
  end
end

local function handle_stream_error(ctx, err)
  local level = err.code == "crc_mismatch" and "warn" or "error"
  proto.status.set("请求解析失败: " .. tostring(err.message), { level = level })
end

function stream()
  return {
    buffer = {
      capacity = 4096,
      overflow = "drop_oldest",
    },
    frames = {
      {
        name = "write_registers",
        header = { 0xA5, 0x5A, FUNC_WRITE_REGISTERS },
        len = { offset = 5, type = "u16_le", means = "payload", extra = 8 },
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "sequence", type = "u8", offset = 4 },
          { name = "start_address", type = "u16_le", offset = 7 },
          { name = "register_count", type = "u8", offset = 9 },
          { name = "values", type = "u16_le", count = "register_count" },
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


function on_timer(ctx, timer_name)
  if timer_name ~= "stream_tick" then
    return
  end
  if (registers[REG_START] or 0) ~= 1 then
    return
  end

  local channel_mask = channel_mask_from_values({
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
  local frame, err = build_frame(protocol, FRAME_ID, FUNC_STREAM_DATA, wave_sequence, {
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
end
