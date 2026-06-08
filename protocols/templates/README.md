# ProtoScope 可复制操作模板

本目录只存放可复制的操作模板。源码中的默认协议示例位于 `protocols/default_protocol` 等顶层目录；打包运行时会把内嵌默认协议释放到可执行目录下的协议目录。每个协议目录都以 `main.lua` 作为入口。

## 模板列表

- `file_dialog`：文件/目录对话框示例，演示 `proto.fs.open_file_dialog()`、`proto.fs.open_dir_dialog()` 与 `on_file_dialog()`。
- `send_file`：文件分块发送示例，演示 `proto.fs.send_file()` 与 `on_tx()` 进度处理。
- `request_guarded`：受保护请求示例，演示 `proto.request_guarded()` 的超时、重试、熔断与 `proto.reset_request_guard()`。

## 使用方式

1. 复制一个模板目录并重命名为自己的协议名。
2. 保留入口文件名 `main.lua`。
3. 在程序的协议面板选择新的协议目录并重新加载。

Lua 宿主 API 和 `stream()` schema 类型说明请参考上一级目录的 `README.md`、`protoscope_api.lua` 和 `stream_types.lua`。
