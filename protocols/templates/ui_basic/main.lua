local click_count = 0

local function bool_text(value)
  if value then
    return "true"
  end
  return "false"
end

function ui()
  return {
    id = "ui_basic",
    title = "UI 快速入门",
    anchor = "left_bottom",
    tab_group = "protocol_tools",
    controls = {
      { "text", "device_id", "设备 ID", default = "01" },
      { "check", "hex_send", "HEX 发送", label_position = "right", default = true },
      { "btn", "send_once", "模拟发送" },
      { "btn", "show_state", "查看状态" },
      { "text", "last_action", "最近动作", default = "待操作" },
    },
    layout = {
      { text = "修改控件后点击按钮，观察 on_control 如何读写控件状态。" },
      { "device_id", "hex_send", "send_once", "show_state" },
      { id = "last_action", min_width = 280 },
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
