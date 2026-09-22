-- 离线解析必须恢复 RX 波形，但回调中的发送和请求不得到达设备。
function on_bytes(ctx, bytes)
  local samples = {}
  for i = 1, #bytes do
    samples[#samples + 1] = { t = i, y = bytes[i] }
  end
  proto.plot.push(1, { samples = samples })
  proto.send({ 0xAA })
  proto.request({ 0xBB }, { timeout_ms = 10 })
end

function ui()
  return { { id = "tx", title = "TX", controls = {
    { type = "button", id = "send", label = "Send" },
    { type = "button", id = "retry", label = "Retry" },
    { type = "button", id = "timeout", label = "Timeout" },
  } } }
end

function on_control(ctx, id, value)
  if id == "send" then
    proto.send({ 0x21, 0x22 })
  elseif id == "retry" then
    proto.request_guarded({ 0x31 }, { timeout_ms = 10, max_attempts = 2, tag = "retry" })
  elseif id == "timeout" then
    proto.send({ 0x41 }, { timeout_ms = 1 })
  end
end

function on_tx(ctx, event)
  if event.state == "sent" and event.tag == "retry" and event.attempt == 2 then
    proto.request_done({ ok = true })
  end
end
