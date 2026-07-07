local timer_name = "tx_sequence:send_frames"
local cursor = 1

local FRAME_HEAD = { 0x01 }
local FRAME_TAIL = {}

local function u16_be(value)
  value = math.floor(tonumber(value) or 0)
  return {
    (value >> 8) & 0xFF,
    value & 0xFF,
  }
end

local function append_bytes(out, bytes)
  for _, byte in ipairs(bytes or {}) do
    out[#out + 1] = byte & 0xFF
  end
end

local function append_crc16_modbus(out)
  local crc = proto.crc16_modbus(out)
  out[#out + 1] = crc & 0xFF
  out[#out + 1] = (crc >> 8) & 0xFF
end

local function build_frame(row)
  local fields = row.fields or {}
  local frame = {}
  append_bytes(frame, FRAME_HEAD)
  frame[#frame + 1] = (fields.func or 0x06) & 0xFF
  append_bytes(frame, u16_be(fields.addr))
  append_bytes(frame, u16_be(fields.value))
  append_bytes(frame, FRAME_TAIL)
  append_crc16_modbus(frame)
  return frame
end

local function enabled_frames(seq)
  local frames = {}
  for _, frame in ipairs(seq.frames or {}) do
    if frame.enabled then
      frames[#frames + 1] = frame
    end
  end
  return frames
end

local function send_one_frame()
  local seq = proto.get_control("send_frames")
  if not seq or not seq.running then
    return
  end

  local frames = enabled_frames(seq)
  if #frames == 0 then
    proto.log("warn", "没有启用的发送帧")
    return
  end
  if cursor > #frames then
    cursor = 1
  end

  local frame = frames[cursor]
  proto.send(build_frame(frame), {
    tag = frame.name or ("frame_" .. tostring(cursor)),
  })

  cursor = cursor + 1
  if cursor <= #frames or seq.loop then
    proto.set_timer(timer_name, math.max(1, seq.interval_ms or 100))
  end
end

function ui()
  return {
    {
      id = "tx_sequence",
      title = "发送序列",
      anchor = "left_bottom",
      controls = {
        {
          type = "tx_sequence",
          id = "send_frames",
          label = "发送帧序列",
          interval_ms = 100,
          loop = false,
          fields = {
            { id = "func", label = "功能码", type = "u8", default = 0x06, radix = "hex",
              options = {
                { label = "03 读保持寄存器", value = 0x03 },
                { label = "06 写单寄存器", value = 0x06 },
                { label = "10 写多个寄存器", value = 0x10 },
              },
            },
            { id = "addr", label = "地址", type = "u16", default = 0x8888, radix = "hex" },
            { id = "value", label = "数值", type = "u16", default = 1, radix = "hex" },
          },
          default = {
            frames = {
              { enabled = true, name = "启动", fields = { func = 0x06, addr = 0x8888, value = 1 } },
              { enabled = true, name = "停止", fields = { func = 0x06, addr = 0x8888, value = 0 } },
            },
          },
        },
      },
    },
  }
end

function on_control(ctx, id, value)
  if id ~= "send_frames" then
    return
  end

  if value.running then
    cursor = 1
    send_one_frame()
  else
    proto.cancel_timer(timer_name)
  end
end

function on_timer(ctx, name)
  if name == timer_name then
    send_one_frame()
  end
end
