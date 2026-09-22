# PSREC 交换格式 v1

独立于 `.psraw` 和 `.pssession`，用于类型化数据记录精确交换。
当前实现是 `protoscope_data` 的流式编解码，不代表 Lua 导入导出任务已接入。

## 字节结构

所有整数为小端，文件头 12 字节：

```text
50 53 52 45 43 0D 0A 1A  01 00 00 00
P  S  R  E  C  CR LF SUB  version=1
```

随后依次为一个模式块、零到多个记录块、一个结束块。
每块：`type:u32 | length:u32 | sequence:u64 | payload[length] | crc32:u32`。
CRC32 使用 IEEE 多项式 `0xEDB88320`，初始/末尾异或 `0xFFFFFFFF`，
覆盖块头 16 字节及 payload。序号从 0 严格递增，不允许跳号或重排。

- `type=1`，模式块：现有类型化 Value 编码的 `[[version, schema], ...]`；
  schema 是 `[dataset, [[field_name, field_type, nullable], ...]]`。
- `type=2`，记录块：Value 编码的
  `[protocol, dataset, device, received_us, device_us|null, schema_version, values]`。
- `type=3`，结束块：payload 为 `record_count:u64`；其后必须是物理 EOF。

Value 标签依次为 `null=0, int64=1, double=2, bool=3, string=4, bytes=5,
array=6, object=7`。整数及 double 位型占 8 字节；bool 后跟 0/1；
字符串/字节为 `length:u64 + bytes`，数组为 `count:u64 + values`，
对象为 `count:u64 + (key_length:u64 + key_bytes + value)`。
模式版本保留源文件值，不把旧记录改解释为当前模式。

## 验证和资源限制

- 块不超过 32MiB，Value 深度 16，单块最多 131072 个 Value 节点，
  防止短标签输入被解码成过大的容器。
- 最多 1024 个模式，合计最多 16384 个字段；每个模式沿用模型的 1024 字段上限。
- 模式版本为正 int64；拒绝重复版本、未知模式、类型不匹配、非有限浮点和额外尾部。
- 字符串保留嵌入 NUL，字节不经文本 HEX；int64、负零、double 位型精确保留。
- 每 64KiB I/O 或 CRC 处理检查停止信号。流对象及其生命周期归调用方所有。
- 输出失败或取消后 writer 不可继续或补结束块；读取失败后 reader 不可继续。
- 只有 reader 返回 `nullopt` 才完成全文件验证。此前读出的记录只能写入不可见暂存区，
  不可触发设备回调或实时记录。目标文件原子替换及暂存卷登记由后续存储任务负责。

CRC 用于损坏检测，不提供密码学真实性或防篡改保证。
