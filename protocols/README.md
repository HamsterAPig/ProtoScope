# ProtoScope Lua 协议脚本指南

`protocols` 是 ProtoScope 的 Lua 协议工作区。宿主会自动注入全局 `proto`，脚本只负责描述 UI、协议解析和在“完整应答完成”时调用 `proto.request_done()`。

## 发送模型

新的发送 API 分成两类：

- `proto.send(payload, opts?)`：普通异步发送，写出成功后会收到一次 `on_tx(..., { state = "sent" })`。
- `proto.request(payload, opts?)`：半双工请求。宿主负责排队、串行下发、超时、取消和下一条自动推进。
- `proto.request_done(result?)`：脚本在**确认收到完整应答**后调用；不需要传 `request_id`。

示例：

```lua
local request_id, err = proto.request({ 0xAA, 0x55, 0x01, 0x0D }, {
  timeout_ms = 1000,
  tag = "read_version",
})

function on_bytes(ctx, bytes)
  if has_full_response(bytes) then
    proto.request_done({ ok = true, message = "版本应答完成" })
  end
end
```

## `on_tx` 回调

宿主只在主线程回调 `on_tx(ctx, evt)`：

```lua
---@class ProtoTxEvent
---@field id integer
---@field kind 'send'|'request'
---@field state 'sent'|'completed'|'timeout'|'rejected'|'dropped'|'canceled'
---@field tag string
---@field bytes integer
---@field queued_ms integer
---@field finished_ms integer
---@field error? string
```

其中：

- `sent`：字节已经写出。
- `completed`：仅 `request` 在 `proto.request_done()` 后触发。
- `timeout` / `rejected` / `dropped` / `canceled`：都属于宿主终态。

## 状态栏与弹窗 API

```lua
proto.status.set("读取版本请求已写出", { level = "info" })
proto.status.clear()

proto.ui.alert({
  title = "请求超时",
  message = "读取版本请求超时",
  level = "warn",
  dedupe_key = "read_version_timeout",
})
```

- `proto.status.set(text, opts?)`：写入底部状态栏，`opts.level` 当前只做轻量级语义标记。
- `proto.ui.alert(opts)`：宿主管理的模态框，固定“关闭”按钮。
- `proto.ui.confirm(opts)`：宿主管理的确认框，固定“确认 / 取消”。
- `dedupe_key`：同一个 key 在未关闭前只保留一个弹窗。
- 用户关闭/确认后会回调 `on_dialog(ctx, evt)`。

## 波形颜色

`proto.plot.setup()` 的 `channels[]` 现在支持颜色：

```lua
proto.plot.setup({
  source = "default_protocol",
  channels = {
    { label = "CH1", unit = "V", color = "#47C971" },
    { label = "CH2", unit = "V", color = "#5B8FF9AA" },
  }
})
```

支持：

- `#RRGGBB`
- `#RRGGBBAA`

只改颜色不会清空历史波形；宿主只刷新显示配置。

## LuaLS 类型提示

`protocols/protoscope_api.lua` 仅供 LuaLS 使用，不参与运行时。

把 `protocols/protoscope_api.lua` 加入 LuaLS 的 `workspace.library` 后，就能获得：

- `proto.send / proto.request / proto.request_done`
- `proto.status.* / proto.ui.*`
- `on_tx / on_dialog`
- `ProtoPlotChannel.color`

## 默认协议示例

`protocols/default_protocol/main.lua` 已经示范新的半双工语义：

- “读取版本”使用 `proto.request(...)`
- 收到完整帧后调用 `proto.request_done(...)`
- 超时由宿主 request timeout 主导
- 状态栏和弹窗都走新的 `proto.status.* / proto.ui.*`
