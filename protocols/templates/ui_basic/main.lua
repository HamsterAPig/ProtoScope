local click_count = 0

local function bool_text(value)
  if value then
    return "true"
  end
  return "false"
end

function ui()
  return {
    {
      id = "ui_basic",
      title = "UI 快速入门",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", label_position = "right", default = true },
        { type = "button", id = "send_once", label = "模拟发送" },
        { type = "button", id = "show_state", label = "查看状态" },
        { type = "input_text", id = "last_action", label = "最近动作", default = "待操作" },
      },
      layout = {
        type = "column",
        children = {
          { type = "text", text = "修改控件后点击按钮，观察 on_control 如何读写控件状态。" },
          {
            type = "flow",
            children = {
              { type = "control", id = "device_id", min_width = 160, max_width = 260 },
              { type = "control", id = "hex_send" },
              { type = "control", id = "send_once" },
              { type = "control", id = "show_state" },
            },
          },
          { type = "control", id = "last_action", min_width = 280 },
        },
      },
    },
  }
end

function on_control(ctx, id, value)
  if id == "send_once" then
    -- 按钮点击后读取其他控件，再把处理结果写回 UI。
    click_count = click_count + 1
    local device_id = proto.get_control("device_id") or "01"
    local hex_send = proto.get_control("hex_send") == true
    local summary = string.format("第 %d 次：设备 %s，HEX=%s", click_count, device_id, bool_text(hex_send))

    proto.set_control("last_action", summary)
    proto.ui.alert({
      title = "UI 回调",
      message = summary,
      level = "info",
    })
  elseif id == "show_state" then
    local device_id = proto.get_control("device_id") or "01"
    local last_action = proto.get_control("last_action") or "暂无"

    proto.ui.alert({
      title = "当前控件状态",
      message = string.format("设备 ID：%s\n最近动作：%s", device_id, last_action),
      level = "info",
    })
  end
end
