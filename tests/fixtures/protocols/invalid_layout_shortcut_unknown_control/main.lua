function ui()
  return {
    {
      id = "invalid_layout_shortcut_unknown_control",
      title = "非法简写未知控件",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "flow",
        controls = { "missing_control" },
      },
    },
  }
end
