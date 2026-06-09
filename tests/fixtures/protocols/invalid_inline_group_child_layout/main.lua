function ui()
  return {
    {
      id = "invalid_inline_group_child_layout",
      title = "非法 inline_group 子布局",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "inline_group",
        children = {
          {
            type = "flow",
            controls = { "read_version" },
          },
        },
      },
    },
  }
end
