# ProtoScope Lua 协议脚本指南

`protocols` 是 ProtoScope 的 Lua 协议工作区。每个协议目录至少包含一个 `main.lua`：

```text
protocols/
├── default_protocol/
│   └── main.lua
├── lua_waveform_demo/
│   └── main.lua
├── README.md
└── protoscope_api.lua
```

启动时如果当前工作目录不存在 `protocols` 目录，程序会自动生成以上默认工作区。若目录已存在，程序不会覆盖用户脚本。

## 快速开始

协议脚本由宿主加载执行，不需要 `require` 任何 ProtoScope 模块。脚本通过全局对象 `proto` 与宿主通信：

```lua
function ui()
  return {
    {
      id = "protocol",
      title = "协议动作",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "button", id = "read_version", label = "读取版本" },
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
      }
    }
  }
end

function on_control(ctx, id, value)
  if id == "read_version" then
    proto.send({ 0xAA, 0x55, 0x01, 0x0D })
    proto.set_timer("read_version_timeout", 1000)
  end
end
```

## UI 定义

脚本可定义 `ui()`，返回 Dock 面板数组。每个 Dock 支持：

- `id`：Dock 唯一 ID。
- `title`：面板标题。
- `anchor`：停靠位置，可用值为 `left`、`left_bottom`、`right_top`、`right_mid`、`right_bottom`、`main_bottom`，默认 `left_bottom`。
- `tab_group`：同组 Dock 以标签页形式组织，可省略。
- `controls`：控件数组。

控件类型：

- `button`：按钮，点击后触发 `on_control(ctx, id, true)`。
- `input_text`：文本输入，`default` 为字符串。
- `input_int`：整数输入，`default` 为整数。
- `input_float`：浮点输入，`default` 为数字。
- `checkbox`：复选框，`default` 为布尔值。
- `combo`：下拉框，必须提供 `options` 字符串数组，`default` 为从 1 开始的选项序号。

## 生命周期回调

按需定义以下全局函数即可：

- `on_open(ctx)`：连接打开。
- `on_close(ctx)`：连接关闭。
- `on_error(ctx, message)`：连接或传输错误。
- `on_control(ctx, id, value)`：UI 控件变化或按钮点击。
- `on_bytes(ctx, bytes)`：收到字节数组，`bytes` 为 `number[]`，元素范围 `0..255`。
- `on_timer(ctx, name)`：定时器触发。

`ctx` 包含：

- `connection_id`：连接 ID。
- `kind`：连接类型，例如 `serial`、`tcp_client`、`tcp_server`。
- `endpoint`：端点描述。

## 通信与事件 API

- `proto.send(payload)`：发送数据，`payload` 可为字符串或 `number[]`。
- `proto.log(level, message)`：写日志，`level` 常用 `debug`、`info`、`warn`、`error`。
- `proto.emit(name, payload)`：向宿主发出结构化脚本事件，`payload` 会序列化显示。
- `proto.get_control(id)`：读取控件当前值。
- `proto.set_control(id, value)`：设置控件当前值。
- `proto.set_timer(name, delay_ms)`：设置一次性定时器。
- `proto.cancel_timer(name)`：取消定时器。

CRC 辅助函数：

- `proto.crc16_modbus(payload)`：返回 Modbus CRC16。
- `proto.crc16_ccitt_false(payload)`：返回 CRC16/CCITT-FALSE。
- `proto.crc32_ieee(payload)`：返回 IEEE CRC32。

## 波形绘图 API

调用 `proto.plot.setup(payload)` 配置波形通道：

```lua
proto.plot.setup({
  source = "demo",
  reset_history = true,
  time_scale = 0.001,
  time_unit = "s",
  vertical_min = -2.0,
  vertical_max = 2.0,
  vertical_unit = "V",
  history_limit = 20000,
  channels = {
    { label = "CH1", unit = "V" },
    { label = "CH2", unit = "V", offset = 1.0 },
  }
})
```

调用 `proto.plot.push(channel_index, payload)` 追加采样点。`channel_index` 从 1 开始：

```lua
proto.plot.push(1, {
  samples = {
    { t = 0.000, y = 0.0 },
    { t = 0.001, y = 0.5 },
  }
})
```

## LuaLS 类型提示

`protocols/protoscope_api.lua` 是给 LuaLS 使用的虚拟 API 文件，只包含注解和空实现，不参与运行时协议逻辑。

在 LuaLS 中把 `protocols/protoscope_api.lua` 加入 workspace/library 后，打开 `default_protocol/main.lua` 或 `lua_waveform_demo/main.lua` 即可获得 `proto.*`、`proto.plot.*` 和回调参数的基础类型提示。

业务脚本无需 `require("protoscope_api")`，运行时由 ProtoScope 注入全局 `proto`。
