-- 核心流程：这里是通用半双工帧 codec，只负责按调用方传入的 schema 组帧、解帧和流式解析。

local M = {}

-- 默认帧字段配置；具体协议可以在自己的 frame schema 里覆盖。
M.DEFAULT_HEADER = { 0xA5, 0x5A }
M.DEFAULT_FUNC = { type = "u8" }
M.DEFAULT_SEQUENCE = { type = "u8", bits = 8 }
M.LENGTH_SPEC = { type = "u16", endian = "le", unit = "bytes" }
M.CRC_SPEC = { type = "crc16_modbus", endian = "le" }
M.SEQUENCE_BITS = 8
M.DEFAULT_BUFFER_LIMIT = 4096

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
  local width = UNIT_WIDTH[unit_name or "bytes"]
  if not width then
    error("不支持的长度单位: " .. tostring(unit_name))
  end
  return width
end

local function signed_limits(type_name)
  if type_name == "i8" then
    return -128, 127, 8
  end
  if type_name == "i16" then
    return -32768, 32767, 16
  end
  if type_name == "i32" then
    return -2147483648, 2147483647, 32
  end
  return nil, nil, nil
end

local function unsigned_limit(type_name)
  if type_name == "u8" then
    return 0x100, 8
  end
  if type_name == "u16" then
    return 0x10000, 16
  end
  if type_name == "u32" then
    return 0x100000000, 32
  end
  return nil, nil
end

local function encode_scalar(type_name, value, endian)
  if type_name == "bytes" then
    return { clamp_byte(value) }, nil
  end

  local signed_min, signed_max, bit_count = signed_limits(type_name)
  local numeric = math.floor(tonumber(value) or 0)
  if signed_min ~= nil then
    if numeric < signed_min or numeric > signed_max then
      return nil, string.format("%s 超出范围: %d", tostring(type_name), numeric)
    end
    if numeric < 0 then
      numeric = numeric + (1 << bit_count)
    end
  else
    local modulus = nil
    modulus, bit_count = unsigned_limit(type_name)
    if not modulus then
      return nil, "不支持的字段类型: " .. tostring(type_name)
    end
    if numeric < 0 or numeric >= modulus then
      return nil, string.format("%s 超出范围: %d", tostring(type_name), numeric)
    end
  end

  local width = type_width(type_name)
  local bytes = {}
  local current = numeric
  for _ = 1, width do
    bytes[#bytes + 1] = clamp_byte(current)
    current = math.floor(current / 256)
  end
  if endian == "be" then
    local reversed = {}
    for index = width, 1, -1 do
      reversed[#reversed + 1] = bytes[index]
    end
    return reversed, nil
  end
  return bytes, nil
end

local function decode_scalar(type_name, bytes, offset, endian)
  local width = type_width(type_name)
  local start = offset or 1
  if start < 1 or (start + width - 1) > #bytes then
    return nil, string.format("字段越界: type=%s offset=%d width=%d payload=%d", tostring(type_name), start, width, #bytes)
  end

  local numeric = 0
  if endian == "be" then
    for index = 0, width - 1 do
      numeric = (numeric << 8) | clamp_byte(bytes[start + index])
    end
  else
    for index = width - 1, 0, -1 do
      numeric = (numeric << 8) | clamp_byte(bytes[start + index])
    end
  end

  local signed_min, _, bit_count = signed_limits(type_name)
  if signed_min ~= nil then
    local sign_mask = 1 << (bit_count - 1)
    if (numeric & sign_mask) ~= 0 then
      numeric = numeric - (1 << bit_count)
    end
  end

  return numeric, nil
end

local function resolve_count(field, values)
  if field.count ~= nil then
    return math.max(0, math.floor(tonumber(field.count) or 0))
  end
  if field.count_from == nil then
    return 1
  end

  local source = field.count_from
  local count = 0
  if type(source) == "number" then
    count = source
  elseif type(source) == "string" then
    count = values[source] or 0
  elseif type(source) == "function" then
    count = source(values or {})
  end
  return math.max(0, math.floor(tonumber(count) or 0))
end

local function field_is_array(field)
  return field.count ~= nil or field.count_from ~= nil
end

-- 通用字段编码器：支持 offset、count_from 和标量/数组字段。
function M.encode_fields(fields, values)
  local bytes = {}
  local cursor = 1
  local source = values or {}

  for _, field in ipairs(fields or {}) do
    local width = type_width(field.type)
    local count = resolve_count(field, source)
    local is_array = field_is_array(field)
    local offset = field.offset and (field.offset + 1) or cursor
    if offset < 1 then
      return nil, string.format("字段 %s 偏移非法", tostring(field.name))
    end

    while #bytes < (offset - 1) do
      bytes[#bytes + 1] = 0
    end

    if is_array then
      local array_value = source[field.name]
      if type(array_value) ~= "table" then
        return nil, string.format("字段 %s 需要数组值", tostring(field.name))
      end
      for index = 1, count do
        local encoded, encode_error = encode_scalar(field.type, array_value[index] or 0, field.endian or "le")
        if encode_error then
          return nil, string.format("字段 %s[%d] 编码失败: %s", tostring(field.name), index, encode_error)
        end
        append_bytes(bytes, encoded)
      end
    elseif count == 1 then
      local encoded, encode_error = encode_scalar(field.type, source[field.name], field.endian or "le")
      if encode_error then
        return nil, string.format("字段 %s 编码失败: %s", tostring(field.name), encode_error)
      end
      append_bytes(bytes, encoded)
    end

    cursor = math.max(cursor, offset + count * width)
  end

  return bytes, nil
end

-- 通用字段解码器：支持 offset、count_from 和标量/数组字段。
function M.decode_fields(fields, bytes)
  local values = {}
  local cursor = 1
  local payload = bytes or {}

  for _, field in ipairs(fields or {}) do
    local width = type_width(field.type)
    local count = resolve_count(field, values)
    local is_array = field_is_array(field)
    local offset = field.offset and (field.offset + 1) or cursor
    if offset < 1 then
      return nil, string.format("字段 %s 偏移非法", tostring(field.name))
    end

    if is_array then
      local items = {}
      for index = 1, count do
        local value, decode_error = decode_scalar(field.type, payload, offset + (index - 1) * width, field.endian or "le")
        if decode_error then
          return nil, string.format("字段 %s[%d] 解码失败: %s", tostring(field.name), index, decode_error)
        end
        items[#items + 1] = value
      end
      values[field.name] = items
    elseif count == 1 then
      local value, decode_error = decode_scalar(field.type, payload, offset, field.endian or "le")
      if decode_error then
        return nil, string.format("字段 %s 解码失败: %s", tostring(field.name), decode_error)
      end
      values[field.name] = value
    else
      values[field.name] = {}
    end

    cursor = math.max(cursor, offset + count * width)
  end

  return values, nil
end

local function encode_length(payload_size, length_spec)
  local unit = unit_width(length_spec.unit)
  if (payload_size % unit) ~= 0 then
    return nil, string.format("长度 %d 不能按 %s 单位编码", payload_size, tostring(length_spec.unit or "bytes"))
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
  if #buffer < #header then
    return nil
  end
  for start = 1, #buffer - #header + 1 do
    local matched = true
    for offset = 1, #header do
      if clamp_byte(buffer[start + offset - 1]) ~= clamp_byte(header[offset]) then
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

local function normalize_frame_schema(protocol, frame_schema, index)
  local sequence_spec = frame_schema.sequence
  if not sequence_spec then
    sequence_spec = {
      type = M.DEFAULT_SEQUENCE.type,
      bits = frame_schema.sequence_bits or protocol.sequence_bits or M.SEQUENCE_BITS,
    }
  elseif not sequence_spec.bits then
    sequence_spec = {
      type = sequence_spec.type or M.DEFAULT_SEQUENCE.type,
      endian = sequence_spec.endian,
      bits = frame_schema.sequence_bits or protocol.sequence_bits or M.SEQUENCE_BITS,
    }
  end

  return {
    id = frame_schema.id or protocol.id or ("frame_" .. tostring(index)),
    header = frame_schema.header or protocol.header or M.DEFAULT_HEADER,
    func = frame_schema.func or protocol.func or M.DEFAULT_FUNC,
    sequence = sequence_spec,
    length = frame_schema.length or protocol.length or M.LENGTH_SPEC,
    crc = frame_schema.crc or protocol.crc or M.CRC_SPEC,
    messages = frame_schema.messages or protocol.messages or {},
  }
end

local function resolve_frames(protocol)
  local raw_frames = protocol and protocol.frames
  if type(raw_frames) == "table" and #raw_frames > 0 then
    local normalized = {}
    for index, item in ipairs(raw_frames) do
      normalized[#normalized + 1] = normalize_frame_schema(protocol, item, index)
    end
    return normalized
  end

  return {
    normalize_frame_schema(protocol or {}, {}, 1),
  }
end

local function frame_messages(frame_schema)
  return frame_schema.messages or {}
end

local function find_frame_schema(protocol, frame_id)
  local frames = resolve_frames(protocol)
  if frame_id == nil then
    if #frames == 1 then
      return frames[1], nil
    end
    return nil, "多 frame schema 时必须指定 frame_id"
  end

  for _, frame_schema in ipairs(frames) do
    if frame_schema.id == frame_id then
      return frame_schema, nil
    end
  end
  return nil, "未定义的 frame_id: " .. tostring(frame_id)
end

local function remove_prefix(buffer, count)
  for _ = 1, count do
    table.remove(buffer, 1)
  end
end

local function max_header_length(frames)
  local result = 0
  for _, frame_schema in ipairs(frames) do
    result = math.max(result, #(frame_schema.header or M.DEFAULT_HEADER))
  end
  return result
end

local function find_candidate(buffer, frames)
  local best_start = nil
  local best_schema = nil

  for _, frame_schema in ipairs(frames) do
    local start = find_header(buffer, frame_schema.header or M.DEFAULT_HEADER)
    if start and (best_start == nil or start < best_start) then
      best_start = start
      best_schema = frame_schema
    end
  end

  if not best_schema then
    return nil
  end

  return {
    start = best_start,
    schema = best_schema,
  }
end

local function append_parse_error(errors, frame_schema, message)
  errors[#errors + 1] = {
    frame_id = frame_schema and frame_schema.id or nil,
    schema_id = frame_schema and frame_schema.id or nil,
    message = message,
  }
end

local function trim_noise_tail(buffer, frames)
  local keep = math.max(max_header_length(frames) - 1, 0)
  if keep == 0 then
    remove_prefix(buffer, #buffer)
    return
  end
  if #buffer > keep then
    remove_prefix(buffer, #buffer - keep)
  end
end

local function trim_buffer_if_needed(state, errors)
  local overflow = #state.buffer - state.buffer_limit
  if overflow > 0 then
    remove_prefix(state.buffer, overflow)
    append_parse_error(errors, nil, string.format("解析缓存超过上限 %d，已丢弃最旧 %d 字节", state.buffer_limit, overflow))
  end
end

local function encode_crc(frame_bytes, crc_spec)
  if (crc_spec.type or "crc16_modbus") ~= "crc16_modbus" then
    return nil, "仅支持 crc16_modbus"
  end
  local crc = proto.crc16_modbus(frame_bytes)
  return encode_scalar("u16", crc, crc_spec.endian or "le")
end

local function decode_crc(frame_bytes, crc_spec)
  if (crc_spec.type or "crc16_modbus") ~= "crc16_modbus" then
    return nil, "仅支持 crc16_modbus"
  end
  return decode_scalar("u16", frame_bytes, #frame_bytes - 1, crc_spec.endian or "le")
end

local function frame_prefix_size(frame_schema)
  local func_width = type_width(frame_schema.func.type or "u8")
  local sequence_width = type_width(frame_schema.sequence.type or "u8")
  local length_width = type_width(frame_schema.length.type or "u16")
  return #(frame_schema.header or M.DEFAULT_HEADER) + func_width + sequence_width + length_width
end

local function try_parse_frame(state, frame_schema)
  local header = frame_schema.header or M.DEFAULT_HEADER
  local func_spec = frame_schema.func or M.DEFAULT_FUNC
  local sequence_spec = frame_schema.sequence or M.DEFAULT_SEQUENCE
  local length_spec = frame_schema.length or M.LENGTH_SPEC
  local crc_spec = frame_schema.crc or M.CRC_SPEC

  local func_width = type_width(func_spec.type or "u8")
  local sequence_width = type_width(sequence_spec.type or "u8")
  local length_width = type_width(length_spec.type or "u16")
  local crc_width = 2

  if #state.buffer < frame_prefix_size(frame_schema) then
    return { action = "need_more" }
  end

  local func_offset = #header + 1
  local sequence_offset = func_offset + func_width
  local length_offset = sequence_offset + sequence_width
  local payload_offset = length_offset + length_width

  local func, func_error = decode_scalar(func_spec.type or "u8", state.buffer, func_offset, func_spec.endian or "le")
  if func_error then
    return { action = "drop_one", message = "功能码解码失败: " .. tostring(func_error) }
  end

  local sequence, sequence_error = decode_scalar(sequence_spec.type or "u8", state.buffer, sequence_offset, sequence_spec.endian or "le")
  if sequence_error then
    return { action = "drop_one", message = "序号解码失败: " .. tostring(sequence_error) }
  end

  local payload_size, length_error = decode_length(state.buffer, length_offset, length_spec)
  if length_error then
    return { action = "drop_one", message = "长度解码失败: " .. tostring(length_error) }
  end

  local total_size = payload_offset - 1 + payload_size + crc_width
  if total_size > state.buffer_limit then
    return {
      action = "drop_one",
      message = string.format("帧长度 %d 超过缓存上限 %d", total_size, state.buffer_limit),
    }
  end

  if #state.buffer < total_size then
    return { action = "need_more" }
  end

  local frame_bytes = copy_bytes(state.buffer, 1, total_size)
  local crc_received, crc_decode_error = decode_crc(frame_bytes, crc_spec)
  if crc_decode_error then
    return { action = "drop_one", message = "CRC 解码失败: " .. tostring(crc_decode_error) }
  end

  local crc_expected = proto.crc16_modbus(copy_bytes(frame_bytes, 1, #frame_bytes - crc_width))
  if crc_received ~= crc_expected then
    return {
      action = "drop_one",
      message = string.format("CRC 校验失败: got=0x%04X expected=0x%04X", crc_received, crc_expected),
    }
  end

  local message_schema = frame_messages(frame_schema)[func]
  if not message_schema then
    return {
      action = "drop_frame",
      frame_size = total_size,
      message = string.format("未定义的功能码: 0x%02X", func),
    }
  end

  local payload_end = payload_offset + payload_size - 1
  local payload_bytes = payload_size > 0 and copy_bytes(frame_bytes, payload_offset, payload_end) or {}
  local payload, decode_error = M.decode_fields(message_schema.fields or {}, payload_bytes)
  if decode_error then
    return {
      action = "drop_frame",
      frame_size = total_size,
      message = "载荷解析失败: " .. tostring(decode_error),
    }
  end

  local sequence_key = string.format("%s:0x%02X", tostring(frame_schema.id), func)
  local sequence_bits = sequence_spec.bits or M.SEQUENCE_BITS
  return {
    action = "frame",
    frame_size = total_size,
    frame = {
      frame_id = frame_schema.id,
      schema_id = frame_schema.id,
      func = func,
      sequence = sequence,
      payload = payload,
      payload_bytes = payload_bytes,
      raw = frame_bytes,
      message_name = message_schema.name,
      sequence_state = M.track_sequence(state.sequence, sequence_key, sequence, sequence_bits),
    },
  }
end

-- 创建流式解析器状态。调用方必须传入具体 protocol schema。
function M.new_parser(protocol)
  local frames = resolve_frames(protocol or {})
  return {
    protocol = protocol or {},
    frames = frames,
    buffer = {},
    sequence = {},
    buffer_limit = (protocol and protocol.buffer_limit) or M.DEFAULT_BUFFER_LIMIT,
  }
end

-- 按具体 frame schema 组一帧：header + func + seq + len + payload + crc16_modbus。
function M.build_frame(protocol, frame_id_or_func, func_or_sequence, sequence_or_payload, payload_values)
  local frame_id = nil
  local func = nil
  local sequence = nil
  local payload_source = nil

  if payload_values == nil and type(sequence_or_payload) == "table" then
    func = frame_id_or_func
    sequence = func_or_sequence
    payload_source = sequence_or_payload
  else
    frame_id = frame_id_or_func
    func = func_or_sequence
    sequence = sequence_or_payload
    payload_source = payload_values
  end

  if not protocol then
    return nil, "必须提供 protocol schema"
  end

  local frame_schema, frame_error = find_frame_schema(protocol, frame_id)
  if not frame_schema then
    return nil, frame_error
  end

  local message_schema = frame_messages(frame_schema)[func]
  if not message_schema then
    return nil, string.format("未定义的功能码: 0x%02X", func)
  end

  local payload, payload_error = M.encode_fields(message_schema.fields or {}, payload_source or {})
  if not payload then
    return nil, payload_error
  end

  local frame = copy_bytes(frame_schema.header or M.DEFAULT_HEADER)
  local encoded_func, func_error = encode_scalar(frame_schema.func.type or "u8", func, frame_schema.func.endian or "le")
  if not encoded_func then
    return nil, func_error
  end
  append_bytes(frame, encoded_func)

  local encoded_sequence, sequence_error = encode_scalar(frame_schema.sequence.type or "u8", sequence or 0, frame_schema.sequence.endian or "le")
  if not encoded_sequence then
    return nil, sequence_error
  end
  append_bytes(frame, encoded_sequence)

  local length_bytes, length_error = encode_length(#payload, frame_schema.length or M.LENGTH_SPEC)
  if not length_bytes then
    return nil, length_error
  end
  append_bytes(frame, length_bytes)
  append_bytes(frame, payload)

  local crc_bytes, crc_error = encode_crc(frame, frame_schema.crc or M.CRC_SPEC)
  if not crc_bytes then
    return nil, crc_error
  end
  append_bytes(frame, crc_bytes)
  return frame, nil
end

-- 追加输入字节并尽可能解析完整帧；半包保留在 state.buffer，坏帧以 errors 返回。
function M.feed_parser(state, incoming_bytes)
  local frames = {}
  local errors = {}
  append_bytes(state.buffer, incoming_bytes or {})
  trim_buffer_if_needed(state, errors)

  while #state.buffer > 0 do
    local candidate = find_candidate(state.buffer, state.frames)
    if not candidate then
      trim_noise_tail(state.buffer, state.frames)
      break
    end

    if candidate.start > 1 then
      remove_prefix(state.buffer, candidate.start - 1)
    end

    local result = try_parse_frame(state, candidate.schema)
    if result.action == "need_more" then
      break
    end

    if result.action == "drop_one" then
      append_parse_error(errors, candidate.schema, result.message)
      remove_prefix(state.buffer, 1)
    elseif result.action == "drop_frame" then
      append_parse_error(errors, candidate.schema, result.message)
      remove_prefix(state.buffer, result.frame_size or 1)
    elseif result.action == "frame" then
      frames[#frames + 1] = result.frame
      remove_prefix(state.buffer, result.frame_size)
    else
      append_parse_error(errors, candidate.schema, "未知解析状态")
      remove_prefix(state.buffer, 1)
    end
  end

  return frames, errors
end

return M
