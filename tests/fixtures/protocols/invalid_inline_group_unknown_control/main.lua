function ui()
  return {
    {
      id = "invalid_inline_group_unknown_control",
      title = "非法 inline_group 未知控件",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "inline_group",
        controls = { "missing_control" },
      },
    },
  }
end
