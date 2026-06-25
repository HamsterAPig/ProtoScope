function ui()
  return {
    {
      id = "invalid_inline_group_fill_width_type",
      title = "非法 inline_group fill_width 类型",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "inline_group",
        fill_width = "yes",
        controls = { "read_version" },
      },
    },
  }
end
