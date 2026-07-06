local timer_name = "tx_sequence:send_frames"
local cursor = 1

local function u16_be(value)
  value = math.floor(tonumber(value) or 0)
  return {
    (value >> 8) & 0xFF,
    value & 0xFF,
  }
end

local function build_fc06_request(addr, value)
  local frame = { 0x01, 0x06 }
  for _, byte in ipairs(u16_be(addr)) do frame[#frame + 1] = byte end
  for _, byte in ipairs(u16_be(value)) do frame[#frame + 1] = byte end
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
  proto.send(build_fc06_request(frame.fields.addr, frame.fields.value), {
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
            { id = "addr", label = "地址", type = "u16", default = 0x8888, radix = "hex" },
            { id = "value", label = "数值", type = "u16", default = 1, radix = "hex" },
          },
          default = {
            frames = {
              { enabled = true, name = "启动", fields = { addr = 0x8888, value = 1 } },
              { enabled = true, name = "停止", fields = { addr = 0x8888, value = 0 } },
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
