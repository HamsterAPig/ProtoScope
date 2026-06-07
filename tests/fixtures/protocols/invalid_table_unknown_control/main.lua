function ui()
  return {
    {
      id = "advanced",
      title = "高级参数",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" }
      },
      layout = {
        type = "table",
        columns = 1,
        rows = {
          {
            { type = "control", id = "missing_control" }
          }
        }
      }
    }
  }
end
