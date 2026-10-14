# ProtoScope 配置参考

本文面向需要手工编辑 `config/protoscope.yaml` 的用户和维护者。配置事实源是 `include/protoscope/config/config.hpp` 的默认值、`src/config/config.cc` 的读写逻辑，以及仓库里的 `config/protoscope.yaml` 示例。

## 生效与保存

- 启动时从默认配置路径加载 YAML；缺失字段使用源码默认值。
- `文件 -> 保存配置` 和通讯面板的保存入口会把当前运行态写回 YAML。
- `文件 -> 重新加载配置` 会先保存当前协议工作区状态，再从磁盘重新加载配置。
- `app.auto_save.enabled` 开启后，配置 dirty 且达到 `interval_ms` 间隔时自动保存。
- `app.config_hot_reload.enabled` 开启后，宿主会检测外部文件变化并提示用户处理；不会在用户未确认时覆盖当前未保存状态。
- `performance.scale` 只影响未显式写出的吞吐预算项。某个预算项写入 YAML 后，该项优先于公共系数。启用 `performance.adaptive.enabled` 后，`performance.scale` 和自适应接管的热调预算不参与运行时调度。

## 设置窗口与 YAML 边界

`设置 → 应用设置…` 提供常规外观、波形偏好、日志历史、性能四页草稿。控件默认直接来自 `AppConfig{}`，应用做字段差异与三方合并，不以 YAML 往返或全量重载模拟热更新。恢复本页默认不重置其它页。应用与保存分离；保存包含最新完整运行态，自动保存不写草稿。启动窗口、渲染后端、中文字形需重启；倍率派生连接读取缓冲只在下次连接生效。

设置窗口不承接通讯、协议、FFT、游标、测量和 Dock 布局；`gui.wave.downsample_mode`、`gui.wave.overview_show_bit_channels` 继续仅由 YAML 配置。当前兼容 `y_axis_double_click_adjust_offset` 不改变适配；图例自动折叠仅 double_click 模式生效；波形动画和交互反馈总开关共同决定动画有效状态。

| 字段/类别 | 设置窗口范围与零值语义 |
| --- | --- |
| 启动宽高 | 正整数逻辑像素；重启生效 |
| 提示/自动保存间隔 | 非负毫秒；0分别为无瞬时提示/无最小保存间隔 |
| 固定卡宽/自适应比例 | 正有限数且转换/乘法安全；比例宽最终限制160–220像素 |
| `performance.scale` | 有限正数；从内建基线重算未显式十类预算，拒绝不可安全转换的派生值 |
| `adaptive.max_multiplier` | 有限0.25–4.0；不改既有自适应规则 |
| `fps_limit` | 非负整数，0至少1FPS；高于1000时调度最小1ms |
| 通道点数/总顶点 | 非负整数；0回退内建预算，不是无限 |
| 概览桶数 | 非负整数；0只取消该项桶数限制 |
| 三 per-pump 预算 | 非负；0有效至少1；追加操作数不是样本数 |
| worker flush | 有限非负毫秒且转微秒不溢出；0无时间预算，可能影响响应性 |
| 日志路径/预览 | 空路径关闭文件日志；相对可执行目录；预览0字节不代表无限 |
| 轮转大小/份数 | 非负；份数至200000；0有效回退1byte/1份，活动文件另计 |
| 五类日志历史 | 非负行数；0不保留；帧容量含待派生行；缩减须确认 |
| 发送历史 | 非负条数；有效容量min(配置,200)，0清空 |

窗口严格拒绝非法输入并保留草稿。手写 YAML 保留原兼容归一化策略：非法 scale 回退1，过大派生整数安全钳制；不要把容错读取当作窗口允许非法值。低频四项手动编辑建立显式覆盖，“跟随倍率”解除；资源安全边界仍受静态倍率/覆盖约束，自适应只接管八项热预算，不能越过队列、内存、背压和连接预算。

保存检测外部快照不依赖热重载开关。独占创建唯一同目录临时文件，持有句柄写入并 flush/close 后替换；失败保留旧文件和本次临时文件，并在错误中报告临时文件路径；不删除文件。外部变化须明确覆盖或重载；坏 YAML 保留旧运行态。该本机防误覆盖机制不承诺抵御恶意跨进程写入的绝对事务隔离。

## performance

```yaml
performance:
  scale: 1.0
  adaptive:
    enabled: false
    max_multiplier: 1.0
```

- `scale`：公共吞吐预算系数，默认 `1.0`；小于等于 `0` 时按 `1.0` 处理。
- 受影响的默认预算：`receive.transport_read_buffer_bytes`、`scripting.worker.rx_queue_limit_bytes`、`scripting.worker.memory_budget_bytes`、`scripting.worker.output_queue_limit`、`scripting.worker.batch_bytes`、`scripting.worker.output_flush_budget_ms`、`gui.realtime_backlog.rx_chunk_bytes_per_pump`、`gui.realtime_backlog.transfer_frame_rows_per_pump`、`gui.realtime_backlog.plot_appends_per_pump`、`gui.realtime_backlog.raw_first_backlog_warn_bytes`。
- 写出后的预算项会被视为显式覆盖，保存时也会继续保留。
- `adaptive.enabled`：自适应性能控制开关，默认 `false`。开启后每秒采样系统 CPU 忙碌率、可用物理内存以及应用 RX、worker、transfer、plot backlog 和脚本处理耗时。
- `adaptive.max_multiplier`：性能上限倍率 K，默认 `1.0`，有效范围 `0.25` 到 `4.0`；缺失、非有限数或非正数回退到 `1.0`，超出范围会钳制。
- 自适应启用时，运行时忽略 `scale`、`app.fps_limit`、`gui.wave.max_render_points_per_channel`、`gui.wave.max_render_vertices`、`gui.wave.overview_max_samples`、三个 `gui.realtime_backlog.*_per_pump` 以及 `scripting.worker.output_flush_budget_ms`。这些 YAML 值仍会保存，关闭自适应后再次生效。
- 自适应内部会拆分两类倍率：系统压力升高时优先收紧 FPS 和波形渲染预算；软件 backlog 高但系统未临界时，清债预算保持在 K 档，避免降低 RX、transfer、plot 和脚本输出追赶能力。
- 自适应预算以内置基线和当前 K 计算：正常为 `K`，轻度、高、严重压力分别为 `0.75K`、`0.5K`、`0.25K`，最终不低于 `0.25`。压力升级立即生效；连续 5 个健康采样后才逐级恢复。
- `receive.transport_read_buffer_bytes`、`scripting.pipeline.worker_threads`、worker 队列/内存/输出上限、`scripting.worker.batch_bytes`、背压水位和 `gui.realtime_backlog.pump_min_interval_ms` 继续按 YAML 生效，作为连接、协议颗粒度或资源安全边界，不会由自适应控制器改写。
- 通讯状态区会显示 K、当前有效倍率、压力等级和主导原因；系统指标无法取得时会标注为仅使用软件指标。

## app

```yaml
app:
  language: zh-CN
  fps_limit: 60
  idle_render: dirty_only
  auto_save:
    enabled: false
    interval_ms: 5000
  config_hot_reload:
    enabled: false
```

- `language`：界面语言标识，默认 `zh-CN`。
- `fps_limit`：主循环帧率上限，默认 `60`。
- `idle_render`：空闲渲染策略，默认 `dirty_only`。
- `auto_save.enabled`：是否自动保存配置。
- `auto_save.interval_ms`：自动保存最小间隔，单位毫秒。
- `config_hot_reload.enabled`：是否检测外部配置变更并提示重载。

## gui

```yaml
gui:
  theme: professional_dark
  show_app_header: false
  window:
    title: ProtoScope
    width: 1600
    height: 900
    maximized: false
  font:
    chinese_glyph_range: simplified_common
```

- `theme`：主题字符串 ID，内置 `professional_dark`（默认，中性炭灰分层与低饱和灰青强调）、
  `debug_high_contrast`（近黑底、明亮信号与清晰焦点）和 `professional_light`（冷白底、白面板与蓝强调）。
  配置文件旁 `themes` 目录仍支持 YAML 用户主题；不再随附额外主题，不删除已有运行副本。
  启动加载失败时显示专业深色，
  但保留原 ID；缺失字段默认 `professional_dark`。详见 [主题管理与模板](theme-management.md)。
- 运行中可通过 `设置 -> 主题` 即时切换，无需重启，也不会重载当前协议。
  切换会把配置标记为待保存；启用自动保存时自动写回，否则使用
  `文件 -> 保存配置` 持久化。
- 主题属于本机全局偏好，不随协议切换或现场包导入改变。
- `gui.wave.overview_selection`：概览缩放框可选覆盖，`mode` 为 `auto`（默认）或 `fixed`，
  `fixed_color` 为 `"#RRGGBB"`，`min_alpha`、`max_alpha` 满足 `0 <= min <= max <= 1`。
  未覆盖项随主题变化；主题默认 alpha 范围为 `0.10..0.28`。
- `gui.wave.cursor_auto_color`：默认 `true`（旧配置缺省也启用），根据实际显示色与背景分配 A/B/T 身份色；
  `false` 恢复主题 `wave.cursor_palette` 索引映射，与主题 `wave.correct_contrast` 独立。
  后者保留源 RGBA，先补最小 alpha，再按需修正明暗；显式全透明与关闭修正不变。
  自动色不能保证任意自定义背景与任意多通道均有解，无解时采用文字回退和图形护边。
- `show_app_header`：是否显示应用顶部 header。
- `window.title`：窗口标题。
- `window.width` / `window.height`：初始窗口尺寸。
- `window.maximized`：启动时是否最大化。
- `font.chinese_glyph_range`：`simplified_common` 或 `full`；`full` 适合需要显示更多 CJK 字形的场景。

### gui.file_dialogs

- `last_import_directory`：内置导入及回放载入共用的历史目录。
- `last_export_directory`：内置导出、报告、日志、请求追踪和录制共用的历史目录。

文件对话框确认后立即保存，取消浏览不更新；目录失效时逐级回退，不创建目录、不改写历史。
未配置新导出字段时从 `gui.last_data_export.directory` 迁移；目录偏好保存不提交其他未保存设置。
ELF 文件继续按协议记忆，Lua 自定义文件对话框与协议根目录选择不使用这两个字段。

### gui.wave

- `measurement_visible_window_only`：默认 `false`，测量完整 A/B 区间，即使游标离开视口。
  开启后只统计 A/B 与可视窗口的交集，与“游标跟随滚动”独立。
- `measurement_follow_refresh_interval_ms`：默认 `200`，只限制跟随持续自动滚动的统计提交频率。
  `0` 不额外限频，仍最多一个在途任务；人工拖动只更新轻量读数，释放后统计最终区间。
  跟随滚动时可显示最近完成快照，浮层标注实际统计 A/B 和待更新状态；停止后立即请求最终区间。

波形状态统一显示在底部状态栏，独立于连接信息和通用操作结果。
普通状态最多每 500ms 更新一次；等待不足 300ms 不显示，已显示的等待至少保留 1000ms。
新错误立即显示；关闭功能、切换协议或清空历史时立即清除过期状态。
这些固定显示时间不影响 FFT 计算、曲线刷新、采集频率或窗口 FPS，也不提供额外配置项。

```yaml
gui:
  wave:
    fullscreen_mode: overlay
    control_mode: oscilloscope
    display_formula: offset_then_scale
    grid_division_readout_mode: display_value
    channel_scale_wheel:
      enabled: true
      acceleration: log
    channel_card_width_mode: fixed
    channel_double_click_action: reset_scale_offset
    x_axis_double_click_action: fit_full_history
    y_axis_double_click_action: fit_visible_channels
    y_axis_double_click_adjust_offset: false
    hidden_channel_policy: visible_only
    cursor_extreme_snap_policy: nearest_waveform
    mouse_y_offset_drag_mode: direct
    legend_overlay_double_click_auto_collapse: true
    interaction_animation_enabled: true
    zoom_selection_auto_exit: false
    channel_card_fixed_width: 128.0
    channel_card_adaptive_ratio: 0.22
    legend_channel_name_max_width: 0.0
    vertical_auto_fit_multiplier: 1.25
    max_render_points_per_channel: 1200
    max_render_vertices: 60000
    peak_detect_downsample: true
    downsample_mode: legacy_uniform
    bit_dense_render_mode: compressed_steps
    downsample_start_multiplier: 2.0
    overview_max_samples: 20000
    overview_normalize_channels: false
    overview_show_bit_channels: false
    max_total_samples: 0
    min_visible_time_span: 0.001
    reset_history_on_time_reset: true
    show_axis_labels: false
    show_channel_legend: true
    show_fft_legend: true
    cursor_fft_highlight_rgba: [0.20, 0.55, 1.00, 0.16]
```

- `fullscreen_mode`：`focus` 或 `overlay`。
- `control_mode`：`legacy_global` 或 `oscilloscope`。
- `display_formula`：`offset_then_scale` 或 `scale_then_offset`。
- `grid_division_readout_mode`：`display_value`、`actual_value` 或 `raw_value`，控制通道卡片展示每格读数的换算口径。
- `channel_scale_wheel.enabled`：默认 `true`，精调关闭时 Y 轴热区滚轮按 1-2-5 工程刻度调整激活模拟通道的实际值/格；设为 `false` 时回退 `pow(1.1, wheel)` 连续缩放。
- `channel_scale_wheel.acceleration`：`none`、`linear` 或 `log`，默认 `log`；非法值回退 `log`。同通道同方向且事件间隔不超过 250ms 时累计加速。
- `channel_card_width_mode`：`fixed` 或 `adaptive`。
- `channel_double_click_action`：`reset_all`、`reset_scale_offset`、`reset_scale`、`reset_offset`。
- `x_axis_double_click_action`：`fit_full_history` 或 `fit_visible_window`。
- `y_axis_double_click_action`：`fit_visible_channels` 或 `fit_active_channel`。默认聚合所有图例可见模拟通道；激活通道模式只取当前激活模拟通道，激活通道无效、隐藏或为 bit-display 时回退到可见模拟通道。
- `y_axis_double_click_adjust_offset`：保留旧配置的读写兼容；主图适配现在统一写入
  `scale` 和 `offset`，将目标模拟通道居中适配到固定 Y 基准，此字段不再改变适配行为。
- `hidden_channel_policy`：`visible_only` 或 `include_hidden`，控制隐藏通道是否参与派生视图。
- `cursor_extreme_snap_policy`：`nearest_waveform` 或 `viewport_zone`。
- `mouse_y_offset_drag_mode`：`direct`、`shift` 或 `disabled`，控制鼠标拖动通道 Y 偏移的触发方式。
- `legend_overlay_double_click_auto_collapse`：双击展开图内图例后，鼠标离开并结束输入/拖动交互时是否自动收起。仅在 `legend_overlay_open_mode: double_click` 时生效，默认 `true`。
- `interaction_animation_enabled`：Wave Dock 交互动效总开关，默认 `true`。关闭后离散视口跳转、工具抽屉、概览折叠和图例浮层直接跳到最终状态。
- `zoom_selection_auto_exit`：框选放大后是否自动退出框选模式。
- `channel_card_fixed_width` / `channel_card_adaptive_ratio`：通道卡片宽度策略参数。
- `legend_channel_name_max_width`：通道图例名称显示宽度上限，单位为 ImGui 逻辑像素；`0.0`、缺失或非正值表示不限制。作用于展开态表格、紧凑态浮窗和底部通道卡片，超长名称会裁剪并在悬浮时显示完整 tooltip。
- `vertical_auto_fit_multiplier`：纵向自动适配余量倍数，默认 `1.25`，即数据包络约占视图高度 80%。
- `max_render_points_per_channel` / `max_render_vertices`：单通道和总顶点渲染预算。
- `downsample_mode`：仅通过配置文件选择模拟波形显示降采样策略，不增加界面控件或专用文件监听。默认、缺省、空字符串及未知值均为 `legacy_uniform`，恢复 `e6320e1` 的快照可见范围、窗口均匀四点分桶，以及主图／Split 的原有绘制分支。仅显式设置 `stable_edges` 时使用稳定时间桶及边沿相邻点保留算法；该模式下主图、堆叠和 Split 直接绘制查询轨迹，避免二次压缩。启动读取、现有“重新加载配置”和保存均支持此项；重载保留原有工作区与应用配置流程及副作用。模式改变会刷新显示、概览与包络缓存并重建余辉，不改变游标改进、概览配色、FFT 计算、测量读数或数字通道算法。
- `peak_detect_downsample`：默认 `true`。在 `legacy_uniform` 中，高密度主图开启时按原有 peak-detect 路径绘制首点、极小值、极大值、末点轨迹，关闭时绘制 min/max 包络；Split 开启时直接绘制查询轨迹，关闭且可见点数超过单通道预算时绘制包络。在 `stable_edges` 中，查询轨迹始终直接绘制，此开关不再二次压缩模拟通道。
- 旧版兼容限制：`legacy_uniform` 且 `peak_detect_downsample: false` 时，查询降采样后的数据还会进入旧包络路径，单点桶或常量桶的 min/max 相等，零高度竖线可能不可见。这是保留的 `e6320e1` 表现；查看连续轨迹可保持 `peak_detect_downsample: true`。
- `bit_dense_render_mode`：密集 bit 轨迹样式，默认 `compressed_steps`。`compressed_steps` 用预算内阶梯表达首尾状态及桶内跳变活动；`activity_band` 用半透明带标记桶内同时出现高低电平的区间，稳定区间保留电平线。缺省或未知字符串使用默认值。两种模式均在低密度时恢复精确阶梯，不改变原始数据或游标读数，也不增加 Lua 字段。
- 绘图预算同时约束压缩输出和 Glow/线段的顶点开销。每通道以 256 点基础块建立二合一摘要，追加和裁剪只更新边界及其上层；改变颜色、偏移、缩放、布局不重建原始摘要。初次建立摘要和松手后的分析输入提取仍有与输入规模相关的开销。
- 拖动期间停止提交统计与 FFT 重计算，旧结果显示为待更新；没有旧结果时显示空状态。松手后使用独立输入快照后台计算，过时查询结果不会覆盖新查询。只移动频谱坐标轴不改变 FFT 输入窗口；持续采集可以发布同查询最近完成的快照。
- 余辉在拖动期间暂停累积，显示轻量轨迹。旧视口纹理在交互后失效，松手按最终坐标重建一次；冻结状态随后继续冻结，不自动恢复数据跟随。触发模式在 32 个时间分区中各选至多一个真实触发，采用原始样本插值确定触发时间，各轨迹共享绘图预算。分屏继续使用普通轨迹回退。
- `downsample_start_multiplier`：最小为 `1.0`，默认 `2.0`；在 `legacy_uniform` 中控制主图进入峰值检测／包络绘制的原始可见点数阈值，Split 包络仍沿用单通道预算阈值。在 `stable_edges` 中控制稀疏点标记阈值，不延后查询层降采样。两种模式的查询输出均严格遵守点数预算，此倍数不会放大预算；概览高密度填充包络维持原算法，概览显示数据、FFT 显示与触发余辉取点传递同一模式。
- `overview_max_samples`：概览桶数上限，每桶最多两个极值点；0 仅取消此项限制。
- `overview_show_bit_channels`：默认 `false`，Bit 通道不参与概览绘制和纵轴范围计算。
  开启后 Bit 通道先绘制，普通通道覆盖其上，选框与游标位于最上层；遵循现有图例隐藏状态。
  所有通道均为 Bit 且关闭此项时，概览仍保留完整历史时间导航。仅支持配置文件控制。
  预算公式、降采样阈值与当前限制见[波形渲染计划](wave-view-render-plan.md#渲染预算)。
- `overview_normalize_channels`：默认 `false`；开启后各 CH 的全历史概览包络独立映射到
  `[-1,1]`，常量位于 0，空通道和非有限值跳过。只改变概览绘制，不修改主图
  `scale`、`offset`、共享缓存或实际读数；隐藏策略、时间导航和游标保持原行为。
- `max_total_samples`：每通道历史样本上限，`0` 表示不额外限制。
- `min_visible_time_span`：X 轴最小可见时间跨度。
- `reset_history_on_time_reset`：时间轴重置时是否清空历史。
- `show_axis_labels` / `show_channel_legend` / `show_fft_legend`：波形轴标签、通道图例、FFT 图例显示开关。
- `cursor_fft_highlight_rgba`：游标分屏模式下主波形 C1~C2 FFT 输入窗口高亮色，按 `[r, g, b, a]` 写入。

### gui 运行态预算

```yaml
gui:
  log_history:
    transfer_raw_limit: 10000
    transfer_frame_limit: 120000
    host_limit: 5000
    script_limit: 5000
  raw_capture:
    live_limit_bytes: 67108864
    recording_queue_limit_bytes: 268435456
  data_import:
    protocol_source: prefer_package
  transfer_log:
    history_on_parsed_switch: ask
    history_scope: retained_rows
    replay_raw_history_on_schema_switch: false
  realtime_backlog:
    mode: responsive
    rx_chunk_bytes_per_pump: 4096
    transfer_frame_rows_per_pump: 2000
    plot_appends_per_pump: 128
    raw_first_backlog_warn_bytes: 33554432
    derived_backlog_degrade_enabled: true
    discard_backlog_on_disconnect: false
    pump_min_interval_ms: 1.0
  elf_symbol_combo:
    limit: 10
    debounce_ms: 300
    auto_refresh_selected_address: true
    auto_refresh_emit_on_control: false
  send_history_limit: 20
  lua_dock_layout_debug: false
```

- `log_history.*`：收发原始行、逐帧行、宿主日志和脚本日志保留上限。
- `raw_capture.live_limit_bytes`：实时原始缓存上限。
- `raw_capture.recording_queue_limit_bytes`：完整录制队列上限。
- `data_import.protocol_source`：本机导入来源偏好，默认 `prefer_package`（优先包内协议；缺失时用当前），可选 `current_protocol`（只用当前，不执行包 Lua）；非法值回退默认。所选包协议存在但损坏时失败，不按缺失回退。同步与 GUI 异步导入均先实际初始化协议再提交数据；确认提示外部 Lua 初始化风险，初次保持原始浏览和波形快照。现场包不覆盖此偏好。
- `transfer_log.history_on_parsed_switch`：原始转逐帧切换的历史策略，默认 `ask`。可选 `ask`（当前来源有 stream schema 且有历史时每次询问）/ `use_history`（按所选范围重建替换）/ `skip_history`（只清半帧，不重放旧字节）；非法值回退 `ask`。
- `transfer_log.history_scope`：历史范围，默认 `retained_rows`（只用收发面板保留的原始 RX/TX 行）；可选 `retained_capture`（底层仍保留的完整采集/导入历史；不保证找回已裁剪字节、不自动重读文件）。不受显示筛选影响。
- `transfer_log.replay_raw_history_on_schema_switch`：保留读写兼容的旧键；仅当 `history_on_parsed_switch` 缺失时映射（`true`→`use_history`，`false`→`ask`），新键优先。

手动成功切换协议后，仍保留且与当前导入关联的原始记录使用新协议分批重新解析，更新波形与帧；启动、配置应用、导入恢复、无变化加载和普通实时采集不触发此重算。只有波形快照而无关联原始数据时保留快照并提示不能重算，不借用旧采集。中途解析失败保留关联原始记录并标记结果不完整。

无有效协议、协议加载失败且没有旧有效协议、有效协议未声明 stream，分别显示不可解析原因；最近一次失败不会否定旧有效 schema。无 schema 可进入逐帧提示视图，不创建历史任务、不清已有帧或 pending。确认生效时重新检查能力和历史。即使旧帧可见仍展示当前不可解析原因，筛选无匹配提示保持独立。

历史确认窗口局部自适应，按字号缩放约 560 宽、320 最高，并限制在 viewport 工作区宽 90%/高 80% 内；说明可滚动、按钮可换行，不修改全局样式。相关新增回归本轮尚未构建或运行，自动化和人工小屏验证结论待集中验证。

#### 原始转逐帧：配置合同与当前实现边界

这两个枚举是本机展示偏好，入口为现有应用设置的日志历史页；沿用字段白名单、三方合并、草稿/运行态 dirty、枚举校验、应用、保存和恢复默认流程。现场包恢复保留本机这两个值。内建值与仓库 YAML 均为 `ask` / `retained_rows`。迁移旧配置时可删除旧 bool 并显式写入新枚举；保留旧键也不会覆盖新键。

原始转逐帧按钮在实时连接、断开与暂停自动滚动时都进入同一个策略入口，不要求暂停采集。`ask` 在当前来源有有效 stream schema 且所选范围有历史时每次询问，确认前保持原始视图，采集继续；取消或 Escape 保持原始视图。自动使用历史在处理请求时固定上界，询问使用历史在选择生效时固定上界，包含弹窗期间已接纳的原始数据，不以物理到达时间推断队列归属。

当前源码的行为与验证边界如下（实现不等于验收通过）：

- `skip_history` 与询问中的“不使用历史”保留已有逐帧结果和已生成的完整待提交帧，只清 RX/TX parser 残留半帧；保留当前 schema/runtime profile，不重放旧字节，之后新数据照常解析。
- `retained_rows` 从面板实际保留行取字节，保存接纳时的 profile 基线，底层裁剪不删除面板仍持有的字节。`retained_capture` 使用底层仍保留事件，兼容 legacy payload；loaded timeline 只使用 `[0,eventIndex)` 已回放前缀，不用面板行数截断事件，也不包含未来事件。
- 原始行、帧行与底层原始字节限额各自生效。清空面板清展示、pending 和半帧，使确认/任务失效，但不删除底层 capture；选择 `retained_capture` 后，底层仍有历史时仍可询问恢复。已裁剪字节不能恢复，不自动重读文件。
- 使用历史通过独立 UI `FrameStreamParser` 重建并替换结果，不重执行 Lua callback、不实际发送 TX、不改波形、不借 seek/replay 副作用生成展示。实时与 BrowseRaw 使用当前可用 schema，显式 CurrentProtocol/PackageProtocol 使用离线 schema；profile set/clear 按顺序作用于 RX/TX，面板起点仅恢复必要基线，不引入范围外字节。无 stream schema 时不能生成逐帧结果。
- 来源启动仅冻结元数据；主循环每轮最多256步骤、64KiB字节、4ms时间预算检查，单大事件带 offset。协议成功重载也安排同一有界任务。重建追赶保存来源位置而不全量复制新增字节；来源替换/裁剪或追赶超预算会取消并提示。完成交接尾半帧/profile，结果及待提交队列有行数上限。4ms 是循环预算检查，不是单次 parser 调用或操作系统调度的硬实时保证；取消后不会恢复已经淘汰的字节。
- 非法枚举文本记录警告并回退默认；`history_scope` 非法值回退 `retained_rows`。有新策略键时不再用旧 bool 决定运行策略，即使同时保留旧键也只作兼容存档。

本轮统一验证日志 `cmake-build-debug/import-history-workflow-logs/focused-*-3.log` 记录配置、构建、专项及全量 CTest 均 exit 0，专项7/7、全量36/36目标通过。这是工作流实际命令日志的结果，不是文档编辑阶段重新执行测试。真实鼠标 GUI 自动化覆盖切换、确认、失效关闭和筛选无匹配提示；不等同于人工桌面截图、剪贴板或实机串口验证。发布说明及证据见 `out/import-transfer-history-publish-20261013-final.md`。
- `realtime_backlog.mode`：实时追赶模式，默认 `responsive`。
- `realtime_backlog.*_per_pump`：每轮 UI 追赶预算，缺省受 `performance.scale` 控制。默认偏向平滑刷新，`rx_chunk_bytes_per_pump` 为 `4096`，`plot_appends_per_pump` 为 `128`。
- `realtime_backlog.raw_first_backlog_warn_bytes`：原始 backlog 首次告警阈值。
- `realtime_backlog.derived_backlog_degrade_enabled`：派生视图 backlog 过高时是否降级。
- `realtime_backlog.discard_backlog_on_disconnect`：断开连接时是否丢弃待追赶 backlog。
- `realtime_backlog.pump_min_interval_ms`：实时追赶最小间隔，默认 `1.0` ms；该项不受 `performance.scale` 控制。
- `elf_symbol_combo.limit` / `debounce_ms`：Lua `elf_symbol_combo` 默认候选数和输入消抖。
- `elf_symbol_combo.auto_refresh_selected_address`：已选符号地址是否随数据源自动刷新。
- `elf_symbol_combo.auto_refresh_emit_on_control`：自动刷新时是否触发控件回调。
- `send_history_limit`：发送历史条数上限。
- `lua_dock_layout_debug`：Lua Dock 布局调试开关。

## protocol

```yaml
protocol:
  root_dir: protocols/templates
  selected_dir: protocols/templates/default_protocol
  tx:
    send_timeout_ms: 1000
    request_timeout_ms: 1000
    max_pending: 64
    overflow_policy: reject_new
    overflow_notify: popup_once
```

- `root_dir`：协议根目录。
- `selected_dir`：当前选中的协议目录。
- `tx.send_timeout_ms`：`proto.send()` 默认发送超时。
- `tx.request_timeout_ms`：`proto.request()` 默认请求超时。
- `tx.max_pending`：待处理 TX 请求上限。
- `tx.overflow_policy`：溢出策略，默认 `reject_new`。
- `tx.overflow_notify`：溢出提示策略，默认 `popup_once`。

## receive

```yaml
receive:
  transport_read_buffer_bytes: 4096
  stream_buffer:
    near_overflow_threshold: 0.8
    popup_enabled: true
```

- `transport_read_buffer_bytes`：底层通讯读取缓冲，默认 `4096` 字节，缺省受 `performance.scale` 控制。
- `stream_buffer.near_overflow_threshold`：接收流缓冲接近溢出的告警比例。
- `stream_buffer.popup_enabled`：接近溢出时是否弹窗提示。

## scripting

```yaml
scripting:
  pipeline:
    # worker_threads: 4
  worker:
    enabled: true
    rx_queue_limit_bytes: 67108864
    memory_budget_bytes: 268435456
    memory_budget_available_ratio: 0.0
    output_queue_limit: 65536
    batch_bytes: 8192
    backpressure_enabled: true
    backpressure_rx_queue_high_watermark: 0.5
    backpressure_rx_queue_low_watermark: 0.3
    output_flush_budget_ms: 2.0
    drain_request_outputs_unbounded: false
```

- `pipeline.worker_threads`：脚本 worker 线程数；省略时由宿主选择默认值。
- `worker.enabled`：是否启用脚本 worker。
- `worker.rx_queue_limit_bytes`：RX 输入队列字节上限。
- `worker.memory_budget_bytes`：worker 内存预算。
- `worker.memory_budget_available_ratio`：按可用内存比例扩展预算，`0.0` 表示关闭。
- `worker.output_queue_limit`：脚本 worker 待 UI 消费的输出**批次数**上限（默认 `65536`），不是字节数或全进程内存上限；达到上限时 worker 暂停发布输出，直到 UI drain 或停止信号唤醒。
- `worker.batch_bytes`：RX 字节合批上限，默认 `8192` 字节，也是调节 worker 输出颗粒度的主控项。
- `worker.backpressure_enabled`：启用实时接收的非阻塞队列背压；队列达到高水位时，Application 暂缓提交尚未接受的 RX 前缀，后续 pump 再继续，已拒绝字节仍由调用方持有。
- `worker.backpressure_rx_queue_high_watermark` / `low_watermark`：分别控制 RX 队列高/低水位比例。该机制只约束相关 worker RX 队列并配合输出队列消费，不代表 transport 上游、解析器、脚本生成数据或整个应用的全链路内存均有硬上限。
- `worker.output_flush_budget_ms`：每轮输出刷新的时间预算，默认 `2.0` ms。
- `worker.drain_request_outputs_unbounded`：超时前是否使用无帧预算限制的 drain，默认关闭。

### scripting.file_io

```yaml
scripting:
  file_io:
    enabled: true
    allow_protocol_dir: true
    allow_dialog_paths: true
    extra_allowed_roots: []
    max_open_files: 8
    default_chunk_bytes: 65536
    max_chunk_bytes: 1048576
    max_file_size_bytes: 1073741824
    max_write_file_size_bytes: 1073741824
    dialog:
      enabled: true
      remember_last_dir: true
    send_file:
      default_chunk_bytes: 65536
      max_inflight_chunks: 2
```

- `enabled`：总开关，关闭后脚本文件 IO 不可用。
- `allow_protocol_dir`：允许访问当前协议目录。
- `allow_dialog_paths`：允许访问用户通过文件对话框授权的路径。
- `extra_allowed_roots`：额外允许访问的根目录列表。
- `max_open_files`：脚本同时打开文件数上限。
- `default_chunk_bytes` / `max_chunk_bytes`：普通读写默认和最大分块大小。
- `max_file_size_bytes` / `max_write_file_size_bytes`：读取和写入文件大小上限。
- `dialog.enabled`：是否允许 `proto.fs.open_file_dialog()` / `open_dir_dialog()`。
- `dialog.remember_last_dir`：文件对话框是否记住上次目录。
- `send_file.default_chunk_bytes`：`proto.fs.send_file()` 默认分块大小。
- `send_file.max_inflight_chunks`：文件发送的最大在途分块数。

## logging

```yaml
logging:
  level: info
  file_path: logs/protoscope.log
```

- `level`：`trace`、`debug`、`info`、`warn`（`warning` 等价）、`error`。
- `file_path`：可选日志文件路径；为空时不写入该字段。

## communication

```yaml
communication:
  kind: tcp_client
  tcp_client:
    host: 127.0.0.1
    port: 9000
  tcp_server:
    bind_address: 0.0.0.0
    port: 9000
    reject_new_connection: true
  serial:
    port_name: COM1
    baud_rate: 115200
    data_bits: 8
    parity: none
    stop_bits: one
    flow_control: none
  udp_peer:
    bind_address: 0.0.0.0
    bind_port: 9001
    remote_host: 127.0.0.1
    remote_port: 9000
```

- `kind`：`tcp_client`、`tcp_server`、`serial`、`udp_peer`。
- `tcp_client.host` / `port`：TCP 客户端目标。
- `tcp_server.bind_address` / `port`：TCP 服务端监听地址和端口。
- `tcp_server.reject_new_connection`：已有连接时是否拒绝新连接。
- `serial.port_name`：串口名。
- `serial.baud_rate`：波特率。
- `serial.data_bits`：数据位。
- `serial.parity`：`none`、`odd`、`even`；其余取值按 `none` 处理。
- `serial.stop_bits`：`one`、`one_point_five`、`two`；其余取值按 `one` 处理。
- `serial.flow_control`：`none`、`hardware`、`software`。
- `udp_peer.bind_address` / `bind_port`：UDP 本地绑定地址和端口。
- `udp_peer.remote_host` / `remote_port`：UDP 远端地址和端口。
