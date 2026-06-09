# src/include 可维护性与可扩展性评估标准

## 评分标准

总分 100。低于 75 分的模块进入重构候选，低于 60 分优先处理。

- 模块边界与依赖方向，25 分：`transport`、`scripting`、`plot`、`config`、`ui`、`app` 职责清晰；底层模块不依赖 UI；插件通过适配层接入。
- 公共 API 可维护性，20 分：`include` 中类型命名清楚、职责单一、参数语义明确；头文件不暴露不必要依赖；测试注入点不污染主路径。
- 实现复杂度，20 分：大文件、大函数、重复分支和长匿名工具函数需要收敛；优先把纯逻辑拆到可单测 helper。
- 可扩展性，15 分：新增协议、Dock、传输、波形状态时，不需要修改过多无关文件；扩展点优先复用已有配置和接口。
- C++20 使用质量，10 分：只在能降低复杂度时使用 `std::string_view`、`std::optional`、结构化初始化等特性，不做机械现代化改写。
- 测试与验证成本，10 分：核心逻辑可被 `protoscope_tests` 覆盖；GUI 展示逻辑至少保持可构建，纯判断逻辑应能脱离 GUI 测试。

## 本轮审查结论

- `gui_runtime.cpp` 原本同时负责窗口渲染、版本解析、GitHub 更新检查和结果展示，更新检查逻辑不属于主渲染流程，拆分后 UI 文件只保留触发与展示职责。
- `version_utils.hpp` 承接语义版本解析和比较，解决版本判断逻辑只能藏在 GUI 实现文件内、难以复用和单测的问题。
- `update_check.hpp/.cpp` 承接 GitHub tags 响应解析、当前构建版本评估和联网检查。`evaluateUpdateCheckTags` 不访问网络，便于覆盖失败、新版本、最新版本、开发构建等分支。
- `wave_dock_renderer.hpp` 原本只保存 `app::Application&` 却包含完整 `application.hpp`，改为前置声明后减少公开头文件依赖扩散。
- `Application` 的现场包导入/导出和 raw capture 导出流程已拆出内部 helper，主流程只保留编排；普通 psraw 与现场包导出共享 raw 窗口归一化逻辑。
- `raw_capture_file.cc` 已把内存编码和文件写入的公共编码准备、事件流写入逻辑收口，减少 `.psraw` 格式规则重复实现。
- `gui_runtime.hpp` 只保存 `app::Application&`，改为前置声明后减少 UI 公开头对应用宿主头文件的扩散。

## 当前优先级

1. 保持本轮重构行为等价，不改变 Lua、配置文件、传输或波形数据契约。
2. 优先拆纯逻辑和头文件依赖，不做大规模模块迁移。
3. 下一批候选按优先级处理：`script_host_core.cc` 的 Lua 值解析/profile 解析、`wave_dock_renderer.cc` 的视图状态计算、`dock_host.cc` 的表格工具栏/空态绘制拆分。
