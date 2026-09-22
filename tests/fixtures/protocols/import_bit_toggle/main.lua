function ui()
  return {{ id = "bits", title = "Bits", controls = {
    { type = "button", id = "enable", label = "Enable" },
    { type = "button", id = "disable", label = "Disable" },
    { type = "button", id = "omit", label = "Omit" },
  }}}
end

function on_control(ctx, id, value)
  local channels = {{
    label = "renamed", unit = "changed", ratio = 99, scale = 88, offset = 77,
    bit_display = id ~= "omit" and {
      enabled = id == "enable", first_bit = 0, bit_count = 1,
      y_offset = 99, hover_readout = true
    } or nil
  }}
  if id == "enable" then
    channels[2] = { label = "second", bit_display = { enabled = true } }
    channels[3] = { label = "extra", bit_display = { enabled = true } }
  end
  -- 尝试修改历史、时间轴、名称和样本，导入保护只应放行共有通道的 Bit 开关。
  proto.plot.setup({
    channels = channels, reset_history = true,
    time_scale = 9, time_unit = "changed", history_limit = 1,
    vertical_min = -99, vertical_max = 99
  })
  proto.plot.push(1, { samples = {{ t = 99, y = 999 }} })
  proto.log("info", "bit-toggle-" .. id)
end
