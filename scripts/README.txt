# ProtoScope Lua Host Guide

## 生命周期
- `on_open(ctx)`：连接打开后触发
- `on_close(ctx)`：连接关闭后触发
- `on_error(ctx, message)`：通讯层错误
- `on_bytes(ctx, bytes)`：收到原始字节
- `on_timer(ctx, name)`：定时器触发
- `on_control(ctx, id, value)`：宿主控件变化或按钮触发

## UI 描述
优先使用 `ui()` 返回多个 Dock：

```lua
function ui()
  return {
    {
      id = "protocol",
      title = "协议动作",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
      }
    }
  }
end
```

兼容旧脚本时也可保留 `controls()`。

## proto API
- `proto.log(level, message)`
- `proto.send(hexStringOrByteArray)`
- `proto.emit(name, payload)`
- `proto.set_timer(name, delayMs)`
- `proto.cancel_timer(name)`
- `proto.get_control(id)`
- `proto.set_control(id, value)`
- `proto.crc16_modbus(payload)`
- `proto.crc16_ccitt_false(payload)`
- `proto.crc32_ieee(payload)`
