function controls()
  return {
    { type = "button", id = "ping", label = "Ping" }
  }
end

function on_control(ctx, id, value)
  if id == "ping" and value then
    proto.emit("ping", { endpoint = ctx.endpoint })
  end
end
