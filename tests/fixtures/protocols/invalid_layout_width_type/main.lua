function ui()
  return {
    {
      id = "invalid_layout_width_type",
      title = "非法宽度类型",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "control",
        id = "read_version",
        min_width = "wide",
      },
    },
  }
end
