# Lua 数据与存储 API

## 声明和生命周期

```lua
function data()
    return {{id = "telemetry", fields = {
        {name = "temperature", type = "double", nullable = false},
        {name = "sequence", type = "int64", nullable = false}
    }}}
end
```

模式只在加载时声明。支持 `int64`、`double`、`bool`、`string`、`bytes`。
字段可空默认 true，发布时仍须显式写 `proto.data.null`，不允许遗漏字段。
整数保留 Lua integer 的完整精度；double 字段接受整数并转换。
`proto.data.bytes(raw_string)` 构造原始字节，不解释 HEX；查询返回 `ProtoBuffer`。

所有运行期 API 必须在回调中调用，不能在脚本顶层或 `data/ui/controls` 声明中调用。
加载失败保留旧会话；成功重载排空旧写入，新 runtime 不接收旧任务回调。
`data()` 同样受加载指令预算保护。

默认存储位置为可执行目录 `data/<协议目录哈希>/`，数据库同时校验完整规范协议路径。
YAML `scripting.storage.root_dir` 可覆盖根目录，相对路径按进程工作目录解析。
协议目录搬迁会得到新的存储身份。测试或独立 ScriptHost 通过 `setStorageRoot` 注入根目录。
预加载探测宿主不设置根目录，因此只验证声明，不创建数据库。

## 发布和最新值

```lua
proto.data.publish({
    dataset = "telemetry", device = "sensor-1", device_time_us = 123,
    values = {temperature = 23.5, sequence = 1}
})
local row = proto.data.latest("telemetry")
```

接收时间由宿主生成。`publish_batch(rows)` 先验证全部行，之后更新有界最新值缓存。
最新值每数据集一条，不按设备缓存；需要区分设备时读取返回的 `device`。
最多 128 个数据集，单次发布最多 1000 行、编码记录总量不超过 8MiB。
未开启记录时仍更新最新值。队列拒绝抛出 Lua 错误，但已验证的最新值仍更新。
API 返回成功不等于落盘成功，必须读取记录状态和任务结果。

## 记录任务

- `proto.record.start()`、`stop()` 返回任务 ID，完成后进入 `on_record(ctx, evt)`。
- 应等待 start 成功事件后发布要记录的数据；stop 成功表示之前接受的记录已处理完毕。
- `status()` 返回 `recording/recovered/faulted/received/queued/committed/failed/queue_bytes/error`。
- `query({dataset, device, from_us, to_us, offset, limit, snapshot})` 异步查询。
  默认页长 200，最多 1000；省略 snapshot 创建固定快照，后续页复用返回的 snapshot。
- `cancel(task)` 请求取消查询。完成与取消竞争时允许收到已完成结果。

`on_record` 事件含 `task/operation/ok/error/records/snapshot/more`。
异步记录故障额外触发 `operation="fault"`、`task=0` 的事件；`status` 附带计数和错误。
记录行含 `protocol/dataset/device/received_at_us/device_time_us/schema_version/values`。
历史行按对应历史模式解码，字段变化不会使旧行被按当前模式误读。
尚未提供分卷和导入导出；当前不是完整长期记录版本。

### 字段条件与排序

```lua
proto.record.query({
    dataset="telemetry", limit=200,
    conditions={{field="sequence",op="ge",value=100}},
    sort={field="temperature",descending=true}
})
```

条件最多 16 个、合计 64KiB，全部满足才匹配，支持 `eq/ne/lt/le/gt/ge/contains/is_null/not_null`。
contains 是区分大小写的字符串子串匹配，不是 SQL 通配符；空值测试不提供 value。
条件严格保留类型，不在 int64/double、布尔、字符串或字节之间隐式转换。
旧模式缺字段不等于显式 null，不会匹配 is_null。

排序在完整固定快照上完成后才分页，相同值按接收时间、记录 ID 稳定排序。
字段类型变化时按 null、int64、double、bool、string、bytes 的类型顺序再比较值，
descending 反转字段类型和值顺序；缺字段排在显式 null 之前（降序时之后）。
查询在独立读线程执行，可取消；排序可能使用 SQLite 临时文件。
字段名和值均用参数绑定，仅固定比较与排序操作可用，不开放任意 SQL。

## KV

`proto.kv.get(key)` 只读取已提交内存缓存，未找到返回 nil。
`set(key,value)`、`delete(key)`、`flush()` 返回任务 ID；
提交成功才更新缓存并进入 `on_kv(ctx,evt)`，事件结构同记录任务。
设备级设置由脚本自行使用键前缀。

支持标量、稠密数组、字符串键对象、显式 null 和 ProtoBuffer。
空 Lua 表按对象保存；不接受混合键、稀疏数组、循环表、函数及其他 userdata。
单值 256KiB、深度 16、每协议总量 8MiB；不自动序列化 metatable。
无副作用的共享子表会按值复制，不保留表身份。
输入错误或队列拒绝抛出 Lua 错误，可用 `pcall` 捕获；已排队任务的失败通过事件报告。

当前的回调轮询周期为 20ms。回调仍在 Lua worker 内执行，受现有回调预算约束。
# 历史行标识与界面查询

历史查询返回的每行包含 `record_id`，它是当前记录数据库内的稳定整数 ID。
`data_table` 选择回调使用同一 ID 的十进制字符串，避免 UI 值转换损失精度。
数据表内部查询由宿主消费，不进入脚本 `on_record`；脚本显式
`proto.record.query` 仍按原有回调返回结果。
# 异步记录导出

```lua
local task = proto.record.export({
    path="telemetry.psrec", format="psrec", overwrite=false,
    dataset="telemetry", snapshot=last_snapshot,
    conditions={{field="temperature",op="gt",value=20.0}},
    sort={field="temperature",descending=true}
})
-- on_record(ctx, evt): operation="export", task, ok, error, path, processed, snapshot
```

支持 `psrec` 与工具 `csv`，导出全部匹配记录，忽略分页的 `offset/limit`。
未指定 snapshot 时创建固定快照；逐条读取，不复制全历史。结果的 `records` 为空，
`processed` 在成功时报告总数，失败时为 0；导出不影响实时记录计数。
`proto.record.cancel(task)` 可取消查询或导出；写入成功后再取消不会撤回已提交文件。

遵循既有 `scripting.file_io.enabled`、协议目录、额外根和文件对话框写权限，
并执行 `max_write_file_size_bytes`。默认不覆盖，`overwrite=true` 必须显式指定。
存储层另外禁止导出到 records/KV 目录，避免覆盖数据库或 WAL。
目录应已存在；超限、取消、磁盘写入或最终替换失败保留原文件，错误异步返回。
数据读取线程串行处理查询与导出，长导出期间后续查询可能等待，取消请求不阻塞 UI。
授权检查不构成针对恶意目录竞争的完整文件系统沙箱。
