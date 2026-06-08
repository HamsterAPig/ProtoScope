function ui()
  return {
    {
      id = "invalid_layout_width_non_positive",
      title = "非法非正宽度",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "control",
        id = "read_version",
        min_width = 0,
      },
    },
  }
end
