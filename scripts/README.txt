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

## 纯 Lua 波形演示
`protocols/lua_waveform_demo/main.lua` 是一个不依赖串口输入的示波器演示协议。它加载后会自动启动定时器，由 Lua 自己按固定时间步进生成采样点，并通过 `proto.plot.setup(...)` 与 `proto.plot.push(channelIndex, ...)` 推送到波形面板。

入口方式：
- 在“协议脚本 / 动态控件”面板点击“重新扫描协议目录”
- 在“扫描结果”或“协议目录”中选择 `protocols/lua_waveform_demo`
- 点击“重新加载协议”

控件说明：
- 运行控制：开始、暂停、恢复、清空历史
- 参数控制：频率、幅值、偏置、相位、采样率、每次点数、刷新间隔
- 通道控制：正弦、三角、方波、锯齿独立显示开关

该 Demo 适合协议脚本作者参考：当数据来自脚本计算、仿真模型或文件回放时，也可以复用同样的 `ui()`、`on_timer()` 和 `proto.plot.*` 写法完成自定义可视化。
