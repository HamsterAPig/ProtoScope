function ui()
  return {
    {
      id = "ui_dialogs",
      title = "UI 弹窗",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "input_text", id = "dialog_message", label = "消息", default = "确认发送当前命令？" },
        { type = "input_int", id = "window_width", label = "宽度", default = 520 },
        { type = "input_int", id = "window_height", label = "高度", default = 240 },
        { type = "button", id = "show_alert", label = "提示" },
        { type = "button", id = "confirm_send", label = "确认发送" },
        { type = "input_text", id = "last_dialog", label = "最近弹窗", default = "尚未打开" },
      },
      layout = {
        type = "column",
        children = {
          { type = "text", text = "alert 用于提示，confirm 用于二次确认；关闭结果在 on_dialog 里处理。" },
          { type = "control", id = "dialog_message", min_width = 320 },
          {
            type = "flow",
            controls = {
              "window_width",
              "window_height",
              "show_alert",
              "confirm_send",
            },
          },
          { type = "control", id = "last_dialog", min_width = 320 },
        },
      },
    },
  }
end

local function dialog_window()
  return {
    width = proto.get_control("window_width") or 520,
    height = proto.get_control("window_height") or 240,
    x = 120,
    y = 80,
    resizable = true,
    movable = true,
    auto_resize = false,
  }
end

function on_control(ctx, id, value)
  local message = proto.get_control("dialog_message") or "确认发送当前命令？"

  if id == "show_alert" then
    local dialog_id, err = proto.ui.alert({
      title = "脚本提示",
      message = message,
      level = "info",
      tag = "alert_demo",
      window = dialog_window(),
    })
    proto.set_control("last_dialog", dialog_id and ("提示已打开：" .. dialog_id) or ("提示失败：" .. tostring(err)))
  elseif id == "confirm_send" then
    local dialog_id, err = proto.ui.confirm({
      title = "确认发送",
      message = message,
      tag = "confirm_send",
      dedupe_key = "confirm_send",
      window = dialog_window(),
    })
    proto.set_control("last_dialog", dialog_id and ("确认框已打开：" .. dialog_id) or ("确认框失败：" .. tostring(err)))
  end
end

function on_dialog(ctx, evt)
  if evt.tag == "confirm_send" then
    if evt.result == "yes" then
      proto.set_control("last_dialog", "用户确认发送")
      proto.emit("dialog_confirmed", { dialog_id = evt.dialog_id, endpoint = ctx.endpoint })
    else
      proto.set_control("last_dialog", "用户取消发送")
    end
  elseif evt.tag == "alert_demo" then
    proto.set_control("last_dialog", "提示已关闭")
  end
end
