local frame_cursor = 0

local function setup_plot(reset_history)
  proto.plot.setup({
    source = "raw_import_chunked_stream",
    reset_history = reset_history,
    time_scale = 0.001,
    time_unit = "s",
    vertical_min = 0,
    vertical_max = 255,
    vertical_unit = "raw",
    history_limit = 64,
    channels = {
      { label = "RX", unit = "raw" },
    },
  })
end

local function on_stream_frame(ctx, frame)
  if frame_cursor == 0 then
    setup_plot(true)
  end
  local fields = frame.fields or {}
  proto.plot.push(1, {
    source = "raw_import_chunked_stream",
    samples = {
      {
        t = frame_cursor * 0.001,
        y = fields.value or 0,
      },
    },
  })
  frame_cursor = frame_cursor + 1
end

function stream()
  return {
    buffer = {
      capacity = 2048,
      overflow = "drop_oldest",
    },
    frames = {
      {
        name = "sample",
        header = { 0xAA, 0x55 },
        len = { offset = 3, type = "u8", means = "payload", extra = 5 },
        crc = { type = "crc16_modbus", order = "lo_hi" },
        fields = {
          { name = "value", type = "u8", offset = 4 },
        },
        on_frame = on_stream_frame,
      },
    },
  }
end

function on_open(ctx)
  frame_cursor = 0
  setup_plot(true)
end
