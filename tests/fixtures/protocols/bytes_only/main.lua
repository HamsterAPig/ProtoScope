function controls()
  return {}
end

function on_bytes(ctx, bytes)
  proto.emit("legacy_bytes", {
    size = #bytes,
    endpoint = ctx.endpoint,
  })
end
