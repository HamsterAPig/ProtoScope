# ProtoScope 用户手册

ProtoScope 是一个面向串口、TCP 和 UDP 调试场景的协议联调工具。它把通讯连接、Lua 协议脚本、收发日志和波形显示放在同一个桌面界面里，适合用来调试嵌入式设备、半双工协议、二进制报文和需要实时观察的采样数据。

本手册面向使用 ProtoScope 的用户，重点说明如何启动程序、连接设备、选择协议脚本、观察收发数据、使用波形视图，以及如何基于内置模板扩展自己的 Lua 协议。

## 应用设置窗口

从 **设置 → 应用设置…** 打开单实例、非模态窗口；再次选择菜单只聚焦，不重置草稿。窗口可移动、缩放，不加入 Dock 或协议布局。小窗口使用滚动表单和滚动底部操作区。

- **常规外观**：主题、标题区、交互反馈、提示时长、自动保存、外部修改提示及启动窗口/渲染后端/中文字形。
- **波形偏好**：操作模式、显示公式、格值读数、工程滚轮及加速、通道卡片宽度、双击、隐藏通道、极值吸附、Y 偏移拖动、图例、动画、框选退出和下次全屏策略。不搬迁 FFT、游标、测量或通讯参数。
- **日志历史**：等级、文件、载荷预览、轮转和六类历史容量。相对日志路径以可执行目录为基准；空路径关闭文件输出。载荷预览可能暴露原始数据。
- **性能**：自适应开关和 K、静态 FPS、公共倍率、三个波形预算及低频预算。页面同时显示实际运行自适应状态、压力、原因和有效预算，不能把草稿或静态值误当成实时调度值。

每项旁的 **(?)** 给出作用、内建默认、单位、边界和条件；悬停自动换行显示。恢复本页默认只改变本页草稿，不立即影响运行态。应用期间草稿暂时锁定，导入导出期间禁止应用/保存。

**应用**只提交合法差异，保留其它面板已应用修改，不重载协议、不清波形数据、不自动重连。**保存**先等待真实应用完成，再保存最新完整运行态（包括其它入口修改）；失败时保留草稿、旧文件或真实已应用预算，并显示错误。开启自动保存后，已应用 dirty 可能按间隔落盘；未应用草稿永不自动保存。

同字段被其它入口修改时，选择刷新运行值或明确确认覆盖。缩减历史会展示当前数量和目标容量，须确认不可恢复的裁剪。外部文件变化在保存时始终检查，不受“外部修改提示”开关影响：可明确覆盖、放弃并重载或继续编辑。坏 YAML 重载失败时保留当前运行态与草稿。重载会放弃未保存运行修改并重载协议，请谨慎选择。

启动尺寸、最大化、后端和中文字形显示“待重启”；连接缓冲变化显示“下次连接生效”，不会自动重连。兼容 Y 适配偏移当前不影响适配；波形动画还受交互反馈总开关控制；图例自动折叠仅双击模式生效。

关闭按钮、标题栏 X 和 Esc 使用同一草稿保护：应用后关闭、放弃草稿、继续编辑。放弃不撤销以前已经应用的修改。设置窗口或其弹窗拥有焦点时屏蔽波形/全局快捷键及全屏 Esc；点击波形重新聚焦后恢复快捷键。弹窗 Esc 返回编辑，不透传给波形。

自适应开启时接管八项热预算（FPS、三波形预算、三 per-pump、flush），显式静态值仍保存但不优先于自适应。关闭后恢复静态配置。公共倍率只从内建默认重新计算未显式的十类资源/吞吐预算，不累乘；低频预算手动编辑会设为显式覆盖，“跟随倍率”解除覆盖。不要把显式标记当成自适应优先级。更多零值和 YAML 语义见[配置参考](config-reference.md)。

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
    ├── send_file/
    ├── oscilloscope_control/
    ├── ui_basic/
    ├── ui_layouts/
    └── ui_dialogs/
```

### Dock 面板

主界面由多个 Dock 面板组成。内置面板包括：

- `通讯配置`：选择通讯模式并连接或断开。
- `协议脚本 / 动态控件`：选择协议根目录和协议目录，重新扫描或重新加载协议。
- `收发数据`：查看 RX/TX 记录，切换原始数据和逐帧视图，手动发送数据。导入含原始字节的记录后默认使用原始视图，保留字节、顺序、时间戳、端点与 TX 状态。原始转逐帧切换的历史策略与范围在设置日志历史页配置：策略默认每次询问（也可固定使用历史/不使用历史），范围默认只用面板保留行（也可选底层完整采集/导入历史；不保证找回已裁剪字节、不自动重读文件），不受显示筛选影响。带 TX 状态的行同时显示状态与原始字节，复制和文本导出同样保留两者。
- `请求追踪`：查看请求、响应和超时链路。
- `离线复现`：管理原始回放时间轴、单步推进和定位。
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
- `F6`：继续或暂停原始回放。
- `F7` / `Shift+F7`：单步推进 / 停止并卸载原始回放时间轴。
- `Ctrl+1` 到 `Ctrl+8`：切换通讯配置、协议脚本 / 动态控件、收发数据、请求追踪、离线复现、日志、脚本、波形 Dock。
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

### 启动故障诊断

如果双击启动后窗口没有出现，或程序在早期初始化阶段失败，可以在 `ProtoScope.exe` 所在目录打开 PowerShell，运行：

```powershell
.\ProtoScope.exe --diagnose
```

诊断模式会生成启动诊断日志，记录版本、进程架构、系统架构、可执行文件路径、当前目录、配置和协议的预期路径、日志目录可写性，以及 `Application`、GLFW、OpenGL、ImGui 等启动阶段的结果。诊断报告只记录环境和路径类信息，不写入 YAML 配置正文、Lua 协议脚本内容或现场收发数据。

普通启动如果在进程入口之后的早期阶段失败，也会弹出 Windows 错误框，显示失败阶段、原因和诊断日志位置。诊断模式能覆盖进程入口之后的问题；如果是缺失导入 DLL、系统拒绝加载 exe 或安全软件阻止进程创建这类入口前失败，需要查看 Windows 事件日志或使用依赖检查工具继续排查。

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

### 3. 写第一个 Lua UI 脚本

如果只想先确认动态 UI 怎么工作，可以新建一个协议目录，只放一个 `main.lua`。下面这段脚本覆盖了最常用闭环：

- `ui()` 返回一个 Dock 面板。
- `controls` 声明输入框、开关和按钮。
- `layout` 决定控件显示顺序。
- `on_control(ctx, id, value)` 响应按钮点击。
- `proto.get_control()` / `proto.set_control()` 读写控件状态。
- `proto.ui.alert()` 弹出脚本提示。

```lua
local click_count = 0

function ui()
  return {
    {
      id = "quick_ui",
      title = "快速 UI",
      anchor = "left_bottom",
      tab_group = "protocol_tools",
      controls = {
        { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
        { type = "checkbox", id = "hex_send", label = "HEX 发送", label_position = "right", default = true },
        { type = "button", id = "send_once", label = "模拟发送" },
        { type = "input_text", id = "last_action", label = "最近动作", default = "待操作" },
      },
      layout = {
        type = "column",
        children = {
          {
            type = "flow",
            children = {
              { type = "control", id = "device_id", min_width = 160, max_width = 260 },
              { type = "control", id = "hex_send" },
              { type = "control", id = "send_once" },
            },
          },
          { type = "control", id = "last_action", min_width = 260 },
        },
      },
    },
  }
end

function on_control(ctx, id, value)
  if id ~= "send_once" then
    return
  end

  -- 用户点击按钮后，读取当前控件值，再把结果写回界面。
  click_count = click_count + 1
  local device_id = proto.get_control("device_id") or "01"
  local hex_send = proto.get_control("hex_send") == true
  local summary = string.format("第 %d 次：设备 %s，HEX=%s", click_count, device_id, tostring(hex_send))

  proto.set_control("last_action", summary)
  proto.ui.alert({
    title = "Lua UI 已响应",
    message = summary,
    level = "info",
  })
end
```

把这段保存为 `main.lua` 后，在 `协议脚本 / 动态控件` 面板把 `协议根目录` 指向它的父目录，选择该协议目录并点击 `重新加载协议`。想继续看布局、弹窗和控件状态读写的完整模板，可以复制 `protocols/templates/ui_basic`、`ui_layouts` 或 `ui_dialogs`。

### 4. 手动发送数据

打开 `收发数据` 面板底部的发送区域：

- 关闭 `HEX` 时，输入内容按文本发送。
- 开启 `HEX` 时，输入内容会按十六进制字节发送。
- HEX 模式会自动整理成大写双字节分组；如果输入奇数个 nibble，发送会被拒绝。
- 点击 `发送` 后，本次内容会进入发送历史。

发送和接收记录会显示在上方列表中。可以使用关键字过滤、RX/TX 状态过滤，也可以切换 `原始` 和 `逐帧` 显示模式。

#### 导入原始记录与切换历史

含原始字节的数据使用默认 BrowseRaw 导入后，收发面板回到原始模式；按记录顺序保留 RX/TX 字节、时间戳、端点和写入状态，端点缺失时回退记录来源。TX 状态不代替字节：带消息的数据行同时格式化消息与字节，纯 INFO/ERROR 文本仍保留文本，逐帧消息仍保留帧内容。复制/文本导出使用共享格式化逻辑，但应注意 HEX/文本显示选择；本次没有人工验证剪贴板、截图或全部导出路径。

纯波形 CSV 只有采样点，不能从波形数值伪造 RX/TX 协议字节；没有原始字节时不会产生对应原始记录。载入时间轴仍停在起点，需主动播放/单步后才有已回放的字节，不能为展示历史而提前播放未来事件。

在现有应用设置的日志历史页配置“逐帧历史策略”和“逐帧历史范围”：默认每次询问与面板保留行，其他策略为使用历史和不使用历史，其他范围为底层保留采集。这是本机偏好，现场包不会覆盖。策略入口对所有原始转逐帧按钮切换生效，不限断开或暂停；收发面板“暂停”只暂停自动滚动，不停止串口接收。

所选范围有历史时，询问提供“使用历史”“不使用历史”“取消”；Escape 等同取消。确认前保持原始模式，采集继续；确认的历史上界是选择生效时已接纳的数据，自动使用历史则在请求处理时固定。取消不切换，所选范围没有历史时直接切换。显示筛选不删除源数据，也不改变重建范围；过滤后没有可见行不能据此认定没有原始历史。

“不使用历史”保留已有逐帧结果及已生成的完整待提交帧，只清 RX/TX 残留半帧，保留 schema/profile、不重放旧字节，之后新数据照常解析。“使用历史”按范围重建替换帧展示，并交接历史尾半帧和 profile；使用独立 UI parser，不额外执行 Lua 回调、物理发送 TX、修改波形或通过 seek 生成展示。半帧交接是连续解析，不是重新加载或运行协议脚本。

面板原始行/帧行限额与底层原始缓存限额分别生效。清空面板清掉原始行、帧缓存、待提交帧与半帧，并使旧确认/任务失效；不断开连接、不删除底层 capture、不换掉当前 schema/profile。选择底层保留采集范围后，仍留存的数据可经同意重建恢复；完整范围不能找回已裁剪字节，不自动重读导入文件。面板范围只读取面板实际保留字节，即使这些字节已不在底层缓存中。

schema 来源随模式不同：实时及 BrowseRaw 使用当前可用 schema，显式 CurrentProtocol/PackageProtocol 使用离线 schema；无 stream schema 时无法生成逐帧结果。runtime profile set/clear 按记录顺序处理，面板行保存接纳时必要基线，不用最新 profile 解释旧行，也不引入范围外字节。底层范围的 timeline 仅使用已回放前缀，不包含未来事件。

重新导入、清空、seek、协议变更与切回原始均使旧任务/确认失效；GUI 关闭失效 modal，并用请求编号隔离后来的新确认。seek 清旧帧并回原始视图。大历史及协议重载历史由主循环按256步骤/64KiB/4ms预算检查分批解析，单大事件使用 offset，完成后有界提交；来源裁剪或追赶超预算取消并提示，不能承诺恢复已淘汰的数据。

迁移配置：`gui.transfer_log.history_on_parsed_switch` 可设 `ask`/`use_history`/`skip_history`，默认 `ask`；`history_scope` 可设 `retained_rows`/`retained_capture`，默认 `retained_rows`，非法值警告并回退默认。仅新策略键缺失且旧 `replay_raw_history_on_schema_switch` 存在时，`true` 映射使用历史，`false` 映射不使用历史；新键优先，旧 bool 保留兼容存档。

过滤无匹配时显示“没有匹配当前筛选条件的记录”，真正没有来源数据时才显示暂无数据；全量导入替换会刷新筛选缓存，不重置用户关键字和 RX/TX 偏好。

**验证与人工边界：**本轮工作流 `focused-*-3.log` 记录配置/构建成功、专项7/7和全量36/36通过，包含真实鼠标按钮、确认失效以及筛选缓存回归。文档阶段未重跑套件，也未进行人工截图、系统剪贴板或实机串口验证；自动化通过不等同于这些人工场景已经检查。详情见 `out/import-transfer-history-publish-20261013-final.md`。

### 5. 查看日志

ProtoScope 将日志分为三类：

- `收发数据日志`：RX/TX 原始记录或解析后的帧记录。
- `系统日志`：连接、配置、协议加载、文件导入导出等宿主事件。
- `Lua 日志`：脚本通过 `proto.log()` 或回调产生的事件。

日志面板支持关键字过滤、等级过滤、导出和清空。顶部菜单 `设置 -> 日志等级` 可以切换调试、信息、警告和错误等级。

### 6. 查看波形

Lua 脚本通过 `proto.plot.setup()` 创建通道，再通过 `proto.plot.push()` 推送采样点。`波形` 面板会显示这些通道。

常用操作：

- 点击波形工具栏最左侧的播放/暂停按钮会调用 `on_oscilloscope_toggle(ctx, current_running, target_running)`；纯 Lua 演示可直接调用 `proto.oscilloscope.set_running(target_running)` 并返回 `true`，真实设备建议先发送启停请求并返回 `false`，等 ACK 到达后再调用 `proto.oscilloscope.set_running(running)`。
- 实时来源下，Lua Dock 可以把 `history_limit` 做成控件，并通过重新调用 `proto.plot.setup({ channels = ..., history_limit = ... })` 调整历史保留；`reset_history = true` 可用于清空当前历史。导入来源下，普通 Lua setup 只按位置更新共有通道的非名称显示参数（包括完整 Bit 配置、颜色、线宽及显示变换），实际/default 波形原名保持不变；既有用户显示覆盖仍优先，恢复默认使用最新 Lua 非名称基线。导入时普通 setup 不清历史、不改变视窗或通道数，普通 push 也不会追加样本。
- 通过通道卡片查看通道状态，设置激活通道。
- 通道卡片悬浮提示和右键设置继续使用原始 Scale；展开图内通道图例后，Scale 列固定显示和编辑“实际值/格”，换算为当前 Y 显示范围除以 `8 × abs(Scale)`。Bit 通道不支持该列，仍使用 Bit Offset。
- “精调”/“滚轮精细调节”开关按当前协议保存。开启后，Y 轴热区调整激活模拟通道及主视图滚轮缩放都使用连续 1% 步幅；关闭后恢复工程刻度和原主视图缩放速度。主图或主图坐标轴悬停时可按 `Shift+中键` 切换，概览滚轮不受影响。
- 使用总览区域快速定位长时间数据。
- 双击主图 X 轴会缩放到当前仍保留的完整历史；如需旧行为，可把 `gui.wave.x_axis_double_click_action` 设为 `fit_visible_window`。
- 双击主图 Y 轴默认按图例可见的模拟通道自动调整 Scale、保留 Offset，让数据包络约占当前视图高度 80%；可把 `gui.wave.y_axis_double_click_action` 设为 `fit_active_channel` 改为只适配当前激活模拟通道，或把 `gui.wave.y_axis_double_click_adjust_offset` 设为 `true` 同时调整 Offset。
- 使用游标和测量覆盖层观察时间差和值差。
- 启用 FFT 频谱模式查看当前可视区频谱。
- 通过 `文件 -> 导入现场会话包...` 恢复包内协议、配置和分析标记，并把原始事件时间轴载入到起点暂停状态。
- 通过 `文件 -> 导入原始波形...` 一次性导入 `.psraw` 文件，立即重建波形结果。
- 通过 `文件 -> 载入原始回放时间轴...` 载入 `.psraw` 事件流，再使用 `回放` 菜单继续、暂停、单步、倍速或定位。
- 通过 `文件 -> 导出当前缓存快照...` 保存当前可回放窗口里的原始数据。
- 通过 `文件 -> 开始完整原始数据录制...` 录制完整原始字节历史。

`导入现场会话包` 和 `载入原始回放时间轴` 都会停在时间轴起点，等待用户主动播放或单步；导入处理默认 BrowseRaw，只浏览已有原始记录和文件提供的波形。显式选择协议处理时，是否运行 Lua 由该离线处理模式决定，而不是由收发视图按钮决定。回放 RX/TX 进入收发记录，原始视图保留录制事件边界；逐帧重建使用独立 UI parser 的 schema，不会为了切换视图额外执行 Lua 帧回调。

载入离线回放前必须先关闭实时连接；时间轴载入期间也不能打开实时连接，避免实时 RX 与回放 RX 混入同一接收框和解析状态。定位使旧历史确认与任务失效，清掉旧逐帧结果并回原始视图；按所选离线模式推进到目标位置，不应将定位当作“使用历史”切换的重建手段。逐帧历史仍须按上述策略独立处理，不能把时间轴定位与视图历史重建混为同一操作。

实时接收时，界面只保留配置允许的最近一段原始字节；普通导出明确是当前缓存快照，也就是当前可回放窗口，不代表全历史。需要完整现场复现时，应使用 `开始完整原始数据录制` 生成完整 `.psraw`，或导出现场会话包。

### 6. 保存和重新加载配置

ProtoScope 使用 YAML 配置保存窗口、通讯、协议、日志、波形和发送历史等状态。默认配置文件参考 `config/protoscope.yaml`，完整字段说明见 `docs/config-reference.md`。

常用入口：

- `文件 -> 保存配置`
- `文件 -> 重新加载配置`
- `通讯配置 -> 保存配置`

如果配置文件在外部被修改，并且启用了外部配置变更提醒，状态栏会提示用户重新加载。

#### 性能配置总览

`performance.scale` 是公共吞吐预算系数，默认 `1.0`。推荐只使用 `0.5`、`1.0`、`2.0` 这类简单值：值越大，接收、脚本 worker 和实时 UI backlog 的缺省预算越大；值越小，处理更保守。`performance.scale <= 0` 会按 `1.0` 回退。

需要让程序按机器和现场负载自动收紧或释放运行时预算时，可开启默认关闭的自适应性能控制：

```yaml
performance:
  adaptive:
    enabled: true
    max_multiplier: 1.0 # K，允许范围 0.25 到 4.0
```

开启后，K 是无压力时的最高性能倍率。控制器每秒综合系统 CPU、可用内存、RX/worker/UI backlog 和脚本处理耗时；正常、轻度、高、严重压力的有效倍率依次为 `K`、`0.75K`、`0.5K`、`0.25K`。压力出现时立即降档，连续 5 个健康采样后才逐级恢复，避免短时波动造成频繁跳档。通讯配置面板会显示当前 K、有效倍率、压力等级和原因；系统指标不可用时会明确显示“仅软件指标”。

自适应开启后，`performance.scale`、`app.fps_limit`、波形的三项渲染预算、三个实时 backlog 的每轮预算，以及 `scripting.worker.output_flush_budget_ms` 不再参与运行时调度，关闭自适应后仍按保存值恢复。自适应以内置基线计算这些热调预算，因此 `K=1.0` 对应程序默认档，不等同于 YAML 中逐项自定义值。

自适应会拆分“渲染预算”和“清债预算”：系统 CPU 或内存压力升高时优先降低 FPS、波形点数、顶点数和总览样本；软件 backlog 高但系统未临界时，RX、transfer、plot 和脚本输出 flush 的清债预算保持在 K 档，避免越积压越慢。

队列与内存上限、worker 线程数、传输读取缓冲、`batch_bytes`、背压水位和 `pump_min_interval_ms` 仍然生效：它们是内存保护、连接参数或协议回调颗粒度，不会在运行中被动态改变。自适应不会丢弃原始数据，原有背压和 backlog 保护继续负责输入安全。

公共系数只作用于没有在 YAML 中显式写出的预算项。写出单项后，单项优先级更高，例如：

```yaml
performance:
  scale: 2.0
scripting:
  worker:
    batch_bytes: 8192
```

上例中，未写出的 RX 队列、内存预算和实时 backlog 预算会按 `2.0` 放大；`batch_bytes` 固定为 `8192`，不再被公共系数覆盖。整数预算使用“默认值 × scale”四舍五入，非零默认值至少保留 `1`；显式写 `0` 时仍保留该单项自己的特殊含义。

受公共系数影响的缺省项包括：`receive.transport_read_buffer_bytes`、`scripting.worker.rx_queue_limit_bytes`、`scripting.worker.memory_budget_bytes`、`scripting.worker.output_queue_limit`、`scripting.worker.batch_bytes`、`scripting.worker.output_flush_budget_ms`、`gui.realtime_backlog.rx_chunk_bytes_per_pump`、`gui.realtime_backlog.transfer_frame_rows_per_pump`、`gui.realtime_backlog.plot_appends_per_pump`、`gui.realtime_backlog.raw_first_backlog_warn_bytes`。

不受公共系数影响的项仍按单项调优，包括 `app.fps_limit`、`gui.wave.*` 渲染和降采样项、日志历史保留、文件 IO 分块、`gui.realtime_backlog.pump_min_interval_ms`、布尔开关和策略字符串。

#### 分块发送与 UI 追赶

默认配置偏向平滑实时刷新：`receive.transport_read_buffer_bytes` 为 `4096`，`scripting.worker.batch_bytes` 为 `8192`，`scripting.worker.output_flush_budget_ms` 为 `2.0`，`gui.realtime_backlog.rx_chunk_bytes_per_pump` 为 `4096`，`gui.realtime_backlog.plot_appends_per_pump` 为 `128`，`gui.realtime_backlog.pump_min_interval_ms` 为 `1.0`。

`scripting.worker.batch_bytes` 是 worker 分块主控项。它会限制相邻 RX 字节事件合并后的最大字节数，并间接决定 worker 每次输出给宿主/UI 的 batch 粗细。

如果要调“worker 给 UI 的块粗细”，优先调 `scripting.worker.batch_bytes`；如果要调“UI 每帧追赶速度”，再调 `gui.realtime_backlog.rx_chunk_bytes_per_pump`、`gui.realtime_backlog.transfer_frame_rows_per_pump`、`gui.realtime_backlog.plot_appends_per_pump` 和 `scripting.worker.output_flush_budget_ms`。

更平滑时，减小 `batch_bytes`、`transport_read_buffer_bytes`、`rx_chunk_bytes_per_pump` 和 `plot_appends_per_pump`，并适当降低 `output_flush_budget_ms`、`pump_min_interval_ms`。更高吞吐时，增大这些批量预算，让 UI 每轮追更多 backlog，但可能出现更明显的大段刷新。渲染性能不足时，降低 `app.fps_limit`、`gui.wave.max_render_points_per_channel`、`gui.wave.max_render_vertices`，并保持 `gui.wave.peak_detect_downsample: true`；需要更高细节时反向提高渲染点数预算或延后降采样。

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
- `oscilloscope_control`：示波器运行态、历史上限和清空历史示例。
- `ui_basic`：最小 Lua UI 闭环，演示 `ui()`、`controls`、`layout`、`on_control()`、控件读写和弹窗提示。
- `ui_layouts`：常见布局组合示例，演示 `column`、`flow`、`inline_group`、`table`、`group`、`collapse` 和控件宽度约束。
- `ui_dialogs`：脚本弹窗示例，演示 `proto.ui.alert()`、`proto.ui.confirm()`、`window` 参数和 `on_dialog()`。

使用模板时，复制整个模板目录并改名为自己的协议名，保留入口文件 `main.lua`。然后在协议面板选择协议根目录和新协议目录，点击重新扫描或重新加载。不要直接修改 `protocols/protoscope_api.lua`；它只给 LuaLS 提供提示，不参与运行。

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
- `value_table`
- `tx_sequence`

布局可以省略，省略时按 `controls` 声明顺序显示；也可以使用 Layout Tree 组合 `column`、`flow`、`inline_group`、`table`、`group`、`collapse` 等节点。多个 Dock 可以通过 `tab_group` 合并到同一组选项卡。完整语法见 `protocols/README.md`。

`tx_sequence` 用于 Lua 定义每行变量列、界面动态维护多帧序列。字段可用 `options = { { label, value } }` 显示为下拉框，脚本读到的仍是 `value` 的真实标量。用户可以增删、复制、移动、启用帧，设置全局发送间隔和循环开关；帧头、帧尾和 CRC 建议保留为 Lua 脚本常量，由 `build_frame()` 组包。脚本在 `on_control()` / `on_timer()` 中读取完整状态，自己组帧并调用 `proto.send()`、`proto.request()` 或 `proto.request_guarded()`。

### 常用回调

- `on_open(ctx)`：连接打开。
- `on_close(ctx, reason)`：连接关闭。
- `on_error(ctx, message)`：通讯错误。
- `on_control(ctx, id, value)`：动态控件变化。
- `on_bytes(ctx, bytes)`：收到原始字节。
- `on_timer(ctx, name)`：定时器触发。
- `on_oscilloscope_toggle(ctx, current_running, target_running)`：波形工具栏请求启动或暂停示波器，返回 `true` 时默认同步到 `target_running`；脚本也可用 `proto.oscilloscope.set_running(running)` 显式同步。
- `on_tx(ctx, evt)`：发送或请求状态变化。
- `on_dialog(ctx, evt)`：脚本弹窗关闭。
- `on_file_dialog(ctx, evt)`：文件或目录对话框返回。

### 常用 `proto` API

- `proto.send(payload, opts?)`：普通异步发送。
- `proto.request(payload, opts?)`：半双工请求，由宿主管理排队、超时和串行发送。
- `proto.request_guarded(payload, opts?)`：受保护半双工请求，`max_attempts` 只统计当前请求；最终失败后熔断后续 guarded 请求。
- `proto.reset_request_guard()`：解除 guarded 熔断，新的 guarded 请求从 `attempt=1` 重新开始。
- `proto.request_done(result?)`：脚本确认当前请求已经收到完整业务应答。
- `proto.oscilloscope.set_running(running)`：主动同步波形工具栏运行状态，适合脚本按钮、定时器或设备 ACK 回调。
- `proto.emit(name, payload)`：输出脚本事件。
- `proto.set_timer(name, interval_ms)` / `proto.cancel_timer(name)`：管理定时器。
- `proto.status.set(text, opts?)` / `proto.status.clear()`：更新状态提示。
- `proto.get_control(id)`：读取动态控件当前值，常用于按钮点击时取回输入框、开关、下拉框或 `tx_sequence` 状态。
- `proto.set_control(id, value)`：把协议结果或脚本状态写回控件，常用于刷新只读状态、回填最近操作、更新 `value_table` 或同步 `tx_sequence`。
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
- `导入现场会话包...`
- `导入原始波形...`
- `导入 CSV 数据...`
- `载入原始回放时间轴...`
- `导出现场会话包...`
- `导出当前缓存快照...`
- `导出波形 CSV...`
- `导出原始事件 CSV...`
- `导出波形分析报告...`
- `开始完整原始数据录制...`
- `停止完整原始数据录制`

### 回放

- `继续/暂停回放`
- `单步推进`
- `停止并卸载时间轴`
- `倍速`：0.5x、1.0x、2.0x、4.0x、8.0x。
- `定位到开头`
- `定位到中点`
- `定位到末尾`

### 视图

- 显示或隐藏 `通讯配置`
- 显示或隐藏 `协议脚本 / 动态控件`
- 显示或隐藏 `收发数据`
- 显示或隐藏 `请求追踪`
- 显示或隐藏 `离线复现`
- 显示或隐藏 `日志`
- 显示或隐藏 `脚本`
- 显示或隐藏 `波形`
- 重置当前协议 Dock 布局

### 设置

- `日志等级`：调试、信息、警告、错误。

### Lua视图

显示或隐藏当前协议脚本声明的动态 Dock。

### 帮助

- `快捷键说明`
- `算法手册`
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

调试时可以使用 `文件 -> 开始完整原始数据录制...` 保存完整 `.psraw` 文件。快速复盘时使用 `文件 -> 导入原始波形...` 一次性重建波形；导入 `.pssession` 现场包或单独载入 `.psraw` 时间轴后，回放会停在起点，再通过 `回放` 菜单继续、暂停、单步、倍速或定位。原始 RX 会同步显示在收发记录中，切换到“逐帧”后可用包内协议的 `stream()` schema 查看解析结果。

## 进一步阅读

- `README.md`：项目快速入口和构建命令。
- `docs/config-reference.md`：YAML 配置字段、默认值和保存/重载行为。
- `protocols/README.md`：Lua 协议脚本详细指南。
- `protocols/protoscope_api.lua`：LuaLS 类型提示和宿主 API 声明。
- `protocols/stream_types.lua`：`stream()` schema 类型提示。
- `docs/transport-extension.md`：新增通讯方式的开发说明。
- `docs/lua-host-integration.md`：Lua 宿主 API 接入说明。
