-- 核心流程：主机只负责三件事——把 SN Scope 请求交给 proto.request 排队、在 ACK 到达时 request_done、把 4 路上传帧推成波形。

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
local REQUEST_TIMEOUT_MS = 1000
local UPLOAD_TICK_MS = 10
local UPLOAD_BATCH_FRAMES = 120
local UPLOAD_SAMPLE_DT = (UPLOAD_TICK_MS / 1000.0) / UPLOAD_BATCH_FRAMES
local CHANNEL_SCALE = 0.001

local controls = {}
local pending_requests = {}
local active_request_id = nil
local upload_frame_cursor = 0
local upload_sequence_state = nil
local stopping_stream = false
local last_noise_discarded_message = nil

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

local function rewrite_crc_lo_hi(frame)
  local payload = {}
  for index = 1, #frame - 2 do
    payload[#payload + 1] = frame[index]
  end
  local crc = proto.crc16_modbus(payload)
  frame[#frame - 1] = crc & 0xFF
  frame[#frame] = (crc >> 8) & 0xFF
end

local function build_fc06_request(address, value)
  local frame = { 0xFF, FUNC_WRITE_SINGLE }
  push_u16_be(frame, address)
  push_u16_be(frame, value)
  frame[#frame + 1] = 0x00
  frame[#frame + 1] = 0x00
  rewrite_crc_lo_hi(frame)
  return frame
end

local function build_fc16_request(address, values)
  local frame = { 0xFF, FUNC_WRITE_MULTI }
  push_u16_be(frame, address)
  push_u16_be(frame, #values)
  frame[#frame + 1] = #values * 2
  for _, value in ipairs(values) do
    push_u16_be(frame, value)
  end
  frame[#frame + 1] = 0x00
  frame[#frame + 1] = 0x00
  rewrite_crc_lo_hi(frame)
  return frame
end

local function build_fc03_request(address, count)
  local frame = { 0xFF, FUNC_READ_HOLDING }
  push_u16_be(frame, address)
  push_u16_be(frame, count)
  frame[#frame + 1] = 0x00
  frame[#frame + 1] = 0x00
  rewrite_crc_lo_hi(frame)
  return frame
end

local function read_control(id, fallback)
  local value = controls[id]
  if value == nil then
    value = proto.get_control(id)
  end
  if value == nil then
    return fallback
  end
  return value
end

local function read_positive_int(id, fallback)
  local value = math.floor(tonumber(read_control(id, fallback)) or fallback or 0)
  if value < 0 then
    return fallback
  end
  return value
end

local function read_selector(id, fallback)
  local value = read_positive_int(id, fallback)
  if value < 1 then
    return fallback
  end
  return value
end

local function read_gain(id, fallback)
  local value = math.floor(tonumber(read_control(id, fallback)) or fallback or 0)
  if value < 0 then
    return fallback
  end
  return value
end

local function setup_plot(reset_history)
  proto.plot.setup({
    source = "sn_scope_master",
    reset_history = reset_history,
    channels = {
      { label = "CH1", unit = "V", scale = 1.0, color = "#4FC3F7" },
      { label = "CH2", unit = "V", scale = 1.0, color = "#81C784" },
      { label = "CH3", unit = "V", scale = 1.0, color = "#FFB74D" },
      { label = "CH4", unit = "V", scale = 1.0, color = "#E57373" },
    },
    history_limit = 30000,
  })
end

local function clear_request_state(request_id)
  pending_requests[request_id] = nil
  if active_request_id == request_id then
    active_request_id = nil
  end
end

local function current_request()
  if not active_request_id then
    return nil
  end
  return pending_requests[active_request_id]
end

local function format_u16(value)
  return string.format("0x%04X", math.floor(tonumber(value) or 0) & 0xFFFF)
end

local function request_summary(meta)
  if not meta then
    return "无活动请求"
  end
  if meta.kind == "fc06" then
    return string.format("%s=%s", format_u16(meta.address), format_u16(meta.value))
  end
  return string.format("%s x %d", format_u16(meta.address), meta.register_count or 0)
end

local function finish_active_request(ok, message, level)
  local meta = current_request()
  proto.request_done({
    ok = ok,
    message = message,
  })
  if meta then
    if meta.tag == "stop_stream" then
      stopping_stream = false
    end
    clear_request_state(meta.id)
  end
  proto.status.set(message, { level = level or (ok and "info" or "error") })
end

local function enqueue_request(frame, meta)
  local request_id, err = proto.request(frame, {
    timeout_ms = REQUEST_TIMEOUT_MS,
    tag = meta.tag,
  })
  if not request_id then
    proto.status.set("请求入队失败: " .. tostring(err), { level = "error" })
    return false
  end
  meta.id = request_id
  pending_requests[request_id] = meta
  return true
end

local function queue_fc16(address, values, tag)
  local frame = build_fc16_request(address, values)
  return enqueue_request(frame, {
    kind = "fc16",
    address = address,
    register_count = #values,
    values = values,
    tag = tag,
  })
end

local function queue_fc06(address, value, tag)
  local frame = build_fc06_request(address, value)
  return enqueue_request(frame, {
    kind = "fc06",
    address = address,
    value = value,
    tag = tag,
  })
end

local function queue_fc03(address, count, tag)
  local frame = build_fc03_request(address, count)
  return enqueue_request(frame, {
    kind = "fc03",
    address = address,
    register_count = count,
    tag = tag,
  })
end

local function auto_configure_and_start()
  local requests = {
    {
      kind = "fc16",
      address = REG_SELECT_CH12,
      values = {
        read_selector("selector_ch1", 1),
        read_selector("selector_ch2", 2),
      },
      tag = "cfg_select_ch12",
    },
    {
      kind = "fc16",
      address = REG_SELECT_CH34,
      values = {
        read_selector("selector_ch3", 3),
        read_selector("selector_ch4", 4),
      },
      tag = "cfg_select_ch34",
    },
    {
      kind = "fc16",
      address = REG_GAIN_CH12,
      values = {
        read_gain("gain_ch1", 1000),
        read_gain("gain_ch2", 1000),
      },
      tag = "cfg_gain_ch12",
    },
    {
      kind = "fc16",
      address = REG_GAIN_CH34,
      values = {
        read_gain("gain_ch3", 1000),
        read_gain("gain_ch4", 1000),
      },
      tag = "cfg_gain_ch34",
    },
    {
      kind = "fc06",
      address = REG_STREAM_SWITCH,
      value = 0x0001,
      tag = "start_stream",
    },
  }

  for _, item in ipairs(requests) do
    local ok = false
    if item.kind == "fc16" then
      ok = queue_fc16(item.address, item.values, item.tag)
    else
      ok = queue_fc06(item.address, item.value, item.tag)
    end
    if not ok then
      return
    end
  end

  proto.status.set("已入队 5 条 SN Scope 请求，等待宿主半双工调度", { level = "info" })
end

local function send_start_stop(start_value)
  local tag = start_value == 0 and "stop_stream" or "start_stream"
  if tag == "stop_stream" then
    stopping_stream = true
  else
    stopping_stream = false
  end
  if not queue_fc06(REG_STREAM_SWITCH, start_value, tag) and tag == "stop_stream" then
    stopping_stream = false
  end
end

local function read_gain_registers()
  queue_fc03(REG_GAIN_CH12, 4, "read_gain")
end

local function track_upload_sequence(sequence)
  local state = upload_sequence_state
  if not state then
    upload_sequence_state = {
      initialized = true,
      expected = (sequence + 1) % 0x10000,
      lost_total = 0,
    }
    return { lost = 0, lost_total = 0 }
  end

  local delta = (sequence - state.expected) % 0x10000
  local lost = delta
  if lost > 0 then
    state.lost_total = state.lost_total + lost
  end
  state.expected = (sequence + 1) % 0x10000
  return {
    lost = lost,
    lost_total = state.lost_total,
  }
end

local function append_upload_sample(samples_by_channel, channel_index, sample_time, value)
  local series = samples_by_channel[channel_index]
  if not series.t0 then
    series.t0 = sample_time
  end
  local values = series.values
  values[#values + 1] = (tonumber(value) or 0) * CHANNEL_SCALE
end

local function flush_upload_samples(samples_by_channel)
  for channel_index = 1, CHANNEL_COUNT do
    local series = samples_by_channel[channel_index]
    if #series.values > 0 then
      proto.plot.push(channel_index, {
        source = "sn_scope_upload",
        t0 = series.t0 or 0,
        dt = UPLOAD_SAMPLE_DT,
        values = series.values,
      })
    end
  end
end

local function new_upload_series_by_channel()
  local result = {}
  for channel_index = 1, CHANNEL_COUNT do
    result[channel_index] = { values = {} }
  end
  return result
end

local function handle_fc03_response(ctx, frame)
  local fields = frame.fields or {}
  local values = fields.register_values or {}
  local message = string.format("读取应答: %d byte / %d regs", fields.byte_count or 0, #values)
  local active = current_request()
  if not active then
    proto.status.set(message, { level = "info" })
    return
  end

  if active.kind == "fc03" and #values == (active.register_count or 0) then
    finish_active_request(true, message .. "，匹配: " .. request_summary(active), "info")
    return
  end

  finish_active_request(
    false,
    string.format("FC03 应答不匹配，收到 %d regs，期望 %s", #values, request_summary(active)),
    "error"
  )
end

local function handle_fc06_ack(ctx, frame)
  local active = current_request()
  if not active then
    proto.status.set("收到 FC06 ACK，但当前没有活动 request", { level = "warn" })
    return
  end

  local fields = frame.fields or {}
  local received_address = fields.address or 0
  local received_value = fields.value or 0
  if active.kind == "fc06" and received_address == active.address and received_value == active.value then
    finish_active_request(true, "FC06 ACK 匹配: " .. request_summary(active), "info")
    return
  end

  finish_active_request(
    false,
    string.format(
      "FC06 ACK 不匹配，收到 %s=%s，期望 %s",
      format_u16(received_address),
      format_u16(received_value),
      request_summary(active)
    ),
    "error"
  )
end

local function handle_fc16_ack(ctx, frame)
  local active = current_request()
  if not active then
    proto.status.set("收到 FC16 ACK，但当前没有活动 request", { level = "warn" })
    return
  end

  local fields = frame.fields or {}
  local received_address = fields.address or 0
  local received_count = fields.register_count or 0
  if active.kind == "fc16" and received_address == active.address and received_count == (active.register_count or 0) then
    finish_active_request(true, "FC16 ACK 匹配: " .. request_summary(active), "info")
    return
  end

  finish_active_request(
    false,
    string.format(
      "FC16 ACK 不匹配，收到 %s x %d，期望 %s",
      format_u16(received_address),
      received_count,
      request_summary(active)
    ),
    "error"
  )
end

local function handle_exception(ctx, frame)
  local active = current_request()
  local fields = frame.fields or {}
  local message = string.format(
    "从机异常应答: %s code=%s",
    tostring(frame.name or "exception"),
    format_u16(fields.exception_code or 0)
  )
  if active then
    finish_active_request(false, message .. "，活动请求 " .. request_summary(active), "error")
  else
    proto.status.set(message, { level = "error" })
  end
end

local function append_upload_frame_samples(ctx, frame, samples_by_channel)
  local fields = frame.fields or {}
  local sequence_state = track_upload_sequence(fields.sequence or 0)
  if sequence_state.lost > 0 then
    proto.status.set(string.format("丢帧: %d", sequence_state.lost_total), { level = "warn" })
  end

  local sample_time = upload_frame_cursor * UPLOAD_SAMPLE_DT
  append_upload_sample(samples_by_channel, 1, sample_time, fields.ch1 or 0)
  append_upload_sample(samples_by_channel, 2, sample_time, fields.ch2 or 0)
  append_upload_sample(samples_by_channel, 3, sample_time, fields.ch3 or 0)
  append_upload_sample(samples_by_channel, 4, sample_time, fields.ch4 or 0)
  upload_frame_cursor = upload_frame_cursor + 1
end

local function handle_upload_frame(ctx, frame)
  local samples_by_channel = new_upload_series_by_channel()
  append_upload_frame_samples(ctx, frame, samples_by_channel)
  flush_upload_samples(samples_by_channel)
end

local function handle_stream_error(ctx, err)
  if err.code == "crc_mismatch" then
    local message = "CRC 校验失败: " .. tostring(err.message)
    if current_request() then
      finish_active_request(false, message, "warn")
    else
      proto.status.set(message, { level = "warn" })
    end
  elseif err.code == "noise_discarded" then
    local message = "已丢弃噪声前缀: " .. tostring(err.message)
    if message ~= last_noise_discarded_message then
      proto.status.set(message, { level = "warn" })
      last_noise_discarded_message = message
    end
  elseif err.code == "invalid_length" then
    last_noise_discarded_message = nil
    proto.status.set("长度非法: " .. tostring(err.message), { level = "error" })
  else
    last_noise_discarded_message = nil
    proto.status.set("解析失败: " .. tostring(err.message), { level = "error" })
  end
end

local function handle_stream_batch(ctx, frames)
  local samples_by_channel = new_upload_series_by_channel()
  for _, frame in ipairs(frames or {}) do
    local name = frame.name
    if name == "upload_ch4" then
      -- 停止阶段仍允许尾包推送波形，等待随后到达的 FC06 ACK 完成 request。
      append_upload_frame_samples(ctx, frame, samples_by_channel)
    elseif name == "fc03_response" then
      handle_fc03_response(ctx, frame)
    elseif name == "fc06_ack" then
      handle_fc06_ack(ctx, frame)
    elseif name == "fc16_ack" then
      handle_fc16_ack(ctx, frame)
    elseif name == "exception_fc03" or name == "exception_fc06" or name == "exception_fc16" then
      handle_exception(ctx, frame)
    end
  end
  flush_upload_samples(samples_by_channel)
  if #(frames or {}) > 0 then
    last_noise_discarded_message = nil
  end
end

function stream()
  ---@type ProtoStreamSchema
  local schema = {
    buffer = {
      capacity = 4096,
    },
    raw_output = "omit",
    low_overhead = true,
    frames = {
      {
        name = "fc03_response",
        header = { 0xFF, FUNC_READ_HOLDING },
        len = { offset = 3, type = "u8", means = "payload", extra = 5 },
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "byte_count", type = "u8", offset = 3 },
          {
            name = "register_values",
            type = "u16_be",
            offset = 4,
            count = { op = "div", field = "byte_count", by = 2 },
          },
        },
        on_frame = handle_fc03_response,
      },
      {
        name = "fc06_ack",
        header = { 0xFF, FUNC_WRITE_SINGLE },
        size = 8,
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "address", type = "u16_be", offset = 3 },
          { name = "value", type = "u16_be", offset = 5 },
        },
        on_frame = handle_fc06_ack,
      },
      {
        name = "fc16_ack",
        header = { 0xFF, FUNC_WRITE_MULTI },
        size = 8,
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "address", type = "u16_be", offset = 3 },
          { name = "register_count", type = "u16_be", offset = 5 },
        },
        on_frame = handle_fc16_ack,
      },
      {
        name = "exception_fc03",
        header = { 0xFF, FUNC_EXCEPTION_READ },
        size = 5,
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "exception_code", type = "u8", offset = 3 },
        },
        on_frame = handle_exception,
      },
      {
        name = "exception_fc06",
        header = { 0xFF, FUNC_EXCEPTION_WRITE_SINGLE },
        size = 5,
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "exception_code", type = "u8", offset = 3 },
        },
        on_frame = handle_exception,
      },
      {
        name = "exception_fc16",
        header = { 0xFF, FUNC_EXCEPTION_WRITE_MULTI },
        size = 5,
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "exception_code", type = "u8", offset = 3 },
        },
        on_frame = handle_exception,
      },
      {
        name = "upload_ch4",
        header = { 0xFF, FUNC_UPLOAD_CH4 },
        size = 14,
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "sequence", type = "u16_be", offset = 3 },
          { name = "ch1", type = "i16_be", offset = 5 },
          { name = "ch2", type = "i16_be", offset = 7 },
          { name = "ch3", type = "i16_be", offset = 9 },
          { name = "ch4", type = "i16_be", offset = 11 },
        },
        on_frame = handle_upload_frame,
      },
    },
    on_batch = handle_stream_batch,
    on_error = handle_stream_error,
  }
  return schema
end

function ui()
  return {
    {
      id = "sn_scope_master",
      title = "SN Scope Master",
      anchor = "left_bottom",
      controls = {
        { type = "input_int", id = "selector_ch1", label = "CH1 选择", default = 1 },
        { type = "input_int", id = "selector_ch2", label = "CH2 选择", default = 2 },
        { type = "input_int", id = "selector_ch3", label = "CH3 选择", default = 3 },
        { type = "input_int", id = "selector_ch4", label = "CH4 选择", default = 4 },
        { type = "input_int", id = "gain_ch1", label = "CH1 系数", default = 1000 },
        { type = "input_int", id = "gain_ch2", label = "CH2 系数", default = 1000 },
        { type = "input_int", id = "gain_ch3", label = "CH3 系数", default = 1000 },
        { type = "input_int", id = "gain_ch4", label = "CH4 系数", default = 1000 },
        { type = "button", id = "auto_start", label = "配置并启动上传" },
        { type = "button", id = "start_stream", label = "启动上传" },
        { type = "button", id = "stop_stream", label = "停止上传" },
        { type = "button", id = "read_gain", label = "读取增益系数" },
        { type = "button", id = "clear_plot", label = "清空波形" },
      },
    },
  }
end

function on_open(ctx)
  pending_requests = {}
  active_request_id = nil
  upload_frame_cursor = 0
  upload_sequence_state = nil
  stopping_stream = false
  last_noise_discarded_message = nil
  proto.status.clear()
  setup_plot(true)
end

function on_close(ctx)
  stopping_stream = false
  last_noise_discarded_message = nil
  proto.status.clear()
end

function on_error(ctx, message)
  proto.status.set("连接错误: " .. tostring(message), { level = "error" })
end

function on_tx(ctx, evt)
  if evt.kind ~= "request" then
    if evt.state == "rejected" then
      proto.status.set("发送失败: " .. tostring(evt.error or evt.state), { level = "error" })
    end
    return
  end

  local meta = pending_requests[evt.id]
  if evt.state == "sent" then
    active_request_id = evt.id
    if meta then
      proto.status.set("请求已写出，等待 ACK: " .. request_summary(meta), { level = "info" })
    end
  elseif evt.state == "completed" then
    if meta then
      proto.status.set("请求流程完成: " .. tostring(evt.tag), { level = "info" })
      clear_request_state(evt.id)
    end
  elseif evt.state == "timeout" then
    local label = meta and request_summary(meta) or tostring(evt.tag)
    if meta and meta.tag == "stop_stream" then
      stopping_stream = false
    end
    clear_request_state(evt.id)
    proto.status.set("请求超时: " .. label, { level = "warn" })
  elseif evt.state == "rejected" or evt.state == "dropped" or evt.state == "canceled" then
    local label = meta and request_summary(meta) or tostring(evt.tag)
    if meta and meta.tag == "stop_stream" then
      stopping_stream = false
    end
    clear_request_state(evt.id)
    proto.status.set("请求失败: " .. label .. " / " .. tostring(evt.error or evt.state), { level = "error" })
  end
end

function on_control(ctx, id, value)
  controls[id] = value
  if id == "auto_start" then
    auto_configure_and_start()
  elseif id == "start_stream" then
    send_start_stop(0x0001)
  elseif id == "stop_stream" then
    send_start_stop(0x0000)
  elseif id == "read_gain" then
    read_gain_registers()
  elseif id == "clear_plot" then
    upload_frame_cursor = 0
    upload_sequence_state = nil
    setup_plot(true)
    proto.status.set("波形已清空", { level = "info" })
  end
end
