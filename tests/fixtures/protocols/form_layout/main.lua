function ui()
  return {
    {
      id = "form_demo",
      title = "表单布局",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", default = true },
        { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
        { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
        { type = "input_float", id = "scale", label = "缩放", default = 1.0 },
      },
      layout = {
        kind = "form",
        items = {
          { text = "修改参数后立即生效。" },
          { controls = { "read_version", "device_id" } },
          { separator = true },
          {
            group = "发送参数",
            items = {
              { controls = { "hex_send", "mode" } },
            }
          },
          {
            collapse = "采样设置",
            default_open = false,
            items = {
              { controls = { "timeout_ms", "scale" } },
            }
          }
        }
      }
    }
  }
end
