# ProtoScope

ProtoScope 当前已具备第一版 `TCP + Lua` 主链路骨架，目标是先打通协议联调工作流：

- `通讯配置`（TCP Client / TCP Server / Serial）
- `接收展示`
- `发送数据`
- `Lua 声明式控件`
- `波形绘制`（第一版仅占位）

## 已接入依赖

- `3rdparty/spdlog`
- `3rdparty/yaml-cpp`
- `3rdparty/imgui`（当前仅保留仓库，尚未接入主目标）
- `3rdparty/libdwarf-code`

> 说明：第一版代码已经预留 `lua/sol2/asio/implot` 接入点，但仓库内尚未引入这些子模块，当前脚本执行采用内建模拟回调以保证编译与测试闭环。

## 构建与测试

首次拉取：

```powershell
git submodule update --init --recursive
```

推荐（若本机有 Ninja）：

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build
ctest --test-dir build --output-on-failure
```

Windows 备用（Visual Studio 2022）：

```powershell
cmake -S . -B build-vs -G "Visual Studio 17 2022" -A x64
cmake --build build-vs --config Debug
ctest --test-dir build-vs -C Debug --output-on-failure
```

运行：

```powershell
.\build-vs\Debug\ProtoScope.exe
```

## 第一版模块边界

- `src/transport`：统一 `ITransport`，提供 `TcpClientTransport` / `TcpServerTransport` / `SerialTransport`。
- `src/scripting`：`ScriptHost` 模拟 Lua 契约回调（`on_open` / `on_close` / `on_error` / `on_bytes` / `on_timer` / `on_control`）。
- `src/dock`：四个 Dock 的状态模型与接收行缓存。
- `src/app`：主循环装配，事件泵（Transport -> Script -> Dock），发送队列与 action 触发。
- `src/protocol_utils`：HEX 编解码、CRC16/CRC32。

## 测试覆盖

- CRC16-Modbus / CRC16-CCITT-FALSE / CRC32-IEEE 标准向量
- HEX 编解码合法与非法输入
- Lua 控件快照、read_version 一问一答成功路径、超时路径

## 下一步（未实现）

- 接入真实 `lua + sol2`，替换 `ScriptHost` 内建模拟逻辑
- 接入 `asio` 的真实 TCP/Serial I/O 线程
- 接入 `GLFW + OpenGL3 + ImGui Docking` 真正窗口渲染
- 接入 `implot` 波形绘制
