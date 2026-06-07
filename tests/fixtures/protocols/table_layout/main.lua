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
        type = "table",
        columns = 2,
        borders = false,
        resizable = true,
        row_bg = false,
        sizing = "stretch",
        rows = {
          {
            { type = "control", id = "device_id" },
            { type = "control", id = "mode" }
          },
          {
            { type = "control", id = "hex_send" },
            { type = "spacer" }
          },
          {
            { type = "control", id = "read_version" },
            { type = "spacer" }
          }
        }
      }
    }
  }
end
