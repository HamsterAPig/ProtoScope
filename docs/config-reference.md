# ProtoScope 配置参考

本文面向需要手工编辑 `config/protoscope.yaml` 的用户和维护者。配置事实源是 `include/protoscope/config/config.hpp` 的默认值、`src/config/config.cc` 的读写逻辑，以及仓库里的 `config/protoscope.yaml` 示例。

## 生效与保存

- 启动时从默认配置路径加载 YAML；缺失字段使用源码默认值。
- `文件 -> 保存配置` 和通讯面板的保存入口会把当前运行态写回 YAML。
- `文件 -> 重新加载配置` 会先保存当前协议工作区状态，再从磁盘重新加载配置。
- `app.auto_save.enabled` 开启后，配置 dirty 且达到 `interval_ms` 间隔时自动保存。
- `app.config_hot_reload.enabled` 开启后，宿主会检测外部文件变化并提示用户处理；不会在用户未确认时覆盖当前未保存状态。
- `performance.scale` 只影响未显式写出的吞吐预算项。某个预算项写入 YAML 后，该项优先于公共系数。启用 `performance.adaptive.enabled` 后，`performance.scale` 和自适应接管的热调预算不参与运行时调度。

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
  show_app_header: false
  window:
    title: ProtoScope
    width: 1600
    height: 900
    maximized: false
  font:
    chinese_glyph_range: simplified_common
```

- `show_app_header`：是否显示应用顶部 header。
- `window.title`：窗口标题。
- `window.width` / `window.height`：初始窗口尺寸。
- `window.maximized`：启动时是否最大化。
- `font.chinese_glyph_range`：`simplified_common` 或 `full`；`full` 适合需要显示更多 CJK 字形的场景。

### gui.wave

```yaml
gui:
  wave:
    fullscreen_mode: overlay
    control_mode: oscilloscope
    display_formula: offset_then_scale
    grid_division_readout_mode: display_value
    channel_card_width_mode: fixed
    channel_double_click_action: reset_scale_offset
    x_axis_double_click_action: fit_full_history
    y_axis_double_click_action: fit_visible_channels
    y_axis_double_click_adjust_offset: true
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
    downsample_start_multiplier: 2.0
    overview_max_samples: 20000
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
- `channel_card_width_mode`：`fixed` 或 `adaptive`。
- `channel_double_click_action`：`reset_all`、`reset_scale_offset`、`reset_scale`、`reset_offset`。
- `x_axis_double_click_action`：`fit_full_history` 或 `fit_visible_window`。
- `y_axis_double_click_action`：`fit_visible_channels` 或 `fit_active_channel`。默认聚合所有图例可见模拟通道；激活通道模式只取当前激活模拟通道，激活通道无效、隐藏或为 bit-display 时回退到可见模拟通道。
- `y_axis_double_click_adjust_offset`：Y 轴双击拟合时是否同步调整通道 offset，默认 `true`，会保持当前主图 Y 视口不变并把目标模拟波形移入视口内部。
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
- `peak_detect_downsample`：高密度主图是否启用示波器式 peak-detect 降采样，默认 `true`。开启时每个桶保留首点、极小值、极大值和末点并连成单条轨迹；关闭时回退旧的 min/max 包络渲染，便于对比。
- `downsample_start_multiplier`：可见点数超过预算多少倍后开始降采样。
- `overview_max_samples`：总览数据上限。
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
  transfer_log:
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
- `transfer_log.replay_raw_history_on_schema_switch`：切换 schema 后是否重放原始历史。
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
- `worker.output_queue_limit`：worker 输出队列上限。
- `worker.batch_bytes`：RX 字节合批上限，默认 `8192` 字节，也是调节 worker 输出颗粒度的主控项。
- `worker.backpressure_enabled`：是否启用背压。
- `worker.backpressure_rx_queue_high_watermark` / `low_watermark`：背压高低水位。
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

- `level`：`debug`、`info`、`warn`、`error`。
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
- `serial.parity`：`none`、`even`、`odd`、`mark`、`space`。
- `serial.stop_bits`：`one`、`onepointfive`、`two`。
- `serial.flow_control`：`none`、`hardware`、`software`。
- `udp_peer.bind_address` / `bind_port`：UDP 本地绑定地址和端口。
- `udp_peer.remote_host` / `remote_port`：UDP 远端地址和端口。
