function ui()
  return {
    {
      id = "invalid_inline_group_children_controls",
      title = "非法 inline_group children controls 混用",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "inline_group",
        controls = { "read_version" },
        children = {
          { type = "control", id = "read_version" },
        },
      },
    },
  }
end
