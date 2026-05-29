-- 核心流程：上位机只做三件事——按寄存器分批入队 request、在 ACK 到达时 request_done、把主动上报帧推成波形。

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

local function track_sequence(tracker, key, sequence, bits)
  local modulus = 1 << (bits or SEQUENCE_BITS)
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
    initialized = slot.initialized,
    expected = slot.expected,
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
  return channel_mask_from_values({
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
  local frame, err = build_frame(protocol, FRAME_ID, FUNC_WRITE_REGISTERS, next_sequence(), {
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
    sequence_state = track_sequence(sequence_tracker, frame.name, sequence, SEQUENCE_BITS)
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
  local enabled_channels = enabled_channels_from_mask(frame.payload.channel_mask or 0, CHANNEL_COUNT)
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
