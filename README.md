# ProtoScope

ProtoScope 是一个面向串口、TCP、UDP 调试场景的 `ImGui + ImPlot + Lua` 协议联调工具。核心工作流是：连接设备、加载协议脚本、观察收发日志和波形，并在需要时录制或回放完整原始数据。

## 快速入口

- 使用程序：见 [ProtoScope 用户手册](docs/user-manual.md)。
- 配置文件：见 [配置参考](docs/config-reference.md)，事实源是 `include/protoscope/config/config.hpp` 和 `src/config/config.cc`。
- Lua 脚本：见 [Lua 协议脚本指南](protocols/README.md)。
- LuaLS 提示：`protocols/protoscope_api.lua` 由 `protocols/protoscope_api_manifest.json` 生成。
- 维护说明：见 [src/include 模块设计导览](docs/module-design.md) 和 [Lua 宿主 API 接入指南](docs/lua-host-integration.md)。
- 发布检查：见 [ProtoScope 发布检查清单](docs/release-checklist.md)。

## 启动参数

- `--diagnose`：启用启动诊断日志。日志会写入可用的 `logs/` 目录，并记录启动阶段、命令行、路径探测、崩溃兜底和 GUI/renderer 初始化信息。
- `--renderer=<backend>` 或 `--renderer <backend>`：指定 GUI 渲染后端，优先级高于 YAML 配置和默认值。可选值：`opengl`、`d3d11`、`d3d11_warp`，其中 `d3d11-warp` 也会归一化为 `d3d11_warp`。
- `--diagnose-renderer-probe`：启用独立 renderer 探测模式，不进入完整应用。该模式会自动启用 `--diagnose`，并单独探测 `glfwInit`、D3D11 hardware、D3D11 WARP 和 OpenGL 版本。

常用启动诊断命令：

```powershell
ProtoScope.exe --diagnose --renderer=d3d11
ProtoScope.exe --diagnose-renderer-probe --renderer=d3d11
```

## 性能调优速查

推荐的平滑实时刷新配置如下。新默认值已经采用这组预算；如果本地 YAML 显式写了旧的大预算值，需要删除或改小这些单项后才会回到新默认。

```yaml
receive:
  transport_read_buffer_bytes: 4096
gui:
  realtime_backlog:
    rx_chunk_bytes_per_pump: 4096
    plot_appends_per_pump: 128
    pump_min_interval_ms: 1.0
scripting:
  worker:
    batch_bytes: 8192
    output_flush_budget_ms: 2.0
```

- `performance.scale` 只影响未显式写出的预算项；YAML 里写出的单项会优先覆盖公共系数。
- 可选的 `performance.adaptive.enabled` 默认关闭。开启后用 `performance.adaptive.max_multiplier` 作为 K（`0.25..4.0`），按系统 CPU/内存和应用 backlog 动态接管 FPS、波形渲染、UI backlog 追赶和脚本输出 flush 预算；系统压力收紧渲染，软件 backlog 高时清债预算保持在 K 档，`performance.scale` 与这些热调项不参与运行时调度。
- 自适应不会热改队列/内存上限、worker 线程、传输读缓冲、`batch_bytes`、背压水位或 `pump_min_interval_ms`；它们继续作为资源安全和协议行为边界。
- `batch_bytes`、`transport_read_buffer_bytes`、`rx_chunk_bytes_per_pump` 控制数据投递颗粒度，越小越容易平滑刷新。
- `fps_limit`、`pump_min_interval_ms` 控制 UI 刷新频率和 CPU 占用。
- `max_render_points_per_channel`、`max_render_vertices`、`peak_detect_downsample` 控制绘制压力，不负责拆分数据批次。
- 更平滑：减小 `batch_bytes`、`transport_read_buffer_bytes`、`rx_chunk_bytes_per_pump`、`plot_appends_per_pump`，适当降低 `output_flush_budget_ms` 和 `pump_min_interval_ms`。
- 更高吞吐：增大上述批量预算，让 UI 一轮追更多 backlog，但可能出现更明显的大段刷新。
- 更省渲染性能：降低 `fps_limit`、`max_render_points_per_channel`、`max_render_vertices`，保持 `peak_detect_downsample: true`。
- 更高细节：提高渲染点数预算或延后降采样，代价是 CPU/GPU 压力上升。

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
发布前按 [发布检查清单](docs/release-checklist.md) 复核构建、测试、LuaLS API 同步和发布资产。

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
