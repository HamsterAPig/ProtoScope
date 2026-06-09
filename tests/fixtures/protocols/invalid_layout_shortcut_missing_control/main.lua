function ui()
  return {
    {
      id = "invalid_layout_shortcut_missing_control",
      title = "非法简写遗漏控件",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
      },
      layout = {
        type = "flow",
        controls = { "read_version" },
      },
    },
  }
end
