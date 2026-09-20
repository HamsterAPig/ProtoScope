# 波形平移性能验证

## 测试入口

```powershell
cmake --build build-wave-pan-release
ctest --test-dir build-wave-pan-release --output-on-failure
.\build-wave-pan-release\verified-bin\protoscope_wave_pan_benchmark.exe --baseline
.\build-wave-pan-release\verified-bin\protoscope_wave_pan_benchmark.exe
.\build-wave-pan-release\verified-bin\protoscope_wave_ui_benchmark.exe
.\build-wave-pan-release\verified-bin\protoscope_wave_ui_benchmark.exe --gl
```

本次独立 Release 配置将 `CMAKE_RUNTIME_OUTPUT_DIRECTORY` 指向构建目录下的
`verified-bin`，避免覆盖用户正在运行的程序。其他构建目录的可执行文件位置取决于其配置。

`protoscope_wave_query_tests` 用独立线性扫描验证随机窗口、256 点块边界脉冲、
多 bit、负缩放、追加、裁剪和重置。`protoscope_wave_analysis_tests` 验证后台统计、
FFT 与原始算法一致，以及输入生命周期和最新请求发布。

`protoscope_wave_ui_multiframe` 创建真实 ImGui/ImPlot 上下文，验证七种模式的多帧绘制、
配置解析和往返、拖动时零统计/FFT 提交、松手更新、查询代次淘汰、持续采集发布和历史重置。
普通 CTest 不断言机器相关耗时。

## 基准定义

窗口宽度预算 1200 像素，4 个模拟通道和 1 个展开 32 bit 的通道；
分别使用每通道 10 万、100 万个可见样本，开启两个固定游标，预热后连续移动 300 帧。
测量和图例浮层关闭，游标与单点读数计算保留。
模式编号：0 普通，1 Glow，2 堆叠，3 分屏，4 GPU 余辉交互轨迹，
5 CPU 触发余辉交互轨迹，6 完整 FFT 幅值和相位视图。

查询基准的 `--baseline` 复用原有 `buildDisplayData` 整窗复制算法；
它是数据准备基线，不是旧 HEAD 的完整应用帧耗时。
`append_index_ms` 包括追加、排序检查和索引构建，单独报告，不混入预热帧。
`summary_bytes` 为摘要有效载荷与层容器大小估计，不包括分配器及 deque 管理开销。
`raw_accesses` 与 `summary_hits` 是整个查询基准累计值。

无 `--gl` 时只测绘图准备和 ImGui/ImPlot 命令生成。
`--gl` 创建隐藏 GLFW/OpenGL 窗口，预热真实 GPU FBO、CPU Texture，
并把 OpenGL 提交、`glFinish` 和交换缓冲纳入耗时。后端预热输出单独列出；
交互帧按产品策略使用轻量轨迹，不在拖动期间累积余辉。
FFT 初次计算等待在预热阶段完成。

## 实测结果

2026-09-20，Intel Core i7-14700KF、NVIDIA RTX 4070 SUPER，
MinGW GCC 15.2、Release（`-O3`）。增量全构建成功；
CTest 6/6 目标通过（15.26 秒），包含 LuaLS API manifest 检查。

真实 OpenGL 基准 P95（毫秒，包含 GL 提交、`glFinish`、交换缓冲）：

| 模式 | 10 万点 | 100 万点 | 100 万点最大顶点数 |
| --- | ---: | ---: | ---: |
| 普通 | 2.158 | 2.661 | 29532 |
| Glow | 1.248 | 1.148 | 25292 |
| 堆叠 | 1.149 | 0.979 | 25772 |
| 分屏 | 1.135 | 1.063 | 32960 |
| GPU 余辉交互轨迹 | 1.100 | 1.165 | 25700 |
| CPU 触发余辉交互轨迹 | 1.098 | 1.554 | 25316 |
| FFT | 1.063 | 2.052 | 9778 |

以上七种自动化场景均低于 16.7 ms；两档点数的总顶点峰值为 33112，
低于配置的 60000 顶点预算。模式同时切换了 bit 样式，不能把模式间差值
单独归因于 Glow 或布局。FFT 初次预热等待分别为 39.501 / 177.338 ms，
不计入交互 P95；该时间包含输入准备、后台分析和轮询等待，不是纯 FFT 算法耗时。

独立查询基准（10 万 / 100 万点）：

- 原整窗复制算法 P95：3.146 / 30.620 ms。
- 摘要查询 P95：1.489 / 2.215 ms，不再随可见点数十倍增长而近似线性增长。
- 追加及首次索引：4.099 / 34.157 ms；摘要内存估计：657280 / 5158400 字节。
- 查询原始样本访问累计：118401715 / 118357875；摘要命中累计：142980 / 1687200。

原始日志位于本机构建目录的 `ctest-final.log`、`benchmark-gl-final.log`、
`benchmark-copy-final.log`、`benchmark-query-final.log`，不纳入源码提交。

## 验收边界

隐藏窗口自动化不代替人工拖动体验验收，也不包含真实串口吞吐压力。
测试使用程序设置逐帧窗口范围和按键状态，不模拟操作系统鼠标输入。
实际呈现延迟还受显示器刷新率、窗口合成器和驱动影响。
旧 HEAD 完整应用基线、人工 GPU/CPU 余辉拖动和真实串口压力尚未验收。
帧缓冲检查仅断言非空及无 OpenGL 错误，不代替波形外观的人工核对。
