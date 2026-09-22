# 存储持续运行验收

## 执行

```powershell
cmake --build build --target protoscope_storage_soak_benchmark -j 4
./build/tests/protoscope_storage_soak_benchmark.exe 60
./build/tests/protoscope_storage_soak_benchmark.exe 86400
```

参数为秒数，允许 10 到 86400，默认 60。独立工具不加入普通 CTest，
避免常规 60 秒超时误杀长测；运行器应另设持续时长加收尾时间的超时。
工具使用专属临时目录，退出由测试夹具清理，不操作用户记录根。

## 验收内容

- 固定每 100ms 发布 100 条，不依赖 UI 帧率。
- 256KiB 分卷、8MiB 总量，持续触发轮转和滚动清理。
- 并发查询、PSREC 导出，每次校验最近已提交的 200 条及精确顺序；
  导出文件通过 PsrecReader 完整读取，验证块校验及结束标记。
- 索引观测不占用数据卷；累计已消失封存卷的记录数，
  最终逐条查询校验 `committed = cleaned + retained`。
- 预热五秒后每秒采样进程工作集（Windows）或 RSS（Linux），
  峰值增长超过 128MiB 失败。该门限不是泄漏证明，仍需比较长期趋势。
- 每十秒输出提交、清理、封存数量和内存峰值；任务失败、容量故障或计数不一致时返回非零。

## 当前证据

已执行 60 秒：`committed=60000 retained=17200 cleaned=42800 cleaned_volumes=77`，
`queried=2400 exported=2400 failed=0`，
`rss_base=9256960 rss_peak=10297344 rss_growth=1040384`。

另已执行 300 秒：`committed=300000 retained=15500 cleaned=284500 cleaned_volumes=509`，
`queried=12000 exported=12000 failed=0`，
`rss_base=9183232 rss_peak=13045760 rss_growth=3862528`。

最初测试在容量边界与短查询占用重叠时进入故障；增加 90% 提前清理目标后本轮通过。
硬上限仍保持故障语义，不静默丢弃或无限放宽容量。

**尚未执行 24 小时测试**。本结果不代表 24 小时稳定性、真实磁盘满、
断电持久性、任意数据库损坏恢复或 GUI 人工交互验收已经通过。
