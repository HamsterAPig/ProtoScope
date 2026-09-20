# 波形视图、悬停读数与概览实施计划

## 范围与提交边界

基线：`252bf480e25b9925d2ed2fbcc55a92f46a7fbff9`。
任务分支：`task/wave-view-render`。保留原有未跟踪文件。

1. `docs: 记录波形渲染预算与分步实施计划`
2. `fix: 将主图纵向操作写入通道缩放与偏移`
3. `feat: 增加 bit 通道悬停读数选项`
4. `feat: 增加概览逐通道归一化配置`

## 渲染预算

`src/ui/wave/wave_channel_renderer.cc` 的 `makeRenderBudget`：

```text
C = max(全部通道数, 1)，包含隐藏通道
E = 开启 Glow 时 16，否则 6
P = 有效单通道上限，0 回退到 1200
V = 有效总顶点预算，0 回退到 60000
B = max(1, min(内容像素宽度, P, max(1, floor(V / (C * E)))))
T = ceil(B * max(downsample_start_multiplier, 1))
```

例如宽度 1000、8 个 CH、默认预算且开启 Glow，B=468，默认 T=936。
预算是每通道目标，不是实际总绘制点数或 GPU 顶点硬上限：
低密度分支可绘制至阈值并附带边界保护点；peak 分支限制输出点数；
envelope 统计桶数；bit 按轨道分摊独立点预算。

概览桶数取宽度、B、非零 `overview_max_samples` 的最小值，每桶缓存最多两个极值点。
`overview_max_samples=0` 只取消这一项限制。
自适应开启时，三项预算使用内置基线乘压力倍率，覆盖静态配置；
`performance.scale` 不直接参与上述公式。
当前分屏普通波形直接绘制窗口样本，没有同样的降采样预算；余辉另走渲染路径。
本次仅解释预算，不修改预算算法。

## 主图纵向操作

根因：堆叠临时改写显示样本，分屏自动改变各子图 Y 范围并回写共享 Y 状态，
分屏返回叠加没有强制刷新轴限；适配仅移动视窗。

将目标显示变换 `y'=a*y+b` 写入真实参数，保留原始样本与 ratio：

```text
scale' = a * scale
ScaleThenOffset: offset' = a * offset + b
OffsetThenScale: offset' = offset + b / scale'
```

统一经 `applyChannelTransformOverride` 写入覆盖状态、失效缓存。
堆叠沿用波幅 1、中心间距 1.6；分屏使用普通模式固定 Y 基准，
通过各 CH 参数居中适配。返回叠加保留参数。
切换模式强制刷新轴限；分屏仅同步共享 X。
布局对齐仅在模式切换、可见 CH 集合变化或显式适配执行，无数据延后；
普通刷新不重复累乘。参数更新后重建帧数据。

适配保留全历史 X 首尾，各可见普通 CH 使用 verticalAutoFitMultiplier 独立适配，
隐藏 CH 不修改。Y 平移、缩放、框选、自动适配统一转换为参数操作；
整图作用于可见普通 CH，单通道作用于目标 CH。X 导航保持原行为。
Y 动画不再叠加隐式变换。保留负 scale，常量只居中，无有效数据跳过。
OffsetThenScale 的零 scale 无法平移时不修改并提示；显式适配可恢复非零 scale。
bit 保持固定数字轨道与 bit_display.y_offset；FFT 不在本项范围。

## bit 悬停读数

新增 `bit_display.hover_readout?: boolean` / `BitDisplaySpec::hoverReadout{false}`。
省略、nil、bit_display=true 默认关闭，非法类型报告字段错误。
通道选项和全局悬停开关同时开启才显示 CH名.bit=0/1，
在候选筛选时排除关闭通道，不影响游标吸附、测量和普通波形。
覆盖三种布局；贯通描述、快照、setup 比较、原始录制回放。
仅修改此选项不清历史，旧录制缺字段按 false。
修改 API Manifest 后生成 LuaLS 文件，同步文档与示例。

## 概览归一化

新增 `gui.wave.overview_normalize_channels: false`，贯通读取、保存、
应用、运行状态回收与配置文档。
开启后复用全历史包络，各 CH 独立映射到 [-1,1]。
仅改概览绘制数据，不改共享缓存、主图参数或读数。
常量在 0，空通道跳过，非有限值不参与范围计算；
保留隐藏策略、导航、选择框和游标。

## 验收

- 先补回归测试并注册，再实现。
- 主图：两种公式、负 scale、常量、零 scale、完整布局循环、
  分屏悬停、重复适配无漂移、隐藏、锁定轴、缓存与状态保存。
  增加真实 ImGui/ImPlot 多帧切换测试。
- bit：默认/false/true/非法类型、全局开关、重叠通道、三布局、
  setup 历史保留、录制兼容、LuaLS。
- 概览：百万倍幅值差、常量/空数据、配置往返、
  全历史导航、主图参数不变。
- 使用独立 Ninja 构建目录；运行构建、CTest、
  `python tools/generate_luals_api.py --check`、`git diff --check`。

## 执行记录

- 既有结果仅作参考：headless 32/32，主测试 505/506；
  application_large_rx_event_drains_by_byte_budget 单独重跑通过。
  这些不是本次重新构建的结果。
- 文档提交：`ad167be`。
- 主图提交：`b810cf7`。
- 主图：新增仿射计算、参数化布局与适配、分屏固定 Y 基准及仅同步 X、
  主图整图 Y 操作提交参数；新增真实 ImGui/ImPlot 多帧布局测试。
  `y_axis_double_click_adjust_offset` 保留配置往返兼容，适配统一居中写入两项参数。
- 配置：`cmake -S . -B build-wave-view -G Ninja
  -DCMAKE_MAKE_PROGRAM=C:/Users/jinming/AppData/Local/Programs/CLion/bin/ninja/win/x64/ninja.exe
  -DCMAKE_BUILD_TYPE=Release` 成功。
- `cmake --build build-wave-view -j 6` 成功；波形定向测试 108/108。
- 首次 `ctest --test-dir build-wave-view --output-on-failure`：
  headless 与 LuaLS 通过，主测试 506/508；
  application_large_rx_event_drains_by_byte_budget 与
  application_complete_disconnect_keeps_realtime_backlog 各自定向重跑通过。
  不将定向重跑解释为全量稳定通过。
- bit：`cmake --build build-wave-view -j 6` 成功；
  `PROTOSCOPE_TEST_FILTER=bit_` 30/30，
  `PROTOSCOPE_TEST_FILTER=raw_capture_file_plot_setup` 2/2，
  `python tools/generate_luals_api.py --check` 通过。
  覆盖开关解析、全局开关、重叠候选、游标不受影响、setup 历史保留、旧录制兼容。
