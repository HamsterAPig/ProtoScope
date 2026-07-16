-- 核心流程：主机只负责三件事——把半双工 Modbus 请求交给 proto.request 排队、在 ACK 到达时 request_done、把 4 路上传帧推成波形。

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
local last_noise_discarded_message = nil
local scope_running = false
local scope_transition = "idle"
local scope_desired_running = false
local pending_start_config = nil
local applied_start_config = nil
local start_batch_failed = false
local start_failure_message = nil
local start_config_dirty = false

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

local function read_gain(id, fallback)
  local value = math.floor(tonumber(read_control(id, fallback)) or fallback or 0)
  if value < 0 then
    return fallback
  end
  return value
end

local function history_limit()
  return read_positive_int("history_limit", 30000)
end

local function setup_plot(config, reset_history)
  local labels = config and config.labels or {}
  proto.plot.setup({
    source = "half_duplex_modbus_master",
    reset_history = reset_history,
    channels = {
      { label = labels[1] or "CH1", unit = "V", scale = 1.0, color = "#4FC3F7" },
      { label = labels[2] or "CH2", unit = "V", scale = 1.0, color = "#81C784" },
      { label = labels[3] or "CH3", unit = "V", scale = 1.0, color = "#FFB74D" },
      { label = labels[4] or "CH4", unit = "V", scale = 1.0, color = "#E57373" },
    },
    history_limit = history_limit(),
  })
end

local function sync_scope_running(running)
  scope_running = running == true
  proto.oscilloscope.set_running(scope_running)
  proto.set_control("scope_state", scope_running and "上传运行中" or "上传已停止")
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
    clear_request_state(meta.id)
  end
  proto.status.set(message, { level = level or (ok and "info" or "error") })
  return meta
end

local function enqueue_request(frame, meta, guarded)
  local options = {
    timeout_ms = REQUEST_TIMEOUT_MS,
    tag = meta.tag,
  }
  local request_id, err
  if guarded then
    options.max_attempts = 1
    request_id, err = proto.request_guarded(frame, options)
  else
    request_id, err = proto.request(frame, options)
  end
  if not request_id then
    proto.status.set("请求入队失败: " .. tostring(err), { level = "error" })
    return false
  end
  meta.id = request_id
  meta.guarded = guarded == true
  pending_requests[request_id] = meta
  return true
end

local function queue_fc16(address, values, tag, guarded)
  local frame = build_fc16_request(address, values)
  return enqueue_request(frame, {
    kind = "fc16",
    address = address,
    register_count = #values,
    values = values,
    tag = tag,
  }, guarded)
end

local function queue_fc06(address, value, tag, guarded)
  local frame = build_fc06_request(address, value)
  return enqueue_request(frame, {
    kind = "fc06",
    address = address,
    value = value,
    tag = tag,
  }, guarded)
end

local function queue_fc03(address, count, tag)
  local frame = build_fc03_request(address, count)
  return enqueue_request(frame, {
    kind = "fc03",
    address = address,
    register_count = count,
    tag = tag,
  }, false)
end

local function build_start_config()
  local address_offset = math.floor(tonumber(read_control("address_offset", 0)) or 0)
  local selectors = {}
  local labels = {}
  local channels = {}

  for channel_index = 1, CHANNEL_COUNT do
    local enabled = read_control("enable_ch" .. channel_index, true) == true
    local selector = 0
    local address = nil
    local label = "CH" .. channel_index .. "（禁用）"
    local symbol = read_control("sym_addr_ch" .. channel_index, nil)

    if enabled then
      if type(symbol) ~= "table" then
        return nil, string.format("CH%d 尚未选择 ELF 变量", channel_index)
      end
      local raw_address = tonumber(symbol.value)
      if not raw_address then
        return nil, string.format("CH%d ELF 地址无效: %s", channel_index, tostring(symbol.value))
      end
      address = math.floor(raw_address) + address_offset
      if address < 0 or address > 0xFFFF then
        return nil, string.format(
          "CH%d 地址加偏移后超出 16 位范围: %s",
          channel_index,
          tostring(symbol.value)
        )
      end
      selector = address & 0xFFFF
      label = tostring(symbol.label or ("CH" .. channel_index))
    end

    selectors[channel_index] = selector
    labels[channel_index] = label
    channels[channel_index] = {
      enabled = enabled,
      label = label,
      address = address,
      selector = selector,
    }
  end

  return {
    address_offset = address_offset,
    channels = channels,
    labels = labels,
    selectors = selectors,
    gains = {
      read_gain("gain_ch1", 1000),
      read_gain("gain_ch2", 1000),
      read_gain("gain_ch3", 1000),
      read_gain("gain_ch4", 1000),
    },
  }
end

local function mark_start_failed(message)
  if start_batch_failed then
    proto.status.set(start_failure_message or "启动配置失败，新变量尚未应用", { level = "error" })
    return
  end
  start_batch_failed = true
  start_failure_message = "启动配置失败，新变量尚未应用: " .. tostring(message)
  scope_transition = "idle"
  scope_desired_running = scope_running
  pending_start_config = nil
  sync_scope_running(scope_running)
  proto.status.set(start_failure_message, { level = "error" })
end

local function mark_stop_failed()
  scope_transition = "idle"
  scope_desired_running = scope_running
  sync_scope_running(scope_running)
  local message = "停止失败，设备可能仍按旧地址运行；新变量配置尚未应用，请重新停止"
  proto.set_control("scope_state", message)
  proto.status.set(message, { level = "error" })
end

local function begin_scope_start(config)
  proto.reset_request_guard()
  scope_transition = "starting"
  scope_desired_running = true
  pending_start_config = config
  start_batch_failed = false
  start_failure_message = nil

  local requests = {
    {
      kind = "fc16",
      address = REG_GAIN_CH12,
      values = {
        config.gains[1],
        config.gains[2],
      },
      tag = "cfg_gain_pair1",
    },
    {
      kind = "fc16",
      address = REG_GAIN_CH34,
      values = {
        config.gains[3],
        config.gains[4],
      },
      tag = "cfg_gain_pair2",
    },
    {
      kind = "fc16",
      address = REG_SELECT_CH12,
      values = {
        config.selectors[1],
        config.selectors[2],
      },
      tag = "cfg_select_pair1",
    },
    {
      kind = "fc16",
      address = REG_SELECT_CH34,
      values = {
        config.selectors[3],
        config.selectors[4],
      },
      tag = "cfg_select_pair2",
    },
    {
      kind = "fc06",
      address = REG_STREAM_SWITCH,
      value = 0x0001,
      tag = "stream_start",
    },
  }

  for _, item in ipairs(requests) do
    local ok = false
    if item.kind == "fc16" then
      ok = queue_fc16(item.address, item.values, item.tag, true)
    else
      ok = queue_fc06(item.address, item.value, item.tag, true)
    end
    if not ok then
      mark_start_failed("请求入队失败")
      return false
    end
  end

  proto.set_control("scope_state", "等待启动 ACK")
  proto.status.set("已入队 5 条受保护半双工 Modbus 启动请求，等待调度", { level = "info" })
  return true
end

local function request_scope_start()
  if scope_transition == "stopping" then
    scope_desired_running = true
    proto.set_control("scope_state", "等待停止 ACK，随后自动启动")
    return true
  end
  if scope_transition == "starting" or scope_running then
    scope_desired_running = true
    return true
  end

  local config, err = build_start_config()
  if not config then
    scope_desired_running = false
    proto.status.set("无法启动: " .. tostring(err), { level = "error" })
    return false
  end
  return begin_scope_start(config)
end

local function begin_scope_stop()
  if scope_transition == "stopping" then
    return true
  end
  scope_transition = "stopping"
  local queued = queue_fc06(REG_STREAM_SWITCH, 0x0000, "stream_stop", false)
  if queued then
    proto.set_control("scope_state", "等待停止 ACK")
  else
    mark_stop_failed()
  end
  return queued
end

local function request_scope_stop()
  scope_desired_running = false
  if scope_transition == "stopping" then
    return true
  end
  if scope_transition == "starting" then
    proto.set_control("scope_state", "等待启动 ACK，随后自动停止")
    return true
  end
  -- 即使本地认为已停止也照常下发，确保设备异常重连后仍有机会被显式停住。
  return begin_scope_stop()
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
        source = "half_duplex_modbus_upload",
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
    local tag = active.tag
    finish_active_request(true, "FC06 ACK 匹配: " .. request_summary(active), "info")
    if tag == "stream_start" then
      if start_batch_failed or not pending_start_config then
        mark_start_failed("启动 ACK 到达时配置快照已失效")
        return
      end
      applied_start_config = pending_start_config
      pending_start_config = nil
      start_config_dirty = false
      scope_transition = "idle"
      setup_plot(applied_start_config, true)
      sync_scope_running(true)
      if not scope_desired_running then
        begin_scope_stop()
      end
    elseif tag == "stream_stop" then
      scope_transition = "idle"
      sync_scope_running(false)
      if scope_desired_running then
        request_scope_start()
      end
    end
    return
  end

  local meta = finish_active_request(
    false,
    string.format(
      "FC06 ACK 不匹配，收到 %s=%s，期望 %s",
      format_u16(received_address),
      format_u16(received_value),
      request_summary(active)
    ),
    "error"
  )
  if meta and meta.tag == "stream_stop" then
    mark_stop_failed()
  elseif meta and meta.guarded then
    mark_start_failed("FC06 ACK 不匹配")
  end
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

  local meta = finish_active_request(
    false,
    string.format(
      "FC16 ACK 不匹配，收到 %s x %d，期望 %s",
      format_u16(received_address),
      received_count,
      request_summary(active)
    ),
    "error"
  )
  if meta and meta.guarded then
    mark_start_failed("FC16 ACK 不匹配")
  end
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
    local meta = finish_active_request(false, message .. "，活动请求 " .. request_summary(active), "error")
    if meta and meta.tag == "stream_stop" then
      mark_stop_failed()
    elseif meta and meta.guarded then
      mark_start_failed(message)
    end
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
      local meta = finish_active_request(false, message, "warn")
      if meta and meta.tag == "stream_stop" then
        mark_stop_failed()
      elseif meta and meta.guarded then
        mark_start_failed(message)
      end
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
      id = "half_duplex_modbus_master",
      title = "Half-Duplex Modbus Master",
      anchor = "left_bottom",
      controls = {
        { type = "checkbox", id = "enable_ch1", label = "启用 CH1", default = true },
        { type = "elf_symbol_combo", id = "sym_addr_ch1", label = "CH1 ELF 变量" },
        { type = "checkbox", id = "enable_ch2", label = "启用 CH2", default = true },
        { type = "elf_symbol_combo", id = "sym_addr_ch2", label = "CH2 ELF 变量" },
        { type = "checkbox", id = "enable_ch3", label = "启用 CH3", default = true },
        { type = "elf_symbol_combo", id = "sym_addr_ch3", label = "CH3 ELF 变量" },
        { type = "checkbox", id = "enable_ch4", label = "启用 CH4", default = true },
        { type = "elf_symbol_combo", id = "sym_addr_ch4", label = "CH4 ELF 变量" },
        { type = "input_int", id = "address_offset", label = "地址偏移", default = 0 },
        { type = "input_int", id = "gain_ch1", label = "CH1 系数", default = 1000 },
        { type = "input_int", id = "gain_ch2", label = "CH2 系数", default = 1000 },
        { type = "input_int", id = "gain_ch3", label = "CH3 系数", default = 1000 },
        { type = "input_int", id = "gain_ch4", label = "CH4 系数", default = 1000 },
        { type = "button", id = "auto_start", label = "配置并启动上传" },
        { type = "button", id = "start_stream", label = "启动上传" },
        { type = "button", id = "stop_stream", label = "停止上传" },
        { type = "button", id = "read_gain", label = "读取增益系数" },
        { type = "button", id = "clear_plot", label = "清空波形" },
        { type = "input_int", id = "history_limit", label = "历史上限", default = 30000 },
        { type = "input_text", id = "scope_state", label = "上传状态", default = "上传已停止" },
      },
    },
  }
end

function on_open(ctx)
  pending_requests = {}
  active_request_id = nil
  upload_frame_cursor = 0
  upload_sequence_state = nil
  last_noise_discarded_message = nil
  scope_transition = "idle"
  scope_desired_running = false
  pending_start_config = nil
  applied_start_config = nil
  start_batch_failed = false
  start_failure_message = nil
  start_config_dirty = false
  proto.status.clear()
  setup_plot(nil, true)
  sync_scope_running(false)
end

function on_close(ctx)
  last_noise_discarded_message = nil
  scope_transition = "idle"
  scope_desired_running = false
  pending_start_config = nil
  start_batch_failed = false
  start_failure_message = nil
  sync_scope_running(false)
  proto.status.clear()
end

function on_error(ctx, message)
  scope_transition = "idle"
  scope_desired_running = false
  pending_start_config = nil
  start_batch_failed = false
  start_failure_message = nil
  sync_scope_running(false)
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
    clear_request_state(evt.id)
    if (meta and meta.tag == "stream_stop") or evt.tag == "stream_stop" then
      mark_stop_failed()
    elseif (meta and meta.guarded) or evt.guarded then
      mark_start_failed("请求超时: " .. label)
    else
      proto.status.set("请求超时: " .. label, { level = "warn" })
    end
  elseif evt.state == "failed" or evt.state == "rejected" or evt.state == "dropped" or evt.state == "canceled" then
    local label = meta and request_summary(meta) or tostring(evt.tag)
    clear_request_state(evt.id)
    if (meta and meta.tag == "stream_stop") or evt.tag == "stream_stop" then
      mark_stop_failed()
    elseif (meta and meta.guarded) or evt.guarded then
      mark_start_failed("请求失败: " .. label .. " / " .. tostring(evt.error or evt.state))
    else
      proto.status.set("请求失败: " .. label .. " / " .. tostring(evt.error or evt.state), { level = "error" })
    end
  end
end

local function is_start_config_control(id)
  return id == "address_offset"
    or id:match("^sym_addr_ch%d$")
    or id:match("^enable_ch%d$")
    or id:match("^gain_ch%d$")
end

function on_control(ctx, id, value)
  controls[id] = value
  if id == "auto_start" then
    scope_desired_running = true
    request_scope_start()
  elseif id == "start_stream" then
    scope_desired_running = true
    request_scope_start()
  elseif id == "stop_stream" then
    request_scope_stop()
  elseif id == "read_gain" then
    read_gain_registers()
  elseif id == "clear_plot" then
    upload_frame_cursor = 0
    upload_sequence_state = nil
    setup_plot(applied_start_config, true)
    proto.status.set("波形已清空", { level = "info" })
  elseif id == "history_limit" then
    setup_plot(applied_start_config, false)
    proto.status.set("波形历史上限已应用: " .. tostring(history_limit()), { level = "info" })
  elseif is_start_config_control(id) then
    start_config_dirty = true
    proto.status.set("启动配置已修改，将在下一次成功启动后生效", { level = "info" })
  end
end

function on_oscilloscope_toggle(ctx, current_running, target_running)
  -- 真实设备范式：工具栏只发起启停请求，等 FC06 ACK 匹配后再同步运行态。
  if target_running then
    scope_desired_running = true
    request_scope_start()
  else
    request_scope_stop()
  end
  return false
end
