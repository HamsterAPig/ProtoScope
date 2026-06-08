---
title: 离线复现工作台接入边界
type: guide
status: accepted
---
# 离线复现工作台接入边界

离线复现工作台是 Windows 主线的固定 Dock，职责是把已有现场复现能力集中到一个入口，不新建数据格式或回放引擎。

## 接入原则

- `.psraw` 导入、导出、回放时间轴继续复用 `GuiRuntime` 现有 Raw Capture 对话框和 `Application` 回放 API。
- `.pssession` 导入、导出继续复用会话包对话框和 `Application::importSessionPackage/exportSessionPackage`。
- 协议目录切换继续走 `requestProtocolWorkspaceSwitch()`，不要绕过工作区状态保存/恢复。
- ELF/ElfStaticView 数据继续走插件桥接和 `openElfStaticAddressDialog()`，不要耦合进工作台内部。
- 日志证据只汇总现有收发日志、宿主日志、脚本日志和请求追踪，并复用既有导出入口。

## UI 状态

固定 Dock 要同步：`showOfflineReplayDock_`、视图菜单、顶部视图 popup、`ProtocolDockVisibilityState`、默认 DockBuilder 布局、全屏 Focus 快照和全局快捷键 `Ctrl+8`。

## 验证

优先跑低成本验证：`cmake --build build`、`PROTOSCOPE_TEST_FILTER=keyboard_shortcut .\build\tests\protoscope_tests.exe`、`PROTOSCOPE_TEST_FILTER=dock_visibility_state .\build\tests\protoscope_tests.exe`。完整 `ctest` 当前很耗时，不作为该类 UI 聚合任务默认验收。