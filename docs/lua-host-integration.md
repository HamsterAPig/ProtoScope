# Lua 宿主 API 接入指南

Lua 脚本通过宿主注入的全局 `proto` 与 C++ 交互。脚本不直接操作串口、TCP、UDP、ImGui 或底层文件句柄，所有能力都由 C++ API 模块挂到 `proto.*`。

## 注册模型

`ScriptHost::loadScriptFile()` 只负责创建 Lua runtime、设置 `package.path`、创建 `proto` 表并调用 `registerLuaApi()`。

新增 C++ 暴露能力时，按职责放入对应注册函数：

- `registerCoreApi`：日志、事件、定时器。
- `registerTxApi`：`proto.send`、`proto.request`、`proto.request_done`。
- `registerStatusApi`：状态栏。
- `registerUiApi`：弹窗。
- `registerFileApi`：文件对话框、文件读写、文件发送。
- `registerPlotApi`：波形 setup/push。
- `registerControlApi`：控件读写。
- `registerCodecApi`：CRC、bits 等纯工具。

不要把新的 `proto.set_function(...)` 直接写回加载流程。

## LuaLS 生成

`protocols/protoscope_api_manifest.json` 是 `protocols/protoscope_api.lua` 的 source of truth。

生成命令：

```powershell
python tools\generate_luals_api.py
```

检查命令：

```powershell
python tools\generate_luals_api.py --check
```

新增、删除或修改 Lua API 时，必须同步更新 Manifest，再重新生成 `protoscope_api.lua`。测试会校验生成结果与提交文件一致。

## 兼容策略

- 现有 `proto.*` 名称保持兼容，不因内部模块化重构改脚本调用方式。
- Lua 仍保持传输无关，只处理 `ProtoPayload`、`ProtoBuffer`、`ProtoConnectionContext` 和 TX/弹窗/文件/波形事件。
- `ctx.kind` 只用于必要的展示或分支，协议解析应优先按 bytes 和业务帧格式编写。
