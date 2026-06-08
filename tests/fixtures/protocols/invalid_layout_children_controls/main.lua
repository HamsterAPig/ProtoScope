function ui()
  return {
    {
      id = "invalid_layout_children_controls",
      title = "非法 children controls 混用",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "column",
        children = {
          { type = "control", id = "read_version" },
        },
        controls = { "read_version" },
      },
    },
  }
end
