function ui()
  return {
    {
      id = "invalid_form_unknown_control",
      title = "非法 form",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        type = "column",
        children = {
          { type = "control", id = "missing_control" }
        }
      }
    }
  }
end
