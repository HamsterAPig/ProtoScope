# ProtoScope

ProtoScope 是一个面向串口、TCP、UDP 调试场景的 `ImGui + ImPlot + Lua` 协议联调工具。核心工作流是：连接设备、加载协议脚本、观察收发日志和波形，并在需要时录制或回放完整原始数据。

## 快速入口

- 使用程序：见 [ProtoScope 用户手册](docs/user-manual.md)。
- 配置文件：见 [配置参考](docs/config-reference.md)，事实源是 `include/protoscope/config/config.hpp` 和 `src/config/config.cc`。
- Lua 脚本：见 [Lua 协议脚本指南](protocols/README.md)。
- LuaLS 提示：`protocols/protoscope_api.lua` 由 `protocols/protoscope_api_manifest.json` 生成。
- 维护说明：见 [src/include 模块设计导览](docs/module-design.md) 和 [Lua 宿主 API 接入指南](docs/lua-host-integration.md)。

## 启动参数

- `--diagnose`：启用启动诊断日志。日志会写入可用的 `logs/` 目录，并记录启动阶段、命令行、路径探测、崩溃兜底和 GUI/renderer 初始化信息。
- `--renderer=<backend>` 或 `--renderer <backend>`：指定 GUI 渲染后端，优先级高于 YAML 配置和默认值。可选值：`opengl`、`d3d11`、`d3d11_warp`，其中 `d3d11-warp` 也会归一化为 `d3d11_warp`。
- `--diagnose-renderer-probe`：启用独立 renderer 探测模式，不进入完整应用。该模式会自动启用 `--diagnose`，并单独探测 `glfwInit`、D3D11 hardware、D3D11 WARP 和 OpenGL 版本。

常用启动诊断命令：

```powershell
ProtoScope.exe --diagnose --renderer=d3d11
ProtoScope.exe --diagnose-renderer-probe --renderer=d3d11
```

## 当前能力

- `通讯配置`：支持 `TCP Client`、`TCP Server`、`Serial`、`UDP Peer`。
- `配置文件管理`：支持保存、显式重载、自动保存，以及可选的外部配置变更提醒。
- `发送编辑器`：支持 HEX 和文本发送，HEX 模式会归一化为大写双字节分组，并在奇数个 nibble 时拒绝发送。
- `Lua 声明式 UI`：脚本可声明 Dock、控件和 Layout Tree。
- `Lua 协议回调`：脚本可处理打开、关闭、收包、控件、定时器、TX、弹窗和文件对话框事件。
- `接收展示`：统一展示系统日志、TX/RX、Lua 事件和逐帧解析结果。
- `波形 Dock`：支持多通道波形、总览、测量、FFT、原始波形导入、当前缓存快照导出和完整原始数据录制。

## 构建与测试

首次拉取后初始化子模块：

```powershell
git submodule update --init --recursive
```

Windows 下已验证过的基础命令：

```powershell
"C:\Program Files\CMake\bin\cmake.exe" -S . -B build-gcc
"C:\Program Files\CMake\bin\cmake.exe" --build build-gcc
"C:\Program Files\CMake\bin\ctest.exe" --test-dir build-gcc --output-on-failure
```

如果使用 Visual Studio 或 Ninja，可以指定自己的构建目录。提交前优先运行与改动相关的目标测试。

## 协议目录

源码中的默认协议和模板位于 `protocols/`：

```text
protocols/
├── protoscope_api.lua
├── stream_types.lua
├── default_protocol/
├── lua_waveform_demo/
├── half_duplex_modbus_master/
├── half_duplex_modbus_slave/
└── templates/
```

每个协议目录入口固定为 `main.lua`。可复制模板见 [protocols/templates/README.md](protocols/templates/README.md)。

## 主要依赖

- `3rdparty/spdlog`
- `3rdparty/yaml-cpp`
- `3rdparty/asio`
- `3rdparty/imgui`
- `3rdparty/implot`
- `3rdparty/libdwarf-code`
- `3rdparty/lua`
- `3rdparty/sol2`
