function ui()
  return {
    {
      id = "invalid_layout_shortcut_duplicate_control",
      title = "非法简写重复控件",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "column",
        controls = { "read_version", "read_version" },
      },
    },
  }
end
