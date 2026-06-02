# ProtoScope

ProtoScope 当前已具备第一版 `TCP + Lua` 联调主链路，核心目标是把“连接设备 -> 执行协议脚本 -> 观察收发与事件”这条路径跑通。

面向使用者的完整说明见 [ProtoScope 用户手册](docs/user-manual.md)。
面向维护者的模块说明见 [src/include 模块设计导览](docs/module-design.md)。

## 当前能力

- `通讯配置`：`TCP Client / TCP Server / Serial / UDP Peer`
- `配置文件管理`：支持保存、显式重载，以及可选的“外部配置变更提醒”
- `发送编辑器`：HEX 模式自动归一化为大写双字节分组，并在奇数个 nibble 时禁止发送
- `Lua 声明式控件`：通过 `controls()` 把脚本控件挂进 Dock
- `Lua 协议回调`：脚本可响应打开、关闭、收包、控件交互和定时器事件
- `接收展示`：统一展示系统日志、TX/RX、Lua 事件
- `波形 Dock`：支持 Lua 推送多通道波形、总览、测量、FFT，以及原始波形导入、导出和录制

## 已接入依赖

- `3rdparty/spdlog`
- `3rdparty/yaml-cpp`
- `3rdparty/asio`
- `3rdparty/imgui`
- `3rdparty/libdwarf-code`
- `3rdparty/lua`
- `3rdparty/sol2`

## 构建与测试

首次拉取：

```powershell
git submodule update --init --recursive
```

如果本机已有 Make/Ninja 等生成器，可直接使用你熟悉的生成目录。Windows 下当前已验证过的命令示例：

```powershell
"C:\Program Files\CMake\bin\cmake.exe" -S . -B build-gcc
"C:\Program Files\CMake\bin\cmake.exe" --build build-gcc
"C:\Program Files\CMake\bin\ctest.exe" --test-dir build-gcc --output-on-failure
```

如需使用 Visual Studio 生成器，也可以单独指定新的构建目录。

## 协议脚本目录结构

协议目录固定按下面约定组织：

```text
protocols/
├── protoscope_api.lua
├── stream_types.lua
└── templates/
    └── <protocol_name>/
        └── main.lua
```

- 入口脚本固定为 `main.lua`
- 主程序当前只要求协议目录下存在 `main.lua`，后续拆分可通过 `require()` 自行组织
- 默认示例目录为 `protocols/templates/default_protocol`

## Lua 宿主 API

脚本侧统一通过全局对象 `proto` 与主程序交互：

- `proto.log(level, message)`
  - 写入 Lua 日志面板
  - `level` 建议使用 `info / warn / error`
- `proto.send(data)`
  - 发送数据到当前连接
  - 支持 `number[]`，也支持 HEX 字符串
- `proto.emit(name, payload)`
  - 向主程序事件面板发出业务事件
  - `payload` 可为字符串、数字、布尔值或 table
- `proto.set_timer(name, delay_ms)`
  - 注册一次性定时器
- `proto.cancel_timer(name)`
  - 取消一次性定时器
- `proto.get_control(id)`
  - 读取当前控件值
- `proto.set_control(id, value)`
  - 从脚本侧更新控件值，更新后 UI 会在下一帧反映
- `proto.crc16_modbus(payload)`
  - 计算 Modbus CRC16，返回整数校验值
- `proto.crc16_ccitt_false(payload)`
  - 计算 CRC16/CCITT-FALSE，返回整数校验值
- `proto.crc32_ieee(payload)`
  - 计算 CRC32/IEEE，返回整数校验值

## Lua 回调生命周期

当前约定的回调面如下：

- `ui()`
  - 返回 Dock 描述数组
  - 每个 Dock 需提供 `id`、`title`、`controls`
  - 当前默认脚本优先使用该入口组织多面板 UI
- `controls()`
  - 返回控件描述数组
  - 当 `ui()` 不存在时，作为兼容旧脚本的回退入口
  - 若结构非法，脚本加载失败
- `on_open(ctx)`
  - 连接建立后调用
- `on_close(ctx)`
  - 连接关闭后调用
- `on_error(ctx, message)`
  - 连接出错后调用
- `on_bytes(ctx, bytes)`
  - 收到数据后调用
- `on_timer(ctx, name)`
  - 定时器到期后调用
- `on_control(ctx, id, value)`
  - 用户点击按钮、修改输入框或切换勾选项后调用

`ctx` 当前包含以下字段：

- `ctx.kind`
- `ctx.endpoint`
- `ctx.connection_id`
- `ctx.timestamp_ms`
- `ctx.ready_for_io`

`bytes` 当前统一是 `number[]`。

## 控件声明格式

当前默认脚本使用 `ui()` 返回 Dock 数组；若只需单面板，也可继续返回 `controls()`。

`ui()` 下的 dock 现在也支持显式 `layout`。默认 demo 已切到 `layout.kind = "table"`，
用 `rows/cells` 声明控件在 dock 内的排布。

`ui()` + `table layout` 示例：

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
      },
      layout = {
        kind = "table",
        columns = 2,
        borders = false,
        resizable = true,
        row_bg = false,
        sizing = "stretch",
        rows = {
          {
            { control = "read_version" },
            { control = "device_id" },
          },
        }
      }
    }
  }
end
```

`controls()` 返回值示例：

```lua
function controls()
  return {
    { type = "button", id = "read_version", label = "读取版本" },
    { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
    { type = "checkbox", id = "hex_send", label = "HEX 发送", default = true },
    { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
    { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
    { type = "input_float", id = "scale", label = "缩放", default = 1.0 }
  }
end
```

当前支持的类型固定为：

- `button`
- `input_text`
- `input_int`
- `input_float`
- `checkbox`
- `combo`

约定说明：

- `id` 与 `label` 必填
- `combo` 必须提供 `options`
- `combo.default` 使用 Lua 习惯的 `1` 基下标

## 最小可运行脚本示例

默认脚本 `protocols/templates/default_protocol/main.lua` 已是可运行示例，覆盖了：

- 两个 table layout dock
- 一个按钮
- 一个文本输入
- 一次发送
- 一次定时器
- 一次事件输出

脚本核心片段如下：

```lua
local rx_buffer = {}

function on_control(ctx, id, value)
  if id == "read_version" and value then
    rx_buffer = {}
    proto.send({ 0xAA, 0x55, 0x30, 0x01, 0x0D })
    proto.set_timer("read_version_timeout", proto.get_control("timeout_ms") or 1000)
  end
end

function on_bytes(ctx, bytes)
  for i = 1, #bytes do
    rx_buffer[#rx_buffer + 1] = bytes[i]
  end

  if #rx_buffer >= 2 and rx_buffer[1] == string.byte("O") and rx_buffer[2] == string.byte("K") then
    local frame_size = #rx_buffer
    rx_buffer = {}
    proto.cancel_timer("read_version_timeout")
    proto.emit("frame", { status = "ok", size = frame_size })
  end
end
```

当前默认协议示例已经演示了“脚本侧跨包缓冲”的最小做法：即便 `OK` 被拆成多次 `on_bytes()` 回调，也会先累计再判定。

## 配置热重载说明

配置文件中新增了独立开关：

```yaml
app:
  config_hot_reload:
    enabled: false
```

行为约定：

- 默认关闭，不会自动轮询并重载磁盘配置
- 开启后，检测到外部文件变更时只提示，不会自动覆盖当前内存态
- 若当前窗口存在未保存改动，会阻止自动保存继续覆盖外部修改
- 用户需显式点击 `重载配置` 或 `忽略本次外部更新提示`

## 错误处理与调试

- 缺失脚本文件
  - 脚本不会加载成功，UI 会显示加载错误
- `controls()` 返回结构非法
  - 视为脚本加载失败
- 缺失某个事件回调
  - 允许缺省，不视为失败
- 某个回调运行时报错
  - 只记录错误日志，宿主继续运行
- 配置文件 YAML 解析失败
  - 主程序回退到默认配置，并把错误写回状态栏

调试建议：

- 先看状态栏与接收日志里的 `Lua` / `SYS` 行
- 再看控件交互是否触发了 `proto.emit()` 或 `proto.log()`
- 若怀疑脚本收包逻辑有问题，可先在 `on_bytes()` 里直接输出 `#bytes`

## 模块边界

- `src/transport`：统一传输接口与 TCP/Serial 实现
- `src/scripting`：Lua VM、脚本装载、宿主 API 与回调桥接
- `src/dock`：Dock 状态模型与接收行缓存
- `src/app`：主循环装配、Transport/Script/Dock 事件泵
- `src/protocol_utils`：HEX 编解码、CRC 与输入归一化
- `src/ui`：ImGui Dock 渲染与配置/发送交互

---
---

## 配置文件参考

配置文件路径：config/protoscope.yaml，YAML 格式。以下列出所有可配置项及其含义。

### app —— 应用运行时

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| language | string | zh-CN | 界面语言 |
| fps_limit | uint32 | 60 | 渲染帧率上限 |
| idle_render | string | dirty_only | 闲置渲染模式 |
| auto_save.enabled | bool | false | 配置自动保存开关 |
| auto_save.interval_ms | uint64 | 5000 | 自动保存间隔（毫秒） |
| config_hot_reload.enabled | bool | false | 外部配置变更检测开关 |

### scripting —— Lua 宿主与文件 IO

**pipeline**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| pipeline.worker_threads | size_t? | 缺省自动 | 后处理线程池线程数；缺省为 `max(1, hardware_concurrency - 1)`，显式配置会裁剪到机器上限 |

**worker**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| worker.enabled | bool | true | 是否启用脚本工作线程 |
| worker.rx_queue_limit_bytes | size_t | 67108864 | 脚本工作线程 RX 队列上限（字节） |
| worker.output_queue_limit | size_t | 65536 | 脚本输出队列上限 |
| worker.batch_bytes | size_t | 262144 | 每轮从 worker 批量提交到宿主的字节预算 |

**file_io**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| file_io.enabled | bool | true | 是否启用脚本文件 IO 能力 |
| file_io.allow_protocol_dir | bool | true | 是否允许访问协议目录 |
| file_io.allow_dialog_paths | bool | true | 是否允许访问文件对话框路径 |
| file_io.extra_allowed_roots | list<string> | [] | 额外允许访问的根目录 |
| file_io.max_open_files | size_t | 8 | 同时打开文件数量上限 |
| file_io.default_chunk_bytes | size_t | 65536 | 默认文件分块大小 |
| file_io.max_chunk_bytes | size_t | 1048576 | 文件分块大小上限 |
| file_io.max_file_size_bytes | uint64 | 1073741824 | 可读文件大小上限 |
| file_io.max_write_file_size_bytes | uint64 | 1073741824 | 可写文件大小上限 |
| file_io.dialog.enabled | bool | true | 是否启用文件对话框能力 |
| file_io.dialog.remember_last_dir | bool | true | 是否记住上次目录 |
| file_io.send_file.default_chunk_bytes | size_t | 65536 | `proto.fs.send_file()` 默认分块大小 |
| file_io.send_file.max_inflight_chunks | size_t | 2 | `proto.fs.send_file()` 最大在途分块数 |

### gui —— 界面与波形

**window**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| window.title | string | ProtoScope | 窗口标题 |
| window.width | int | 1600 | 窗口初始宽度 |
| window.height | int | 900 | 窗口初始高度 |
| window.maximized | bool | false | 启动时是否最大化 |

**wave**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| wave.max_render_points_per_channel | size_t | 1200 | 每通道目标渲染点预算，控制主波形区最多显示多少个点/包络桶 |
| wave.max_render_vertices | size_t | 60000 | 总渲染顶点预算，多通道或开启磷光辉光时会进一步压缩每通道可显示点数 |
| wave.downsample_start_multiplier | double | 2.0 | 降采样启动倍数；源样本数超过“渲染点预算 × 该系数”后切换到包络降采样 |
| wave.channel_double_click_action | string | reset_scale_offset | CH 卡片双击恢复行为：`reset_all`、`reset_scale_offset`、`reset_scale`、`reset_offset` |
| wave.hidden_channel_policy | string | visible_only | 主图 Legend 隐藏通道策略：`visible_only` 表示隐藏通道不参与主图包络、概览图和 Y 轴自动缩放；`include_hidden` 表示仍参与 |
| wave.overview_max_samples | size_t | 20000 | 总览图最多参与绘制的最近样本数，限制长历史下 overview 的开销 |
| wave.min_visible_time_span | double | 0.001 | 最小可见时间跨度（秒） |
| wave.show_axis_labels | bool | false | 显示坐标轴标签 |

**realtime_backlog**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| realtime_backlog.mode | string | responsive | 实时 backlog 策略：`responsive` 断开时丢弃未显示的 UI 派生 backlog，`complete` 继续小步补完 |
| realtime_backlog.rx_chunk_bytes_per_pump | size_t | 65536 | 单轮主循环最多解析的 RX 字节数，大包会拆分到后续 pump |
| realtime_backlog.transfer_frame_rows_per_pump | size_t | 2000 | 单轮最多提交到逐帧收发视图的行数 |
| realtime_backlog.plot_appends_per_pump | size_t | 4096 | 单轮最多提交到波形缓冲的 `proto.plot.push` append 请求数 |
| realtime_backlog.discard_backlog_on_disconnect | bool | true | `responsive` 模式下断开连接时是否立即丢弃未显示的实时 UI backlog |

#### 波形点数配置关系

波形相关配置里最容易混淆的是下面 3 个概念：

1. **历史采样点数**
   - 指每个通道最多在内存里保留多少原始样本。
   - 这个值**不是** `gui.wave.*` 里的 YAML 配置项，而是协议脚本里 `proto.plot.setup()` 的 `view.history_limit`。
   - 超过上限后，最旧的样本会被裁掉。

2. **当前视口源样本数**
   - 指当前时间窗内，真正落入可视范围的原始样本数。
   - UI 工具栏里的 `源样本` 显示的就是这个数量。
   - 这个值会随着缩放、拖动、时间轴模式变化而变化。

3. **最终显示点数**
   - 指本帧真正送给 ImPlot 渲染的点数。
   - UI 工具栏里的 `渲染点` 显示的就是这个数量。
   - 当视口内样本较少时，直接画原始点；样本过多时，会先降采样成包络点再绘制。

可以把链路理解为：

```text
proto.plot.setup().view.history_limit
    -> 决定每通道最多保留多少原始样本
    -> 当前视口内会取出一部分样本，形成“源样本”
    -> 如果源样本过多，按 gui.wave.* 的预算做降采样
    -> 得到最终“渲染点”
```

#### 历史采样点数：`view.history_limit`

如果你想控制“每个通道最多累计多少采样点”，要在协议脚本里配置：

```lua
proto.plot.setup({
  source = "default_protocol",
  channels = {
    { label = "CH1", unit = "V" },
  },
  view = {
    time_scale = 0.2,
    time_unit = "s",
    vertical_min = -1.5,
    vertical_max = 1.5,
    vertical_unit = "V",
    history_limit = 12000,
  }
})
```

- `history_limit` 是**每通道历史保留上限**。
- 例如设成 `12000`，表示单个通道最多保留 `12000` 个样本；新样本继续进入时，最旧的样本会被丢弃。
- 这个值影响的是“能保留多少采样历史”和内存占用，不直接决定屏幕上一次显示多少点。

#### 显示点数与降采样：`gui.wave.*`

`config/protoscope.yaml` 里的 `gui.wave.*` 负责控制“显示预算”和“何时降采样”：

```yaml
gui:
  wave:
    channel_card_width_mode: fixed
    channel_double_click_action: reset_scale_offset
    hidden_channel_policy: visible_only
    channel_card_fixed_width: 128.0
    channel_card_adaptive_ratio: 0.22
    vertical_auto_fit_multiplier: 1.2
    max_render_points_per_channel: 1200
    max_render_vertices: 60000
    downsample_start_multiplier: 2.0
    overview_max_samples: 20000
```

主波形区每通道的实际渲染点预算由下面 3 个约束一起决定：

```text
每通道渲染点预算
  = min(
      当前绘图区像素宽度,
      wave.max_render_points_per_channel,
      wave.max_render_vertices / (通道数 × 每点估算顶点数)
    )
```

说明：

- `wave.max_render_points_per_channel`
  - 直接限制主波形区每个通道最多显示多少个点或包络桶。
  - 调大后细节保留更多，但渲染负担也更高。

- `wave.max_render_vertices`
  - 是总顶点保护阈值，用来避免单帧绘制过重。
  - 通道越多、开启磷光辉光后每个点估算的顶点数越高，最终能分给每个通道的点数就越少。
  - 所以多通道场景下，即使 `max_render_points_per_channel` 很大，最终显示点数也可能被它压下来。

- `wave.downsample_start_multiplier`
  - 用来控制什么时候从“直接画原始点”切到“先做包络降采样再画”。
  - 触发条件可以理解为：

```text
当 源样本数 > 每通道渲染点预算 × wave.downsample_start_multiplier
时，开始降采样
```

  - 例如预算是 `1200`，系数是 `2.0`，那么源样本数超过约 `2400` 时，会改为画包络。
  - 调大这个值：更晚触发降采样，细节更多，但更吃性能。
  - 调小这个值：更早触发降采样，性能更稳，但显示会更“概览化”。

- `wave.overview_max_samples`
  - 只影响底部总览图 overview，不影响主波形区的历史保留上限。
  - 当历史很长时，overview 只取最近的这部分样本参与绘制，避免总览图拖慢界面。

- `wave.channel_card_width_mode`
  - 控制顶部 CH 卡片宽度，默认 `fixed` 使用 `channel_card_fixed_width`，可改为 `adaptive` 按 `channel_card_adaptive_ratio` 随内容宽度计算。

- `wave.channel_double_click_action`
  - 控制顶部 CH 卡片双击恢复行为，默认 `reset_scale_offset` 只恢复当前通道 `scale/offset`，也可设为 `reset_all`、`reset_scale` 或 `reset_offset`。

- `wave.hidden_channel_policy`
  - 控制通过主图右键 `Legend -> Show` 取消勾选的通道是否继续参与派生视图，默认 `visible_only`。
  - `visible_only`：隐藏通道不参与高密度主图包络绘制、概览图绘制、Y 轴双击/Auto Fit 范围计算。
  - `include_hidden`：隐藏通道仍参与概览和缩放，适合只想临时隐藏线条但保留全量范围参考的场景。

- `wave.vertical_auto_fit_multiplier`
  - 控制 Y 轴 Auto Fit 的留白系数，默认 `1.2`，例如显示值范围 `[-10, 5]` 会自动扩到 `[-12, 12]`。

#### 怎么判断该调哪个参数

- 想保留更长历史，或者“旧波形太快被顶掉”
  - 调大 Lua 里的 `view.history_limit`

- 想让主波形区显示更多细节、少一点包络感
  - 先调大 `wave.max_render_points_per_channel`
  - 再按需要调大 `wave.downsample_start_multiplier`

- 想降低多通道、高刷新率下的渲染压力
  - 先调小 `wave.max_render_points_per_channel`
  - 或者调小 `wave.downsample_start_multiplier`
  - 如果是 overview 卡顿，再调小 `wave.overview_max_samples`

- 发现单通道设置很高，但多通道时显示点数还是不多
  - 检查 `wave.max_render_vertices`
  - 通道数变多后，它会按总预算把每通道点数继续压缩

#### 推荐配置示例

**1）高细节调试**

适合单通道或少量通道，优先看清尖峰、边沿和细节：

```yaml
gui:
  wave:
    max_render_points_per_channel: 2400
    max_render_vertices: 120000
    downsample_start_multiplier: 3.0
    overview_max_samples: 50000
```

```lua
proto.plot.setup({
  channels = {
    { label = "CH1", unit = "V" },
  },
  view = {
    history_limit = 20000,
  }
})
```

**2）长时间运行、偏稳定低负载**

适合长时间挂着看趋势，优先保证界面稳定和资源可控：

```yaml
gui:
  wave:
    max_render_points_per_channel: 800
    max_render_vertices: 40000
    downsample_start_multiplier: 1.5
    overview_max_samples: 8000
```

```lua
proto.plot.setup({
  channels = {
    { label = "CH1", unit = "V" },
    { label = "CH2", unit = "V" },
  },
  view = {
    history_limit = 6000,
  }
})
```

如果你看到工具栏里 `源样本` 很大、`渲染点` 明显更小，说明当前视口已经进入降采样绘制路径；这是预期行为，不代表数据丢失，只是显示层做了包络压缩。

**其他**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| lua_dock_layout_debug | bool | false | Lua Dock 布局调试开关 |

### protocol —— 协议脚本

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| root_dir | string | protocols/templates | 协议模板根目录 |
| selected_dir | string | protocols/templates/default_protocol | 当前选中的协议目录 |

### logging —— 日志

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| level | enum | info | 日志级别：debug / info / warn / error |
| file_path | string | (空) | 日志文件路径，为空则不写文件 |

### communication —— 通讯连接

**通用**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| kind | enum | tcp_client | 通讯类型：tcp_client / tcp_server / serial |

**tcp_client**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| tcp_client.host | string | 127.0.0.1 | 目标地址 |
| tcp_client.port | uint16 | 9000 | 目标端口 |

**tcp_server**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| tcp_server.bind_address | string | 0.0.0.0 | 绑定地址 |
| tcp_server.port | uint16 | 9000 | 监听端口 |
| tcp_server.reject_new_connection | bool | true | 拒绝新连接 |

**serial**：

| 键 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| serial.port_name | string | COM1 | 串口名称 |
| serial.baud_rate | uint32 | 115200 | 波特率 |
| serial.data_bits | uint32 | 8 | 数据位 |
| serial.parity | string | none | 校验位：none / even / odd / mark / space |
| serial.stop_bits | string | one | 停止位：one / onepointfive / two |
| serial.flow_control | string | none | 流控制：none / hardware / software |

### 完整默认配置文件参考

[config/protoscope.yaml](config/protoscope.yaml)
