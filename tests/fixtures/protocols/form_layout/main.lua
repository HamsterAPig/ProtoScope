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
        type = "column",
        children = {
          { type = "text", text = "修改参数后立即生效。" },
          {
            type = "flow",
            children = {
              { type = "control", id = "read_version" },
              { type = "control", id = "device_id" },
            }
          },
          { type = "separator" },
          {
            type = "group",
            title = "发送参数",
            children = {
              {
                type = "flow",
                children = {
                  { type = "control", id = "hex_send" },
                  { type = "control", id = "mode" },
                }
              },
            }
          },
          {
            type = "collapse",
            title = "采样设置",
            default_open = false,
            children = {
              {
                type = "flow",
                children = {
                  { type = "control", id = "timeout_ms" },
                  { type = "control", id = "scale" },
                }
              },
            }
          }
        }
      }
    }
  }
end
