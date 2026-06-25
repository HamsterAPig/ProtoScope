function ui()
  return {
    {
      id = "invalid_layout_fill_width_type",
      title = "非法 fill_width 类型",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "control",
        id = "read_version",
        fill_width = "yes",
      },
    },
  }
end
