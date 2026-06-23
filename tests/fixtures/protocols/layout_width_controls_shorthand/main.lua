function ui()
  return {
    {
      id = "layout_width_controls_shorthand",
      title = "布局宽度与简写",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
        { type = "input_float", id = "scale", label = "缩放", default = 1.0 },
        { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
      },
      layout = {
        type = "column",
        children = {
          {
            type = "flow",
            controls = { "read_version", "device_id" },
          },
          {
            type = "column",
            controls = { "timeout_ms", "scale" },
          },
          {
            type = "control",
            id = "mode",
            min_width = 120,
            max_width = 240,
            fill_width = true,
          },
        },
      },
    },
  }
end
