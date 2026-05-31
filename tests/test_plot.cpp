#include "test_registry.hpp"

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/plot/wave_math.hpp"
#include "protoscope/plot/wave_state.hpp"

#include <cmath>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

protoscope::plot::WaveDisplayData makeDisplayData(std::vector<protoscope::plot::WaveSample> displaySamples,
                                                  std::vector<double> actualValues) {
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = std::move(displaySamples),
        .actualValues = std::move(actualValues),
    });
    return displayData;
}

} // namespace

void test_plot_history_trim_and_envelope() {
    protoscope::plot::OscilloscopeBuffer buffer;
    const auto initialRevision = buffer.dataRevision();
    buffer.setViewConfig(protoscope::plot::ViewConfig{
        .timeScale = 1.0,
        .timeUnit = "s",
        .verticalMin = -2.0,
        .verticalMax = 2.0,
        .verticalUnit = "V",
        .historyLimit = 5,
    });
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V"});
    require(buffer.dataRevision() > initialRevision, "配置变更应推进波形数据版本");

    protoscope::plot::WaveAppendRequest request{.source = "test"};
    for (int i = 0; i < 8; ++i) {
        request.samples.push_back({.time = static_cast<double>(i), .value = std::sin(static_cast<double>(i))});
    }
    require(buffer.append(0, request), "追加采样应成功");
    const auto appendRevision = buffer.dataRevision();

    const auto snapshot = buffer.snapshot(0.0, 10.0);
    require(snapshot.channels.size() == 1, "应存在 1 个通道");
    require(snapshot.channels[0].totalSamples == 5, "历史长度应裁剪到 5");
    const auto latest = buffer.latestTime();
    require(latest.has_value() && std::abs(*latest - 7.0) < 1e-12, "最新时间应来自裁剪后的尾部样本");
    require(buffer.dataRevision() == appendRevision, "只读快照不应推进波形数据版本");

    const auto envelope = buffer.buildEnvelope(0, 3.0, 7.0, 2);
    require(!envelope.points.empty(), "降采样包络不应为空");
    require(envelope.sourceSampleCount == 5, "可视区样本数应匹配");
    require(envelope.points[0].sampleCount >= 1, "包络点应记录桶内样本数");
}

void test_plot_limited_envelope_preserves_spikes() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.setViewConfig(protoscope::plot::ViewConfig{
        .timeScale = 1.0,
        .timeUnit = "s",
        .verticalMin = -20.0,
        .verticalMax = 20.0,
        .verticalUnit = "V",
        .historyLimit = 200,
    });
    buffer.configureChannels(1);

    protoscope::plot::WaveAppendRequest request{.source = "spike"};
    for (int index = 0; index < 100; ++index) {
        double value = 0.0;
        if (index == 20) {
            value = 12.0;
        } else if (index == 70) {
            value = -9.0;
        }
        request.samples.push_back({.time = static_cast<double>(index), .value = value});
    }
    require(buffer.append(0, std::move(request)), "追加尖峰采样应成功");

    const auto envelope = buffer.buildLimitedEnvelope(0, 0.0, 99.0, 5, 0);
    require(envelope.points.size() <= 5, "受限包络点数不应超过预算");

    bool foundPositiveSpike = false;
    bool foundNegativeSpike = false;
    for (const auto& point : envelope.points) {
        foundPositiveSpike = foundPositiveSpike || point.maxValue >= 12.0;
        foundNegativeSpike = foundNegativeSpike || point.minValue <= -9.0;
    }
    require(foundPositiveSpike, "min/max 桶应保留正向尖峰");
    require(foundNegativeSpike, "min/max 桶应保留负向尖峰");
}

void test_wave_layout_solver_clamps_without_overflow() {
    const auto layout = protoscope::plot::solveWaveLayout(900.0F,
                                                          420.0F,
                                                          240.0F,
                                                          300.0F,
                                                          34.0F,
                                                          false,
                                                          6.0F,
                                                          6.0F,
                                                          72.0F,
                                                          160.0F,
                                                          220.0F,
                                                          520.0F,
                                                          58.0F);
    require(layout.toolsWidth >= 220.0F && layout.toolsWidth <= 520.0F, "工具栏宽度应被夹取到允许范围");
    require(layout.overviewHeight >= 72.0F, "概览高度不应低于最小值");
    require(layout.mainHeight >= 160.0F, "主视图高度不应低于最小值");
    require(layout.overviewHeight + layout.mainHeight + 6.0F + 58.0F <= 420.0F + 1e-3F, "布局总高度不应溢出父窗口");

    const auto draggedOverview = protoscope::plot::solveWaveLayout(900.0F,
                                                                   600.0F,
                                                                   260.0F,
                                                                   300.0F,
                                                                   34.0F,
                                                                   false,
                                                                   6.0F,
                                                                   6.0F,
                                                                   72.0F,
                                                                   160.0F,
                                                                   220.0F,
                                                                   520.0F,
                                                                   70.0F);
    require(std::abs(draggedOverview.overviewHeight - 260.0F) < 1e-3F, "概览高度应尊重用户拖拽值，不应被 35% 上限拉回");

    const auto splitterReserved = protoscope::plot::solveWaveLayout(230.0F,
                                                                    420.0F,
                                                                    120.0F,
                                                                    280.0F,
                                                                    34.0F,
                                                                    false,
                                                                    6.0F,
                                                                    6.0F,
                                                                    72.0F,
                                                                    160.0F,
                                                                    220.0F,
                                                                    520.0F,
                                                                    58.0F);
    require(splitterReserved.toolsWidth <= 224.0F, "工具栏宽度应为内容区保留 splitter 空间");

    const auto compact = protoscope::plot::solveWaveLayout(320.0F,
                                                           120.0F,
                                                           160.0F,
                                                           300.0F,
                                                           34.0F,
                                                           true,
                                                           6.0F,
                                                           6.0F,
                                                           72.0F,
                                                           160.0F,
                                                           220.0F,
                                                           520.0F,
                                                           40.0F);
    require(compact.toolsWidth == 34.0F, "折叠工具栏应使用折叠宽度");
    require(compact.overviewHeight + compact.mainHeight + 6.0F + 40.0F <= 120.0F + 1e-3F, "紧凑布局也不应产生纵向溢出");
}

void test_plot_low_density_envelope_keeps_single_value_line() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.setViewConfig(protoscope::plot::ViewConfig{
        .timeScale = 1.0,
        .timeUnit = "s",
        .verticalMin = -10.0,
        .verticalMax = 10.0,
        .verticalUnit = "V",
        .historyLimit = 20,
    });
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = 2.0, .offset = -1.0});

    require(buffer.append(0, protoscope::plot::WaveAppendRequest{
                               .source = "low-density",
                               .samples = {
                                   {.time = 0.0, .value = 1.0},
                                   {.time = 1.0, .value = -2.0},
                                   {.time = 2.0, .value = 3.0},
                               },
                           }),
            "追加低密度采样应成功");

    const auto envelope = buffer.buildEnvelope(0, 0.0, 2.0, 4);
    require(envelope.sourceSampleCount == 3, "低密度包络应记录原始样本数量");
    require(envelope.points.size() == 3, "低密度包络应保留每个原始样本");
    for (const auto& point : envelope.points) {
        require(point.sampleCount == 1, "低密度包络每点应只对应一个样本");
        require(std::abs(point.minValue - point.maxValue) < 1e-12, "低密度包络应退化为单值折线");
    }
    require(std::abs(envelope.points[0].minValue) < 1e-12, "低密度包络应应用 offset_then_scale");
    require(std::abs(envelope.points[1].minValue + 6.0) < 1e-12, "低密度包络应保留 offset_then_scale 后的负值");
    require(std::abs(envelope.points[2].minValue - 4.0) < 1e-12, "低密度包络应保留 offset_then_scale 后的正值");
}

void test_plot_cursor_snap_and_delta() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V"});
    buffer.append(0, protoscope::plot::WaveAppendRequest{
        .source = "test",
        .samples = {
            {.time = 0.0, .value = 0.0},
            {.time = 0.5, .value = 1.0},
            {.time = 1.0, .value = 0.0},
        },
    });

    const auto left = buffer.findNearest(0, 0.48, 0.9, 0.1, 0.2);
    const auto right = buffer.findNearest(0, 1.01, 0.05, 0.1, 0.2);
    require(left.has_value(), "左游标应吸附到附近数据点");
    require(right.has_value(), "右游标应吸附到附近数据点");
    require(left->sampleIndex == 1, "左游标应吸附到峰值点");
    require(right->sampleIndex == 2, "右游标应吸附到末尾点");

    const auto delta = protoscope::plot::OscilloscopeBuffer::makeDelta(*left, *right);
    require(delta.valid, "游标差值应有效");
    require(std::abs(delta.deltaTime - 0.5) < 1e-9, "delta t 计算错误");
    require(std::abs(delta.frequencyHz - 2.0) < 1e-9, "频率计算错误");
}

void test_plot_cursor_snap_by_time_and_measurement() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V"});
    buffer.append(0, protoscope::plot::WaveAppendRequest{
        .source = "test",
        .samples = {
            {.time = 0.0, .value = -1.0},
            {.time = 0.5, .value = 1.0},
            {.time = 1.0, .value = 0.0},
            {.time = 1.5, .value = 0.5},
        },
    });

    const auto snap = buffer.findNearestByTime(0, 0.94, 0.2);
    require(snap.has_value(), "按时间吸附应成功");
    require(snap->sampleIndex == 2, "应吸附到最近时间点");

    const auto measurement = buffer.measureWindow(0, 0.0, 1.0);
    require(measurement.valid, "测量窗口应有效");
    require(measurement.sampleCount == 3, "窗口样本数错误");
    require(std::abs(measurement.minValue + 1.0) < 1e-9, "最小值错误");
    require(std::abs(measurement.maxValue - 1.0) < 1e-9, "最大值错误");
    require(std::abs(measurement.peakToPeak - 2.0) < 1e-9, "峰峰值错误");
}

void test_wave_cursor_smart_snap_edge() {
    const auto displayData = makeDisplayData({
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 0.0},
        {.time = 1.1, .value = -10.0},
        {.time = 2.0, .value = -10.0},
    }, {0.0, 0.0, 5.0, 5.0});

    const auto edge = protoscope::plot::findStrongestEdgeNearTime(displayData, 0, 1.02, 0.08);
    require(edge.has_value(), "智能吸附应找到附近最大跳变边沿");
    require(edge->sampleIndex == 2, "边沿吸附应记录跳变右侧样本索引");
    require(std::abs(edge->time - 1.05) < 1e-9, "边沿吸附应落到跳变中点");
    require(std::abs(edge->value - 5.0) < 1e-9, "边沿吸附文本值应取右侧样本真实值");
    require(std::abs(edge->displayValue + 5.0) < 1e-9, "边沿吸附锚点应取显示几何中点");

    const auto farEdge = protoscope::plot::findStrongestEdgeNearTime(displayData, 0, 0.5, 0.1);
    require(!farEdge.has_value(), "窗口外边沿不应被吸附");
}

void test_wave_cursor_smart_snap_extreme() {
    const auto displayData = makeDisplayData({
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = -4.0},
        {.time = 2.0, .value = 0.0},
        {.time = 3.0, .value = 6.0},
        {.time = 4.0, .value = 0.0},
        {.time = 5.0, .value = -2.0},
        {.time = 6.0, .value = 0.0},
    }, {0.0, 2.0, 0.0, -3.0, 0.0, 1.0, 0.0});

    const auto peak = protoscope::plot::findLocalExtremeNearTime(
        displayData, 0, 2.8, 0.4, protoscope::plot::WaveExtremeKind::Maximum);
    require(peak.has_value(), "顶部极值吸附应找到局部最大值");
    require(peak->sampleIndex == 3, "顶部极值吸附样本索引错误");
    require(std::abs(peak->value + 3.0) < 1e-9, "顶部极值吸附文本值应保持真实值");
    require(std::abs(peak->displayValue - 6.0) < 1e-9, "顶部极值吸附锚点应使用显示值");

    const auto trough = protoscope::plot::findLocalExtremeNearTime(
        displayData, 0, 1.2, 0.4, protoscope::plot::WaveExtremeKind::Minimum);
    require(trough.has_value(), "底部极值吸附应找到局部最小值");
    require(trough->sampleIndex == 1, "底部极值吸附样本索引错误");
    require(std::abs(trough->value - 2.0) < 1e-9, "底部极值吸附文本值应保持真实值");
    require(std::abs(trough->displayValue + 4.0) < 1e-9, "底部极值吸附锚点应使用显示值");

    const auto farPeak = protoscope::plot::findLocalExtremeNearTime(
        displayData, 0, 4.5, 0.2, protoscope::plot::WaveExtremeKind::Maximum);
    require(!farPeak.has_value(), "窗口外极值不应被吸附");
}

void test_wave_cursor_extreme_snap_falls_back_to_window_peak_with_transforms() {
    struct Case {
        const char* name;
        double scale;
        double offset;
    };
    const std::vector<Case> cases{
        {"no_transform", 1.0, 0.0},
        {"offset_only", 1.0, 2.0},
        {"scale_only", 3.0, 0.0},
        {"offset_and_scale", 2.0, -0.5},
    };

    for (const auto& item : cases) {
        protoscope::plot::WaveDisplayData displayData;
        const std::vector<double> actualValues{-1.0, 0.25, 1.0, 1.0, 0.25, -1.0};
        auto& channel = displayData.channels.emplace_back();
        channel.actualValues = actualValues;
        for (std::size_t index = 0; index < actualValues.size(); ++index) {
            const double displayValue = (actualValues[index] + item.offset) * item.scale;
            channel.samples.push_back({
                .time = static_cast<double>(index),
                .value = displayValue,
            });
        }

        const auto peak = protoscope::plot::findLocalExtremeNearTime(
            displayData, 0, 2.9, 1.5, protoscope::plot::WaveExtremeKind::Maximum);
        require(peak.has_value(), item.name);
        require(peak->sampleIndex == 3, "平顶峰值应回退到搜索窗口内距离鼠标时间最近的视觉峰值样本");
        require(std::abs(peak->value - 1.0) < 1e-9, "峰值吸附读数应保持真实测量值");
        require(std::abs(peak->displayValue - ((1.0 + item.offset) * item.scale)) < 1e-9,
            "峰值吸附锚点应使用 offset/scale 后的显示值");
    }
}

void test_wave_cursor_extreme_snap_falls_back_to_window_trough() {
    const auto displayData = makeDisplayData({
        {.time = 0.0, .value = 2.0},
        {.time = 1.0, .value = -3.0},
        {.time = 2.0, .value = -3.0},
        {.time = 3.0, .value = -1.0},
    }, {2.0, -30.0, -30.0, -1.0});

    const auto trough = protoscope::plot::findLocalExtremeNearTime(
        displayData, 0, 1.2, 1.3, protoscope::plot::WaveExtremeKind::Minimum);
    require(trough.has_value(), "平底谷值应能在窗口内回退吸附");
    require(trough->sampleIndex == 1, "平底谷值应选距离鼠标时间最近的视觉谷值样本");
    require(std::abs(trough->value + 30.0) < 1e-9, "谷值吸附读数应保持真实测量值");
    require(std::abs(trough->displayValue + 3.0) < 1e-9, "谷值吸附锚点应使用显示值");

    const auto farPeak = protoscope::plot::findLocalExtremeNearTime(
        displayData, 0, 3.0, 0.2, protoscope::plot::WaveExtremeKind::Maximum);
    require(!farPeak.has_value(), "搜索窗口外的平底谷值不应污染峰值吸附");
}

void test_wave_cursor_smart_snap_fallback_to_nearest() {
    const std::vector<protoscope::plot::WaveSample> flatSamples{
        {.time = 0.0, .value = 1.0},
        {.time = 1.0, .value = 1.0},
        {.time = 2.0, .value = 1.0},
    };
    const auto displayData = makeDisplayData(flatSamples, {1.0, 1.0, 1.0});
    require(!protoscope::plot::findStrongestEdgeNearTime(displayData, 0, 1.0, 0.5).has_value(),
        "平坦波形不应产生边沿吸附");
    require(!protoscope::plot::findLocalExtremeNearTime(
                displayData, 0, 1.0, 0.5, protoscope::plot::WaveExtremeKind::Maximum)
                 .has_value(),
        "平坦波形不应产生极值吸附");

    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.append(0, protoscope::plot::WaveAppendRequest{.source = "test", .samples = flatSamples});
    const auto nearest = buffer.findNearestByTime(0, 0.9, 0.2);
    require(nearest.has_value(), "无智能吸附目标时应保留按时间最近点兜底能力");
    require(nearest->sampleIndex == 1, "兜底最近点索引错误");
}

void test_wave_cursor_drag_time_uses_smart_snap() {
    const double dragTime = 1.02;
    const std::optional<protoscope::plot::CursorReadout> smartSnap = protoscope::plot::CursorReadout{
        .valid = true,
        .channelIndex = 0,
        .sampleIndex = 2,
        .time = 1.05,
        .value = 2.5,
    };

    const double snappedTime = protoscope::plot::applyCursorDragSnap(dragTime, smartSnap);
    require(std::abs(snappedTime - 1.05) < 1e-9, "智能吸附时间应覆盖 DragLineX 写入的鼠标时间");

    const double fallbackTime = protoscope::plot::applyCursorDragSnap(dragTime, std::nullopt);
    require(std::abs(fallbackTime - dragTime) < 1e-9, "无智能吸附结果时应保留拖动时间");
}

void test_plot_channel_scale_and_offset_apply_to_display_only() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = -2.0, .offset = 1.5});
    buffer.append(0, protoscope::plot::WaveAppendRequest{
        .source = "test",
        .samples = {
            {.time = 0.0, .value = -1.0},
            {.time = 0.5, .value = 1.0},
            {.time = 1.0, .value = 0.0},
        },
    });

    const auto snapshot = buffer.snapshot(0.0, 1.0);
    require(snapshot.channels.size() == 1, "应存在 1 个通道");
    require(snapshot.channels[0].samples != nullptr, "原始样本指针不应为空");
    require(std::abs(snapshot.channels[0].samples[0].value + 1.0) < 1e-9, "原始样本值不应被 offset 污染");
    require(std::abs(snapshot.channels[0].stats.minValue + 5.0) < 1e-9, "统计最小值应应用 offset_then_scale");
    require(std::abs(snapshot.channels[0].stats.maxValue + 1.0) < 1e-9, "统计最大值应应用 offset_then_scale");

    const auto envelope = buffer.buildEnvelope(0, 0.0, 1.0, 64);
    require(!envelope.points.empty(), "包络点不应为空");
    require(std::abs(envelope.points[0].minValue + 1.0) < 1e-9, "包络首点应应用 offset_then_scale");

    const auto mapped = protoscope::plot::buildDisplayData(snapshot, 0.0);
    require(mapped.channels.size() == 1, "显示数据应保留 1 个通道");
    require(std::abs(mapped.channels[0].samples[0].value + 1.0) < 1e-9, "显示数据应应用 offset_then_scale");
    require(std::abs(mapped.channels[0].samples[1].value + 5.0) < 1e-9, "显示数据应保留负缩放后的值");

    const auto nearestByTime = buffer.findNearestByTime(0, 0.52, 0.2);
    require(nearestByTime.has_value(), "按时间吸附应成功");
    require(std::abs(nearestByTime->value - 1.0) < 1e-9, "游标真实值应保持原始测量值");
    require(std::abs(nearestByTime->displayValue + 5.0) < 1e-9, "游标显示值应继续应用 offset_then_scale");

    const auto nearestByPoint = buffer.findNearest(0, 0.52, -5.1, 0.2, 0.3);
    require(nearestByPoint.has_value(), "按点吸附应成功");
    require(std::abs(nearestByPoint->value - 1.0) < 1e-9, "点吸附真实值应保持原始测量值");
    require(std::abs(nearestByPoint->displayValue + 5.0) < 1e-9, "点吸附显示值应继续应用 offset_then_scale");

    const auto measurement = buffer.measureWindow(0, 0.0, 1.0);
    require(measurement.valid, "窗口测量应有效");
    require(std::abs(measurement.minValue + 1.0) < 1e-9, "测量最小值应回到真实值");
    require(std::abs(measurement.maxValue - 1.0) < 1e-9, "测量最大值应回到真实值");
    require(std::abs(measurement.meanValue) < 1e-9, "测量平均值应回到真实值");

    protoscope::plot::OscilloscopeBuffer zeroScaleBuffer;
    zeroScaleBuffer.configureChannels(1);
    zeroScaleBuffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = 0.0, .offset = 1.25});
    zeroScaleBuffer.append(0, protoscope::plot::WaveAppendRequest{
        .source = "test",
        .samples = {
            {.time = 0.0, .value = -10.0},
            {.time = 0.5, .value = 5.0},
            {.time = 1.0, .value = 9.0},
        },
    });
    const auto zeroMeasurement = zeroScaleBuffer.measureWindow(0, 0.0, 1.0);
    require(zeroMeasurement.valid, "zero-scale 测量应有效");
    require(std::abs(zeroMeasurement.minValue + 10.0) < 1e-9, "scale=0 时最小值仍应保持真实值");
    require(std::abs(zeroMeasurement.maxValue - 9.0) < 1e-9, "scale=0 时最大值仍应保持真实值");
    require(std::abs(zeroMeasurement.rmsValue - std::sqrt(206.0 / 3.0)) < 1e-9, "scale=0 时 RMS 仍应按真实值计算");
}

void test_plot_channel_ratio_and_formula_modes() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.setViewConfig({
        .timeScale = 1.0,
        .timeUnit = "s",
        .verticalMin = -10.0,
        .verticalMax = 10.0,
        .verticalUnit = "V",
        .historyLimit = 32,
        .displayFormula = protoscope::plot::WaveDisplayFormula::ScaleThenOffset,
    });
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .ratio = 0.25, .scale = 3.0, .offset = 1.0});
    buffer.append(0, protoscope::plot::WaveAppendRequest{
        .source = "test",
        .samples = {
            {.time = 0.0, .value = 8.0},
            {.time = 1.0, .value = -4.0},
        },
    });

    const auto snapshot = buffer.snapshot(0.0, 1.0);
    require(std::abs(snapshot.channels[0].stats.maxValue - 7.0) < 1e-9, "scale_then_offset 最大值错误");
    require(std::abs(snapshot.channels[0].stats.minValue + 2.0) < 1e-9, "scale_then_offset 最小值错误");

    const auto mapped = protoscope::plot::buildDisplayData(snapshot, 0.0);
    require(std::abs(mapped.channels[0].samples[0].value - 7.0) < 1e-9, "ratio 后再 scale_then_offset 结果错误");
    require(std::abs(mapped.channels[0].samples[1].value + 2.0) < 1e-9, "负值 ratio 结果错误");
    require(std::abs(mapped.channels[0].actualValues[0] - 2.0) < 1e-9, "display data 应保留 ratio 后真实值");
    require(std::abs(mapped.channels[0].actualValues[1] + 1.0) < 1e-9, "display data 应保留负值真实值");

    const auto nearest = protoscope::plot::findNearestDisplayByTime(mapped, 0, 0.05, 0.2);
    require(nearest.has_value(), "display data 按时间吸附应成功");
    require(std::abs(nearest->value - 2.0) < 1e-9, "display data 读数应返回真实值");
    require(std::abs(nearest->displayValue - 7.0) < 1e-9, "display data 应同时返回显示值");
}

void test_plot_channel_transform_updates_are_isolated() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(2);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .ratio = 1.0, .scale = 2.0, .offset = 0.5});
    buffer.setChannelSpec(1, {.label = "CH2", .unit = "V", .ratio = 3.0, .scale = -1.0, .offset = -2.0});

    auto first = *buffer.channelSpec(0);
    const double originalOffset = first.offset;
    first.scale = 4.0;
    buffer.setChannelSpec(0, first);
    const auto afterScale = *buffer.channelSpec(0);
    require(std::abs(afterScale.scale - 4.0) < 1e-9, "更新 scale 应生效");
    require(std::abs(afterScale.offset - originalOffset) < 1e-9, "更新 scale 不应改动 offset");

    first = afterScale;
    const double originalScale = first.scale;
    first.offset = -3.0;
    first.ratio = 0.125;
    buffer.setChannelSpec(0, first);
    const auto afterOffset = *buffer.channelSpec(0);
    require(std::abs(afterOffset.offset + 3.0) < 1e-9, "更新 offset 应生效");
    require(std::abs(afterOffset.scale - originalScale) < 1e-9, "更新 offset 不应改动 scale");
    require(std::abs(afterOffset.ratio - 0.125) < 1e-9, "更新 ratio 应生效");

    const auto second = *buffer.channelSpec(1);
    require(std::abs(second.ratio - 3.0) < 1e-9, "单通道 ratio 变更不应影响其他通道");
    require(std::abs(second.scale + 1.0) < 1e-9, "单通道 scale 变更不应影响其他通道");
    require(std::abs(second.offset + 2.0) < 1e-9, "单通道 offset 变更不应影响其他通道");
}

void test_plot_cursor_snap_scope_selection() {
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.resize(2);
    displayData.channels[0].samples = {
        {.time = 0.0, .value = 0.0},
        {.time = 0.7, .value = 7.0},
    };
    displayData.channels[1].samples = {
        {.time = 0.5, .value = 5.0},
        {.time = 1.2, .value = 12.0},
    };

    const auto activeOnly = protoscope::plot::findNearestDisplayByTime(displayData, 0, 0.52, 0.3);
    require(activeOnly.has_value(), "当前激活通道模式应返回当前通道候选");
    require(activeOnly->channelIndex == 0, "当前激活通道模式不应跳到其他通道");
    require(std::abs(activeOnly->time - 0.7) < 1e-9, "当前激活通道模式应保留当前通道最近点");

    const auto allChannels = protoscope::plot::findNearestDisplayByTimeAcrossChannels(displayData, 0.52, 0.3);
    require(allChannels.has_value(), "全部波形模式应返回跨通道候选");
    require(allChannels->channelIndex == 1, "全部波形模式应选择跨通道最近点");
    require(std::abs(allChannels->time - 0.5) < 1e-9, "全部波形模式应命中真正最近的时间点");
}

void test_plot_hover_readout_ignores_hidden_channels() {
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.resize(2);
    displayData.channels[0].samples = {
        {.time = 1.0, .value = 10.0},
    };
    displayData.channels[1].samples = {
        {.time = 1.02, .value = 10.1},
    };

    const std::vector<std::size_t> onlyFirstVisible{0};
    const auto visibleOnly = protoscope::plot::findNearestDisplayPointInChannels(displayData, onlyFirstVisible, 1.02, 10.1, 0.1, 0.2);
    require(visibleOnly.has_value(), "悬停吸附应在可见通道内继续命中候选");
    require(visibleOnly->channelIndex == 0, "隐藏通道不应参与悬停吸附评分");

    const std::vector<std::size_t> noVisibleChannels{};
    const auto allHidden = protoscope::plot::findNearestDisplayPointInChannels(displayData, noVisibleChannels, 1.02, 10.1, 0.1, 0.2);
    require(!allHidden.has_value(), "所有候选通道隐藏时悬停吸附应为空");
}

void test_plot_limited_envelope_edges() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V"});

    const auto empty = buffer.buildLimitedEnvelope(0, 0.0, 1.0, 64, 10);
    require(empty.points.empty(), "空数据包络应为空");
    require(empty.sourceSampleCount == 0, "空数据源样本数应为 0");

    protoscope::plot::WaveAppendRequest request{.source = "test"};
    for (int index = 0; index < 10; ++index) {
        request.samples.push_back({.time = static_cast<double>(index), .value = static_cast<double>(index * 2)});
    }
    require(buffer.append(0, request), "追加采样应成功");

    const auto limited = buffer.buildLimitedEnvelope(0, 0.0, 9.0, 64, 3);
    require(limited.sourceSampleCount == 10, "截断前源样本数应保留完整可视区数量");
    require(limited.points.size() == 3, "样本上限应限制实际输出点数");
    require(std::abs(limited.points.front().time - 7.0) < 1e-9, "截断后应保留最近样本起点");
    require(std::abs(limited.points.back().time - 9.0) < 1e-9, "截断后应保留最近样本终点");

    const auto reversed = buffer.buildLimitedEnvelope(0, 9.0, 0.0, 64, 3);
    require(reversed.sourceSampleCount == 10, "反向可视区应先归一化再统计源样本数");
    require(reversed.points.size() == 3, "反向可视区也应执行样本上限");
    require(std::abs(reversed.points.front().time - 7.0) < 1e-9, "反向可视区截断起点错误");

    const auto unlimited = buffer.buildLimitedEnvelope(0, 0.0, 9.0, 64, 0);
    require(unlimited.sourceSampleCount == 10, "上限为 0 应表示不截断");
    require(unlimited.points.size() == 10, "上限为 0 应输出完整直接包络");

    const auto single = buffer.buildLimitedEnvelope(0, 0.0, 9.0, 64, 1);
    require(single.sourceSampleCount == 10, "上限为 1 也应保留源样本数量");
    require(single.points.size() == 1, "上限为 1 应只输出一个最近样本");
    require(std::abs(single.points[0].time - 9.0) < 1e-9, "上限为 1 应保留最新样本");

    const auto zeroWidth = buffer.buildLimitedEnvelope(0, 0.0, 9.0, 0, 3);
    require(zeroWidth.points.empty(), "像素宽度为 0 时包络应为空");
    require(zeroWidth.sourceSampleCount == 0, "像素宽度为 0 时不应统计源样本");
}

void test_wave_frequency_parse_and_axis_mapping() {
    using protoscope::plot::WaveSample;
    const auto tenK = protoscope::plot::parseSampleFrequencyText("10e3");
    require(tenK.accepted && std::abs(tenK.valueHz - 10000.0) < 1e-9, "10e3 应解析为 10000Hz");
    const auto tenMilli = protoscope::plot::parseSampleFrequencyText("10e-3");
    require(tenMilli.accepted && std::abs(tenMilli.valueHz - 0.01) < 1e-12, "10e-3 应解析为 0.01Hz");
    const auto oneK = protoscope::plot::parseSampleFrequencyText("1e3");
    require(oneK.accepted && std::abs(oneK.valueHz - 1000.0) < 1e-9, "1e3 应解析为 1000Hz");
    require(protoscope::plot::parseSampleFrequencyText("0").accepted, "0 应表示不使用控件频率");
    require(protoscope::plot::parseSampleFrequencyText("").accepted, "空字符串应表示不使用控件频率");
    require(!protoscope::plot::parseSampleFrequencyText("abc").accepted, "非法字符串不应生效");

    protoscope::plot::WaveSnapshot snapshot{};
    snapshot.config.timeUnit = "ms";
    std::vector<WaveSample> scriptSamples{
        {.time = 2.0, .value = 1.0},
        {.time = 4.0, .value = 2.0},
        {.time = 6.0, .value = 3.0},
    };
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "V",
        .scale = 1.0,
        .offset = 0.0,
        .totalSamples = scriptSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = scriptSamples.size(),
        .samples = scriptSamples.data(),
    });

    auto mapped = protoscope::plot::buildDisplayData(snapshot, 1000.0);
    require(mapped.axisSource == protoscope::plot::WaveTimeAxisSource::SampleFrequency, "控件频率应优先作为时间轴来源");
    require(std::abs(mapped.channels[0].samples[2].time - 0.002) < 1e-12, "频率时间轴应按样本序号换算秒");

    mapped = protoscope::plot::buildDisplayData(snapshot, 0.0);
    require(mapped.axisSource == protoscope::plot::WaveTimeAxisSource::ScriptTime, "无控件频率时应使用脚本时间");
    require(mapped.timeUnit == "ms", "脚本时间轴应保留配置单位");

    const std::vector<WaveSample> badScriptSamples{
        {.time = 2.0, .value = 1.0},
        {.time = 2.0, .value = 2.0},
    };
    snapshot.channels[0].totalSamples = badScriptSamples.size();
    snapshot.channels[0].visibleEnd = badScriptSamples.size();
    snapshot.channels[0].samples = badScriptSamples.data();
    mapped = protoscope::plot::buildDisplayData(snapshot, 0.0);
    require(mapped.axisSource == protoscope::plot::WaveTimeAxisSource::SampleIndex, "脚本时间不可用时应退回点数轴");
    require(mapped.timeUnit == "sample", "点数轴单位应为 sample");
}

void test_wave_viewport_zoom_modes_and_clamp() {
    const protoscope::plot::WaveViewport viewport{
        .minTime = 2.0,
        .maxTime = 6.0,
        .minValue = -2.0,
        .maxValue = 2.0,
    };
    const protoscope::plot::WaveDataBounds bounds{
        .minTime = 0.0,
        .maxTime = 10.0,
        .minValue = -5.0,
        .maxValue = 5.0,
        .minStep = 0.1,
        .valid = true,
    };

    const auto xOnly = protoscope::plot::zoomViewport(
        viewport, protoscope::plot::WaveZoomMode::XOnly, 1.0, 4.0, 0.0, bounds, 0.1, true);
    require((xOnly.maxTime - xOnly.minTime) < 4.0, "X-only 滚轮应缩小横轴范围");
    require(std::abs(xOnly.minValue - viewport.minValue) < 1e-12, "X-only 不应改变纵轴最小值");
    require(std::abs(xOnly.maxValue - viewport.maxValue) < 1e-12, "X-only 不应改变纵轴最大值");

    const auto yOnly = protoscope::plot::zoomViewport(
        viewport, protoscope::plot::WaveZoomMode::YOnly, 1.0, 4.0, 0.0, bounds, 0.1, true);
    require(std::abs(yOnly.minTime - viewport.minTime) < 1e-12, "Y-only 不应改变横轴最小值");
    require((yOnly.maxValue - yOnly.minValue) < 4.0, "Y-only 滚轮应缩小纵轴范围");

    const auto xy = protoscope::plot::zoomViewport(
        viewport, protoscope::plot::WaveZoomMode::XY, 1.0, 4.0, 0.0, bounds, 0.1, true);
    require((xy.maxTime - xy.minTime) < 4.0, "XY 滚轮应缩小横轴范围");
    require((xy.maxValue - xy.minValue) < 4.0, "XY 滚轮应缩小纵轴范围");

    const protoscope::plot::WaveViewport nearLeft{
        .minTime = 0.0,
        .maxTime = 1.0,
        .minValue = -1.0,
        .maxValue = 1.0,
    };
    const auto clamped = protoscope::plot::zoomViewport(
        nearLeft, protoscope::plot::WaveZoomMode::XOnly, -5.0, 0.0, 0.0, bounds, 0.1, true);
    require(clamped.minTime >= bounds.minTime - 1e-12, "概览缩放应夹紧到数据左边界");
    require((clamped.maxTime - clamped.minTime) >= 0.1, "缩放宽度不应小于最小范围");
}

void test_wave_overview_viewport_normalize() {
    const protoscope::plot::WaveDataBounds bounds{
        .minTime = 0.0,
        .maxTime = 10.0,
        .minValue = -1.0,
        .maxValue = 1.0,
        .minStep = 0.1,
        .valid = true,
    };

    const auto reversed = protoscope::plot::normalizeOverviewViewport(
        {.minTime = 8.0, .maxTime = 3.0, .minValue = -1.0, .maxValue = 1.0}, bounds, 0.5);
    require(std::abs(reversed.minTime - 3.0) < 1e-12, "反向拖动后左边界应归一化");
    require(std::abs(reversed.maxTime - 8.0) < 1e-12, "反向拖动后右边界应归一化");

    const auto overflow = protoscope::plot::normalizeOverviewViewport(
        {.minTime = -2.0, .maxTime = 3.0, .minValue = -1.0, .maxValue = 1.0}, bounds, 0.5);
    require(overflow.minTime >= bounds.minTime - 1e-12, "概览时间窗应夹紧到左边界");
    require(std::abs((overflow.maxTime - overflow.minTime) - 5.0) < 1e-12, "越界夹紧应保持窗口宽度");

    const protoscope::plot::WaveViewport viewport{.minTime = 2.0, .maxTime = 5.0, .minValue = -0.5, .maxValue = 0.5};
    const auto movedRight = protoscope::plot::moveViewportByDelta(viewport, 2.0, bounds, 0.5);
    require(std::abs(movedRight.minTime - 4.0) < 1e-12, "概览整窗拖动应平移左边界");
    require(std::abs(movedRight.maxTime - 7.0) < 1e-12, "概览整窗拖动应平移右边界");
    require(std::abs(movedRight.minValue - viewport.minValue) < 1e-12, "概览整窗拖动不应改变纵轴最小值");
    require(std::abs(movedRight.maxValue - viewport.maxValue) < 1e-12, "概览整窗拖动不应改变纵轴最大值");

    const auto movedOverflow = protoscope::plot::moveViewportByDelta(viewport, 20.0, bounds, 0.5);
    require(movedOverflow.maxTime <= bounds.maxTime + 1e-12, "概览整窗拖动应夹紧到右边界");
    require(std::abs((movedOverflow.maxTime - movedOverflow.minTime) - 3.0) < 1e-12, "整窗拖动越界应保持时间窗口宽度");

    const auto tiny = protoscope::plot::normalizeOverviewViewport(
        {.minTime = 4.0, .maxTime = 4.01, .minValue = -1.0, .maxValue = 1.0}, bounds, 0.5);
    require((tiny.maxTime - tiny.minTime) >= 0.5 - 1e-12, "概览窗口宽度不应小于最小时间宽度");
}

void test_wave_cursor_position_in_viewport() {
    const protoscope::plot::WaveViewport viewport{
        .minTime = 10.0,
        .maxTime = 20.0,
        .minValue = -1.0,
        .maxValue = 1.0,
    };

    require(std::abs(protoscope::plot::cursorTimeInViewport(viewport, 0.5) - 15.0) < 1e-12,
        "单游标快捷定位应落在视窗中点");
    require(std::abs(protoscope::plot::cursorTimeInViewport(viewport, 0.4) - 14.0) < 1e-12,
        "双游标左侧快捷定位应落在视窗 40% 位置");
    require(std::abs(protoscope::plot::cursorTimeInViewport(viewport, 0.6) - 16.0) < 1e-12,
        "双游标右侧快捷定位应落在视窗 60% 位置");
    require(std::abs(protoscope::plot::cursorTimeInViewport(viewport, -1.0) - 10.0) < 1e-12,
        "游标快捷定位比例应夹紧到左边界");
    require(std::abs(protoscope::plot::cursorTimeInViewport(viewport, 2.0) - 20.0) < 1e-12,
        "游标快捷定位比例应夹紧到右边界");
}

void test_wave_cursor_interval_text_by_axis() {
    const protoscope::plot::CursorReadout left{
        .valid = true,
        .channelIndex = 0,
        .sampleIndex = 1,
        .time = 2.0,
        .value = 1.0,
    };
    const protoscope::plot::CursorReadout right{
        .valid = true,
        .channelIndex = 0,
        .sampleIndex = 5,
        .time = 6.0,
        .value = 3.0,
    };

    const auto sample = protoscope::plot::makeCursorIntervalText(
        left, right, protoscope::plot::WaveTimeAxisSource::SampleIndex, "sample");
    require(sample.valid, "点数轴游标间隔应有效");
    require(!sample.showFrequency, "点数轴不应伪装显示 Hz");
    require(sample.deltaUnit == "sample", "点数轴间隔单位应为 sample");

    const auto scriptTime = protoscope::plot::makeCursorIntervalText(
        left, right, protoscope::plot::WaveTimeAxisSource::ScriptTime, "ms");
    require(scriptTime.valid, "脚本时间轴游标间隔应有效");
    require(scriptTime.showFrequency, "脚本时间轴应显示倒数频率");
    require(std::abs(scriptTime.frequencyHz - 0.25) < 1e-12, "脚本时间轴频率计算错误");

    const auto sampledTime = protoscope::plot::makeCursorIntervalText(
        left, right, protoscope::plot::WaveTimeAxisSource::SampleFrequency, "s");
    require(sampledTime.valid, "采样频率时间轴游标间隔应有效");
    require(sampledTime.showFrequency, "采样频率时间轴应显示倒数频率");
    require(std::abs(sampledTime.frequencyHz - 0.25) < 1e-12, "采样频率时间轴频率计算错误");
}

void test_wave_cursor_interval_lock() {
    double right = 3.0;
    protoscope::plot::lockCursorInterval(1.0, right, 2.0, true);
    require(std::abs(right - 3.0) < 1e-12, "移动左游标时右游标应保持锁定间隔");

    double left = 1.0;
    protoscope::plot::lockCursorInterval(5.0, left, 2.0, false);
    require(std::abs(left - 3.0) < 1e-12, "移动右游标时左游标应保持锁定间隔");
}

void test_wave_channel_card_width_modes() {
    const double fixedWidth = protoscope::plot::resolveChannelCardWidth(
        protoscope::plot::WaveChannelCardWidthMode::Fixed, 128.0, 0.22, 1000.0);
    require(std::abs(fixedWidth - 128.0) < 1e-12, "固定模式应直接使用配置宽度");

    const double adaptiveWidth = protoscope::plot::resolveChannelCardWidth(
        protoscope::plot::WaveChannelCardWidthMode::Adaptive, 128.0, 0.22, 1000.0);
    require(std::abs(adaptiveWidth - 220.0) < 1e-12, "自适应模式应按比例计算并保留上限夹取");
}

void test_wave_vertical_auto_fit_multiplier() {
    const auto negativeRange = protoscope::plot::makeVerticalAutoFitRange(-10.0, 5.0, 1.2);
    require(std::abs(negativeRange.minValue + 12.0) < 1e-12, "负向范围 Auto Fit 下限应乘以系数");
    require(std::abs(negativeRange.maxValue - 12.0) < 1e-12, "负向范围 Auto Fit 上限应乘以系数");

    const auto positiveRange = protoscope::plot::makeVerticalAutoFitRange(2.0, 3.0, 1.2);
    require(std::abs(positiveRange.minValue + 3.6) < 1e-12, "正向范围 Auto Fit 下限应围绕 0 对称");
    require(std::abs(positiveRange.maxValue - 3.6) < 1e-12, "正向范围 Auto Fit 上限应围绕 0 对称");
}

void test_wave_visible_channel_bounds_ignore_hidden_channels() {
    protoscope::plot::WaveDisplayData data;
    data.channels.push_back({
        .samples = {{.time = 0.0, .value = -1.0}, {.time = 1.0, .value = 1.0}},
        .actualValues = {},
    });
    data.channels.push_back({
        .samples = {{.time = -100.0, .value = -50.0}, {.time = 100.0, .value = 50.0}},
        .actualValues = {},
    });
    data.channels.push_back({
        .samples = {{.time = 2.0, .value = -3.0}, {.time = 4.0, .value = 4.0}},
        .actualValues = {},
    });

    const auto visibleOnly = protoscope::plot::computeDisplayBoundsForChannels(data, {0, 2}, 0.001);
    require(visibleOnly.valid, "可见通道 bounds 应有效");
    require(std::abs(visibleOnly.minTime - 0.0) < 1e-12, "隐藏通道不应拉低 X 下限");
    require(std::abs(visibleOnly.maxTime - 4.0) < 1e-12, "可见通道应决定 X 上限");
    require(std::abs(visibleOnly.minValue + 3.0) < 1e-12, "可见通道应决定 Y 下限");
    require(std::abs(visibleOnly.maxValue - 4.0) < 1e-12, "隐藏通道不应拉高 Y 上限");

    const auto empty = protoscope::plot::computeDisplayBoundsForChannels(data, {}, 0.001);
    require(!empty.valid, "没有可见通道时 bounds 应保持无效");
}

void test_wave_offset_reset_uses_protocol_default_only() {
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.defaultChannelSpecs.push_back({
        .label = "CH1",
        .unit = "V",
        .ratio = 2.0,
        .scale = 1.5,
        .offset = -0.25,
    });
    wave.buffer.setChannelSpec(0, {
        .label = "Renamed",
        .unit = "V",
        .ratio = 3.0,
        .scale = 4.0,
        .offset = 10.0,
    });
    wave.channelOverrides.resize(1);
    wave.channelOverrides[0].labelOverridden = true;
    wave.channelOverrides[0].ratioOverridden = true;
    wave.channelOverrides[0].scaleOverridden = true;
    wave.channelOverrides[0].offsetOverridden = true;
    wave.channelOverrides[0].label = "Renamed";
    wave.channelOverrides[0].ratio = 3.0;
    wave.channelOverrides[0].scale = 4.0;
    wave.channelOverrides[0].offset = 10.0;

    require(protoscope::plot::resetChannelOffsetToDefault(wave, 0), "offset 复位应成功");
    const auto spec = wave.buffer.channelSpec(0);
    require(spec.has_value(), "复位后通道配置仍应存在");
    require(std::abs(spec->offset + 0.25) < 1e-12, "offset 应恢复协议默认值");
    require(std::abs(spec->scale - 4.0) < 1e-12, "offset 复位不应修改 scale");
    require(std::abs(spec->ratio - 3.0) < 1e-12, "offset 复位不应修改 ratio");
    require(spec->label == "Renamed", "offset 复位不应修改标签");
    require(!wave.channelOverrides[0].offsetOverridden, "offset override 应被清除");
    require(wave.channelOverrides[0].scaleOverridden, "scale override 应保留");
    require(wave.channelOverrides[0].ratioOverridden, "ratio override 应保留");
    require(wave.channelOverrides[0].labelOverridden, "label override 应保留");
}

void test_raw_capture_file_roundtrip() {
    const auto tempPath = std::filesystem::temp_directory_path() / "protoscope-roundtrip.psraw";
    std::filesystem::remove(tempPath);

    const protoscope::plot::RawCaptureFileData capture{
        .protocolName = "default_protocol",
        .protocolDir = "protocols/templates/default_protocol",
        .sampleFrequencyHz = 4096.0,
        .capturedAtMs = 123456789,
        .payload = {0x01, 0x02, 0x7F, 0x00, 0x41},
    };

    std::string error;
    require(protoscope::plot::writeRawCaptureFile(tempPath, capture, error), "psraw 写入应成功");
    const auto loaded = protoscope::plot::readRawCaptureFile(tempPath, error);
    require(loaded.has_value(), "psraw 读回应成功");
    require(loaded->protocolName == capture.protocolName, "psraw 应保留协议名");
    require(loaded->protocolDir == capture.protocolDir, "psraw 应保留协议目录");
    require(loaded->sampleFrequencyHz == capture.sampleFrequencyHz, "psraw 应保留采样频率");
    require(loaded->capturedAtMs == capture.capturedAtMs, "psraw 应保留采集时间");
    require(loaded->payload == capture.payload, "psraw 应保留原始 payload");

    std::filesystem::remove(tempPath);
}

void test_raw_capture_file_rejects_size_mismatch() {
    const std::string broken = "ProtoScopeRawCapture\n"
                               "version: 1\n"
                               "protocol_name: default_protocol\n"
                               "protocol_dir: protocols/templates/default_protocol\n"
                               "sample_frequency_hz: 1024\n"
                               "raw_size: 5\n"
                               "captured_at_ms: 1\n"
                               "\n"
                               "abc";
    std::string error;
    const auto parsed = protoscope::plot::decodeRawCaptureFile(broken, error);
    require(!parsed.has_value(), "raw_size 不匹配时应拒绝解析");
}

void test_raw_capture_file_requires_protocol_fields() {
    const std::string broken = "ProtoScopeRawCapture\n"
                               "version: 1\n"
                               "sample_frequency_hz: 1024\n"
                               "raw_size: 3\n"
                               "captured_at_ms: 1\n"
                               "\n"
                               "abc";
    std::string error;
    const auto parsed = protoscope::plot::decodeRawCaptureFile(broken, error);
    require(!parsed.has_value(), "缺少 protocol 字段时应拒绝解析");
}
