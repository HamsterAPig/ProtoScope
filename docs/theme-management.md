# 主题与概览选区

## 应用内操作

“设置 → 主题”提供专业深色、仪器深黑（高对比）、专业浅色，以及用户主题。
选择主题会标记配置未保存，沿用手动保存和自动保存机制，不重载协议。
主题解析完成后在下一帧开始时应用。主题切换不清空采集数据、视口、游标身份或冻结状态，
只使颜色相关余辉缓存失效。

- “打开主题目录”：打开当前配置文件旁的 `themes` 目录。
- “导出当前主题”：生成 `themes/exported_<时间戳>.yaml`，包含当前所有令牌。
  文件名作为新 ID，不覆盖已有文件或内置 ID。导出后使用“重载主题”更新列表。
- “重载主题”：手动扫描 `.yaml`、`.yml` 文件并重新应用当前选择。
  任一文件解析失败时保留旧注册表及当前外观，状态栏显示文件、字段、行列。
- 启动时找不到所选主题或解析失败，使用专业深色；配置中的原 ID 保留。

## 文件格式

参见 [带注释的模板](examples/theme.yaml)。一个文件一个主题，必填字段：

```yaml
version: 1
id: my_theme
name: 我的主题
base: professional_light
```

`id` 仅允许字母、数字、下划线和连字符，须与所有内置及用户主题唯一。
`base` 只能为 `professional_dark`、`debug_high_contrast`、`professional_light`。
不支持用户主题多级继承，不执行脚本。

`ui` 与 `wave` 中的颜色使用带引号的 `"#RRGGBB"` 或 `"#RRGGBBAA"`。
不写的字段直接继承基底；未知字段、重复键、无效颜色、非有限尺寸均报错。
完整可用字段可通过导出内置主题获得，字段名为 snake_case。

- `ui`：应用及面板背景、边框、强调色、状态色、主文字、辅助文字、普通绘图区。
- `wave`：波形背景、三级网格、通道标签、状态浮层、图例状态及测量浮层颜色。
- `wave.channel_palette`、`wave.cursor_palette`：1 至 64 个颜色，按序号循环使用。
  游标色板前两项对应 A/B，其余项按 T 游标的稳定颜色序号选取；
  切换主题保留游标身份和颜色序号，仅改变显示配色。
- `wave.correct_contrast`：默认 `true`。通道原始色值保留，在绘制时补足对比度；
  先调整 RGB 明暗，必要时再提高 alpha。显式全透明颜色始终透明。
- `wave.light_persistence`：浅色基底默认 `true`，CPU/GPU 都使用预乘 alpha 的覆盖累积，
  防止重复叠加趋白；深色基底默认使用原有加法余辉。
- `wave.selection_color`：固定模式默认颜色，RGB 用于边框，填充 alpha 独立计算。
- `wave.selection_min_alpha`、`wave.selection_max_alpha`：默认 `0.10`、`0.28`，
  必须满足 `0 <= min <= max <= 1`。
- `metrics`：圆角、窗口及控件留白、间距、网格线宽与短刻度长度。
  界面尺寸允许 `[0,64]`，网格尺寸允许 `(0,64]`。

主题不配置字体加载、Dock 布局或协议行为。原内置枚举 API 仍可使用，
`GuiConfig.theme` 现在为字符串，旧配置中的两个内置 ID 无须迁移。

## 概览选区

波形设置中的“概览设置”提供自动/固定选色、固定色色块、填充不透明度范围以及恢复主题默认。
保存配置后对应：

```yaml
gui:
  theme: professional_light
  wave:
    overview_selection:
      mode: auto            # auto 或 fixed
      # fixed_color: "#4477AA"
      # min_alpha: 0.10
      # max_alpha: 0.28
```

未覆盖的颜色和 alpha 随主题变化。只覆盖一侧 alpha 后若与新主题另一侧冲突，
以用户显式值为准收缩继承侧，保证范围有效；同时显式配置时下限大于上限会报错。
固定模式固定 RGB，两种模式都自适应填充；悬停和拖动不会突破最大 alpha。

自动模式在 Paul Tol Bright 与 Okabe-Ito 候选中先筛选背景对比度至少 3:1 的颜色，
再按实际概览合成色的 20% 分位对比度排序，感知色差用于平分排序。
采样仅光栅化已有折线和包络，按实际顺序叠加到 96×24 色栅格，排除隐藏通道；
不重新读取原始样本或 GPU 帧缓冲。

数据选色最多每 200ms 评估一次，候选连续 400ms 优于当前至少 15% 才切换；
拖拽锁色，主题、模式、通道显隐与显示颜色变化立即失效。
填充从下限搜索，使至少 60% 采样位置的填充前后对比度达到 1.15:1，
未达到时使用上限；alpha 平滑变化，2px 边框、对比护边及左右把手保障定位。
极窄或裁剪选区只扩展视觉标记，不改变实际时间范围。

## 验证入口

```powershell
cmake --build build-theme
ctest --test-dir build-theme --output-on-failure --timeout 60
.\build-theme\tests\protoscope_theme_render_tests.exe build-theme/theme-captures
.\build-theme\tests\protoscope_auxiliary_cursor_tests.exe --capture build-theme/cursor-captures
```

`theme_render_tests` 使用隐藏 OpenGL 窗口，检查三套主题和深色往返的正文像素、
辅助文字像素、实际绘图区背景及 CPU/GPU 余辉后端，输出 BMP 截图。
测试覆盖主波形、Split、数字通道、FFT、概览和极窄固定选区。
此图形入口验证 OpenGL；D3D11 和 D3D11 WARP 需单独进行后端验收。

本次实现验收：独立 Ninja/MinGW Release 构建成功，CTest 18/18 通过；
隐藏 OpenGL 窗口的 24 个主题/场景检查通过（18 个唯一场景及 6 个深色回切场景），
另通过三套主题的辅助游标截图与鼠标交互测试。
实际像素检查覆盖正文 4.5:1、高对比主文字 12:1、辅助文字 7:1，
以及 CPU/GPU 余辉蓝色核心线约 3:1（允许 8-bit 量化误差）。
这不替代所有平台、任意用户主题和所有抗锯齿边缘像素的人工检查。
