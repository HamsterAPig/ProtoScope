# 主题与概览选区

## 应用内操作

“设置 → 主题”提供石墨青绿、石墨琥珀、冷白青蓝，以及用户主题。
选择主题会标记配置未保存，沿用手动保存和自动保存机制，不重载协议。
主题解析完成后在下一帧开始时应用。主题切换不清空采集数据、视口、游标身份或冻结状态，
只使颜色相关余辉缓存失效。

- “打开主题目录”：打开当前配置文件旁的 `themes` 目录。
- “导出当前主题”：生成 `themes/exported_<时间戳>.yaml`，包含当前所有令牌。
  文件名作为新 ID，不覆盖已有文件或内置 ID。导出后使用“重载主题”更新列表。
- “重载主题”：手动扫描 `.yaml`、`.yml` 文件并重新应用当前选择。
  任一文件解析失败时保留旧注册表及当前外观，状态栏显示文件、字段、行列。
- 启动时找不到所选主题或解析失败，使用石墨青绿；配置中的原 ID 保留。低对比或透明颜色不是解析错误。

## 内置与随附外观

内置外观：

- `professional_dark` / **石墨青绿**：石墨应用底、分层灰面板、克制青绿强调。
- `debug_high_contrast` / **石墨琥珀**：石墨灰底与琥珀选中态，细灰边框；保留历史配置 ID，不再附带高对比特判。
- `professional_light` / **冷白青蓝**：冷白内容与浅灰蓝宿主、青蓝操作与选中态。

三款均按文字、控件、容器、浮层分层设计；两深一浅，不新增第四款。
内置外观是否清晰舒适以实际截图评审为准，不以固定文字/边框比例作为加载条件。
视觉参考图不属于当前程序运行证据。

仓库不再随附 `graphite_cyan`、`warm_industrial`、`paper_lab` 三套额外主题。
普通构建和发布包仍创建 `config/themes` 用户目录；不会删除已有运行副本或用户配置。
旧 ID 对应文件缺失时启动回退石墨青绿并提示错误，配置原 ID 保留；
若目录仍有有效同名 YAML，则继续作为用户主题发现。用户主题格式、显式颜色、继承与导出能力不变。

普通按钮使用低饱和填充而非常驻亮框；工具栏与 ghost 默认透明，悬停/按下柔和填充，
切换按钮保留持续选中态。每个操作组最多一个主操作；输入、组合框与复选框保留必要边界。
去框仅发生在按钮局部作用域，退出恢复样式；自绘侧栏复用原生按钮以保留键盘激活和可见焦点。
三款共用这一角色策略，历史 `debug_high_contrast` 不再强制按钮/徽标常驻边框。

UI 不强制 4.5:1、3:1、12:1 或 7:1；合法低对比、同色、半透明和全透明配置都可加载，
不会因此回退、提亮、压暗或重写 alpha。普通悬停/按下状态只做固定强度叠色，
保留源 alpha，不搜索对比度；显式标签状态和图例状态字段直接使用。
用户可以有意选择不可见颜色，这与内置款追求清晰舒适的视觉设计是两回事。

业务禁用范围成对使用 `ui::beginDisabled` / `ui::endDisabled`，沿用原生不可交互与嵌套恢复。
三款统一 `DisabledAlpha=0.72`，不补偿正文 alpha，不改为空黑表面。
导航焦点、键盘激活、关闭/ESC、拖宽与菜单保护仍属交互验收要求，取消的是审美比例门槛。

### 边框、浮层与 Dock 状态

- `text_strong` / `text_muted` 是文字；`control_border` 是控件边框；
  `panel_border` 是容器、分隔线和普通图表边框，彼此不再通过正文颜色派生。
- `floating_background` 用于原生 Popup/Tooltip 背景和六类工具抽屉背景；
  `floating_border` 用于应用直接拥有的工具抽屉等浮层窗口装饰。
- ImGui 只有一个 `Border` 槽，**原生 Popup/Tooltip 外框复用 `control_border`**，
  `PopupBorderSize=1`。`floating_border` 不承诺控制 ColorEdit 内部生成的每层弹窗。
  不重写原生拾色器、不补画第二层边框，也不引入帧末装饰钩子。
- Main/Cursor/Measure/View/FFT/Renderer 抽屉仍是普通 NoDocking 窗口；Begin 前局部设置
  浮层背景/边框与 `WindowBorderSize=1`，Begin 后立即恢复内容样式。
  动画 alpha 独立作用于整个窗口；内部输入和嵌套 Popup 不继承浮层装饰边框。
- 图例宿主在 Begin 时关闭原生窗口边框，保留唯一自绘 `wave.legend_overlay_border`。
  透明装饰样式在内容绘制前恢复，右键菜单、ColorEdit 拾色弹窗和内部输入恢复正常原生边框。

| 可选 UI 字段 | 原生槽 / 状态 |
| --- | --- |
| `tab_background` | `Tab`：激活节点未选页 |
| `tab_selected` | `TabSelected`：激活节点已选页 |
| `tab_dimmed` | `TabDimmed`：失焦节点未选页 |
| `tab_dimmed_selected` | `TabDimmedSelected`：失焦节点已选页 |
| `tab_hovered` | `TabHovered`：共用悬停/按住填充，优先于上述填充 |
| `tab_selected_overline` | 激活已选页上沿 |
| `tab_dimmed_selected_overline` | 失焦已选页上沿 |

使用原生 Dock，标签上沿宽 1，整圈标签边框为 0。只有已选页绘制上沿；
悬停已选页保留对应上沿，未选页没有额外上沿。失焦已选页有独立填充和上沿，
不新增原生没有独立槽的“失焦悬停”设置。

以下历史场景不代表本次已经验收；本次仍需完整构建、CTest 与实际截图入口。
用户显式覆盖优先，不在解析后重新生成预设，不重写用户主题文件。

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
不写的字段直接继承新基底；上述十个新 UI 字段均可省略，旧 `version: 1` 文件继续加载。
例如只覆盖 `accent` 或 `panel_border`，不会隐式重算遗漏的 `tab_*`、`control_border` 或 `wave.*`。
解析顺序为复制完整基底再覆盖显式字段；应用与导出不重新执行预设初始化。
换一个合法自定义 ID 不改变实际配色，RGBA 和色板顺序保留；十六进制导出精度为每分量 8 bit。
未知字段、重复键、无效颜色、非有限尺寸仍报错；低对比不属于错误。
新文件含新字段，不保证旧程序能读取。完整字段可通过导出内置主题获得，字段名为 snake_case。

- `ui`：应用及面板背景、边框、强调色、状态色、主文字、辅助文字、普通绘图区。
- `wave`：波形背景、三级网格、通道标签、状态浮层、图例状态及测量浮层颜色。
- `wave.channel_palette`、`wave.cursor_palette`：1 至 64 个颜色，按序号循环使用。
  关闭主配置 `gui.wave.cursor_auto_color` 时，游标色板前两项对应 A/B，T 使用
  `(colorIndex + 2) % palette.size()`；切换主题保留游标身份和颜色序号。
  此开关默认 `true`，旧配置缺失时也启用。它独立于主题 `wave.correct_contrast`，
  不改写主题文件、通道源 RGBA 或 T 的 id/time/colorIndex。
- `wave.correct_contrast`：默认 `true`，只控制波形/游标身份显示修正，不控制 UI 正文、
  控件/浮层边框、按钮状态、错误/指示/日志颜色或读数正文。通道原始色值保留，在绘制时补足对比度；
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
A/B模拟、数字、交点及FFT读数标签使用主题 `panel_background_alt`、`text_strong` 和身份色边框，
保留显式 RGBA，不按 UI 文字阈值重新校色；算法评估背景与实际半透明背景分开。
身份色标签仍可使用自动算法的 `textPass` 结果，但不再增加 debug 基底 12:1 例外。
不依赖 ImPlot 按身份填充色经验选择黑/白字。
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

本次由工作流使用独立日志脚本：实现前 Configure → Build → Baseline；
实现后重新 Configure → Build → Acceptance → DiffCheck。
复用 `cmake-build-debug`，先重新配置再完整构建；GUI/BUILD_TESTING 开启，Ninja 和本地 GLFW 源目录不变。
完整 CTest 不设统一 60 秒超时，保留全量单测 900 秒。截图入口必须另行执行，
因为 `theme_render` 不是 CTest 用例，`--controls-only` 不能替代完整入口。

下列命令从仓库工作目录运行；直接启动 EXE 只在子进程 PATH 补当前 MinGW DLL 目录。
截图目录每轮独立，不覆盖 before：

```powershell
ctest --test-dir cmake-build-debug --output-on-failure --no-tests=error
.\cmake-build-debug\tests\protoscope_theme_render_tests.exe <本轮目录>/theme_render
.\cmake-build-debug\tests\protoscope_industrial_ui_tests.exe protocols/data_storage_demo <本轮目录>/industrial_ui
.\cmake-build-debug\tests\protoscope_auxiliary_cursor_tests.exe --capture <本轮目录>/auxiliary_cursor
.\cmake-build-debug\tests\protoscope_wave_ui_benchmark.exe --verify --gl --capture <本轮目录>/wave_ui_benchmark
.\cmake-build-debug\tests\protoscope_file_operation_ui_tests.exe --capture <本轮目录>/file_operation_ui
```

当前验收标准为字段忠实应用、RGBA 合成、真实 Dock 状态、六抽屉完整边框、图例右键/拾色器、
样式恢复、键盘/禁用交互、低对比/透明配置往返及 UI/波形开关隔离；保留波形算法与 CPU/GPU 回归。
内置款用实际截图评审清晰度，不把自定义低对比当错误。

### 本轮验收状态（2026-10-10，三轮后仍未通过）

工作流在 `out/theme-redesign-20261010-115742-096/logs/` 中记录：修改后 Configure 与完整 `cmake --build cmake-build-debug` 三轮均退出 0。完整命令
`ctest --test-dir cmake-build-debug --output-on-failure --no-tests=error`
第一轮退出 8（`protoscope_record_import_tests` 的 `asynchronous_import` 报
`unrecognized record volume file`），第二、三轮退出 0（35/35）；此瞬时失败未通过放宽文件校验或修改测试处理。
三轮 Acceptance 的完整 `theme_render_tests.exe <round>/theme_render` 均退出 1，stderr 为
`primary text color pixels missing`，每轮只生成 1 张截图。失败对应的像素断言位于
`tests/test_theme_render.cc`；在诊断采样区域和命中数未被报告前，不将其推定为生产缺陷或测试缺陷。
其它截图入口三轮报告退出 0：`industrial_ui`、`auxiliary_cursor`、`wave_ui_benchmark`、`file_operation_ui`；但 `theme_render` 提前失败，其内后续的真实 Dock、六类抽屉完整四边框、图例原生菜单/ColorEdit、样式恢复、低对比/全透明状态等场景未获验证，不能记作通过。D3D11/WARP、实际 DPI/跨屏和人工清晰度评审亦未完成。
三轮 Acceptance 阶段整体均未通过（退出码依次 8、1、1）；独立 `DiffCheck` 没有结果。故本任务**未获完整验收**，本节记录失败和未覆盖范围，不表示验证通过；未提交/未推送，因任务明确禁止提交且 Acceptance/DiffCheck 未通过。

工作流结果中前置首次 Configure 退出码 0；首次 Build 阶段因 PowerShell 包装脚本读取 `ExitCode` 失败退出 1，未启动 CMake 构建；局部修复后 before-retry Build 退出 0。主题实现前 Baseline 的 `theme_render`、`industrial_ui`、`auxiliary_cursor` 均退出 0。修改后每轮 Configure 和完整 Build 均退出 0；第二、三轮 CTest 均为 35/35。第一轮 CTest 的导入测试失败仅出现一次，后两轮通过，仍在计划中如实记录。


本文修改不取代工作流的原始命令日志；详细时间、工具参数及逐项退出码见任务计划 `ProtoScope-task-plan-20261010-115742-096.md`。

### 既有截图场景说明（历史覆盖，不代替本次验收）

`theme_render_tests` 使用隐藏 OpenGL 窗口，检查三套主题和深色往返的正文像素、
辅助文字像素、实际绘图区背景及 CPU/GPU 余辉后端，输出 BMP 截图。
测试覆盖主波形、Split、数字通道、FFT、概览和极窄固定选区。固定 T 随机种子，深色回切使用 `-return` 后缀避免覆盖。
三主题主要控件场景 `-controls-0..7-current`：默认、悬停、按下、键盘输入焦点、菜单弹出、模态、危险按钮悬停/按下，
同时展示复选框、标签页、表格、禁用及 ghost 控件。历史扩展场景 `-controls-8..11-current` 覆盖工具栏持续选中、键盘焦点、悬停和按下；`-business-docks` 使用真实通讯、协议、收发及日志 Dock，同入口输出供人工复核（不以截图非空冒充每项像素验收）。
Lua 业务控件的现有入口为 `tests/protoscope_industrial_ui_tests.exe protocols/data_storage_demo <截图目录>`，现在按三内置主题和宽/窄布局输出。按控件区域检查预期文字核心像素与实际内部背景；
输入焦点使用空文本排除文字冒充光标。历史边界核心颜色容差为每RGB分量2/255；
本次须按实际背景、字段 alpha、样式 alpha 和纹理覆盖率核验，而不是沿用旧正文比例门槛。
同场景另输出 `-baseline-tokens-current-renderer`，仅回放旧 UI 基础令牌，使用当前算法与控件实现；
它不是原始版本截图，不可标记为原始 before 或已通过新标准。真正 before/after 仍需各自版本同入口运行。
辅助游标集成场景在三主题 × Overlay/Stacked/Split 中固定可见窗0..1秒，A/B位于真实采样点0.25/0.75秒，
启用且固定，清理前序交互留下的旧读数并关闭跟随；验证真实绘图返回的两条有效读数及完整两行标签字形，
同时保留T身份与截图检查。独立 `readout-labels` 图不能替代该集成覆盖。
此图形入口验证 OpenGL；D3D11 和 D3D11 WARP 需单独进行后端验收。

### 历史验收（2026-09-22，不代表本次主题改版）

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
`tests/protoscope_auxiliary_cursor_tests.exe --capture <截图目录>`。完整工具参数及日志索引见该历史交付报告。
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
该历史验收采用的普通正文 4.5、高对比正文 12、辅助文字 7 目标现已取消，
既不作为当前 UI 加载条件，也不作为新截图通过标准。

### 历史记录

以下是此前主题实现的历史验收，不作为本轮通过数字：

历史主题实现验收：独立 Ninja/MinGW Release 构建成功，CTest 18/18 通过；
隐藏 OpenGL 窗口的 24 个主题/场景检查通过（18 个唯一场景及 6 个深色回切场景），
另通过三套主题的辅助游标截图与鼠标交互测试。
实际像素检查覆盖正文 4.5:1、高对比主文字 12:1、辅助文字 7:1，
以及 CPU/GPU 余辉蓝色核心线约 3:1（允许 8-bit 量化误差）。
这不替代所有平台、任意用户主题和所有抗锯齿边缘像素的人工检查。
