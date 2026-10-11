function ui()
  return {{ id = "bits", title = "Bits", controls = {
    { type = "button", id = "enable", label = "Enable" },
    { type = "button", id = "disable", label = "Disable" },
    { type = "button", id = "omit", label = "Omit" },
    { type = "button", id = "invalid", label = "Invalid" },
  }}}
end

function on_control(ctx, id, value)
  local channels = {{
    label = "renamed", unit = "changed", ratio = 99, scale = 88, offset = 77,
    color = "#FF0000", line_width = 4,
    bit_display = {
      enabled = id == "enable", first_bit = 6, bit_count = 3,
      y_offset = 9, hover_readout = true
    }
  }}
  if id == "omit" then
    -- 缺省字段沿用完整 Lua 规格的默认值；省略父通道 label 不得产生间接改名。
    channels[1] = {}
  elseif id == "invalid" then
    -- 后续通道非法时整份 setup 拒绝，第一通道不能先被局部更新。
    channels[2] = { bit_display = { first_bit = 63, bit_count = 2 } }
  elseif id == "enable" then
    channels[2] = {
      label = "second", unit = "flags", ratio = 5, scale = 6, offset = -7,
      color = "#00FF00", line_width = 3.5,
      bit_display = { first_bit = 11, bit_count = 2, y_offset = -3, hover_readout = false }
    }
    channels[3] = { label = "extra", bit_display = { enabled = true } }
  end
  -- 只放行共有通道的非名称显示规格，不能修改历史、时间轴、父名称或样本。
  proto.plot.setup({
    source = "lua-source", channels = channels, reset_history = true,
    time_scale = 9, time_unit = "changed", history_limit = 1,
    vertical_min = -99, vertical_max = 99
  })
  proto.plot.push(1, { samples = {{ t = 99, y = 999 }} })
  proto.log("info", "bit-toggle-" .. id)
end
