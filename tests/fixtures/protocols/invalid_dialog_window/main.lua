function on_open(ctx)
  proto.ui.alert({
    title = "非法窗口参数",
    message = ctx.endpoint,
    window = {
      width = 0,
    }
  })
end
