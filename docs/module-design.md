# src/include 模块设计导览

本文档面向后续维护者，用于快速理解 `include/protoscope` 与 `src` 下各模块的设计风格、对外接口、工作职责和维护边界。

阅读代码时可以按下面的规则建立第一印象：

- `include/protoscope/<module>` 是模块公开契约，放对外可见的数据模型、接口类和少量可复用纯函数。
- `src/<module>` 是模块实现，放具体流程、平台细节、第三方库调用和内部拆分。
- `src/CMakeLists.txt` 是模块依赖关系的真实入口；新增模块或拆分 target 时先看这里。
- `Application`、`GuiRuntime`、`ScriptHost`、`DockStore`、`OscilloscopeBuffer`、`ITransport` 是当前工程的主要边界对象。

## 总体依赖方向

当前工程的运行链路可以粗略看成：

```text
transport events
      |
      v
Application <-> ScriptHost
      |             |
      v             v
  DockStore     OscilloscopeBuffer / WaveDockState
      \             /
       \           /
             ui
```

更准确地说：

- `transport` 只负责 I/O 和字节流事件，不依赖 UI、Lua 或波形。
- `scripting` 只通过宿主 API 处理协议逻辑，不直接操作 ImGui 或底层 socket/串口句柄。
- `plot` 只负责波形数据、视图状态、数学计算和原始采集文件，不直接绘制 Dock。
- `dock` 是 UI 状态仓库，不直接调用 ImGui。
- `ui` 消费 `Application` 和 `DockStore` 绘制界面，是最靠近 ImGui/GLFW/ImPlot 的层。
- `app` 是应用宿主编排层，负责把配置、通讯、脚本、波形、日志和插件桥接在一起。
- `plugin` 通过适配层接入外部能力，避免第三方实现扩散进主应用核心。

维护时尽量保持这个方向，不要让底层模块反向依赖 `ui` 或 `app`。

## CMake target 映射

| Target | 对应模块 | 主要职责 | 维护含义 |
|---|---|---|---|
| `protoscope_protocol_utils` | `protocol_utils` | HEX、CRC 等协议工具 | 纯工具库，适合单元测试 |
| `protoscope_transport` | `transport` | TCP、串口、UDP Peer I/O | 新增通讯方式时改这里和配置/UI |
| `protoscope_plot` | `plot` | 波形缓冲、FFT、原始采集文件 | 不应依赖 UI |
| `protoscope_ui_layout` | `ui` 的布局/状态文件子集 | Lua Dock 布局、协议状态文件、波形状态序列化 | 可脱离 GUI 主运行时测试 |
| `protoscope_scripting` | `scripting` | Lua 宿主、回调、`proto.*` API、stream parser | Lua API 契约最敏感 |
| `protoscope_elf_static_view` | `plugin` | ElfStaticView 数据加载和符号查询 | 插件适配边界 |
| `protoscope_dock` | `dock` | Dock 状态仓库、收发/日志格式化和过滤 | UI 状态模型，不绘制 |
| `protoscope_config` | `config` | YAML 配置读写、默认协议释放 | 配置字段 source of truth |
| `protoscope_logging` | `logging` | 日志门面、Host/Script 日志分流 | 横切基础设施 |
| `protoscope_app` | `app` | 应用编排、主循环泵、配置应用、协议加载、收发调度 | 系统级协调层 |
| `protoscope_update_check` | `ui/update_check` | 版本检查和 GitHub tag 解析 | 可单独测试的 UI 辅助逻辑 |
| `protoscope_ui` | `ui` GUI 主体 | ImGui/GLFW/ImPlot 窗口、菜单、Dock、对话框、波形绘制 | 受 `PROTOSCOPE_ENABLE_GUI` 控制 |
| `ProtoScope` | `src/main.cpp` | 程序入口 | 只负责启动和关闭宿主 |

第三方 target 在 `cmake/Dependencies.cmake` 中封装，例如 `protoscope_lua`、`protoscope_sol2`、`protoscope_asio`、`protoscope_imgui`、`protoscope_implot`、`protoscope_pocketfft` 和 `elf_static_view_core`。业务模块不要直接散落第三方 include path 和链接细节，应优先通过这些 target 使用依赖。

## app

### 公开接口

头文件：`include/protoscope/app/application.hpp`

核心公开类型是 `app::Application`。它提供：

- 生命周期：`initialize()`、`shutdown()`。
- 配置：`applyConfig()`、`captureConfig()`。
- 协议：`reloadProtocolDirectory()`。
- 主循环：`pumpOnce()`。
- 通讯：`openTransport()`、`closeTransport()`、`sendManualPayload()`。
- Lua 控件：`updateControlValue()`、`restoreControlValue()`。
- UI 状态：`docks()`。
- 日志：`logger()`、`setLogLevel()`、`setStatusMessage()`。
- 波形原始数据：`exportWaveRawCapture()`、`importWaveRawCapture()`、`startRawCaptureRecording()`、`stopRawCaptureRecording()`。
- ElfStaticView：`loadElfStaticAddressFile()`、`queryElfStaticAddresses()`。

### 实现职责

源码：`src/app/application.cpp`

`Application` 是宿主编排层。它把这些模块接起来：

- `ConfigStore`：配置加载、应用和捕获。
- `DockStore`：所有 UI 状态和日志状态。
- `ITransport`：通讯打开、关闭、事件泵和发送。
- `ScriptHost`：Lua 脚本加载、回调、TX 请求、弹窗和文件事件。
- `OscilloscopeBuffer` / `WaveDockState`：波形数据和视图状态。
- `LoggingFacade`：系统日志和脚本日志。
- `ElfStaticViewBridge`：静态地址数据。

### 设计风格

`Application` 是明确的门面和协调器，不追求小而纯。它的价值在于集中处理跨模块时序，例如配置变更、协议重载、旧 Lua runtime 输出丢弃、传输事件分发、实时 backlog 泵和原始采集录制。

维护时不要把绘制细节塞进 `Application`；也不要让底层模块为了方便直接反调 UI。跨模块的新流程应优先通过 `Application` 编排。

### 维护注意点

- 改 `applyConfig()` / `captureConfig()` 时，要同步考虑 YAML、Dock 状态和协议状态文件。
- 改协议重载时，要注意旧 `ScriptHost` 的 TX、弹窗、文件请求和波形输出是否会泄漏到新协议。
- 改实时收包链路时，要看 `realtime_backlog` 配置和 UI 响应性，不要在单帧里无限处理大包。
- 改原始数据录制/导入时，要区分“实时保留的最近一段”和“用户显式导入/录制的完整历史”。

## config

### 公开接口

头文件：

- `include/protoscope/config/config.hpp`
- `include/protoscope/config/embedded_protocols.hpp`

主要公开类型：

- `AppConfig`：聚合 `app`、`gui`、`protocol`、`scripting`、`logging`、`communication`。
- `ConfigStore`：加载、保存、规范化路径、协议目录扫描、配置快照、配置与 Dock 状态互转。
- `config::embedded`：可执行目录识别、内嵌协议资源释放、默认协议补齐。

### 实现职责

源码：

- `src/config/config.cpp`
- `src/config/embedded_protocols.cpp`

`config` 负责 YAML 与 C++ 配置结构之间的转换，也负责把配置应用到 `DockStore`，以及从 `DockStore` 捕获当前配置。内嵌协议资源由 CMake 生成的资源表驱动，运行时只补齐缺失文件，不覆盖用户已有协议。

### 设计风格

配置层以直接的数据结构为主，字段默认值写在结构体里，解析时尽量保持容错。`ConfigStore` 是配置读写和路径规则的唯一入口。

### 维护注意点

- 新增配置字段时，要同步结构体默认值、YAML 读写、默认配置文件、文档和测试。
- 配置字段如果影响 UI 状态，要确认 `applyToDock()` 与 `captureFromDock()` 是否需要同步。
- 协议目录相关字段要保持相对路径和可执行目录释放逻辑一致。

## dock

### 公开接口

头文件：`include/protoscope/dock/docks.hpp`

主要公开类型：

- `CommDockState`：通讯面板状态。
- `ReceiveDockState`：收发数据面板状态。
- `LogDockState` / `ScriptDockState`：系统日志和 Lua 日志状态。
- `SendDockState`：手动发送编辑器和历史。
- `LuaDockState`：当前协议脚本、动态 Dock 和控件快照。
- `ConfigDockState`：配置文件状态、脏标记和外部冲突提示。
- `DockStore`：上述状态的统一仓库。

### 实现职责

源码：`src/dock/docks.cpp`

`dock` 管理界面需要展示的数据，但不直接绘制界面。它负责日志行格式化、过滤、历史上限裁剪、TX/RX 记录追加、脚本事件追加、配置脏标记和冲突状态。

### 设计风格

这是一个轻量状态仓库，偏向“plain state + helper function”。它给 UI 提供稳定数据入口，也给 `Application` 和 `LoggingFacade` 提供追加状态的接口。

### 维护注意点

- 不要在 `dock` 里引入 ImGui 依赖。
- 新增 Dock 状态时，优先先定义状态结构，再让 UI 消费它。
- 日志过滤和格式化是可测试逻辑，改动时应补充或更新测试。
- 历史上限裁剪要避免无界增长，尤其是 RX/TX、脚本日志和波形相关状态。

## logging

### 公开接口

头文件：`include/protoscope/logging/logging.hpp`

主要公开类型：

- `LogRecord`：统一日志记录。
- `LoggingFacade`：Host 和 Script 日志入口。

`LoggingFacade` 提供 `debug()`、`info()`、`warn()`、`error()`、`host()`、`script()`，并可绑定 `DockStore`。

### 实现职责

源码：`src/logging/logging.cpp`

日志层把运行时日志写入底层 logger，并按受众写入 `DockStore` 的系统日志或脚本日志。它还负责日志等级过滤和日志 sink 重建。

### 设计风格

这是横切基础设施，使用门面隔离具体日志实现。业务代码不直接依赖 spdlog 细节。

### 维护注意点

- 追加新的日志受众前，先确认是否真的需要新的 UI 面板或过滤维度。
- 日志 sink 失败时要避免递归报错。
- 改日志等级时要同时考虑菜单、配置、运行时 logger 和 Dock 展示。

## protocol_utils

### 公开接口

头文件：`include/protoscope/protocol_utils/codec.hpp`

主要公开函数：

- `bytesToHex()`
- `hexToBytes()`
- `normalizeHexText()`
- `normalizeHexEditorInput()`
- `countHexDigits()`
- `crc16Modbus()`
- `crc16CcittFalse()`
- `crc32Ieee()`

### 实现职责

源码：`src/protocol_utils/codec.cpp`

该模块只处理协议相关的纯工具逻辑：HEX 文本转换、HEX 编辑器归一化和常用 CRC。

### 设计风格

纯函数、无状态、低依赖。它适合作为底层工具被 `transport`、`scripting`、`dock`、`ui` 和测试复用。

### 维护注意点

- 不要把业务协议状态放进这里。
- 修改 HEX 规则时，要同步手动发送编辑器、Lua payload 解析和相关测试。
- CRC 函数应保持输入输出明确，避免引入隐式字节序策略。

## transport

### 公开接口

头文件：`include/protoscope/transport/transport.hpp`

主要公开类型：

- `TransportKind`：`TcpClient`、`TcpServer`、`Serial`、`UdpPeer`。
- `TransportConfig`：上述通讯配置的 `std::variant`。
- `ConnectionContext`：连接来源、端点、连接 ID 和时间戳。
- `TransportEvent`：打开、关闭、错误、字节、TX 状态事件。
- `TransportTxTask` / `TransportTxEvent`：发送任务和发送结果。
- `ITransport`：通讯实现接口。
- `TransportBase`：事件队列、状态和计数基础类。

### 实现职责

源码：

- `src/transport/transport.cpp`
- `src/transport/serial_ports.cpp`
- `src/transport/udp_transport.cpp`

`transport` 负责把 TCP、串口和 UDP Peer 统一成字节流事件。上层只关心 `TransportEvent`，不直接关心 socket、acceptor、serial_port 或 UDP endpoint 的生命周期。

### 设计风格

接口抽象清晰，具体实现封装线程、asio 对象和平台细节。事件通过 `takeEvents()` 批量交给 `Application` 泵，不在底层直接调用 Lua 或 UI。

### 维护注意点

- 新增通讯方式时，要更新 `TransportKind`、`TransportConfig`、`transportDescriptors()`、配置读写、通讯配置 UI 和文档。
- 底层线程退出和资源关闭必须可重复调用，避免关闭时悬挂回调。
- TX 结果要走统一事件，不要让调用方猜测底层发送状态。
- 串口枚举属于平台细节，不要扩散到 UI 外层逻辑。

## scripting

### 公开接口

头文件：

- `include/protoscope/scripting/script_host.hpp`
- `include/protoscope/scripting/frame_stream_parser.hpp`
- `include/protoscope/scripting/file_io_config.hpp`

主要公开类型：

- `ScriptHost`：Lua 宿主运行时、脚本加载、回调、输出队列和 `proto.*` API 桥接。
- `ControlDescriptor` / `ControlSnapshot`：Lua 声明式控件。
- `DockDescriptor` / `DockSnapshot`：Lua 动态 Dock。
- `PlotSetup`、`PlotChannelDescriptor`、`PlotOutput`：Lua 波形输出。
- `TxRequest`、`TxEvent`、`RequestDoneResult`：Lua 发送和半双工请求。
- `DialogRequest`、`FileDialogRequest`、`FileDialogEvent`：脚本弹窗和文件对话框。
- `FrameStreamParser` 相关类型：schema 驱动的流式帧解析。
- `FileIoConfig`：Lua 文件 IO 权限与大小限制。

### 实现职责

源码：

- `src/scripting/frame_stream_parser.cpp`
- `src/scripting/host/script_host_core.cpp`
- `src/scripting/host/script_host_loader.cpp`
- `src/scripting/host/script_callback_dispatcher.cpp`
- `src/scripting/host/script_file_system_facade.cpp`
- `src/scripting/host/*_api_module.cpp`
- `src/scripting/host/stream_schema_module.cpp`

`scripting` 把 Lua 脚本和 C++ 宿主连接起来。它负责：

- 创建 Lua runtime。
- 加载协议 `main.lua`。
- 解析 `ui()`、控件布局、`stream()` schema、波形 setup/push。
- 注册 `proto.log/send/request/request_done/status/ui/fs/plot/control/codec` 等 API。
- 分发 `on_open/on_close/on_error/on_bytes/on_timer/on_control/on_tx/on_dialog/on_file_dialog` 回调。
- 维护 TX 请求队列、定时器、弹窗、文件 IO、文件发送和实时输出丢弃计数。

### 设计风格

`ScriptHost` 是大边界对象，公开面承载 Lua 契约。实现侧已经按 API 模块拆分注册逻辑，避免所有 `proto.set_function()` 都堆在加载流程里。

### 维护注意点

- Lua API 是用户脚本契约；新增、删除或修改 API 时必须同步 `protocols/protoscope_api_manifest.json`，再生成 `protocols/protoscope_api.lua`。
- 新增 `proto.*` 能力时，要按职责放入对应 `*_api_module.cpp`，不要直接写回加载主流程。
- `ScriptHost` 的 drain 队列是宿主和 Lua 的异步边界，改动时要考虑事件顺序、旧 runtime 丢弃和 request 完成语义。
- `stream()` schema 与 `on_bytes()` 是两条输入路径，改解析器时要保持错误事件和帧事件可诊断。
- 文件 IO 必须继续受 `FileIoConfig` 限制，避免脚本无限打开或写入。

## plot

### 公开接口

头文件：

- `include/protoscope/plot/oscilloscope.hpp`
- `include/protoscope/plot/wave_state.hpp`
- `include/protoscope/plot/wave_math.hpp`
- `include/protoscope/plot/wave_fft.hpp`
- `include/protoscope/plot/raw_capture_file.hpp`

主要公开类型：

- `OscilloscopeBuffer`：多通道采样缓存、快照、统计和测量。
- `WaveDockState` / `WaveViewState`：波形 Dock 视图状态、游标、通道配置和缓存。
- `WaveViewport`、`WaveDisplayData`、`WaveLayoutMetrics`：视图计算结果。
- `WaveFftConfig`、`WaveFftFrame`、`WaveFftCache`：FFT 配置和缓存。
- `RawCaptureFileData`、`RawCaptureStreamWriter`：`.psraw` 文件读写、事件流保存和流式录制。

### 实现职责

源码：

- `src/plot/oscilloscope.cpp`
- `src/plot/wave_math.cpp`
- `src/plot/wave_fft.cpp`
- `src/plot/raw_capture_file.cpp`

`plot` 负责波形数据层，不负责 ImGui 绘制。Lua 或导入回放产生的采样点进入 `OscilloscopeBuffer`，UI 从快照和视图状态渲染波形。

### 设计风格

数据处理、视图状态和绘制适配分离。`plot` 保持可测试的纯逻辑：采样追加、降采样/包络、游标测量、FFT、文件编码和解码。`.psraw` 的 v3 event stream 负责保存原始 RX 字节、运行配置事件和绘图 setup 事件；Application 负责把事件流按时间轴重放到脚本/波形链路，UI 只提供载入、继续、暂停、单步、倍速、定位和卸载控制。

### 维护注意点

- 不要把 `scale/offset` 写回原始采样；它们属于显示变换。
- 原始数据一次性导入和原始回放时间轴是两个入口：前者立即重建波形，后者按 `.psraw` 事件顺序复现现场输入。
- 原始数据导入和时间轴回放应保留用户显式导入/录制的完整历史，不应被实时历史上限立即裁掉。
- FFT 基于当前可视区输入，改动时要确认频域视图和时域视图状态不会互相污染。
- `.psraw` 文件格式变更要考虑向后兼容和错误提示。

## plugin

### 公开接口

头文件：`include/protoscope/plugin/elf_static_view_bridge.hpp`

主要公开类型：

- `ElfStaticAddressEntry`
- `ElfStaticViewBridge`

`ElfStaticViewBridge` 提供 `loadFile()`、`query()`、`loaded()`、`sourcePath()`。

### 实现职责

源码：`src/plugin/elf_static_view_bridge.cpp`

该模块把 ElfStaticView 的工程模型、导出数据或 ELF/DWARF 解析结果包装成 ProtoScope 可查询的静态地址候选。Lua 侧通过 `elf_symbol_combo` 使用这些候选。

### 设计风格

PIMPL 适配层，公开面小，第三方模型隐藏在实现文件里。这样可以把 ElfStaticView 依赖限制在插件桥接模块。

### 维护注意点

- 不要把 ElfStaticView 内部类型暴露到 `app`、`scripting` 或 `ui` 的公开头文件。
- 查询语义要和 `elf_symbol_combo` 的输入、消抖和结果上限保持一致。
- 支持新文件格式时，先确认加载错误信息能被 UI 展示。

## ui

### 公开接口

头文件：

- `include/protoscope/ui/gui_runtime.hpp`
- `include/protoscope/ui/ui_component.hpp`
- `include/protoscope/ui/ui_host_context.hpp`
- `include/protoscope/ui/dock_layout.hpp`
- `include/protoscope/ui/protocol_state_file.hpp`
- `include/protoscope/ui/protocol_ui_state.hpp`
- `include/protoscope/ui/wave_dock_renderer.hpp`
- `include/protoscope/ui/editable_combo.hpp`
- `include/protoscope/ui/render_frame_scheduler.hpp`
- `include/protoscope/ui/update_check.hpp`
- `include/protoscope/ui/version_utils.hpp`
- `include/protoscope/ui/icons.hpp`

主要公开类型：

- `GuiRuntime`：GUI 生命周期和主循环。
- `IUiComponent`、`IMenuContributor`、`IDialogComponent`、`IDockComponent`：UI 组件接口。
- `RuntimeUiContext`：UI 组件共享上下文。
- `WorkspaceController`：运行时工作区操作门面。
- `LuaDockLayoutRequest`、`ProtocolWorkspaceSwitchDecision`：Lua Dock 布局和协议工作区切换。
- `ProtocolDockVisibilityState`：协议级 Dock 可见性。
- `WaveDockRenderer`：波形 Dock 绘制入口。
- `UpdateCheckResult`、`SemanticVersion`：更新检查与版本解析。

### 实现职责

源码分组：

- `src/ui/runtime/*`：GUI 生命周期、组件注册、菜单、配置轮询、平台控制、协议工作区控制。
- `src/ui/docks/*`：通讯配置、协议脚本、收发数据、日志、Lua 动态 Dock 绘制。
- `src/ui/dialogs/*`：关于、更新检查、脚本弹窗、文件对话框、原始数据导入导出、日志导出、ELF 文件加载。
- `src/ui/wave/*`：波形工具栏、通道卡片、主图、总览、交互、测量覆盖层、FFT 图层。
- `src/ui/dock_layout.cpp`：Lua Dock 布局和协议工作区布局路径。
- `src/ui/protocol_state_file.cpp` / `src/ui/protocol_ui_state.cpp`：协议级 UI 状态持久化。
- `src/ui/update_check.cpp`：联网检查和 tag 解析。

### 设计风格

UI 层是 ImGui/GLFW/ImPlot 的唯一集中区域。它通过 `Application` 读取和修改业务状态，通过 `DockStore` 获取界面数据，通过 `UiComponentRegistry` 将菜单、Dock 和对话框拆成组件。

### 维护注意点

- UI 文件可以知道 `Application`，但底层模块不应反向知道 UI。
- `GuiRuntime` 已经比较大，新增功能时优先放到 `runtime`、`docks`、`dialogs` 或 `wave` 的对应文件，不要继续堆进核心文件。
- 协议工作区布局和协议 UI 状态是持久化契约，字段变更要考虑旧状态文件恢复。
- 波形交互涉及可见性、hover、游标和 FFT 多个状态，改动后要至少验证可构建和基本人工路径。
- 更新检查解析逻辑应保持可脱离网络测试，联网函数只做边界调用。

## src/main.cpp

`src/main.cpp` 是程序入口。它只负责：

1. 创建 `Application`。
2. 初始化应用。
3. 创建并初始化 `GuiRuntime`。
4. 运行 GUI 主循环。
5. 按顺序关闭 GUI 和应用。

Windows 下额外提供 `wWinMain`，但仍然复用同一个 `runProtoScope()`。

维护时不要把业务逻辑放到入口文件。入口文件应保持薄层。

## 新增功能时的落点建议

| 需求 | 首选落点 | 同步检查 |
|---|---|---|
| 新增通讯方式 | `transport` + `config` + `ui/docks` | `TransportKind`、YAML、通讯配置面板、测试 |
| 新增 Lua API | `scripting/host/*_api_module.cpp` | Manifest、`protoscope_api.lua`、协议文档、测试 |
| 新增波形计算能力 | `plot` | UI 调用点、性能、测试 |
| 新增波形绘制控件 | `ui/wave` | `WaveDockState`、协议状态持久化、人工验证 |
| 新增配置字段 | `config` | 默认 YAML、Dock apply/capture、README 或相关文档 |
| 新增 Dock 面板 | `dock` 状态 + `ui/docks` 绘制 | 可见性状态、菜单、布局持久化 |
| 新增日志视图或导出 | `logging` + `dock` + `ui/dialogs` | 过滤、历史上限、导出格式 |
| 新增插件能力 | `plugin` 适配层 | 不泄漏第三方类型、UI 错误展示、Lua 控件契约 |

## 维护检查清单

提交涉及模块边界时，建议按下面顺序自检：

1. 公开头文件是否只暴露必要类型和函数。
2. 新增依赖是否符合“底层不依赖 UI”的方向。
3. CMake target 是否链接到正确层级，是否引入了不必要的 PUBLIC 依赖。
4. 配置、Lua API、协议状态文件、`.psraw` 文件这类外部契约是否同步文档和测试。
5. 纯逻辑是否能放在 `protocol_utils`、`plot`、`ui_layout` 或小 helper 中测试。
6. UI 改动是否保持 Dock 状态、菜单可见性和布局持久化一致。
7. 日志和错误是否能在用户可见位置呈现。
8. 高速收包、大文件导入、长历史波形这类路径是否有上限或分帧处理。
