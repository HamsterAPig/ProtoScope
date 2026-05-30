function controls()
  return {
    { type = "button", id = "detached_dialog", label = "Detached Dialog" },
    { type = "button", id = "send_one", label = "Send One" },
  }
end

function on_open(ctx)
  proto.ui.alert({
    title = "连接弹窗",
    message = ctx.endpoint,
    level = "warn",
    dedupe_key = "dialog-open",
  })
end

function on_control(ctx, id, value)
  if id == "detached_dialog" and value then
    proto.ui.confirm({
      title = "离线确认",
      message = "detached path",
      level = "info",
      dedupe_key = "dialog-detached",
    })
  elseif id == "send_one" and value then
    proto.send({ 0x01 }, { tag = "overflow-test" })
  end
end
