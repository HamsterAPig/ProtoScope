function ui()
  return {
    {
      id = "scope",
      title = "示波器",
      controls = {
        { type = "button", id = "emit_scope", label = "推送波形" }
      }
    }
  }
end

local function send_scope(reset_history)
  proto.plot.setup({
    source = "plot_stream",
    reset_history = reset_history,
    time_scale = 0.001,
    time_unit = "s",
    vertical_min = -2.0,
    vertical_max = 2.0,
    vertical_unit = "V",
    history_limit = 2048,
    channels = {
      { label = "CH1", unit = "V" },
      { label = "CH2", unit = "V" }
    }
  })

  proto.plot.push(1, {
    samples = {
      { t = 0.000, y = 0.0 },
      { t = 0.001, y = 0.8 },
      { t = 0.002, y = -0.2 }
    }
  })

  proto.plot.push(2, {
    samples = {
      { t = 0.000, y = -0.3 },
      { t = 0.001, y = 0.1 },
      { t = 0.002, y = 1.1 }
    }
  })
end

function on_open(ctx)
  send_scope(true)
end

function on_control(ctx, id, value)
  if id == "emit_scope" then
    send_scope(false)
  end
end
