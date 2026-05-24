function controls()
  return {
    { type = "button", id = "explode", label = "Explode" }
  }
end

function on_control(ctx, id, value)
  error("boom from lua")
end

function on_bytes(ctx, bytes)
  proto.emit("after_error", { size = #bytes })
end
