function ui()
  return {
    {
      id = "advanced",
      title = "高级参数",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", default = true },
        { type = "button", id = "read_version", label = "读取版本" }
      },
      layout = {
        kind = "table",
        columns = 2,
        borders = false,
        resizable = true,
        row_bg = false,
        sizing = "stretch",
        rows = {
          {
            { control = "device_id" },
            { control = "mode" }
          },
          {
            { control = "hex_send" },
            { spacer = true }
          },
          {
            { control = "read_version" },
            { spacer = true }
          }
        }
      }
    }
  }
end
