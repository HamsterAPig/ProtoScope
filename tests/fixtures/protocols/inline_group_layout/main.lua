function ui()
  return {
    {
      id = "inline_group_layout",
      title = "Inline Group 布局",
      controls = {
        { type = "button", id = "read_version", label = "读取版本", short_label = "读", compact_label_below = 80 },
        { type = "input_text", id = "device_id", label = "设备 ID", short_label = "ID", compact_label_below = 180, default = "01" },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", short_label = "HEX", compact_label_below = 90, default = true },
        { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
      },
      layout = {
        type = "flow",
        spacing = 4,
        run_spacing = 7,
        children = {
          {
            type = "inline_group",
            spacing = 3,
            min_width = 200,
            fill_width = true,
            controls = { "read_version", "device_id" },
          },
          {
            type = "inline_group",
            spacing = 2,
            children = {
              { type = "text", text = "发送选项" },
              { type = "control", id = "hex_send" },
            },
          },
          { type = "control", id = "timeout_ms" },
        },
      },
    },
  }
end
