function ui()
  return {
    {
      id = "invalid_label_position",
      title = "非法标签位置",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", label_position = "top", default = "01" },
      }
    }
  }
end
