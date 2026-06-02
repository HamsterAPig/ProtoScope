function on_open(ctx)
  proto.ui.alert({
    title = "窗口选项弹窗",
    message = ctx.endpoint,
    level = "info",
    dedupe_key = "dialog-window-open",
    window = {
      width = 520,
      height = 260,
      x = 120,
      y = 80,
      resizable = false,
      movable = false,
      auto_resize = false,
    }
  })
end
