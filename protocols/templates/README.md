# ProtoScope 可复制操作模板

本目录只存放可复制的操作模板。源码中的默认协议示例位于 `protocols/default_protocol` 等顶层目录；打包运行时会把内嵌默认协议释放到可执行目录下的协议目录。每个协议目录都以 `main.lua` 作为入口。

## 模板列表

- `file_dialog`：适合需要让用户选择样本文件、授权目录、读取小块数据的协议。演示 `proto.fs.open_file_dialog()`、`proto.fs.open_dir_dialog()`、`proto.fs.open()`、`proto.fs.read()` 和 `on_file_dialog()`。
- `send_file`：适合固件、脚本或二进制样本按块发送。演示 `proto.fs.send_file()`、`chunk_size`、`tag` 和 `on_tx()` 进度处理。
- `request_guarded`：适合半双工请求需要超时、重试、熔断保护的场景。演示 `proto.request_guarded()`、`proto.request_done()`、`proto.reset_request_guard()` 和 `on_tx()`。

## 使用方式

1. 复制一个模板目录并重命名为自己的协议名。
2. 保留入口文件名 `main.lua`。
3. 在程序的协议面板选择协议根目录，重新扫描，然后选择新的协议目录并重新加载。
4. 按实际协议改 `ui()` 控件、`on_control()` 发送逻辑和回调处理。

推荐复制整个目录，不要只复制片段。模板里的 `ui()`、回调和局部状态通常是一组完整示例。

Lua 宿主 API 和 `stream()` schema 类型说明请参考上一级目录的 `README.md`、`protoscope_api.lua` 和 `stream_types.lua`。
