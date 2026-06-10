function ui()
  return {
    id = "ui_layouts",
    title = "UI 布局组合",
    anchor = "left_bottom",
    tab_group = "protocol_tools",
    controls = {
      { "text", "device_id", "设备 ID", default = "01" },
      { "select", "mode", "模式", options = { "轮询", "单次" }, default = 1 },
      { "btn", "read_version", "读取版本" },
      { "check", "hex_send", "HEX 发送", label_position = "right", default = true },
      { "check", "auto_send", "自动发送", label_position = "right", default = false },
      { "int", "sample_limit", "采样数", default = 128 },
      { "int", "timeout_ms", "超时(ms)", default = 1000 },
      { "float", "scale", "缩放", default = 1.0 },
      { "int", "channel", "通道", default = 1 },
      { "float", "gain", "增益", default = 1.0 },
    },
    layout = {
      { text = "这个模板演示常用 Layout Tree 节点。" },
      {
        type = "flow",
        children = {
          { id = "device_id", min_width = 180, max_width = 260 },
          "mode",
          { id = "read_version", min_width = 120 },
        },
      },
      { separator = true },
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
              "sample_limit",
            },
          },
        },
      },
      {
        type = "collapse",
        title = "采样设置",
        default_open = false,
        children = {
          { "timeout_ms", "scale" },
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
            "channel",
            "gain",
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
