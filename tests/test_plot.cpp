#include "test_registry.hpp"

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/wave_math.hpp"

#include <cmath>
#include <stdexcept>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

void test_plot_history_trim_and_envelope() {
    protoscope::plot::OscilloscopeBuffer buffer;
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

    protoscope::plot::WaveAppendRequest request{.source = "test"};
    for (int i = 0; i < 8; ++i) {
        request.samples.push_back({.time = static_cast<double>(i), .value = std::sin(static_cast<double>(i))});
    }
    require(buffer.append(0, request), "追加采样应成功");

    const auto snapshot = buffer.snapshot(0.0, 10.0);
    require(snapshot.channels.size() == 1, "应存在 1 个通道");
    require(snapshot.channels[0].totalSamples == 5, "历史长度应裁剪到 5");

    const auto envelope = buffer.buildEnvelope(0, 3.0, 7.0, 2);
    require(!envelope.points.empty(), "降采样包络不应为空");
    require(envelope.sourceSampleCount == 5, "可视区样本数应匹配");
    require(envelope.points[0].sampleCount >= 1, "包络点应记录桶内样本数");
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

void test_plot_channel_offset_applies_to_display_only() {
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .offset = 1.5});
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
    require(std::abs(snapshot.channels[0].stats.minValue - 0.5) < 1e-9, "统计最小值应叠加 offset");
    require(std::abs(snapshot.channels[0].stats.maxValue - 2.5) < 1e-9, "统计最大值应叠加 offset");

    const auto envelope = buffer.buildEnvelope(0, 0.0, 1.0, 64);
    require(!envelope.points.empty(), "包络点不应为空");
    require(std::abs(envelope.points[0].minValue - 0.5) < 1e-9, "包络最小值应叠加 offset");

    const auto nearestByTime = buffer.findNearestByTime(0, 0.52, 0.2);
    require(nearestByTime.has_value(), "按时间吸附应成功");
    require(std::abs(nearestByTime->value - 2.5) < 1e-9, "游标读数应叠加 offset");

    const auto nearestByPoint = buffer.findNearest(0, 0.52, 2.4, 0.2, 0.3);
    require(nearestByPoint.has_value(), "按点吸附应成功");
    require(std::abs(nearestByPoint->value - 2.5) < 1e-9, "点吸附读数应叠加 offset");

    const auto measurement = buffer.measureWindow(0, 0.0, 1.0);
    require(measurement.valid, "窗口测量应有效");
    require(std::abs(measurement.minValue - 0.5) < 1e-9, "测量最小值应叠加 offset");
    require(std::abs(measurement.maxValue - 2.5) < 1e-9, "测量最大值应叠加 offset");
    require(std::abs(measurement.meanValue - 1.5) < 1e-9, "测量平均值应叠加 offset");
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

    const auto tiny = protoscope::plot::normalizeOverviewViewport(
        {.minTime = 4.0, .maxTime = 4.01, .minValue = -1.0, .maxValue = 1.0}, bounds, 0.5);
    require((tiny.maxTime - tiny.minTime) >= 0.5 - 1e-12, "概览窗口宽度不应小于最小时间宽度");
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
