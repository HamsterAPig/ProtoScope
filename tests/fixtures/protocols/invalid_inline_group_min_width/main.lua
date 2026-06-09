function ui()
  return {
    {
      id = "invalid_inline_group_min_width",
      title = "非法 inline_group min_width",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "inline_group",
        min_width = 0,
        controls = { "read_version" },
      },
    },
  }
end
