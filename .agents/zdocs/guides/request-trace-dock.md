---
title: 请求追踪 Dock 接入边界
type: guide
status: accepted
---
请求追踪 Dock 采用固定 Dock 接入，不属于 Lua 动态 Dock。

关键边界：
- 数据源在 `DockStore::requestTraceState()`，历史上限由 `gui.log_history.request_trace_limit` 控制。
- `Application` 在 Lua Tx 队列生命周期写入时间线：排队、已发送、完成、失败、超时、拒绝、丢弃、取消、熔断重置。
- Lua 回调契约不变，仍使用现有 `TxRequest` / `TxEvent` / `proto.request_done` / `proto.reset_request_guard()`。
- UI 入口是固定窗口“请求追踪”，可通过视图菜单和 `Ctrl+7` 切换，并随协议工作区保存可见性。
- 测试优先使用 `PROTOSCOPE_TEST_FILTER=request_trace|dock|keyboard_shortcut|application_request_done_success_does_not_set_comm_error|application_guarded_request`，避免默认跑全量长耗时测试。