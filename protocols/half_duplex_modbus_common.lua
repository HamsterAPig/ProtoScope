-- 核心流程：这里是通用半双工帧 codec，只负责按调用方传入的 schema 组帧、解帧和流式解析。

local M = {}

-- 默认帧头和长度字段配置；具体协议可以在自己的 protocol 表中覆盖。
M.DEFAULT_HEADER = { 0xA5, 0x5A }
M.LENGTH_SPEC = { type = "u16", endian = "le", unit = "bytes" }
M.SEQUENCE_BITS = 8

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
  return math.floor(value) & 0xFF
end

local function scalar_width(type_name)
  return TYPE_WIDTH[type_name]
end

local function unit_width(unit_name)
  return UNIT_WIDTH[unit_name or "bytes"] or 1
end

local function read_unsigned(bytes, offset, width, endian)
  local value = 0
  if endian == "be" then
    for step = 0, width - 1 do
      value = (value << 8) | (bytes[offset + step + 1] or 0)
    end
  else
    for step = width - 1, 0, -1 do
      value = (value << 8) | (bytes[offset + step + 1] or 0)
    end
  end
  return value
end

local function write_unsigned(value, width, endian)
  local unsigned = math.floor(value)
  local bytes = {}
  if endian == "be" then
    for step = width - 1, 0, -1 do
      bytes[#bytes + 1] = clamp_byte(unsigned >> (step * 8))
    end
  else
    for step = 0, width - 1 do
      bytes[#bytes + 1] = clamp_byte(unsigned >> (step * 8))
    end
  end
  return bytes
end

local function signed_from_unsigned(value, width)
  local bits = width * 8
  local sign_bit = 1 << (bits - 1)
  local modulus = 1 << bits
  if value >= sign_bit then
    return value - modulus
  end
  return value
end

local function unsigned_from_signed(value, width)
  local bits = width * 8
  local modulus = 1 << bits
  if value < 0 then
    return value + modulus
  end
  return value
end

local function resolve_reference(ref, values, bytes_remaining, field)
  if type(ref) == "function" then
    return ref(values, bytes_remaining, field)
  end
  if type(ref) == "string" then
    return values[ref]
  end
  return ref
end

local function resolve_repeat_count(field, values, bytes_remaining, item_width)
  if field.count ~= nil then
    return resolve_reference(field.count, values, bytes_remaining, field), nil
  end
  if field.count_from ~= nil then
    return resolve_reference(field.count_from, values, bytes_remaining, field), nil
  end
  if field.length_from ~= nil then
    local byte_length = resolve_reference(field.length_from, values, bytes_remaining, field) or 0
    if (byte_length % item_width) ~= 0 then
      return nil, string.format("字段 %s 长度 %d 无法按 %d 字节对齐", field.name, byte_length, item_width)
    end
    return math.floor(byte_length / item_width), nil
  end
  if field.until_end then
    if (bytes_remaining % item_width) ~= 0 then
      return nil, string.format("字段 %s 剩余长度 %d 无法按 %d 字节对齐", field.name, bytes_remaining, item_width)
    end
    return math.floor(bytes_remaining / item_width), nil
  end
  return 1, nil
end

local function is_array_field(field)
  return field.count ~= nil
      or field.count_from ~= nil
      or field.length_from ~= nil
      or field.until_end == true
      or field.is_array == true
end

local function encode_scalar(type_name, value, endian)
  local width = scalar_width(type_name)
  if not width then
    return nil, "不支持的字段类型: " .. tostring(type_name)
  end
  local unsigned = type_name:sub(1, 1) == "i" and unsigned_from_signed(value, width) or math.floor(value)
  return write_unsigned(unsigned, width, endian or "le"), nil
end

local function decode_scalar(type_name, bytes, offset, endian)
  local width = scalar_width(type_name)
  if not width then
    return nil, "不支持的字段类型: " .. tostring(type_name)
  end
  local value = read_unsigned(bytes, offset, width, endian or "le")
  if type_name:sub(1, 1) == "i" then
    value = signed_from_unsigned(value, width)
  end
  return value, nil
end

-- 按 schema.fields 把 Lua table 编码成 payload 字节数组。
-- 支持标量字段、定长/动态数组字段和 bytes 字段；offset 可用于跳过保留字节。
function M.encode_fields(fields, values)
  local buffer = {}
  local cursor = 0
  for _, field in ipairs(fields) do
    local offset = field.offset ~= nil and field.offset or cursor
    while #buffer < offset do
      buffer[#buffer + 1] = 0
    end

    local field_bytes = {}
    if field.type == "bytes" then
      local source = values[field.name] or {}
      if type(source) ~= "table" then
        return nil, "字段 " .. field.name .. " 必须提供 bytes table"
      end
      field_bytes = copy_bytes(source)
    elseif is_array_field(field) then
      local items = values[field.name] or {}
      if type(items) ~= "table" then
        return nil, "字段 " .. field.name .. " 必须提供数组"
      end
      local width = scalar_width(field.type)
      local expected_count, count_error = resolve_repeat_count(field, values, #items * width, width)
      if not expected_count then
        return nil, count_error
      end
      if expected_count ~= #items then
        return nil, string.format("字段 %s 期望 %d 项，实际 %d 项", field.name, expected_count, #items)
      end
      for item_index = 1, #items do
        local encoded, encode_error = encode_scalar(field.type, items[item_index], field.endian)
        if not encoded then
          return nil, encode_error
        end
        append_bytes(field_bytes, encoded)
      end
    else
      local encoded, encode_error = encode_scalar(field.type, values[field.name] or 0, field.endian)
      if not encoded then
        return nil, encode_error
      end
      field_bytes = encoded
    end

    append_bytes(buffer, field_bytes)
    cursor = math.max(cursor, offset + #field_bytes)
  end
  return buffer, nil
end

-- 按 schema.fields 从 payload 字节数组解出 Lua table。
-- 数组字段可通过 count/count_from/length_from/until_end 描述长度来源。
function M.decode_fields(fields, bytes)
  local values = {}
  local cursor = 0
  for _, field in ipairs(fields) do
    local offset = field.offset ~= nil and field.offset or cursor
    if offset > #bytes then
      return nil, string.format("字段 %s 偏移 %d 超出负载长度 %d", field.name, offset, #bytes)
    end

    if field.type == "bytes" then
      local byte_length, count_error = resolve_repeat_count(field, values, #bytes - offset, 1)
      if not byte_length then
        return nil, count_error
      end
      if offset + byte_length > #bytes then
        return nil, string.format("字段 %s 长度不足", field.name)
      end
      values[field.name] = copy_bytes(bytes, offset + 1, offset + byte_length)
      cursor = math.max(cursor, offset + byte_length)
    elseif is_array_field(field) then
      local width = scalar_width(field.type)
      local item_count, count_error = resolve_repeat_count(field, values, #bytes - offset, width)
      if not item_count then
        return nil, count_error
      end
      local total_width = item_count * width
      if offset + total_width > #bytes then
        return nil, string.format("字段 %s 长度不足，期望 %d 字节，实际剩余 %d", field.name, total_width, #bytes - offset)
      end
      local decoded = {}
      for item_index = 0, item_count - 1 do
        local value, decode_error = decode_scalar(field.type, bytes, offset + item_index * width, field.endian)
        if decode_error then
          return nil, decode_error
        end
        decoded[#decoded + 1] = value
      end
      values[field.name] = decoded
      cursor = math.max(cursor, offset + total_width)
    else
      local width = scalar_width(field.type)
      if offset + width > #bytes then
        return nil, string.format("字段 %s 长度不足", field.name)
      end
      local value, decode_error = decode_scalar(field.type, bytes, offset, field.endian)
      if decode_error then
        return nil, decode_error
      end
      values[field.name] = value
      cursor = math.max(cursor, offset + width)
    end
  end
  return values, nil
end

local function encode_length(payload_size, length_spec)
  local unit = unit_width(length_spec.unit)
  if (payload_size % unit) ~= 0 then
    return nil, string.format("长度 %d 不能按 %s 单位编码", payload_size, length_spec.unit or "bytes")
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
  for start = 1, #buffer - #header + 1 do
    local matched = true
    for offset = 1, #header do
      if buffer[start + offset - 1] ~= header[offset] then
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

local function message_schema(protocol, func)
  return protocol.messages[func]
end

local function remove_prefix(buffer, count)
  for _ = 1, count do
    table.remove(buffer, 1)
  end
end

-- 创建流式解析器状态。调用方必须传入具体 protocol schema。
function M.new_parser(protocol)
  return {
    protocol = protocol,
    buffer = {},
    sequence = {},
  }
end

-- 按具体 protocol schema 组一帧：header + func + seq + len + payload + crc16_modbus。
function M.build_frame(protocol, func, sequence, payload_values)
  local codec = protocol
  if not codec then
    return nil, "必须提供 protocol schema"
  end
  local schema = message_schema(codec, func)
  if not schema then
    return nil, string.format("未定义的功能码: 0x%02X", func)
  end

  local payload, payload_error = M.encode_fields(schema.fields or {}, payload_values or {})
  if not payload then
    return nil, payload_error
  end

  local length_bytes, length_error = encode_length(#payload, codec.length or M.LENGTH_SPEC)
  if not length_bytes then
    return nil, length_error
  end

  local frame = copy_bytes(codec.header or M.DEFAULT_HEADER)
  frame[#frame + 1] = clamp_byte(func)
  frame[#frame + 1] = clamp_byte(sequence or 0)
  append_bytes(frame, length_bytes)
  append_bytes(frame, payload)

  local crc = proto.crc16_modbus(frame)
  frame[#frame + 1] = clamp_byte(crc)
  frame[#frame + 1] = clamp_byte(crc >> 8)
  return frame, nil
end

-- 追加输入字节并尽可能解析完整帧；半包保留在 state.buffer，坏帧以 errors 返回。
function M.feed_parser(state, incoming_bytes)
  append_bytes(state.buffer, incoming_bytes)

  local frames = {}
  local errors = {}
  local header = state.protocol.header or M.DEFAULT_HEADER
  local length_width = scalar_width((state.protocol.length or M.LENGTH_SPEC).type or "u16")
  local frame_prefix_size = #header + 1 + 1 + length_width
  local frame_min_size = frame_prefix_size + 2

  while true do
    local start = find_header(state.buffer, header)
    if not start then
      if #state.buffer > (#header - 1) then
        state.buffer = copy_bytes(state.buffer, #state.buffer - #header + 2, #state.buffer)
      end
      break
    end

    if start > 1 then
      remove_prefix(state.buffer, start - 1)
    end

    if #state.buffer < frame_min_size then
      break
    end

    local payload_size, length_error = decode_length(state.buffer, #header + 2, state.protocol.length or M.LENGTH_SPEC)
    if not payload_size then
      errors[#errors + 1] = { kind = "length", message = length_error }
      remove_prefix(state.buffer, 1)
      goto continue
    end

    local total_size = frame_prefix_size + payload_size + 2
    if #state.buffer < total_size then
      break
    end

    local frame_bytes = copy_bytes(state.buffer, 1, total_size)
    remove_prefix(state.buffer, total_size)

    local crc_expected = proto.crc16_modbus(copy_bytes(frame_bytes, 1, #frame_bytes - 2))
    local crc_low = frame_bytes[#frame_bytes - 1]
    local crc_high = frame_bytes[#frame_bytes]
    local crc_actual = crc_low | (crc_high << 8)
    if crc_expected ~= crc_actual then
      errors[#errors + 1] = {
        kind = "crc",
        message = string.format("CRC 不匹配，期望 0x%04X，实际 0x%04X", crc_expected, crc_actual),
      }
      goto continue
    end

    local func = frame_bytes[#header + 1]
    local sequence = frame_bytes[#header + 2]
    local payload = copy_bytes(frame_bytes, frame_prefix_size + 1, frame_prefix_size + payload_size)
    local schema = message_schema(state.protocol, func)
    if not schema then
      errors[#errors + 1] = {
        kind = "func",
        func = func,
        sequence = sequence,
        message = string.format("未知功能码 0x%02X", func),
      }
      goto continue
    end

    local decoded, decode_error = M.decode_fields(schema.fields or {}, payload)
    if not decoded then
      errors[#errors + 1] = {
        kind = "schema",
        func = func,
        sequence = sequence,
        message = decode_error,
      }
      goto continue
    end

    local frame = {
      func = func,
      name = schema.name,
      sequence = sequence,
      payload_size = payload_size,
      payload = decoded,
      payload_bytes = payload,
      raw = frame_bytes,
      sequence_state = M.track_sequence(state.sequence, func, sequence, state.protocol.sequence_bits or M.SEQUENCE_BITS),
    }
    frames[#frames + 1] = frame

    ::continue::
  end

  return frames, errors
end

return M
