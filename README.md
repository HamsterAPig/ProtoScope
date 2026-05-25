# ProtoScope

ProtoScope 当前已具备第一版 `TCP + Lua` 联调主链路，核心目标是把“连接设备 -> 执行协议脚本 -> 观察收发与事件”这条路径跑通。

## 当前能力

- `通讯配置`：`TCP Client / TCP Server / Serial`
- `配置文件管理`：支持保存、显式重载，以及可选的“外部配置变更提醒”
- `发送编辑器`：HEX 模式自动归一化为大写双字节分组，并在奇数个 nibble 时禁止发送
- `Lua 声明式控件`：通过 `controls()` 把脚本控件挂进 Dock
- `Lua 协议回调`：脚本可响应打开、关闭、收包、控件交互和定时器事件
- `接收展示`：统一展示系统日志、TX/RX、Lua 事件
- `波形 Dock`：当前仍为占位，保留后续 ImPlot 接口边界

## 已接入依赖

- `3rdparty/spdlog`
- `3rdparty/yaml-cpp`
- `3rdparty/asio`
- `3rdparty/imgui`
- `3rdparty/libdwarf-code`
- `3rdparty/lua`
- `3rdparty/sol2`

## 构建与测试

首次拉取：

```powershell
git submodule update --init --recursive
```

如果本机已有 Make/Ninja 等生成器，可直接使用你熟悉的生成目录。Windows 下当前已验证过的命令示例：

```powershell
"C:\Program Files\CMake\bin\cmake.exe" -S . -B build-gcc
"C:\Program Files\CMake\bin\cmake.exe" --build build-gcc
"C:\Program Files\CMake\bin\ctest.exe" --test-dir build-gcc --output-on-failure
```

如需使用 Visual Studio 生成器，也可以单独指定新的构建目录。

## 协议脚本目录结构

协议目录固定按下面约定组织：

```text
protocols/
└── <protocol_name>/
    └── main.lua
```

- 入口脚本固定为 `main.lua`
- 主程序当前只要求协议目录下存在 `main.lua`，后续拆分可通过 `require()` 自行组织
- 默认示例目录为 `protocols/default_protocol`

## Lua 宿主 API

脚本侧统一通过全局对象 `proto` 与主程序交互：

- `proto.log(level, message)`
  - 写入 Lua 日志面板
  - `level` 建议使用 `info / warn / error`
- `proto.send(data)`
  - 发送数据到当前连接
  - 支持 `number[]`，也支持 HEX 字符串
- `proto.emit(name, payload)`
  - 向主程序事件面板发出业务事件
  - `payload` 可为字符串、数字、布尔值或 table
- `proto.set_timer(name, delay_ms)`
  - 注册一次性定时器
- `proto.cancel_timer(name)`
  - 取消一次性定时器
- `proto.get_control(id)`
  - 读取当前控件值
- `proto.set_control(id, value)`
  - 从脚本侧更新控件值，更新后 UI 会在下一帧反映
- `proto.crc16_modbus(payload)`
  - 计算 Modbus CRC16，返回整数校验值
- `proto.crc16_ccitt_false(payload)`
  - 计算 CRC16/CCITT-FALSE，返回整数校验值
- `proto.crc32_ieee(payload)`
  - 计算 CRC32/IEEE，返回整数校验值

## Lua 回调生命周期

当前约定的回调面如下：

- `ui()`
  - 返回 Dock 描述数组
  - 每个 Dock 需提供 `id`、`title`、`controls`
  - 当前默认脚本优先使用该入口组织多面板 UI
- `controls()`
  - 返回控件描述数组
  - 当 `ui()` 不存在时，作为兼容旧脚本的回退入口
  - 若结构非法，脚本加载失败
- `on_open(ctx)`
  - 连接建立后调用
- `on_close(ctx)`
  - 连接关闭后调用
- `on_error(ctx, message)`
  - 连接出错后调用
- `on_bytes(ctx, bytes)`
  - 收到数据后调用
- `on_timer(ctx, name)`
  - 定时器到期后调用
- `on_control(ctx, id, value)`
  - 用户点击按钮、修改输入框或切换勾选项后调用

`ctx` 当前包含以下字段：

- `ctx.kind`
- `ctx.endpoint`
- `ctx.connection_id`
- `ctx.timestamp_ms`
- `ctx.ready_for_io`

`bytes` 当前统一是 `number[]`。

## 控件声明格式

当前默认脚本使用 `ui()` 返回 Dock 数组；若只需单面板，也可继续返回 `controls()`。

`controls()` 返回值示例：

```lua
function controls()
  return {
    { type = "button", id = "read_version", label = "读取版本" },
    { type = "input_text", id = "device_id", label = "设备 ID", default = "01" },
    { type = "checkbox", id = "hex_send", label = "HEX 发送", default = true },
    { type = "combo", id = "mode", label = "模式", options = { "轮询", "单次" }, default = 1 },
    { type = "input_int", id = "timeout_ms", label = "超时(ms)", default = 1000 },
    { type = "input_float", id = "scale", label = "缩放", default = 1.0 }
  }
end
```

当前支持的类型固定为：

- `button`
- `input_text`
- `input_int`
- `input_float`
- `checkbox`
- `combo`

约定说明：

- `id` 与 `label` 必填
- `combo` 必须提供 `options`
- `combo.default` 使用 Lua 习惯的 `1` 基下标

## 最小可运行脚本示例

默认脚本 `protocols/default_protocol/main.lua` 已是可运行示例，覆盖了：

- 一个按钮
- 一个文本输入
- 一次发送
- 一次定时器
- 一次事件输出

脚本核心片段如下：

```lua
local rx_buffer = {}

function on_control(ctx, id, value)
  if id == "read_version" and value then
    rx_buffer = {}
    proto.send({ 0xAA, 0x55, 0x30, 0x01, 0x0D })
    proto.set_timer("read_version_timeout", proto.get_control("timeout_ms") or 1000)
  end
end

function on_bytes(ctx, bytes)
  for i = 1, #bytes do
    rx_buffer[#rx_buffer + 1] = bytes[i]
  end

  if #rx_buffer >= 2 and rx_buffer[1] == string.byte("O") and rx_buffer[2] == string.byte("K") then
    local frame_size = #rx_buffer
    rx_buffer = {}
    proto.cancel_timer("read_version_timeout")
    proto.emit("frame", { status = "ok", size = frame_size })
  end
end
```

当前默认协议示例已经演示了“脚本侧跨包缓冲”的最小做法：即便 `OK` 被拆成多次 `on_bytes()` 回调，也会先累计再判定。

## 配置热重载说明

配置文件中新增了独立开关：

```yaml
app:
  config_hot_reload:
    enabled: false
```

行为约定：

- 默认关闭，不会自动轮询并重载磁盘配置
- 开启后，检测到外部文件变更时只提示，不会自动覆盖当前内存态
- 若当前窗口存在未保存改动，会阻止自动保存继续覆盖外部修改
- 用户需显式点击 `重载配置` 或 `忽略本次外部更新提示`

## 错误处理与调试

- 缺失脚本文件
  - 脚本不会加载成功，UI 会显示加载错误
- `controls()` 返回结构非法
  - 视为脚本加载失败
- 缺失某个事件回调
  - 允许缺省，不视为失败
- 某个回调运行时报错
  - 只记录错误日志，宿主继续运行
- 配置文件 YAML 解析失败
  - 主程序回退到默认配置，并把错误写回状态栏

调试建议：

- 先看状态栏与接收日志里的 `Lua` / `SYS` 行
- 再看控件交互是否触发了 `proto.emit()` 或 `proto.log()`
- 若怀疑脚本收包逻辑有问题，可先在 `on_bytes()` 里直接输出 `#bytes`

## 模块边界

- `src/transport`：统一传输接口与 TCP/Serial 实现
- `src/scripting`：Lua VM、脚本装载、宿主 API 与回调桥接
- `src/dock`：Dock 状态模型与接收行缓存
- `src/app`：主循环装配、Transport/Script/Dock 事件泵
- `src/protocol_utils`：HEX 编解码、CRC 与输入归一化
- `src/ui`：ImGui Dock 渲染与配置/发送交互
