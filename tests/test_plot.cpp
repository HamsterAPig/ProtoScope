#include "test_registry.hpp"

#include "protoscope/plot/oscilloscope.hpp"

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
