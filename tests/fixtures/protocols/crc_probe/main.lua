function controls()
  return {
    { type = "button", id = "probe_crc", label = "CRC 探针" }
  }
end

function on_control(ctx, id, value)
  if id ~= "probe_crc" then
    return
  end

  local bytes = { string.byte("1"), string.byte("2"), string.byte("3"), string.byte("4"), string.byte("5"), string.byte("6"), string.byte("7"), string.byte("8"), string.byte("9") }
  proto.emit("crc", {
    modbus = proto.crc16_modbus(bytes),
    ccitt = proto.crc16_ccitt_false(bytes),
    ieee = proto.crc32_ieee(bytes),
  })
end
