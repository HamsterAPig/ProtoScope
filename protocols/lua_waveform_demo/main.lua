-- 核心流程：本脚本不依赖串口输入，Lua 定时生成采样点并推送到波形面板。

local timer_name = "lua_waveform_tick"
local running = true
local time_cursor = 0.0
local plot_ready = false

local defaults = {
  frequency_hz = 1.0,
  amplitude = 1.0,
  offset = 0.0,
  phase_deg = 0.0,
  sample_rate_hz = 200,
  points_per_tick = 8,
  timer_ms = 40,
  show_sine = true,
  show_triangle = true,
  show_square = true,
  show_saw = false
}

local function read_number(id, fallback)
  local value = proto.get_control(id)
  if type(value) ~= "number" then
    return fallback
  end
  return value
end

local function read_bool(id, fallback)
  local value = proto.get_control(id)
  if type(value) ~= "boolean" then
    return fallback
  end
  return value
end

local function read_positive_number(id, fallback, minimum)
  local value = read_number(id, fallback)
  if value < minimum then
    return minimum
  end
  return value
end

local function phase_ratio()
  return read_number("phase_deg", defaults.phase_deg) / 360.0
end

local function wrap01(value)
  return value - math.floor(value)
end

local function sine_wave(cycle)
  return math.sin(cycle * 2.0 * math.pi)
end

local function triangle_wave(cycle)
  local position = wrap01(cycle)
  return 1.0 - 4.0 * math.abs(position - 0.5)
end

local function square_wave(cycle)
  return wrap01(cycle) < 0.5 and 1.0 or -1.0
end

local function saw_wave(cycle)
  return wrap01(cycle) * 2.0 - 1.0
end

local function scaled(value)
  local amplitude = read_number("amplitude", defaults.amplitude)
  local offset = read_number("offset", defaults.offset)
  return offset + amplitude * value
end

local function channel_enabled(channel_id)
  return read_bool(channel_id, defaults[channel_id])
end

local function setup_plot(reset_history)
  local vertical_range = math.abs(read_number("amplitude", defaults.amplitude)) +
  math.abs(read_number("offset", defaults.offset)) + 0.5
  proto.plot.setup({
    source = "lua_waveform_demo",
    reset_history = reset_history,
    time_scale = 1.0,
    time_unit = "s",
    vertical_min = -vertical_range,
    vertical_max = vertical_range,
    vertical_unit = "V",
    history_limit = 12000,
    channels = {
      { label = "正弦", unit = "V", offset = 0.0 },
      { label = "三角", unit = "V", offset = 0.0 },
      { label = "方波", unit = "V", offset = 0.0 },
      { label = "锯齿", unit = "V", offset = 0.0 }
    }
  })
  plot_ready = true
end

local function push_channel(channel_index, samples)
  if #samples > 0 then
    proto.plot.push(channel_index, {
      source = "lua_waveform_demo",
      samples = samples
    })
  end
end

local function emit_samples()
  if not plot_ready then
    setup_plot(true)
  end

  local frequency = read_positive_number("frequency_hz", defaults.frequency_hz, 0.01)
  local sample_rate = read_positive_number("sample_rate_hz", defaults.sample_rate_hz, 1.0)
  local points_per_tick = math.floor(read_positive_number("points_per_tick", defaults.points_per_tick, 1.0))
  local time_step = 1.0 / sample_rate
  local phase = phase_ratio()
  local sine_samples = {}
  local triangle_samples = {}
  local square_samples = {}
  local saw_samples = {}

  -- 核心流程：固定步长推进逻辑时间，避免串口状态或系统刷新抖动影响演示曲线连续性。
  for _ = 1, points_per_tick do
    local time_value = time_cursor
    local cycle = time_value * frequency + phase
    if channel_enabled("show_sine") then
      sine_samples[#sine_samples + 1] = { t = time_value, y = scaled(sine_wave(cycle)) }
    end
    if channel_enabled("show_triangle") then
      triangle_samples[#triangle_samples + 1] = { t = time_value, y = scaled(triangle_wave(cycle)) }
    end
    if channel_enabled("show_square") then
      square_samples[#square_samples + 1] = { t = time_value, y = scaled(square_wave(cycle)) }
    end
    if channel_enabled("show_saw") then
      saw_samples[#saw_samples + 1] = { t = time_value, y = scaled(saw_wave(cycle)) }
    end
    time_cursor = time_cursor + time_step
  end

  push_channel(1, sine_samples)
  push_channel(2, triangle_samples)
  push_channel(3, square_samples)
  push_channel(4, saw_samples)
end

local function schedule_next_tick()
  proto.set_timer(timer_name, math.floor(read_positive_number("timer_ms", defaults.timer_ms, 10.0)))
end

local function restart(reset_history)
  setup_plot(reset_history)
  schedule_next_tick()
end

function ui()
  return {
    {
      id = "wave_run",
      title = "Lua 波形演示",
      anchor = "left_bottom",
      tab_group = "wave_tools",
      controls = {
        { type = "button", id = "start", label = "启动演示" },
        { type = "button", id = "pause", label = "暂停演示" },
        { type = "button", id = "resume", label = "继续演示" },
        { type = "button", id = "clear_history", label = "清空历史" },
        { type = "input_float", id = "frequency_hz", label = "频率(Hz)", default = defaults.frequency_hz },
        { type = "input_float", id = "amplitude", label = "幅值", default = defaults.amplitude },
        { type = "input_float", id = "offset", label = "偏置", default = defaults.offset },
        { type = "input_float", id = "phase_deg", label = "相位(度)", default = defaults.phase_deg },
        { type = "input_float", id = "sample_rate_hz", label = "采样率(Hz)", default = defaults.sample_rate_hz },
        { type = "input_int", id = "points_per_tick", label = "每次点数", default = defaults.points_per_tick },
        { type = "input_int", id = "timer_ms", label = "刷新间隔(ms)", default = defaults.timer_ms },
        { type = "checkbox", id = "show_sine", label = "显示正弦", default = defaults.show_sine },
        { type = "checkbox", id = "show_triangle", label = "显示三角", default = defaults.show_triangle },
        { type = "checkbox", id = "show_square", label = "显示方波", default = defaults.show_square },
        { type = "checkbox", id = "show_saw", label = "显示锯齿", default = defaults.show_saw },
      },
      layout = {
        type = "column",
        children = {
          {
            type = "collapse",
            title = "演示控制",
            default_open = true,
            children = {
              {
                type = "flow",
                children = {
                  { type = "control", id = "start" },
                  { type = "control", id = "pause" },
                  { type = "control", id = "resume" },
                  { type = "control", id = "clear_history" },
                }
              }
            }
          },
          {
            type = "collapse",
            title = "运行选项",
            default_open = true,
            children = {
              { type = "control", id = "frequency_hz" },
              { type = "control", id = "amplitude" },
              { type = "control", id = "offset" },
              { type = "control", id = "phase_deg" },
              { type = "control", id = "sample_rate_hz" },
              { type = "control", id = "points_per_tick" },
              { type = "control", id = "timer_ms" },
            }
          },
          {
            type = "collapse",
            title = "波形显示",
            default_open = true,
            children = {
              {
                type = "flow",
                children = {
                  { type = "control", id = "show_sine" },
                  { type = "control", id = "show_triangle" },
                  { type = "control", id = "show_square" },
                  { type = "control", id = "show_saw" },
                }
              }
            }
          }
        }
      }
    }
  }
end

function on_control(ctx, id, value)
  if id == "start" then
    running = true
    time_cursor = 0.0
    restart(true)
  elseif id == "pause" then
    running = false
  elseif id == "resume" then
    running = true
    schedule_next_tick()
  elseif id == "clear_history" then
    time_cursor = 0.0
    restart(true)
  elseif id == "frequency_hz" or id == "amplitude" or id == "offset" or id == "phase_deg"
      or id == "sample_rate_hz" or id == "points_per_tick" or id == "timer_ms"
      or id == "show_sine" or id == "show_triangle" or id == "show_square" or id == "show_saw" then
    setup_plot(true)
  end
end

function on_timer(ctx, name)
  if name ~= timer_name then
    return
  end
  if running then
    emit_samples()
    schedule_next_tick()
  end
end

-- 加载后自动启动，让用户无需串口连接即可立即看到波形。
restart(true)
