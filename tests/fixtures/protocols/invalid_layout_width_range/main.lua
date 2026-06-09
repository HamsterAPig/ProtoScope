function ui()
  return {
    {
      id = "invalid_layout_width_range",
      title = "非法宽度范围",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "control",
        id = "read_version",
        min_width = 220,
        max_width = 120,
      },
    },
  }
end
