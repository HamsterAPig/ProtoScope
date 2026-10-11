-- 原始文件不携带 waveform，首次解析必须能够合法建立名称和样本。
function on_bytes(ctx, bytes)
  -- 短前缀故意没有 setup，仅高索引追加会隐式建立位置 0。
  if #bytes == 1 and bytes[1] == 77 then
    proto.plot.push(2, { samples = {{ t = 1, y = 77 }} })
    return
  end
  proto.plot.setup({ channels = {
    { label = "raw-original" }, { label = "raw-second" }
  }, reset_history = true })
  for channel = 1, 2 do
    local samples = {}
    for i = 1, #bytes do
      samples[i] = { t = i, y = bytes[i] + (channel - 1) * 100 }
    end
    proto.plot.push(channel, { samples = samples })
  end
end

function ui()
  return {{ id = "source-label", title = "Source label", controls = {
    { type = "button", id = "rename", label = "Rename" }
  }}}
end

function on_control(ctx, id, value)
  if id ~= "rename" then return end
  -- 同一真实配置在导入时必须被保护，在明确切回实时后必须正常生效。
  proto.plot.setup({ channels = {
    { label = "lua-renamed" }, { label = "lua-second" }
  }, reset_history = true })
  proto.plot.push(1, { samples = {{ t = 99, y = 999 }} })
  proto.log("info", "source-label-rename-consumed")
end
