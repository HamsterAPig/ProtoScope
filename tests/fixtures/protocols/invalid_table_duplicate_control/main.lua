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
        kind = "table",
        columns = 2,
        rows = {
          {
            { control = "device_id" },
            { control = "device_id" }
          },
          {
            { control = "read_version" }
          }
        }
      }
    }
  }
end
