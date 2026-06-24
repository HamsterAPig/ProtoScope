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

源码中的默认示例位于 `protocols/default_protocol`、`protocols/lua_waveform_demo`、
`protocols/half_duplex_modbus_master` 和 `protocols/half_duplex_modbus_slave`。
`protocols/templates` 只放可复制的操作模板，例如 `file_dialog`、`request_guarded`、`send_file`、`ui_basic`、`ui_layouts` 和 `ui_dialogs`。
每个协议目录只要求存在 `main.lua`，例如 `protocols/default_protocol/main.lua`。
打包运行时会把内嵌默认协议释放到可执行目录下的协议目录，供用户直接选择和复制。

## Lua UI 最短路径

脚本通过 `ui()` 声明停靠面板。宿主会在加载脚本时调用它，允许直接返回单个 `ProtoDockDescriptor`，也允许返回 `ProtoDockDescriptor[]`。最短路径是：先在 `controls` 里声明控件，再用 `layout` 排列控件，最后用 `on_control(ctx, id, value)` 响应用户操作。

下面是一个可以直接保存为 `main.lua` 的完整闭环：

```lua
local click_count = 0

function ui()
  return {
    id = "protocol_tools",
    title = "协议工具",
    anchor = "left_bottom",
    tab_group = "protocol_tools",
    controls = {
      { "text", "device_id", "设备 ID", default = "01" },
      { "check", "hex_send", "HEX 发送", label_position = "right", default = true },
      { "btn", "send_once", "发送一次" },
      { "text", "last_action", "最近动作", default = "待操作" },
    },
    layout = {
      { "device_id", "hex_send", "send_once" },
      { id = "last_action", min_width = 260 },
    },
  }
end

function on_control(ctx, id, value)
  if id ~= "send_once" then
    return
  end

  -- 按钮点击后读取当前 UI 状态，并把处理结果写回控件。
  click_count = click_count + 1
  local device_id = proto.get_control("device_id") or "01"
  local hex_send = proto.get_control("hex_send") == true
  local summary = string.format("第 %d 次发送：设备 %s，HEX=%s", click_count, device_id, tostring(hex_send))

  proto.set_control("last_action", summary)
  proto.ui.alert({
    title = "发送动作",
    message = summary,
    level = "info",
  })
end
```

可复制模板位于 `protocols/templates/ui_basic`、`ui_layouts` 和 `ui_dialogs`。如果需要完整布局组合或弹窗回调示例，优先复制这些模板目录。

### `ui()` 返回值

`ui()` 返回一个数组，每一项都是一个 `DockDescriptor`：

- `id`：停靠面板的稳定标识，必填。
- `title`：面板标题，必填。
- `anchor`：默认停靠位置，可选，默认是 `left_bottom`。
- `tab_group`：分组名，可选。相同 `tab_group` 的 dock 会落到同一个 tab 组里。
- `controls`：控件列表，必填。
- `layout`：新 Layout Tree，可选。省略时宿主会按 `controls` 的声明顺序逐个渲染。

可用 `anchor`：`left`、`left_bottom`、`right_top`、`right_mid`、`right_bottom`、`main_bottom`。

### 控件类型

`controls` 是控件描述数组。每个控件必须提供：

- `type`：控件类型。
- `id`：稳定控件 ID，回调和布局节点都通过它引用控件。
- `label`：展示文案。`checkbox`、`input_text`、`input_int`、`input_float` 允许省略可见 label。
- `label_position`：可选，`"left"` 或 `"right"`，默认 `"left"`；`button` 始终把 `label` 当按钮文本。
- `short_label` / `compact_label_below`：可选紧凑标签。布局宽度低于 `compact_label_below` 且显式提供 `short_label` 时显示短标签，悬浮显示完整 `label`。

当前控件类型：

- `button`：按钮。点击后会触发 `on_control(ctx, id, true)`。
- `input_text`：文本输入，`default` 是字符串。
- `input_int`：整数输入，`default` 是整数。
- `input_float`：浮点输入，`default` 是数字。
- `checkbox`：布尔开关，`default` 是 boolean。
- `combo`：下拉选择，必须提供 `options = { ... }`，`default` 是 1 基索引。
- `elf_symbol_combo`：ELF 静态地址候选输入框，值是 `{ label, value, type }` 结构；可选 `debounce_ms` 和 `limit`，未写时使用 `gui.elf_symbol_combo.debounce_ms` 与 `gui.elf_symbol_combo.limit`。
- `value_table`：只读寄存器显示表。`rows` 支持普通行（`id + label + unit? + note?`）、bit 展开行（在一个源 `id` 下声明 `bits = { ... }`，`bit` 使用 U32 下标）、批量行（`start_id + len + labels + units`）。row id 只用于内部匹配，不显示到界面；`note` 字段在悬浮时作为 tooltip 展示。

控件支持短类型和位置参数糖：`{ "text", "device_id", "设备 ID", default = "01" }` 等价于 `{ type = "input_text", id = "device_id", label = "设备 ID", default = "01" }`。短类型映射为：`btn -> button`、`text -> input_text`、`int -> input_int`、`float -> input_float`、`check -> checkbox`、`select -> combo`、`symbol -> elf_symbol_combo`、`values -> value_table`。

```lua
controls = {
  { "values", "holding_values", "保持寄存器",
    rows = {
      { 0x1010, "电压", "V", note = "母线电压" },

      { id = 0x1020, label = "状态字",
        bits = {
          { bit = 0, label = "运行", values = { [0] = "停止", [1] = "运行" } },
          { bit = 1, label = "告警", values = { [0] = "正常", [1] = "告警" }, note = "设备告警位" },
          { bit = 5, label = "远程模式" },
        },
      },

      { start_id = 0x1030, len = 3,
        labels = { "温度", "湿度", "压力" },
        units = { "C", "%", "kPa" },
      },
    },
  },
}
```

`proto.set_control("holding_values", { [0x1010] = "220.1", [0x1020] = 0x0023 })` 按 row id 更新。对于 bit 源行，收到非负整数、整数字符串、HEX 字符串、ProtoBuffer 或 number[] 后自动展开 bit：整数源按 64 位读取，字节源按 `bit0 = 第 1 个字节最低位`、`bit8 = 第 2 个字节最低位` 读取。也可以传 `{ start_id = ..., values = { ... } }` 做范围更新。schema 自动化流填充使用 `value_targets.controls` 映射，解析帧后自动写入目标 value_table 控件，再调用 on_batch/on_frame，handler 覆盖优先。

### 控件状态读写

`on_control(ctx, id, value)` 的 `value` 是当前触发控件的新值；如果按钮点击时需要读取其他控件状态，使用 `proto.get_control(id)`：

```lua
function on_control(ctx, id, value)
  if id == "send_once" then
    local device_id = proto.get_control("device_id") or "01"
    local hex_send = proto.get_control("hex_send") == true
    proto.emit("send_once", { device_id = device_id, hex_send = hex_send })
  end
end
```

`proto.set_control(id, value)` 用于脚本反向更新 UI，常见场景是把协议解析结果、最近操作或只读表格值写回控件：

```lua
proto.set_control("last_action", "已发送读取版本命令")
proto.set_control("holding_values", {
  [0x1010] = "220.1",
  [0x1020] = 0x0023,
})
proto.set_control("holding_values", {
  start_id = 0x1030,
  values = { "36.5", "42", "101.3" },
})
```

找不到控件时，`proto.get_control()` 返回 `nil`；`proto.set_control()` 会写入 Lua 警告日志并忽略本次更新。`elf_symbol_combo` 的值可能是字符串，也可能是 `{ label = "...", value = 123, type = "FUNC" }` 这样的表，脚本处理时应先判断 `type(value) == "table"`。

### UI 事件回调

动态 UI 常用回调：

- `on_control(ctx, id, value)`：用户操作控件时触发。按钮的 `value` 固定为 `true`；输入框、数字框、开关、下拉框会传当前值；`elf_symbol_combo` 可能传字符串或符号表。
- `on_dialog(ctx, evt)`：`proto.ui.confirm()` 或 `proto.ui.alert()` 关闭后触发。常用字段是 `evt.tag`、`evt.dialog_id` 和 `evt.result`。
- `on_oscilloscope_toggle(ctx, current_running, target_running)`：波形工具栏播放/暂停按钮触发。脚本返回 `true` 时宿主默认把按钮同步到 `target_running`；返回 `false`、缺失回调、异常或非 boolean 返回都不会默认切换。启动或暂停设备的实际动作应由脚本自行调用 `proto.send()` / `proto.request()` 等 API 完成。
- `proto.oscilloscope.set_running(running)`：脚本主动同步波形工具栏运行状态；适合按钮、定时器或 ACK 回调在真实启动/停止完成后调用。同一次 `on_oscilloscope_toggle` 回调里显式调用时，显式状态优先于 `return true` 的默认目标状态。

如果只需要处理当前控件，直接使用 `value`；如果一次按钮点击需要组装多个控件值，再用 `proto.get_control()` 读取其他控件。

### Layout Tree

显式布局统一使用 `type + children` 的递归树，不再兼容旧的 `layout.kind`、`form.items`、`table.rows` control-only 写法。
`column`、`flow` 和 `inline_group` 可用 `controls = { "id1", "id2" }` 简写连续控件；同一个 layout 节点上 `children` 与 `controls` 互斥，不能同时填写。
需要约束宽度时使用显式 `control` 节点，例如 `{ type = "control", id = "device_id", min_width = 180, max_width = 260 }`。需要让输入控件跟随 dock 宽度拉伸时，可在 layout control 上写 `fill_width = true`。宽度约束只属于 layout 的 `control` 节点；顶层控件描述只声明控件类型、标签、默认值和选项。

语法糖写法会在加载时展开为同一套 Layout Tree，因此校验规则不变：

- `layout = { ... }` 且未写 `type` 时默认是 `column`。
- `"device_id"` 等价于 `{ type = "control", id = "device_id" }`。
- `{ "device_id", "hex_send", "send_once" }` 等价于一个 `flow`，内部按顺序引用这些控件。
- `{ id = "last_action", min_width = 280, fill_width = true }` 等价于 `control` 节点，并保留宽度约束与行尾填充语义。
- `{ text = "说明文字" }`、`{ separator = true }`、`{ spacer = true }` 分别等价于 `text`、`separator`、`spacer` 节点。

通用规则：

- `{ type = "column", children = { ... } }` 或 `{ type = "column", controls = { "id1", "id2" } }`：纵向块级布局。
- `{ type = "flow", spacing = 6, run_spacing = 5, children = { ... } }` 或 `{ type = "flow", controls = { "id1", "id2" } }`：横向流式布局，空间不足时自动换行。
- `{ type = "inline_group", spacing = 4, min_width = 160, fill_width = true, children = { ... } }` 或 `{ type = "inline_group", controls = { "id1", "id2" } }`：在外层 `flow` 中作为整体参与换行；组内只允许 `control` / `text`，始终横向排列。组本身设置 `fill_width = true` 时可占满当前行剩余宽度，组内通常只给一个输入类 control 设置 `fill_width = true`。
- `{ type = "table", columns = 2, rows = { ... } }`：表格布局，单元格可以放任意 layout node。
- `{ type = "group", title = "...", children = { ... } }`：标题分组。
- `{ type = "collapse", title = "...", default_open = true, children = { ... } }`：折叠分组。
- `{ type = "control", id = "xxx" }`：引用一个已声明控件。可选 `min_width` / `max_width` 约束控件宽度，值必须是正数；可以只写其中一个，同时填写时要求 `min_width <= max_width`。`fill_width = true` 仅接受 boolean；在 `flow` 中会作为行尾填充项，绘制后后续项换行。
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
        {
          type = "inline_group",
          spacing = 4,
          controls = { "send_once", "device_id" },
        },
        { type = "control", id = "hex_send" },
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

### 脚本弹窗

`proto.ui.alert()` 适合提示结果，`proto.ui.confirm()` 适合让用户确认下一步。它们通常从 `on_control()` 里发起，关闭结果由 `on_dialog()` 接收：

```lua
function on_control(ctx, id, value)
  if id == "confirm_send" then
    proto.ui.confirm({
      title = "确认发送",
      message = "是否发送当前命令？",
      tag = "confirm_send",
      dedupe_key = "confirm_send",
    })
  end
end

function on_dialog(ctx, evt)
  if evt.tag == "confirm_send" and evt.result == "yes" then
    proto.emit("confirmed_send", { dialog_id = evt.dialog_id })
  end
end
```

弹窗初始尺寸、位置和拖动缩放开关写在可选的 `window` 子表里；完整参数见本文后面的“状态、弹窗和波形”，维护宿主绑定时再看 `docs/lua-host-integration.md`。

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
- `value_targets`：声明解析帧后自动填充 value_table 控件。`controls` 里每项指定目标控件 id、`values_field` 和注册起始 `start_field` 或 `start_id`；当 `values_field` 是 `bytes` 字段时，会把整段 bytes 作为一个 bit 源更新对应 `start_id` 的 bit 行，不按字节拆成连续 row。

```lua
value_targets = {
  controls = {
    { id = "holding_values", start_field = "start_addr", values_field = "registers" },
  },
},
```

自动填充发生在 `on_batch/on_frame` 之前，Lua handler 里再次 `set_control` 同一行会覆盖自动值。

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
- “导出当前缓存快照”只保存当前可回放窗口里的原始字节，并会补齐这段字节回放所依赖的活动 `profile_set` / `profile_clear` 和最后一次 `plot_setup` 快照。
- “开始完整原始数据录制”保存完整事件流、完整 RX 历史和录制开始时的波形配置快照；需要完整复现长时间采集时，应优先使用完整录制或现场会话包，而不是普通导出。

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
- `proto.ui.confirm({ title = "...", message = "...", tag = "confirm_send", dedupe_key = "confirm_send" })`
- `proto.plot.setup({ source = "...", reset_history = true, channels = { ... } })`
- `proto.plot.push(channel_index, { source = "...", samples = { { t = 0.0, y = 1.23 } } })`

脚本弹窗的 `window` 子表只影响 `proto.ui.alert/confirm` 这类 ImGui 自绘模态窗口：

- `width` / `height`：首次出现时的初始宽高
- `x` / `y`：相对主视口左上角的初始位置，仅首次出现生效
- `resizable` / `movable`：是否允许用户拖动缩放或移动
- `auto_resize`：是否使用 ImGui 自动尺寸

如果不传 `window`，ProtoScope 会继续沿用原有自动尺寸弹窗行为；标题只保留在窗口标题栏，不会在正文重复显示。
`dedupe_key` 可用于同类弹窗去重；未设置时每次调用都按独立弹窗请求处理。

`proto.fs.*` 仍使用系统原生文件对话框，不支持用同一套 `window` 参数控制宽高、位置或拖动开关。

文件 IO 常用调用：

```lua
proto.fs.open_file_dialog({
  title = "选择样本",
  mode = "open",
  filters = {
    { name = "Binary", pattern = "*.bin" },
    { name = "All", pattern = "*" },
  },
})

local handle, err = proto.fs.open(path, {
  mode = "read",
  binary = true,
  create_dirs = false,
  overwrite = false,
})

local chunk, read_err = proto.fs.read(handle, { size = 4096 })
proto.fs.close(handle)

local job_id, send_err = proto.fs.send_file(path, {
  kind = "send",
  chunk_size = 256,
  tag = "firmware",
})
```

- `open_file_dialog.mode` 支持 `open` 和 `save`，结果通过 `on_file_dialog(ctx, evt)` 异步返回。
- `proto.fs.open()` 的 `mode` 支持 `read`、`write`、`append`；写入模式可配合 `create_dirs` 和 `overwrite`。
- `proto.fs.send_file()` 会分块读取文件并通过宿主 TX 队列发送，`kind` 可选择 `send` 或 `request`，进度与结果通过 `on_tx(ctx, evt)` 返回。

最小波形示例：

```lua
proto.plot.setup({
  source = "demo",
  reset_history = true,
  channels = {
    { label = "CH1", unit = "V", color = "#4FC3F7", line_width = 2.5 },
    { label = "CH2", unit = "raw", color = "#81C784", bit_display = { enabled = true, first_bit = 0, bit_count = 8 } },
  },
})

proto.plot.push(1, {
  source = "demo",
  samples = {
    { t = 0.000, y = 1.2 },
    { t = 0.001, y = 1.3 },
  },
})

proto.plot.push(2, {
  source = "demo",
  samples = {
    { t = 0.000, y = 0x55 },
    { t = 0.001, y = 0xAA },
  },
})
```

`bit_display` 读取同一通道 `push()` 的原始 `y` 作为非负整数 bitfield；
这些 bit 不会套用 `ratio`、`scale` 或 `offset`。

## 半双工 Modbus Schema Demo

仓库内置两个半双工 Modbus 示例：

- `protocols/half_duplex_modbus_master`
- `protocols/half_duplex_modbus_slave`

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
