function ui()
  return {
    {
      id = "invalid_form_missing_control",
      title = "非法 form",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
      },
      layout = {
        kind = "form",
        items = {
          { control = "read_version" }
        }
      }
    }
  }
end
