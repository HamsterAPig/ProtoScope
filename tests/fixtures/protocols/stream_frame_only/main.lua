function controls()
  return {}
end

local function on_stream_frame(ctx, frame)
  proto.emit("stream_frame", {
    name = frame.name,
    value = frame.fields.value,
    raw_size = #frame.raw,
    crc_ok = frame.crc_ok,
    connection_id = ctx.connection_id,
  })
end

local function on_stream_error(ctx, err)
  proto.emit("stream_error", {
    code = err.code,
    message = err.message,
    dropped_bytes = err.dropped_bytes,
    connection_id = ctx.connection_id,
  })
end

function stream()
  return {
    buffer = {
      capacity = 8,
      overflow = "drop_oldest",
    },
    frames = {
      {
        name = "stream_sample",
        header = { 0xAA, 0x55 },
        len = { offset = 3, type = "u8", means = "payload", extra = 5 },
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "value", type = "u8", offset = 4 },
        },
        on_frame = on_stream_frame,
      },
    },
    on_error = on_stream_error,
  }
end

function on_bytes(ctx, bytes)
  proto.emit("legacy_bytes", {
    size = #bytes,
  })
end
