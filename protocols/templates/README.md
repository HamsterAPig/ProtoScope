# ProtoScope 内置协议模板

本目录用于存放随程序释放的默认 Lua 协议模板。每个协议目录都以 `main.lua` 作为入口。

## 模板列表

- `default_protocol`：最小可运行协议，演示控件、发送、定时器和事件输出。
- `lua_waveform_demo`：纯 Lua 生成波形，适合验证 UI 控件与曲线显示。
- `half_duplex_modbus_master`：半双工主机示例，演示 `proto.request()`、`stream()` 和 ACK/上传帧解析。
- `half_duplex_modbus_slave`：半双工从机示例，演示请求解析、ACK 返回和批量波形上传。
- `file_dialog`：文件/目录对话框示例，演示 `proto.fs.open_file_dialog()`、`proto.fs.open_dir_dialog()` 与 `on_file_dialog()`。
- `send_file`：文件分块发送示例，演示 `proto.fs.send_file()` 与 `on_tx()` 进度处理。
- `request_guarded`：受保护请求示例，演示 `proto.request_guarded()` 的超时、重试、熔断与 `proto.reset_request_guard()`。

## 使用方式

1. 复制一个模板目录并重命名为自己的协议名。
2. 保留入口文件名 `main.lua`。
3. 在程序的协议面板选择新的协议目录并重新加载。

Lua 宿主 API 和 `stream()` schema 类型说明请参考上一级目录的 `README.md`、`protoscope_api.lua` 和 `stream_types.lua`。
