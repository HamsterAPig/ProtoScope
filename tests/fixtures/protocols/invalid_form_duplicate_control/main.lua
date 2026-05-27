function ui()
  return {
    {
      id = "invalid_form_duplicate_control",
      title = "非法 form",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
      },
      layout = {
        kind = "form",
        items = {
          { control = "read_version" },
          { control = "read_version" }
        }
      }
    }
  }
end
