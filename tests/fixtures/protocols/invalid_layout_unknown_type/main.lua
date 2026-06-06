function ui()
  return {
    {
      id = "invalid_layout_unknown_type",
      title = "非法布局类型",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "grid",
        children = {
          { type = "control", id = "read_version" },
        }
      }
    }
  }
end
