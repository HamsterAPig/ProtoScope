function ui()
  return {
    {
      id = "advanced",
      title = "高级参数",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" }
      },
      layout = {
        kind = "table",
        columns = 1,
        rows = {
          {
            { control = "missing_control" }
          }
        }
      }
    }
  }
end
