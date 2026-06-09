function ui()
  return {
    {
      id = "ui_layouts",
      title = "UI 布局组合",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", label_position = "right", default = true },
        { type = "checkbox", id = "auto_send", label = "自动发送", label_position = "right", default = false },
        { type = "input_int", id = "sample_limit", label = "采样数", default = 128 },
        { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
        { type = "input_float", id = "scale", label = "缩放", default = 1.0 },
        { type = "input_int", id = "channel", label = "通道", default = 1 },
        { type = "input_float", id = "gain", label = "增益", default = 1.0 },
      },
      layout = {
        type = "column",
        children = {
          { type = "text", text = "这个模板演示常用 Layout Tree 节点。" },
          {
            type = "flow",
            children = {
              { type = "control", id = "device_id", min_width = 180, max_width = 260 },
              { type = "control", id = "mode" },
              { type = "control", id = "read_version", min_width = 120 },
            },
          },
          { type = "separator" },
          {
            type = "group",
            title = "发送选项",
            children = {
              {
                type = "flow",
                children = {
                  {
                    type = "inline_group",
                    spacing = 4,
                    min_width = 220,
                    controls = { "hex_send", "auto_send" },
                  },
                  { type = "control", id = "sample_limit" },
                },
              },
            },
          },
          {
            type = "collapse",
            title = "采样设置",
            default_open = false,
            children = {
              {
                type = "flow",
                controls = { "timeout_ms", "scale" },
              },
            },
          },
          {
            type = "table",
            columns = 2,
            borders = false,
            resizable = true,
            row_bg = false,
            sizing = "stretch",
            rows = {
              {
                { type = "control", id = "channel" },
                { type = "control", id = "gain" },
              },
            },
          },
        },
      },
    },
  }
end

function on_control(ctx, id, value)
  if id == "read_version" then
    -- 模板只演示 UI 布局；真实协议里这里通常会调用 proto.send/request。
    proto.emit("layout_demo_action", {
      device_id = proto.get_control("device_id"),
      mode = proto.get_control("mode"),
      endpoint = ctx.endpoint,
    })
  end
end
