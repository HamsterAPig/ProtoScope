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

## 三套内置外观

- `professional_dark`：深墨蓝灰应用底、分层蓝灰面板、蓝青强调。
- `professional_light`：冷白应用底、白色内容面板、深蓝灰正文与蓝色强调，工具组不使用大片灰底。
- `debug_high_contrast`：近黑底与明亮信号、清晰键盘焦点；装饰边框和网格保持克制。

内置正文/辅助文字目标分别为普通主题 4.5:1/4.5:1、高对比主题 12:1/7:1，
悬停、按下、选中填充同时受这两个阈值约束。必要输入/按钮边界从辅助文字生成，
与低对比装饰 `panel_border` 分工；焦点、输入光标、选中标签上沿等使用显式状态槽。
危险按钮/状态徽标通过身份色边界与克制填充表达状态，正文保持主题文字色。
业务禁用范围成对使用 `ui::beginDisabled` / `ui::endDisabled`，保留 ImGui 禁用交互语义及嵌套恢复。
普通主题整体 alpha 为 0.72；高对比主题边界等元素仍为 0.86，但正文独立保留 0.98 的 alpha，
按钮/输入表面改为空黑填充，避免把所有元素做成几乎相同的启用态来维持对比度。
禁用与可用按钮同文字同尺寸的真实像素必须可区分（至少31个像素有通道差≥20）；
按字体图集及实际 UV 滤波确认的≥90%覆盖字形核心必须**全部**满足普通4.5/高对比12阈值，
不是仅有少量核心达标，不要求低覆盖抗锯齿边缘也达到正文阈值。原生 `ImGui::BeginDisabled`
不自动分离正文和表面，因此业务调用应使用上述主题入口。

以上目标已在下述本轮固定 OpenGL 场景中完成回归和独立复核，不代表任意设备或用户主题均已验收。用户显式覆盖优先，未填写字段继承新基底；
不改主题ID/格式，不在解析后重新生成预设，不重写用户主题文件。

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
  关闭主配置 `gui.wave.cursor_auto_color` 时，游标色板前两项对应 A/B，T 使用
  `(colorIndex + 2) % palette.size()`；切换主题保留游标身份和颜色序号。
  此开关默认 `true`，旧配置缺失时也启用。它独立于主题 `wave.correct_contrast`，
  不改写主题文件、通道源 RGBA 或 T 的 id/time/colorIndex。
- `wave.correct_contrast`：默认 `true`。通道原始色值保留，在绘制时补足对比度；
  已达标色保持原样；低 alpha 优先寻找保持 RGB 的最小可行 alpha，只有不透明原色也不达标才
  分别求黑/白两方向的最小可行调整，再按RGB欧氏距离选较小者，而非固定最大端点对比方向。半透明背景先合成当前宿主 WindowBg/ChildBg/PopupBg（无 UI 上下文时使用主题应用底）。
  显式全透明色和关闭修正始终保留原语义；概览淡化、glow 与余辉层次须由真实截图复核。
- `wave.light_persistence`：浅色基底默认 `true`，CPU/GPU 都使用预乘 alpha 的覆盖累积，
  防止重复叠加趋白；深色基底默认使用原有加法余辉。
- `wave.selection_color`：固定模式默认颜色，RGB 用于边框，填充 alpha 独立计算。
- `wave.selection_min_alpha`、`wave.selection_max_alpha`：默认 `0.10`、`0.28`，
  必须满足 `0 <= min <= max <= 1`。
- `metrics`：圆角、窗口及控件留白、间距、网格线宽与短刻度长度。
  界面尺寸允许 `[0,64]`，网格尺寸允许 `(0,64]`。

主题不配置字体加载、Dock 布局或协议行为。原内置枚举 API 仍可使用，
`GuiConfig.theme` 现在为字符串，旧配置中的两个内置 ID 无须迁移。

## 数据游标自动色

主配置示例：`gui: { wave: { cursor_auto_color: true } }`。自动色复用概览的固定科研候选，
先检查实际合成背景的图形 3:1 和文字 4.5:1，再比较最差波形对比与最小 Lab 色差。
无解时保留未达标状态，图形加黑/白护边，标签正文回退主题文字；不保证任意重叠点或抗锯齿像素达标。
A/B模拟、数字、交点及FFT读数标签使用统一不透明背景、主题正文和身份色边框，不再依赖ImPlot按身份填充色经验选择黑/白字。
A/B 与 T 分别分配身份槽；A/B 实线、T 虚线及编号保留非颜色线索，T 不是触发线。

每个 Dock 在帧准备阶段冻结结果，概览、Overlay/Stacked/Split、FFT A/B、标签和读数共用；
绘制期发现的 Split 行裁剪和 FFT 图例显隐在下一帧生效。Split 首帧按默认前四行初始化。
普通拖动锁定显隐变化，主题/背景或自动开关变化在下一帧重建；采样推进和鼠标坐标不进入颜色键。
仅收集已有显示元数据，不扫描原始样本或 GPU 像素；余辉累积与多轨迹叠加仍需真实像素验收。

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
宿主层捕获整个框体用于水平平移，左右边缘缩放；极窄框只在中段把手附近抓边，上下框体仍可平移。
边缘双击恢复相应历史端点；图外释放、失焦、无数据和折叠会清理捕获，框外按下移入不抢占。

## 验证入口

```powershell
cmake --build build-theme
ctest --test-dir build-theme --output-on-failure
.\build-theme\tests\protoscope_theme_render_tests.exe build-theme/theme-captures
.\build-theme\tests\protoscope_auxiliary_cursor_tests.exe --capture build-theme/cursor-captures
```

`theme_render_tests` 使用隐藏 OpenGL 窗口，检查三套主题和深色往返的正文像素、
辅助文字像素、实际绘图区背景及 CPU/GPU 余辉后端，输出 BMP 截图。
测试覆盖主波形、Split、数字通道、FFT、概览和极窄固定选区。固定 T 随机种子，深色回切使用 `-return` 后缀避免覆盖。
新增三主题主要控件场景 `-controls-0..7-current`：默认、悬停、按下、键盘输入焦点、菜单弹出、模态、危险按钮悬停/按下，
同时展示复选框、标签页、表格、禁用及 ghost 控件。按控件区域检查预期文字核心像素与实际内部背景；
输入焦点使用空文本排除文字冒充光标。边界核心颜色容差为每RGB分量2/255；正文目标不降低。
同场景另输出 `-baseline-tokens-current-renderer`，仅回放旧 UI 基础令牌，使用当前算法与控件实现；
它不是原始版本截图，不可标记为原始 before 或已通过新标准。真正 before/after 仍需各自版本同入口运行。
辅助游标集成场景在三主题 × Overlay/Stacked/Split 中固定可见窗0..1秒，A/B位于真实采样点0.25/0.75秒，
启用且固定，清理前序交互留下的旧读数并关闭跟随；验证真实绘图返回的两条有效读数及完整两行标签字形，
同时保留T身份与截图检查。独立 `readout-labels` 图不能替代该集成覆盖。
此图形入口验证 OpenGL；D3D11 和 D3D11 WARP 需单独进行后端验收。

### 本轮验收（2026-09-22）

工作流在 `build-overview-cursor-colors-20260922-162101` 使用 Ninja / MinGW Release、GUI 和测试开启，
最终配置、构建、两个截图入口均退出 0；完整 CTest 为 **35/35，0 失败，47.82 秒**。
这是工作流实际执行及日志记录，不是文档整理阶段重新运行的结果。最终独立复核 `issues=[]`；
早期读数标签、禁用反馈、RGB 调整方向、字体核心断言及 A/B 夹具覆盖问题已修复并复验。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File build-overview-cursor-colors-20260922-162101/verify.ps1 Configure
powershell -NoProfile -ExecutionPolicy Bypass -File build-overview-cursor-colors-20260922-162101/verify.ps1 Build
powershell -NoProfile -ExecutionPolicy Bypass -File build-overview-cursor-colors-20260922-162101/verify.ps1 Test
# RenderBefore 已在实施前执行；不要用修改后的程序覆盖基线。
powershell -NoProfile -ExecutionPolicy Bypass -File build-overview-cursor-colors-20260922-162101/verify.ps1 RenderAfter
```

原生命令为 `cmake -S . -B <上述目录> -G Ninja`（显式设置工具链、Release、GUI/测试和本地 GLFW）、
`cmake --build <上述目录>`、`ctest --test-dir <上述目录> --output-on-failure --no-tests=error`；
截图入口分别为 `tests/protoscope_theme_render_tests.exe <截图目录>` 与
`tests/protoscope_auxiliary_cursor_tests.exe --capture <截图目录>`。完整工具参数及日志索引见本次交付报告。
不设置统一 60 秒超时，保留全量单测的 900 秒 CTest 配置。

最终日志位于上述构建目录：`verify-Build-41db3fc29f3d4ba1806858637f3fc3f3.log`、
`verify-Test-565c72cf496b450ab230c8c6df913217.log`、
`verify-RenderAfter-c0d8581749c6497f8ea2d7b723183b34.log`（24 个波形/往返、24 个当前控件场景）、
`verify-RenderAfter-2f2b72eb6f0844f4bb2599ab5743e3df.log`（辅助游标）。
截图目录 `theme-captures-before/after` 分别 18/75 张，`cursor-captures-before/after` 各 25 张。
新增控件、独立读数以及单独命名深色回切场景没有原版 before，不以旧令牌回放或 after 冒充。
A/B 集成 after 夹具重置为 250/750 ms，before 的视窗/读数未追改，因此不把所有前后差异归因于配色。

真实图形验收仅限 OpenGL 固定场景；D3D11/WARP、实际系统 DPI、跨原生窗口/跨屏释放、
任意自定义透明表面、多轨迹叠加的所有像素及用户设备感知效果未验证。
普通正文 4.5、高对比正文 12、辅助文字 7 的目标不扩展为低覆盖抗锯齿边缘保证。

### 历史记录

以下是此前主题实现的历史验收，不作为本轮通过数字：

历史主题实现验收：独立 Ninja/MinGW Release 构建成功，CTest 18/18 通过；
隐藏 OpenGL 窗口的 24 个主题/场景检查通过（18 个唯一场景及 6 个深色回切场景），
另通过三套主题的辅助游标截图与鼠标交互测试。
实际像素检查覆盖正文 4.5:1、高对比主文字 12:1、辅助文字 7:1，
以及 CPU/GPU 余辉蓝色核心线约 3:1（允许 8-bit 量化误差）。
这不替代所有平台、任意用户主题和所有抗锯齿边缘像素的人工检查。
