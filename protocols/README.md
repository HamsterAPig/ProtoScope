# ProtoScope Lua 协议脚本指南

`protocols` 是 ProtoScope 的 Lua 协议工作区。宿主会自动注入全局 `proto`，脚本主要负责 4 件事：

- 描述 UI 布局与控件。
- 描述接收流的 `stream()` schema。
- 在业务确认“完整应答已完成”时调用 `proto.request_done()`。
- 在需要时推送状态、弹窗和波形数据。

配套提示文件：

- `protocols/protoscope_api.lua`：LuaLS 用的宿主 API 声明。
- `protocols/stream_types.lua`：`stream()` schema 的类型注解。

## 发送模型

发送 API 分成两类：

- `proto.send(payload, opts?)`：普通异步发送。适合从机 ACK、主动上报、广播或不需要宿主排队等待的报文。
- `proto.request(payload, opts?)`：半双工请求。宿主负责排队、串行下发、超时、取消，以及前一条完成后的自动推进。
- `proto.request_done(result?)`：脚本在确认收到完整业务应答后调用；不需要传 `request_id`。

最小示例：

```lua
local pending = {}
local active_request_id = nil

local request_id, err = proto.request({ 0xAA, 0x55, 0x01, 0x0D }, {
  timeout_ms = 1000,
  tag = "read_version",
})

if request_id then
  pending[request_id] = { tag = "read_version" }
end

function on_tx(ctx, evt)
  if evt.kind ~= "request" then
    return
  end
  if evt.state == "sent" then
    active_request_id = evt.id
  elseif evt.state == "completed" or evt.state == "timeout" or evt.state == "rejected" then
    pending[evt.id] = nil
    if active_request_id == evt.id then
      active_request_id = nil
    end
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
- `completed`：仅 `request` 在 `proto.request_done()` 之后触发。
- `timeout` / `rejected` / `dropped` / `canceled`：都属于宿主终态。

推荐做法：

- 在 `sent` 时记录当前活动 request。
- 在 `completed` / `timeout` / `rejected` 时清理脚本侧状态。
- 不要自己再实现第二套 `request_queue/send_next_request` 调度器。

## 推荐接收模型：`stream()`

如果协议是“固定帧头 + 长度/定长 + CRC + 字段解析”，优先使用 `stream()`，不要再用 `on_bytes()` 手写缓冲、切帧和 CRC。

典型返回值：

```lua
---@type ProtoStreamSchema
local schema = {
  buffer = {
    capacity = 4096,
    overflow = "drop_oldest",
  },
  frames = {
    {
      name = "fc06_ack",
      header = { 0xFF, 0x06 },
      size = 8,
      crc = { type = "crc16_modbus", order = "hi_lo" },
      fields = {
        { name = "address", type = "u16_be", offset = 3 },
        { name = "value", type = "u16_be", offset = 5 },
      },
      on_frame = handle_fc06_ack,
    },
  },
  on_error = handle_stream_error,
}

return schema
```

宿主负责：

- 半包累计。
- 粘包连续解析。
- 噪声前缀丢弃。
- CRC 校验。
- 固定长度或变长长度解析。
- 字段解码后再回调 `on_frame`。

`stream()` 里的常用字段：

- `header`：固定帧头字节。
- `size`：整帧固定长度；适合 `FC06` / `FC16 ACK` 这种固定 8 字节帧。
- `len`：变长帧定义；适合 `FC03` 这类带字节数的响应。
- `crc`：CRC 类型与字节顺序。
- `fields`：字段定义，`offset` 从 1 开始计数。
- `on_frame`：完整有效帧回调。
- `on_error`：解析错误回调。

字段类型、`crc.order`、`len.means` 的可选值请直接参考 `protocols/stream_types.lua`。

## `on_bytes()` 仍然可用，但只建议用于两类场景

- 协议不是标准帧流，而是纯文本、终止符协议或临时调试输入。
- 你还在做最初的抓包验证，暂时不想先写 schema。

如果已经能明确写出：

- 固定帧头；
- 固定长度或长度字段；
- CRC 或其他校验；
- 结构化字段；

那就应当优先迁到 `stream()`。

## 状态、弹窗和波形

常用辅助 API：

- `proto.status.set(text, { level = "info"|"warn"|"error" })`
- `proto.status.clear()`
- `proto.ui.alert({ title = "...", message = "...", level = "warn" })`
- `proto.plot.setup({ source = "...", reset_history = true, channels = { ... } })`
- `proto.plot.push(channel_index, { source = "...", samples = { { t = 0.0, y = 1.23 } } })`

最小波形示例：

```lua
proto.plot.setup({
  source = "demo",
  reset_history = true,
  channels = {
    { label = "CH1", unit = "V", color = "#4FC3F7" },
    { label = "CH2", unit = "V", color = "#81C784" },
  },
})

proto.plot.push(1, {
  source = "demo",
  samples = {
    { t = 0.000, y = 1.2 },
    { t = 0.001, y = 1.3 },
  },
})
```

## SN Scope Lua Demo

仓库里现在有两类 SN Scope 相关示例：

- `protocols/half_duplex_modbus_master` / `protocols/half_duplex_modbus_slave`
- `cmake-build-debug/protocols/sn_scope_master` / `cmake-build-debug/protocols/sn_scope_slave_sim`

它们表达的是同一类协议约束：主机请求、从机 ACK、以及 `0x26` 上传帧。

### 固定寄存器语义

- `0x1010`：CH1/CH2 选择寄存器。
- `0x1012`：CH3/CH4 选择寄存器。
- `0x5A5A`：CH1/CH2 系数寄存器。
- `0x5A5C`：CH3/CH4 系数寄存器。
- `0x8888`：启动位，写 `1` 开始上传，写 `0` 停止。

### 主机端推荐写法

主机端推荐把自动配置拆成 5 个 request：

- `FC16 0x1010`：写 CH1/CH2 选择。
- `FC16 0x1012`：写 CH3/CH4 选择。
- `FC16 0x5A5A`：写 CH1/CH2 系数。
- `FC16 0x5A5C`：写 CH3/CH4 系数。
- `FC06 0x8888=0x0001`：启动上传。

脚本侧只维护：

- `pending_requests[id]`
- `active_request_id`

不要自己维护发送推进队列；宿主 `proto.request()` 已经负责串行调度。

### ACK 匹配规则

主机在 `on_frame` 中只校验当前活动 request：

- `FC06 ACK`：`address` 和 `value` 都必须与当前 request 一致。
- `FC16 ACK`：`address` 和 `register_count` 都必须与当前 request 一致。
- 不匹配时直接 `proto.request_done({ ok = false })`，并把“收到/期望”写入状态栏，避免迟到 ACK 污染后续请求。

### 从机端推荐写法

从机接收侧统一走 `stream()`：

- `fc03_request`：固定 8 字节。
- `fc06_request`：固定 8 字节。
- `fc16_request`：固定 13 字节。

业务逻辑放在对应 `on_frame` 回调里：

- `FC03`：回读 `0x5A5A..0x5A61`。
- `FC06`：处理 `0x8888` 启停上传。
- `FC16`：固定要求 `count = 2`、`byte_count = 4`，写 2 个寄存器。

### 上传帧建议

SN Scope 上传帧固定 14 字节：

```text
0xFF 0x26 + sequence(u16_be) + ch1(i16_be) + ch2(i16_be) + ch3(i16_be) + ch4(i16_be) + crc16(hi_lo)
```

推荐节拍：

- 每 `10ms` 生成 `120` 个上传帧。
- 把 `120 * 14 = 1680` 字节拼成一个 payload。
- 每个 tick 只调用一次 `proto.send(payload)`。

这样主机端的 `stream().frames[].on_frame` 可以从一个粘包 payload 中连续解析出 120 个 `upload_ch4` 帧。

## 文档同步约定

当你改了以下任一项时，建议同步更新文档或提示文件：

- 宿主暴露的 Lua API：同步 `protocols/protoscope_api.lua`
- `stream()` schema 类型：同步 `protocols/stream_types.lua`
- 协议脚本约定、推荐模式或 demo 行为：同步 `protocols/README.md`
