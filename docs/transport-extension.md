# 通讯接入扩展指南

ProtoScope 的通讯层负责把 TCP、串口、UDP 等 I/O 统一成字节流事件。上层只依赖 `ITransport`、`TransportConfig` 和 `TransportEvent`，不要在 Lua、Dock 或波形层直接耦合具体 socket/串口实现。

## 当前模型

- `TransportKind` 是编译期枚举，当前支持 `tcp_client`、`tcp_server`、`serial`、`udp_peer`。
- `transportDescriptors()` 是 UI 和配置展示的统一入口，新增通讯方式时必须同步注册 id 和显示名。
- `transport::createTransport(kind)` 是默认工厂，`Application` 的测试工厂只用于单元测试注入。
- `TransportConfig` 使用 `std::variant` 保存各通讯方式配置，`Application::currentTransportConfig()` 只负责从运行态 Dock 配置取出对应配置。

## 新增通讯方式步骤

1. 在 `transport.hpp` 增加 `TransportKind`、配置结构和 `ITransport` 实现类。
2. 在 `transportDescriptors()` 注册稳定 id，配置文件使用这个 id。
3. 在 `transport::createTransport()` 增加创建分支。
4. 在配置读写中增加 YAML 节点，字段名使用 snake_case。
5. 在通讯配置 Dock 增加最小必要输入项，不把通讯细节泄漏给 Lua。
6. 补充 transport 层收发测试、配置 roundtrip 测试、Application 工厂参数测试。

## UDP Peer

`udp_peer` 是面向调试的双向 UDP 对等模式：

```yaml
communication:
  kind: udp_peer
  udp_peer:
    bind_address: 0.0.0.0
    bind_port: 9001
    remote_host: 127.0.0.1
    remote_port: 9000
```

- `bind_address/bind_port` 决定本地监听端。
- `remote_host/remote_port` 决定 `send()` 和 `enqueueSend()` 的目标端。
- 收包事件仍统一为 `TransportBytesEvent`，Lua 只看到 `ctx.kind == "udp_peer"` 和原始 bytes。
- UDP Peer 面向调试场景，首轮不按 `remote_host/remote_port` 过滤发送方；事件里的 endpoint 会记录真实发送方。

## 约束

- 不做运行时动态插件发现；当前阶段只用编译期注册，保持 KISS。
- 底层 transport 不依赖 UI、Lua、配置存储或 Dock 状态。
- 新通讯方式必须保持 `send()`、`enqueueSend()`、`takeEvents()` 的行为契约一致。
