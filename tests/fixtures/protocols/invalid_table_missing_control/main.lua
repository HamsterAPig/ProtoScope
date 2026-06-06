function ui()
  return {
    {
      id = "advanced",
      title = "高级参数",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "button", id = "read_version", label = "读取版本" }
      },
      layout = {
        type = "table",
        columns = 2,
        rows = {
          {
            { type = "control", id = "device_id" },
            { type = "spacer" }
          }
        }
      }
    }
  }
end
