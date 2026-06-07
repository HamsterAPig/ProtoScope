function ui()
  return {
    {
      id = "flow_demo",
      title = "Flow 布局",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", label_position = "right", default = true },
        { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
        { type = "input_float", id = "scale", label = "缩放", default = 1.0 },
        { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
        { type = "checkbox", id = "auto_send", label = "自动发送", default = false },
      },
      layout = {
        type = "column",
        children = {
          {
            type = "flow",
            spacing = 6,
            run_spacing = 5,
            children = {
              { type = "control", id = "read_version" },
              { type = "control", id = "device_id" },
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
              }
            }
          },
          {
            type = "table",
            columns = 2,
            rows = {
              {
                { type = "control", id = "mode" },
                {
                  type = "flow",
                  children = {
                    { type = "control", id = "hex_send" },
                    { type = "control", id = "auto_send" },
                  }
                },
              }
            }
          },
        }
      }
    }
  }
end
