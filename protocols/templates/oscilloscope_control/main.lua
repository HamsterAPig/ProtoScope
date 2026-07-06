-- 核心流程：用 Lua Dock 控制示波器运行态、历史上限和清空历史。

local timer_name = "scope_template_tick"
local running = false
local time_cursor = 0.0

local defaults = {
  history_limit = 5000,
  sample_rate_hz = 100,
  points_per_tick = 5,
  timer_ms = 50,
}

local function read_number(id, fallback)
  local value = proto.get_control(id)
  if type(value) ~= "number" then
    return fallback
  end
  return value
end

local function read_non_negative_int(id, fallback)
  local value = math.floor(tonumber(proto.get_control(id)) or fallback or 0)
  if value < 0 then
    return fallback
  end
  return value
end

local function history_limit()
  return read_non_negative_int("history_limit", defaults.history_limit)
end

local function setup_plot(reset_history)
  proto.plot.setup({
    source = "oscilloscope_control_template",
    reset_history = reset_history,
    time_scale = 1.0,
    time_unit = "s",
    vertical_min = -1.5,
    vertical_max = 1.5,
    vertical_unit = "V",
    history_limit = history_limit(),
    channels = {
      { label = "模板波形", unit = "V", color = "#4FC3F7" },
    },
  })
end

local function update_state_text()
  proto.set_control("scope_state", running and "运行中" or "已暂停")
end

local function schedule_tick()
  local timer_ms = math.floor(read_number("timer_ms", defaults.timer_ms))
  if timer_ms < 10 then
    timer_ms = 10
  end
  proto.set_timer(timer_name, timer_ms)
end

local function set_running_state(next_running)
  running = next_running == true
  if running then
    schedule_tick()
  else
    proto.cancel_timer(timer_name)
  end
  update_state_text()
  proto.oscilloscope.set_running(running)
end

local function push_samples()
  local sample_rate_hz = read_number("sample_rate_hz", defaults.sample_rate_hz)
  if sample_rate_hz <= 0 then
    sample_rate_hz = defaults.sample_rate_hz
  end

  local points_per_tick = read_non_negative_int("points_per_tick", defaults.points_per_tick)
  if points_per_tick < 1 then
    points_per_tick = 1
  end

  local dt = 1.0 / sample_rate_hz
  local samples = {}
  for _ = 1, points_per_tick do
    samples[#samples + 1] = {
      t = time_cursor,
      y = math.sin(time_cursor * 2.0 * math.pi),
    }
    time_cursor = time_cursor + dt
  end

  proto.plot.push(1, {
    source = "oscilloscope_control_template",
    samples = samples,
  })
end

function ui()
  return {
    id = "oscilloscope_control",
    title = "示波器控制模板",
    anchor = "left_bottom",
    tab_group = "protocol_tools",
    controls = {
      { "btn", "start", "启动" },
      { "btn", "pause", "暂停" },
      { "btn", "clear_history", "清空历史" },
      { "int", "history_limit", "历史上限", default = defaults.history_limit },
      { "float", "sample_rate_hz", "采样率(Hz)", default = defaults.sample_rate_hz },
      { "int", "points_per_tick", "每次点数", default = defaults.points_per_tick },
      { "int", "timer_ms", "刷新间隔(ms)", default = defaults.timer_ms },
      { "text", "scope_state", "运行状态", default = "已暂停" },
    },
    layout = {
      { text = "工具栏播放/暂停会调用 on_oscilloscope_toggle；历史上限通过 plot.setup 重新应用。" },
      { "start", "pause", "clear_history" },
      { "history_limit", "sample_rate_hz" },
      { "points_per_tick", "timer_ms" },
      { id = "scope_state", min_width = 220 },
    },
  }
end

function on_control(ctx, id, value)
  if id == "start" then
    set_running_state(true)
  elseif id == "pause" then
    set_running_state(false)
  elseif id == "clear_history" then
    time_cursor = 0.0
    setup_plot(true)
    proto.status.set("模板波形历史已清空", { level = "info" })
  elseif id == "history_limit" then
    -- 0 表示不裁剪；正数会立即裁剪现有历史。
    setup_plot(false)
    proto.status.set("模板波形历史上限已应用: " .. tostring(history_limit()), { level = "info" })
  elseif id == "sample_rate_hz" or id == "points_per_tick" or id == "timer_ms" then
    setup_plot(false)
  end
end

function on_oscilloscope_toggle(ctx, current_running, target_running)
  set_running_state(target_running)
  return true
end

function on_timer(ctx, name)
  if name ~= timer_name then
    return
  end
  if running then
    push_samples()
    schedule_tick()
  end
end

setup_plot(true)
proto.oscilloscope.set_running(false)
