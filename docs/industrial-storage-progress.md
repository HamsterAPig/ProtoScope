# 工业控件与持久化实施状态

本文件记录已实现边界，不替代完整扩展方案。当前提供基础 Lua 记录示例，
尚未提供工业控件、历史表和文件交换的完整界面。

## 已验证的基础修复与执行保护

- 定时器在同一轮被取消或重新设置后，不再执行旧代次回调。
- 文件发送通过请求 ID 跟踪在途块；请求块等待 `completed`，普通发送等待 `sent`。
  失败、拒绝、丢弃、取消及超时终止文件任务并释放句柄。
- 追加写入按整个文件计入限额，多追加句柄共享限制；关闭显式报告刷新及关闭错误。
- 重载校验控件类型及选项兼容性，实测表值和发送运行状态不跨重载保留。
- Lua 指令预算覆盖加载声明和业务回调，默认加载 5s、回调 500ms；
  worker 停止信号可以中断 Lua 忙循环，超时后成功重载可恢复。

对应提交：`128dec2`。文件 API 参数和预算说明见 `lua-host-integration.md`。

## 数据与存储基础

### 模块边界

- `protoscope_data`：纯 C++20 类型化值、模式、记录、验证及二进制值编码。
  不依赖 Lua、ImGui 或 SQLite。
- `protoscope_storage`：基于固定 SQLite 3.50.4 的独立 C++ 服务。
  已通过独立 ScriptDataSession 接入 Lua worker 和应用配置。
- `protoscope_sqlite`：随仓库静态编译，关闭动态扩展加载；来源和归档摘要见第三方目录说明。

### 当前 C++ 接口

`storage::Store(root, protocol, schemas, config)` 接收已解析的协议标识、专用根目录及固定模式。
根目录由调用方传入，应用默认使用可执行目录下的 data，支持 `scripting.storage.root_dir`。

- `start/stop/set/erase/flush/query` 返回任务 ID；`poll()` 返回完成结果。
- `publish` 整批校验后进入有界记录队列，成功代表已入队。
- `status` 分开报告收到、入队、提交和失败数；队列满或事务失败进入记录故障。
- `get` 只解码已提交的内存缓存，不执行磁盘 I/O；异步写入成功提交后才更新缓存。
- 析构等待已接受的写入处理完毕；查询使用独立只读连接和后台线程。
- `cancel` 设置查询取消标记，完成事件可能为已成功结果或取消错误，不返回半页数据。

默认记录队列 32MiB，批处理目标 100ms / 1000 行；单次 `publish` 批次保持在同一事务中，
因此该批次大于目标行数时事务也可能超过 1000 行。写连接使用 WAL 和 `synchronous=FULL`。
KV 单值 256KiB、深度 16、每协议 8MiB。最多保留 1024 个未消费异步任务，
其中查询最多 16 个，并对查询页及模式元数据施加结果内存预算。

### 当前磁盘格式

```text
<调用方指定的协议根目录>/
  kv/values.sqlite
  records/records.sqlite
```

两类数据库均验证应用身份、协议归属、格式版本和完整性。
模式变化登记新版本，查询页携带该页涉及的历史模式，不重解释旧记录。
查询按接收时间、记录 ID 排序，使用已提交 ID 高水位固定快照；
默认每页 200 条，上限 1000 条，支持设备、数据集和时间条件。

记录启动状态持久化，未主动停止时重新打开 Store 恢复开关。
这只是恢复基础，尚未实现“重启开启新分卷”和异常退出区间标记。
KV 独立于记录文件，记录故障不阻断 KV 任务。

## Lua 接入

- Lua 基础发布、KV、记录启停/状态/查询/取消及回调已接入；详见 `lua-data-storage.md`。
  加载阶段禁止运行期 API，KV 初始化读取在成功激活后提供。
- 每个 runtime 独占会话，旧任务不跨重载投递；KV 缓存只在提交后更新。
- 转换检查循环、混合键、稀疏数组、大小和深度，保留 int64、null、字节和嵌入 NUL。
- 存储轮询使用固定到期时间，连续收包或获取快照不会延迟任务结果。
- `protocols/data_storage_demo/main.lua` 使用现有按钮和状态栏展示基础发布、记录和查询。

## 剩余工作

- 现有控件的属性原子更新、worker 显隐/禁用/只读及约束校验已接入；
  见 `lua-control-properties.md`。
- 现有 UI 控件、示波器切换、弹窗/文件对话框已加入运行时代次隔离。
- 新工业控件、编辑草稿、业务菜单和新控件 UI 状态兼容仍待实现。
- 实时字段绑定和历史表；字段条件查询。
- 记录分卷、目录索引、占用保护、滚动清理、磁盘容量监测和完整重启恢复。
- CSV 与 `.psrec` 导入导出、暂存分卷与导入取消。
- 已提供基础存储示例及对应 Manifest/LuaLS/文档；完整 UI 与导入导出示例仍待完成。
- 故障注入、持续 1000 条/秒以及 24 小时稳定性验收。

## 验证

```powershell
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

已有 `build` 缓存使用 MinGW Makefiles，不能直接用 `-G Ninja` 改写该缓存。
新增安全与存储测试独立于 GUI，CTest 超时均为 60s。
目前未执行磁盘满故障注入和 24 小时稳定性测试。

本轮 Lua 接入验证：

- `cmake --build build -j 4`：通过。
- `ctest --test-dir build --output-on-failure`：21/21 通过，33.91 秒。
- 新增 `protoscope_script_data_tests`：记录分页、KV 重载、输入验证、声明预算、worker 回调。
- 专项测试曾连续 5 次通过；补充固定轮询期限断言后全量测试通过。
- `build-industrial-headless` 的 Lua 数据目标构建及专项测试通过。
- `build/tests/protoscope_storage_rate_benchmark.exe 10`：
  `rate=1000/s seconds=10 received=10000 queued=10000 committed=10000 queried=10000 failed=0`。
  工具接受秒数参数（1..86400），目前仅运行 10 秒；不包含清理、导出和内存增长验收。

动态属性基础验证：

- `protoscope_control_properties_tests`：3 组通过，覆盖整批回滚、worker 拒绝非法事件和重载约束。
- `cmake --build build -j 4`：通过，包含 GUI；首次重编译超过命令超时，确认进程退出后增量重跑成功。
- `ctest --test-dir build --output-on-failure`：22/22 通过，37.05 秒。
- GUI 显隐、禁用和只读呈现尚未进行人工交互验收。

运行时代次验证：属性专项现为 5 组，新增旧输入拒绝、失败重载保留代次和
旧文件对话框不授予新协议路径权限。完整构建通过；CTest 22/22，34.02 秒。
