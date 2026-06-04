function controls()
  return {}
end

local function on_stream_frame(ctx, frame)
  proto.emit("stream_frame", {
    name = frame.name,
    value = frame.fields.value,
  })
end

local function on_stream_batch(ctx, frames)
  local values = {}
  for index, frame in ipairs(frames) do
    values[index] = frame.fields.value
  end
  proto.emit("stream_batch", {
    count = #frames,
    first = values[1] or 0,
    second = values[2] or 0,
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
        on_frame = on_stream_frame,
      },
    },
    on_batch = on_stream_batch,
  }
end
