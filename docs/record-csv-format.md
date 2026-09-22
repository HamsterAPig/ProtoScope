# 类型化记录 CSV v1

当前提供流式编解码与普通 CSV 映射，尚未接入 Lua 记录交换任务。
这是结构化记录交换格式，与既有波形 CSV、原始采集 CSV 保持各自格式标识。

## 工具 CSV

```csv
protoscope_csv,1
schema,1,telemetry,sequence,int64,false,temperature,double,true
record,example,telemetry,device-a,123456,,1,i:1,d:20.5
record,example,telemetry,device-a,123457,,1,i:2,n:
end,2
```

- schema 行：`schema,version,dataset` 后每字段三个单元格：名称、类型、nullable。
  类型为 `int64/double/bool/string/bytes`，nullable 为 `true/false`。
- record 行：`record,protocol,dataset,device,received_us,device_us,version` 后接各字段。
  未提供设备时间用空单元格。
- 值标签：`i:` 为 int64，`d:` 为 double，`b:true/b:false` 为布尔，
  `s:` 为字符串，`x:` 为连续 HEX 字节，`n:` 为显式 null。
  因而 `s:`、`s:n:`、`x:` 与 `n:` 各自保留不同含义。
- double 使用 `max_digits10` 输出，按 locale 无关规则读取，保留负零与有限浮点位型。
- end 行记录总数，其后必须 EOF；缺失结束行、列数不符、未知模式或类型标签不符均报错。
- 最多 1024 个模式，合计 16384 个字段；模式版本为正 int64。

转义复用现有 CSV 规则：逗号、双引号、CR/LF、`#` 需要引用，双引号重复。
嵌入 NUL 原样保留；该能力不保证电子表格软件兼容。
每行编码后最多 32MiB、4096 单元格；读取不累积全文件。
支持 UTF-8 BOM 和 CRLF。类型化导出不会产生公式起始单元格值，字符串总带 `s:` 标签。

## 普通 CSV

`MappedCsvReader` 接收已声明的 `Schema` 与 `CsvImportMapping`：

- `protocol` 必填；`device` 为固定默认设备，可通过 `deviceColumn` 读取。
- `receivedColumn` 默认 `received_at_us`，严格解析 int64 微秒时间戳。
- `deviceTimeColumn` 可选；空单元格或显式 null 标记表示未提供。
- `fields` 将声明字段映射到表头名称；未指定的字段按同名表头读取。
- 所有声明字段必须存在，拒绝重复/空表头、未知映射字段和不等长行；额外未映射列可以保留。
- 不自动去掉空格，不将数字字符串隐式截断，不接受非有限数值；布尔只接受 `true/false`。
- 字节字段要求偶数长度连续 HEX。
- `nullToken` 仅在显式配置时生效；默认空字符串仍是空字符串，空数值不视为 null。
- 返回记录暂用 schemaVersion=1；导入服务必须映射到独立暂存卷的已登记模式。

普通 CSV 没有结束计数和校验和，无法检测语法仍合法的尾部截断。
需要损坏检测或精确归档时使用带 CRC32 的 `.psrec`。

## 文件替换

共享 `data::DataFileOutput` 沿用同目录临时输出、flush/close 检查和最终替换；
Windows 使用 `MoveFileExW(REPLACE_EXISTING|WRITE_THROUGH)`。
取消或失败不替换原文件，销毁时清理本次临时文件。
这不等同于防御恶意目录竞争或提供所有平台上的硬件持久化保证。
