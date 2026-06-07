# ProtoScope Lua 协议脚本指南

`protocols` 是 ProtoScope 的 Lua 协议资源根目录。宿主会自动注入全局 `proto`，协议脚本主要负责 4 件事：

- 描述 UI 布局与控件。
- 描述接收流的 `stream()` schema。
- 在业务确认“完整应答已完成”时调用 `proto.request_done()`。
- 在需要时推送状态、弹窗和波形数据。

配套提示文件：

- `protocols/protoscope_api.lua`：LuaLS 用的宿主 API 声明。
- `protocols/protoscope_api_manifest.json`：`protoscope_api.lua` 的生成源，新增宿主 API 时先改它再生成。
- `protocols/stream_types.lua`：`stream()` schema 的类型注解。
- `protocols/templates/README.md`：内置协议模板列表和复制使用说明。

默认协议模板位于 `protocols/templates`。每个协议目录只要求存在 `main.lua`，
例如 `protocols/templates/default_protocol/main.lua`。

## UI 布局

脚本通过 `ui()` 声明停靠面板。宿主会在加载脚本时调用它，要求返回 `ProtoDockDescriptor[]`。

最小骨架：

```lua
function ui()
  return {
    {
      id = "protocol_tools",
      title = "协议工具",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", label_position = "right", default = true },
        { type = "button", id = "send_once", label = "发送一次" },
      },
      layout = {
        type = "flow",
        children = {
          { type = "control", id = "device_id" },
          { type = "control", id = "hex_send" },
          { type = "control", id = "send_once" },
        },
      },
    },
  }
end
```

### `ui()` 返回值

`ui()` 返回一个数组，每一项都是一个 `DockDescriptor`：

- `id`：停靠面板的稳定标识，必填。
- `title`：面板标题，必填。
- `anchor`：默认停靠位置，可选，默认是 `left_bottom`。
- `tab_group`：分组名，可选。相同 `tab_group` 的 dock 会落到同一个 tab 组里。
- `controls`：控件列表，必填。
- `layout`：新 Layout Tree，可选。省略时宿主会按 `controls` 的声明顺序逐个渲染。

### 控件类型

`controls` 是控件描述数组。每个控件必须提供：

- `type`：控件类型。
- `id`：稳定控件 ID，回调和布局节点都通过它引用控件。
- `label`：展示文案。`checkbox`、`input_text`、`input_int`、`input_float` 允许省略可见 label。
- `label_position`：可选，`"left"` 或 `"right"`，默认 `"left"`；`button` 始终把 `label` 当按钮文本。

当前控件类型：

- `button`：按钮。点击后会触发 `on_control(ctx, id, true)`。
- `input_text`：文本输入，`default` 是字符串。
- `input_int`：整数输入，`default` 是整数。
- `input_float`：浮点输入，`default` 是数字。
- `checkbox`：布尔开关，`default` 是 boolean。
- `combo`：下拉选择，必须提供 `options = { ... }`，`default` 是 1 基索引。
- `elf_symbol_combo`：ELF 静态地址候选输入框。

### Layout Tree

显式布局统一使用 `type + children` 的递归树，不再兼容旧的 `layout.kind`、`form.items`、`table.rows` control-only 写法。

通用规则：

- `{ type = "column", children = { ... } }`：纵向块级布局。
- `{ type = "flow", spacing = 6, run_spacing = 5, children = { ... } }`：横向流式布局，空间不足时自动换行。
- `{ type = "table", columns = 2, rows = { ... } }`：表格布局，单元格可以放任意 layout node。
- `{ type = "group", title = "...", children = { ... } }`：标题分组。
- `{ type = "collapse", title = "...", default_open = true, children = { ... } }`：折叠分组。
- `{ type = "control", id = "xxx" }`：引用一个已声明控件。
- `{ type = "text", text = "..." }`：说明文字。
- `{ type = "separator" }`：分割线。
- `{ type = "spacer" }`：占位空白。

加载阶段会校验显式布局中的控件引用：未知控件、重复引用、遗漏已声明控件都会让协议加载失败。

完整示例：

```lua
layout = {
  type = "column",
  children = {
    { type = "text", text = "修改参数后立即生效。" },
    {
      type = "flow",
      spacing = 6,
      run_spacing = 5,
      children = {
        { type = "control", id = "send_once" },
        { type = "control", id = "device_id" },
      }
    },
    {
      type = "collapse",
      title = "采样设置",
      default_open = false,
      children = {
        {
          type = "flow",
          children = {
            { type = "control", id = "timeout_ms" },
            { type = "control", id = "scale" },
          }
        }
      }
    },
    {
      type = "table",
      columns = 2,
      borders = false,
      resizable = true,
      row_bg = false,
      sizing = "stretch",
      rows = {
        {
          { type = "control", id = "mode" },
          {
            type = "flow",
            children = {
              { type = "control", id = "hex_send" },
              { type = "control", id = "auto_send" },
            }
          },
        }
      }
    }
  }
}
```

## 发送模型

发送 API 分成两类：

- `proto.send(payload, opts?)`：普通异步发送。适合从机 ACK、主动上报、广播或不需要宿主排队等待的报文。
- `proto.request(payload, opts?)`：半双工请求。宿主负责排队、串行下发、超时、取消，以及前一条完成后的自动推进。
- `proto.request_guarded(payload, opts?)`：受保护半双工请求。只有这个接口参与 guarded 超时重试和熔断；普通 `proto.request()` / `proto.send()` 不读取 guarded 状态。
- `proto.reset_request_guard()`：显式解除 guarded 熔断，让新的 guarded 请求从 `attempt=1` 重新开始。
- `proto.request_done(result?)`：脚本在确认收到完整业务应答后调用；不需要传 `request_id`。

`proto.request(payload, opts?)` 的 `opts` 目前支持：

- `timeout_ms`：等待 `request_done` 的超时时间。
- `tag`：业务标签，便于日志和 `on_tx` 识别。

`proto.request_guarded(payload, opts?)` 额外支持：

- `max_attempts`：当前这一次 request 的最大尝试次数，最小值按 `1` 处理，空值默认 `1`。例如 `max_attempts = 3` 表示首发 1 次，超时后最多再重发 2 次。

Lua 脚本保持传输无关：无论底层是 TCP、串口还是 UDP Peer，协议脚本都只处理 bytes、`ctx` 和 `proto.send/request/request_guarded`。如确实需要展示当前通讯来源，可读取 `ctx.kind`，取值为 `tcp_client`、`tcp_server`、`serial` 或 `udp_peer`。

最小示例：

```lua
local pending = {}
local active_request_id = nil

local request_id, err = proto.request_guarded({ 0xAA, 0x55, 0x01, 0x0D }, {
  timeout_ms = 1000,
  tag = "read_version",
  max_attempts = 3,
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
---@field guarded? boolean
---@field attempt? integer
---@field max_attempts? integer
---@field guard_state? 'active'|'retrying'|'halted'|'reset'
```

其中：

- `sent`：字节已经写出。
- `completed`：仅 `request` 在 `proto.request_done()` 之后触发。
- `timeout` / `rejected` / `dropped` / `canceled`：都属于宿主终态。
- `guard_state="retrying"`：当前 guarded request 的本次 attempt 超时，宿主会把下一次 attempt 放回队首。
- `guard_state="halted"`：当前 guarded request 已最终失败，后续 guarded request 会被拒绝发送。
- `guard_state="reset"`：脚本调用 `proto.reset_request_guard()` 后的恢复事件。

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
    max_capacity = 268435456,
  },
  raw_output = "omit",
  low_overhead = true,
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
  on_batch = handle_stream_batch,
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
- `runtime_profile = true` 时，从 `proto.stream.set_profile()` 读取运行时整帧长度与通道映射。
- 字段解码后回调脚本；定义 `on_batch(ctx, frames)` 时批量调用，否则逐帧调用 `on_frame`。

`stream()` 里的常用字段：

- `header`：固定帧头字节。
- `size`：整帧固定长度；适合 `FC06` / `FC16 ACK` 这种固定 8 字节帧。
- `len`：变长帧定义；适合 `FC03` 这类带字节数的响应。
- `crc`：CRC 类型与字节顺序。
- `fields`：字段定义，`offset` 从 1 开始计数；`count` 可写整数、已解析字段名或纯 C++ count 表达式 table。
- `on_frame`：完整有效帧回调；未定义 `on_batch` 时必填。
- `on_batch`：完整有效帧批量回调，`frames` 中每项保持旧 `on_frame` 的 frame 结构；若同时定义 `on_batch` 与 `on_frame`，宿主只调用 `on_batch`。
- `on_error`：解析错误回调。
- `raw_output`：默认 `full` 会向 Lua 暴露 `frame.raw`；高速连续采样建议写 `omit`，避免逐字节展开 raw。
- `low_overhead`：默认 `false` 保持调试快照兼容；高速场景可与 `raw_output = "omit"` 配合，成功帧不保留到 `lastStreamParseBatch()`。
- `field_output`：默认 `compat` 同时写 `frame.fields.xxx` 和 `frame.xxx`；高频回调可写 `fields_only`，只保留 `frame.fields`。

补充约定：

- Lua `buffer.capacity` 是初始环形缓冲容量；默认不丢弃，容量不足时会在 `buffer.max_capacity`（默认 256MiB）内自动扩容。
- 只有显式写 `overflow = "drop_oldest"` 时，parser 才会在容量超限时丢弃最旧字节。
- 宿主 YAML `receive.stream_buffer.near_overflow_threshold` 与 `receive.stream_buffer.popup_enabled` 只控制“接近溢出”告警阈值和弹窗，不会改写协议 schema 的缓冲容量。
- 高速协议推荐组合：`on_batch` + `raw_output = "omit"` + `low_overhead = true`；如果脚本只读 `frame.fields`，可进一步加 `field_output = "fields_only"`。

### 运行时长度 / 通道映射

当某个帧的真实长度、业务通道顺序需要在运行时由脚本先探测、再持续生效时，可以这样声明：

```lua
{
  name = "upload_dynamic",
  header = { 0xFF, 0x26 },
  runtime_profile = true,
  crc = { type = "crc16_modbus", order = "hi_lo" },
  fields = {
    { name = "sequence", type = "u16_be", offset = 3 },
    { name = "values", type = "i16_be", offset = 5, count = { op = "remaining", unit = 2 } },
  },
  on_frame = handle_upload_frame,
}
```

然后在 Lua 里设置：

```lua
local ok, err = proto.stream.set_profile({
  frame = "upload_dynamic",
  length = 14,
  channel_map = { 2, 3, 1, 4 },
})
```

更完整的写法通常是在协议加载后、目标帧到达前完成 profile 设置。例如设备握手已经返回帧长度和通道顺序时：

```lua
local function apply_upload_profile(frame_length, channel_order)
  local ok, err = proto.stream.set_profile({
    frame = "upload_dynamic",
    -- 完整帧长度：帧头、字段、CRC 都计算在内。
    length = frame_length,
    -- Lua 侧使用 1-based 通道号；宿主内部会转换为 0-based。
    channel_map = channel_order,
  })
  if not ok then
    proto.status.set("运行时 profile 设置失败: " .. tostring(err), { level = "error" })
  end
end

function on_open(ctx)
  -- 示例：传输打开后、真实业务数据到达前，先按设备协商结果设置 profile。
  apply_upload_profile(14, { 2, 3, 1, 4 })
end
```

约定：

- `length` 是完整帧长度。
- `channel_map` 用 Lua 侧 1-based 通道号声明；宿主内部会转换为 0-based。
- profile 一旦设置会持续生效，直到 `proto.stream.clear_profile("upload_dynamic")` 或 `proto.stream.clear_profile()`。
- `runtime_profile = true` 的帧如果回放时缺少对应 profile 事件，宿主会给出明确错误，而不是静默套旧长度。
- “导出当前可见 raw”只保存当前波形窗口可见的原始字节，并会补齐这段字节回放所依赖的活动 `profile_set` / `profile_clear` 和最后一次 `plot_setup` 快照。
- “开始完整原始数据录制”保存完整事件流、完整 RX 历史和录制开始时的波形配置快照；需要完整复现长时间采集时，应优先使用完整录制，而不是普通导出。

字段类型、`crc.order`、`len.means` 的可选值请直接参考 `protocols/stream_types.lua`。

动态字段数量不要再写 Lua 回调；旧 `field.count = function(...) end` 属于历史残留，现已在加载期直接报错拦截。历史重放和实时解析会共用纯 C++ parser。常见 `count` 表达式：

```lua
-- 字段引用
count = { op = "field", name = "byte_count" }

-- 字节数转寄存器数
count = { op = "div", field = "byte_count", by = 2 }

-- 长度字段减头尾
count = { op = "sub", field = "length", value = 2 }

-- 元素数转字节数
count = { op = "mul", field = "item_count", by = 4 }

-- 采样点数乘以通道掩码位数
count = { op = "mul", field = "sample_count", by = { op = "bit_count", field = "channel_mask" } }

-- 当前字段到帧尾剩余元素
count = { op = "remaining", unit = 2, exclude_crc = true }

-- 按 flag 控制可选字段
count = { op = "if_flag", field = "flags", mask = 0x01, then = 1, else = 0 }

-- 按功能码选择
count = {
  op = "case",
  field = "func",
  cases = {
    [0x03] = { op = "div", field = "byte_count", by = 2 },
    [0x10] = 2,
  },
  default = 1,
}
```

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
- `proto.ui.alert({ title = "...", message = "...", level = "warn", window = { width = 520, height = 260, x = 120, y = 80, resizable = true, movable = true, auto_resize = false } })`
- `proto.plot.setup({ source = "...", reset_history = true, channels = { ... } })`
- `proto.plot.push(channel_index, { source = "...", samples = { { t = 0.0, y = 1.23 } } })`

脚本弹窗的 `window` 子表只影响 `proto.ui.alert/confirm` 这类 ImGui 自绘模态窗口：

- `width` / `height`：首次出现时的初始宽高
- `x` / `y`：相对主视口左上角的初始位置，仅首次出现生效
- `resizable` / `movable`：是否允许用户拖动缩放或移动
- `auto_resize`：是否使用 ImGui 自动尺寸

如果不传 `window`，ProtoScope 会继续沿用原有自动尺寸弹窗行为；标题只保留在窗口标题栏，不会在正文重复显示。

`proto.fs.*` 仍使用系统原生文件对话框，不支持用同一套 `window` 参数控制宽高、位置或拖动开关。

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

## 半双工 Modbus Schema Demo

仓库内置两个半双工 Modbus 模板：

- `protocols/templates/half_duplex_modbus_master`
- `protocols/templates/half_duplex_modbus_slave`

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

上传帧固定 14 字节：

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

- 宿主暴露的 Lua API：同步 `protocols/protoscope_api_manifest.json`，再运行 `python tools/generate_luals_api.py`
- `stream()` schema 类型：同步 `protocols/stream_types.lua`
- UI 布局类型、控件字段或 dock 约定：同步 `protocols/protoscope_api_manifest.json`、重新生成 `protocols/protoscope_api.lua`，并更新本 README 的示例。
- 协议脚本约定、推荐模式或 demo 行为：同步 `protocols/README.md`
