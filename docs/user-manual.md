# ProtoScope 用户手册

ProtoScope 是一个面向串口、TCP 和 UDP 调试场景的协议联调工具。它把通讯连接、Lua 协议脚本、收发日志和波形显示放在同一个桌面界面里，适合用来调试嵌入式设备、半双工协议、二进制报文和需要实时观察的采样数据。

本手册面向使用 ProtoScope 的用户，重点说明如何启动程序、连接设备、选择协议脚本、观察收发数据、使用波形视图，以及如何基于内置模板扩展自己的 Lua 协议。

## 适用场景

- 调试 TCP Client、TCP Server、串口或 UDP Peer 字节流通讯。
- 通过 Lua 脚本把原始字节解析成业务事件、动态控件和波形通道。
- 手动发送 HEX 或文本报文，并保存常用发送历史。
- 查看 RX/TX 原始收发记录、逐帧解析结果、系统日志和 Lua 日志。
- 录制、导入和导出完整原始数据，用于离线回放和问题复现。
- 载入 ELF 或 ElfStaticView 导出数据，为 Lua 控件提供静态符号地址候选。

## 基本概念

### 通讯方式

ProtoScope 当前支持 4 种通讯方式：

- `TCP 客户端`：连接远端设备或服务。
- `TCP 服务端`：监听端口，等待设备或工具主动连接。
- `串口`：选择 COM 口、波特率、数据位、校验位、停止位和流控。
- `UDP Peer`：绑定本地地址和端口，并向指定远端地址发送数据。

无论底层使用哪种通讯方式，Lua 协议脚本看到的都是统一的字节流事件。

### 协议脚本

协议脚本是一个目录，入口文件固定为 `main.lua`。程序加载协议后，会向 Lua 注入全局 `proto` API，脚本可以声明界面控件、发送数据、处理接收字节、启动定时器、弹出对话框、读写文件和推送波形。

源码中的默认协议资源位于 `protocols/` 顶层；打包运行时会把内嵌默认协议释放到可执行目录下供程序加载。

```text
protocols/
├── protoscope_api.lua
├── stream_types.lua
├── default_protocol/
│   └── main.lua
├── lua_waveform_demo/
│   └── main.lua
├── half_duplex_modbus_master/
│   └── main.lua
├── half_duplex_modbus_slave/
│   └── main.lua
└── templates/
    ├── file_dialog/
    ├── request_guarded/
    └── send_file/
```

### Dock 面板

主界面由多个 Dock 面板组成。内置面板包括：

- `通讯配置`：选择通讯模式并连接或断开。
- `协议脚本 / 动态控件`：选择协议根目录和协议目录，重新扫描或重新加载协议。
- `收发数据`：查看 RX/TX 记录，切换原始数据和逐帧视图，手动发送数据。
- `日志`：查看系统日志。
- `脚本`：查看 Lua 日志和脚本事件。
- `波形`：查看 Lua 推送的多通道波形、总览、游标测量和 FFT 频谱。
- Lua 动态 Dock：由当前协议脚本的 `ui()` 返回值创建。

### 快捷键

常用快捷键会在顶部菜单和 `帮助 -> 快捷键说明` 中显示。全局快捷键只在普通浏览态生效，输入框、文件路径框和弹窗会优先获得键盘控制权。

- `Ctrl+S`：保存配置。
- `Ctrl+R`：重新加载配置。
- `F5`：重新加载当前协议。
- `Ctrl+O`：打开 ELF/ElfStaticView 数据文件。
- `Ctrl+I` / `Ctrl+E`：导入原始波形 / 导出当前缓存快照。
- `Ctrl+Shift+R`：开始或停止完整原始数据录制。
- `Ctrl+1` 到 `Ctrl+6`：切换通讯配置、协议脚本、收发数据、日志、脚本、波形 Dock。
- 波形 Dock 聚焦时，`Space` 暂停或恢复自动跟随，`A` 适配可见波形，`Z` 切换框选放大，`F` 切换 FFT，`Ctrl+Shift+C` 清空波形历史。

## 启动程序

如果已经拿到编译好的程序，直接运行 `ProtoScope` 可执行文件即可。

如果需要从源码构建，先初始化子模块，再配置和构建：

```powershell
git submodule update --init --recursive
cmake -S . -B build -G "Ninja"
cmake --build build
```

默认会构建 GUI 程序。Windows 下目标名为 `ProtoScope.exe`；实际输出目录由 CMake 生成器决定，Ninja 构建通常在 `build/src/` 下。

## 快速上手

### 1. 选择通讯方式

打开 `通讯配置` 面板，在 `模式` 中选择通讯方式。

TCP 客户端需要填写：

- `主机`
- `端口`

TCP 服务端需要填写：

- `监听地址`
- `监听端口`
- 是否 `拒绝新连接`

串口需要填写：

- `端口`
- `波特率`
- `数据位`
- `奇偶校验`
- `停止位`
- `流控`

UDP Peer 需要填写：

- `本地地址`
- `本地端口`
- `远端地址`
- `远端端口`

确认参数后点击 `连接`。连接成功后，状态栏和通讯配置面板会显示当前连接状态、TX 计数和 RX 计数。

### 2. 选择协议脚本

打开 `协议脚本 / 动态控件` 面板：

1. 设置 `协议根目录`。运行时默认指向可执行目录下的 `protocols/templates`；源码示例位于 `protocols/default_protocol` 等顶层目录。
2. 在 `协议目录` 中选择一个包含 `main.lua` 的协议目录。
3. 点击 `重新扫描协议目录` 更新列表。
4. 点击 `重新加载协议` 应用当前脚本。

协议加载成功后，面板会显示入口脚本路径。如果脚本声明了动态 Dock 或控件，它们会出现在主界面中，也会出现在顶部菜单的 `Lua视图` 下。

### 3. 手动发送数据

打开 `收发数据` 面板底部的发送区域：

- 关闭 `HEX` 时，输入内容按文本发送。
- 开启 `HEX` 时，输入内容会按十六进制字节发送。
- HEX 模式会自动整理成大写双字节分组；如果输入奇数个 nibble，发送会被拒绝。
- 点击 `发送` 后，本次内容会进入发送历史。

发送和接收记录会显示在上方列表中。可以使用关键字过滤、RX/TX 状态过滤，也可以切换 `原始` 和 `逐帧` 显示模式。

### 4. 查看日志

ProtoScope 将日志分为三类：

- `收发数据日志`：RX/TX 原始记录或解析后的帧记录。
- `系统日志`：连接、配置、协议加载、文件导入导出等宿主事件。
- `Lua 日志`：脚本通过 `proto.log()` 或回调产生的事件。

日志面板支持关键字过滤、等级过滤、导出和清空。顶部菜单 `设置 -> 日志等级` 可以切换调试、信息、警告和错误等级。

### 5. 查看波形

Lua 脚本通过 `proto.plot.setup()` 创建通道，再通过 `proto.plot.push()` 推送采样点。`波形` 面板会显示这些通道。

常用操作：

- 通过通道卡片查看通道状态，设置激活通道。
- 在通道卡片里修改标签、缩放和偏移。
- 使用总览区域快速定位长时间数据。
- 双击主图 X 轴会缩放到当前仍保留的完整历史；如需旧行为，可把 `gui.wave.x_axis_double_click_action` 设为 `fit_visible_window`。
- 双击主图 Y 轴会按当前可见通道的数值范围自动缩放，并应用 `gui.wave.vertical_auto_fit_multiplier` 留出余量。
- 使用游标和测量覆盖层观察时间差和值差。
- 启用 FFT 频谱模式查看当前可视区频谱。
- 通过 `文件 -> 导入原始波形...` 一次性导入 `.psraw` 文件，立即重建波形结果。
- 通过 `文件 -> 载入原始回放时间轴...` 载入 `.psraw` 事件流，再使用 `回放` 菜单继续、暂停、单步、倍速或定位。
- 通过 `文件 -> 导出当前缓存快照...` 保存当前可回放窗口里的原始数据。
- 通过 `文件 -> 开始完整原始数据录制...` 录制完整原始字节历史。

`导入原始波形` 适合快速查看离线结果；`载入原始回放时间轴` 适合按原始事件时间轴复现现场串口输入。实时接收时，界面只保留配置允许的最近一段原始字节；普通导出明确是当前缓存快照，也就是当前可回放窗口，不代表全历史。需要完整现场复现时，应使用 `开始完整原始数据录制` 生成完整 `.psraw`，或导出现场会话包。

### 6. 保存和重新加载配置

ProtoScope 使用 YAML 配置保存窗口、通讯、协议、日志、波形和发送历史等状态。默认配置文件参考 `config/protoscope.yaml`。

常用入口：

- `文件 -> 保存配置`
- `文件 -> 重新加载配置`
- `通讯配置 -> 保存配置`

如果配置文件在外部被修改，并且启用了外部配置变更提醒，状态栏会提示用户重新加载。

#### 性能配置总览

`performance.scale` 是公共吞吐预算系数，默认 `1.0`。推荐只使用 `0.5`、`1.0`、`2.0` 这类简单值：值越大，接收、脚本 worker 和实时 UI backlog 的缺省预算越大；值越小，处理更保守。`performance.scale <= 0` 会按 `1.0` 回退。

公共系数只作用于没有在 YAML 中显式写出的预算项。写出单项后，单项优先级更高，例如：

```yaml
performance:
  scale: 2.0
scripting:
  worker:
    batch_bytes: 131072
```

上例中，未写出的 RX 队列、内存预算和实时 backlog 预算会按 `2.0` 放大；`batch_bytes` 固定为 `131072`，不再被公共系数覆盖。整数预算使用“默认值 × scale”四舍五入，非零默认值至少保留 `1`；显式写 `0` 时仍保留该单项自己的特殊含义。

受公共系数影响的缺省项包括：`receive.transport_read_buffer_bytes`、`scripting.worker.rx_queue_limit_bytes`、`scripting.worker.memory_budget_bytes`、`scripting.worker.output_queue_limit`、`scripting.worker.batch_bytes`、`scripting.worker.output_flush_budget_ms`、`gui.realtime_backlog.rx_chunk_bytes_per_pump`、`gui.realtime_backlog.transfer_frame_rows_per_pump`、`gui.realtime_backlog.plot_appends_per_pump`、`gui.realtime_backlog.raw_first_backlog_warn_bytes`。

不受公共系数影响的项仍按单项调优，包括 `app.fps_limit`、`gui.wave.*` 渲染和降采样项、日志历史保留、文件 IO 分块、`gui.realtime_backlog.pump_min_interval_ms`、布尔开关和策略字符串。

#### 分块发送与 UI 追赶

`scripting.worker.batch_bytes` 是 worker 分块主控项。它会限制相邻 RX 字节事件合并后的最大字节数，并间接决定 worker 每次输出给宿主/UI 的 batch 粗细。

如果要调“worker 给 UI 的块粗细”，优先调 `scripting.worker.batch_bytes`；如果要调“UI 每帧追赶速度”，再调 `gui.realtime_backlog.rx_chunk_bytes_per_pump`、`gui.realtime_backlog.transfer_frame_rows_per_pump`、`gui.realtime_backlog.plot_appends_per_pump` 和 `scripting.worker.output_flush_budget_ms`。

## 内置协议模板

内置示例位于 `protocols` 顶层：

- `default_protocol`：最小可运行协议，演示控件、发送、定时器、事件输出和基础波形。
- `lua_waveform_demo`：纯 Lua 生成多通道波形，不依赖外部设备，适合验证波形视图。
- `half_duplex_modbus_master`：半双工主机示例，演示 `proto.request()`、ACK 完成、逐帧解析和波形上传。
- `half_duplex_modbus_slave`：半双工从机示例，演示请求解析、ACK 或异常帧返回和批量波形上传。

可复制操作模板位于 `protocols/templates`：

- `file_dialog`：文件/目录对话框示例。
- `request_guarded`：受保护请求示例。
- `send_file`：文件分块发送示例。

创建自己的协议时，建议复制一个示例或模板目录并改名，然后保留 `main.lua` 作为入口。

## Lua 脚本能力速览

### UI 和控件

协议脚本可以通过 `ui()` 声明一个或多个动态 Dock。控件类型包括：

- `button`
- `input_text`
- `input_int`
- `input_float`
- `checkbox`
- `combo`
- `elf_symbol_combo`

布局支持普通顺序渲染，也支持 `table` 和 `form`。多个 Dock 可以通过 `tab_group` 合并到同一组选项卡。

### 常用回调

- `on_open(ctx)`：连接打开。
- `on_close(ctx, reason)`：连接关闭。
- `on_error(ctx, message)`：通讯错误。
- `on_control(ctx, id, value)`：动态控件变化。
- `on_bytes(ctx, bytes)`：收到原始字节。
- `on_timer(ctx, name)`：定时器触发。
- `on_tx(ctx, evt)`：发送或请求状态变化。
- `on_dialog(ctx, evt)`：脚本弹窗关闭。
- `on_file_dialog(ctx, evt)`：文件或目录对话框返回。

### 常用 `proto` API

- `proto.send(payload, opts?)`：普通异步发送。
- `proto.request(payload, opts?)`：半双工请求，由宿主管理排队、超时和串行发送。
- `proto.request_guarded(payload, opts?)`：受保护半双工请求，`max_attempts` 只统计当前请求；最终失败后熔断后续 guarded 请求。
- `proto.reset_request_guard()`：解除 guarded 熔断，新的 guarded 请求从 `attempt=1` 重新开始。
- `proto.request_done(result?)`：脚本确认当前请求已经收到完整业务应答。
- `proto.emit(name, payload)`：输出脚本事件。
- `proto.set_timer(name, interval_ms)` / `proto.cancel_timer(name)`：管理定时器。
- `proto.status.set(text, opts?)` / `proto.status.clear()`：更新状态提示。
- `proto.ui.alert(opts)` / `proto.ui.confirm(opts)`：弹出脚本对话框，支持可选 `window` 子表配置初始宽高、位置、是否可拖动和是否自动尺寸。
- `proto.fs.*`：打开文件对话框、读写文件、查询文件状态和分块发送文件；文件对话框仍使用系统原生窗口，不支持 Lua 控制宽高、位置或拖动能力。
- `proto.plot.setup(config)` / `proto.plot.push(channel, data)`：声明并推送波形。
- `proto.crc16_modbus()`、`proto.crc16_ccitt_false()`、`proto.crc32_ieee()`：常用校验计算。

更完整的脚本说明请参考 `protocols/README.md` 和 `protocols/protoscope_api.lua`。

## ELF / ElfStaticView 数据

顶部菜单 `文件 -> 打开 ELF/ElfStaticView 数据文件...` 可以载入 `.elf`、`.out`、`.axf`、`.json` 或 `.esv` 文件。

载入后，Lua 控件中的 `elf_symbol_combo` 可以基于该数据搜索静态符号。这个能力适合协议参数中需要选择变量地址、全局符号或调试符号的场景。

## 菜单参考

### 文件

- `保存配置`
- `重新加载配置`
- `重新加载协议`
- `打开 ELF/ElfStaticView 数据文件...`
- `导入原始波形...`
- `导出当前缓存快照...`
- `开始完整原始数据录制...`
- `停止完整原始数据录制`

### 视图

- 显示或隐藏 `通讯配置`
- 显示或隐藏 `协议脚本 / 动态控件`
- 显示或隐藏 `收发数据`
- 显示或隐藏 `日志`
- 显示或隐藏 `脚本`
- 显示或隐藏 `波形`
- 重置当前协议 Dock 布局

### 设置

- `日志等级`：调试、信息、警告、错误。

### Lua视图

显示或隐藏当前协议脚本声明的动态 Dock。

### 帮助

- `检查更新`
- `关于 ProtoScope`

## 常见问题

### 连接不上设备怎么办？

先检查通讯模式是否正确，再确认地址、端口或串口参数。串口模式下可以点击 `刷新串口列表`。如果仍然失败，查看 `日志` 面板里的错误信息。

### HEX 发送为什么被拒绝？

HEX 模式要求输入能组成完整字节。比如 `0A FF 10` 是合法输入，`0A F` 会因为最后只有半个字节而被拒绝。

### 协议目录为什么加载失败？

确认选择的目录下存在 `main.lua`。如果 Lua 语法或运行时报错，错误会显示在 `协议脚本 / 动态控件` 面板和 `脚本` 日志里。

### 为什么 Lua 动态 Dock 没有出现？

确认协议脚本提供了 `ui()`，并且重新加载了协议。也可以从顶部菜单 `Lua视图` 检查对应 Dock 是否被隐藏。

### 波形没有数据怎么办？

确认当前脚本已经调用 `proto.plot.setup()` 声明通道，并通过 `proto.plot.push()` 推送采样点。可以先加载 `lua_waveform_demo` 模板验证波形视图是否正常。

### 如何复现一次现场问题？

调试时可以使用 `文件 -> 开始完整原始数据录制...` 保存完整 `.psraw` 文件。快速复盘时使用 `文件 -> 导入原始波形...` 一次性重建波形；需要按现场节奏复现时，使用 `文件 -> 载入原始回放时间轴...`，再在 `回放` 菜单中继续、暂停、单步、倍速或定位到开头/中点/末尾。复现时结合收发日志和脚本日志定位问题。

## 进一步阅读

- `README.md`：项目构建、配置和 Lua API 总览。
- `protocols/README.md`：Lua 协议脚本详细指南。
- `protocols/protoscope_api.lua`：LuaLS 类型提示和宿主 API 声明。
- `protocols/stream_types.lua`：`stream()` schema 类型提示。
- `docs/transport-extension.md`：新增通讯方式的开发说明。
- `docs/lua-host-integration.md`：Lua 宿主 API 接入说明。
