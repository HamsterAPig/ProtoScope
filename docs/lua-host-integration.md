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

## 脚本弹窗窗口参数

`proto.ui.alert(opts)` / `proto.ui.confirm(opts)` 现在支持可选的 `window` 子表，用于控制脚本弹窗的初始尺寸、位置和交互行为。

```lua
proto.ui.alert({
  title = "连接弹窗",
  message = "设备已连接",
  window = {
    width = 520,
    height = 260,
    x = 120,
    y = 80,
    resizable = true,
    movable = true,
    auto_resize = false,
  }
})
```

- `width` / `height`：弹窗首次出现时的初始宽高。
- `x` / `y`：相对主视口左上角的初始位置，仅首次出现时生效。
- `resizable` / `movable`：控制用户是否可以拖动改变大小或位置。
- `auto_resize`：为 `true` 时恢复 ImGui 自动尺寸模式，未传时默认关闭。

当前未传 `window` 时，会继续沿用原有的自动尺寸弹窗行为；正文里不再重复绘制 `title`。

## 文件对话框边界

`proto.fs.*` 仍走系统原生文件/目录对话框，不和 `proto.ui.alert/confirm` 共用同一套 ImGui 自绘窗口体系。

当前 `proto.fs.*` 只稳定支持：

- `title`
- `default_path`
- `filters`

像宽高、位置、是否允许拖动这类窗口行为，暂不承诺由 Lua 控制。
