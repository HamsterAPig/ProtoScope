proto.plot.setup({
  source = "raw_import_history_limit",
  reset_history = true,
  time_scale = 0.001,
  time_unit = "s",
  vertical_min = 0.0,
  vertical_max = 255.0,
  vertical_unit = "raw",
  history_limit = 3,
  channels = {
    { label = "RX", unit = "raw" },
  }
})

local next_t = 0.0

function on_bytes(ctx, bytes)
  proto.plot.setup({
    source = "raw_import_history_limit",
    reset_history = false,
    time_scale = 0.001,
    time_unit = "s",
    vertical_min = 0.0,
    vertical_max = 255.0,
    vertical_unit = "raw",
    history_limit = 3,
    channels = {
      { label = "RX", unit = "raw" },
    }
  })

  local samples = {}
  for _, value in ipairs(bytes) do
    samples[#samples + 1] = { t = next_t, y = value }
    next_t = next_t + 0.001
  end

  proto.plot.push(1, {
    source = "rx",
    samples = samples,
  })
end
