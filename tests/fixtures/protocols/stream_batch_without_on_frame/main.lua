function controls()
  return {}
end

local function on_stream_batch(ctx, frames)
  local total = 0
  for _, frame in ipairs(frames) do
    total = total + (frame.fields.value or 0)
  end
  proto.emit("stream_batch_only", {
    count = #frames,
    total = total,
    connection_id = ctx.connection_id,
  })
end

function stream()
  return {
    buffer = {
      capacity = 16,
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
      },
    },
    on_batch = on_stream_batch,
  }
end
