-- 第一版 Lua 示例脚本：当前 C++ 以内建模拟回调执行，保留此文件作为后续 sol2 接入契约。

function controls()
  return {
    { type = "button", id = "read_version", label = "读取版本" },
    { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
    { type = "checkbox", id = "hex_send", label = "HEX 发送", default = true },
    { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 }
  }
end

function on_open(ctx)
  proto.log("info", "连接已打开: " .. ctx.kind)
end

function on_close(ctx)
  proto.log("info", "连接已关闭")
end

function on_error(ctx, message)
  proto.log("error", "连接错误: " .. message)
end

function on_control(ctx, id, value)
  if id == "read_version" then
    local frame = build_read_version_frame()
    proto.send(frame)
    proto.set_timer("read_version_timeout", 1000)
  end
end

function on_bytes(ctx, bytes)
  local result = parse_frame(bytes)
  if result then
    proto.cancel_timer("read_version_timeout")
    proto.emit("frame", result)
  end
end

function on_timer(ctx, name)
  if name == "read_version_timeout" then
    proto.emit("warning", { message = "读取版本超时" })
  end
end
