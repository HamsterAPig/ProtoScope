#include "../src/ui/wave/wave_render_service.hpp"

#include "protoscope/config/config.hpp"
#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/plot/wave_fft.hpp"
#include "protoscope/plot/wave_math.hpp"
#include "protoscope/plot/wave_state.hpp"
#include "protoscope/session/session_package.hpp"

#include "test_registry.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct ScopedTempFile {
    explicit ScopedTempFile(const char* stem)
    {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() / (std::string(stem) + "-" + std::to_string(ticks) + ".psraw");
    }

    ~ScopedTempFile()
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

protoscope::plot::WaveDisplayData makeDisplayData(std::vector<protoscope::plot::WaveSample> displaySamples,
                                                  std::vector<double> actualValues)
{
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = std::move(displaySamples),
        .actualValues = std::move(actualValues),
    });
    return displayData;
}

protoscope::plot::WaveDockState makeChannelResetWave()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.defaultChannelSpecs.push_back({
        .label = "CH1",
        .unit = "V",
        .ratio = 2.0,
        .scale = 1.5,
        .offset = -0.25,
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 4, .yOffset = 1.0},
    });
    wave.buffer.setChannelSpec(0,
                               {
                                   .label = "Renamed",
                                   .unit = "V",
                                   .ratio = 3.0,
                                   .scale = 4.0,
                                   .offset = 10.0,
                                   .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 4, .yOffset = 3.0},
                               });
    wave.channelOverrides.resize(1);
    wave.channelOverrides[0].labelOverridden = true;
    wave.channelOverrides[0].ratioOverridden = true;
    wave.channelOverrides[0].scaleOverridden = true;
    wave.channelOverrides[0].offsetOverridden = true;
    wave.channelOverrides[0].bitYOffsetOverridden = true;
    wave.channelOverrides[0].label = "Renamed";
    wave.channelOverrides[0].ratio = 3.0;
    wave.channelOverrides[0].scale = 4.0;
    wave.channelOverrides[0].offset = 10.0;
    wave.channelOverrides[0].bitYOffset = 3.0;
    return wave;
}

} // namespace

void test_plot_history_trim_and_envelope()
{
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
    const auto baseSpecRevision = buffer.dataRevision();
    buffer.setChannelSpec(0,
                          {.label = "CH1",
                           .unit = "V",
                           .color = std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F},
                           .lineWidth = std::optional<float>{2.5F}});
    const auto styledSpec = buffer.channelSpec(0);
    require(styledSpec.has_value(), "样式更新后通道配置仍应存在");
    require(styledSpec->color.has_value(), "仅修改颜色也应更新通道配置");
    require(styledSpec->lineWidth.has_value(), "仅修改 line_width 也应更新通道配置");
    require(std::abs(*styledSpec->lineWidth - 2.5F) < 1e-6F, "通道 line_width 应保存到 buffer");
    require(buffer.dataRevision() > baseSpecRevision, "样式变更应推进波形数据版本");

    protoscope::plot::WaveAppendRequest request{.source = "test"};
    for (int i = 0; i < 8; ++i) {
        request.samples.push_back({.time = static_cast<double>(i), .value = std::sin(static_cast<double>(i))});
    }
    require(buffer.append(0, request), "追加采样应成功");
    const auto appendRevision = buffer.dataRevision();

    const auto snapshot = buffer.snapshot(0.0, 10.0);
    require(snapshot.channels.size() == 1, "应存在 1 个通道");
    require(snapshot.channels[0].lineWidth.has_value(), "快照应携带通道 line_width");
    require(std::abs(*snapshot.channels[0].lineWidth - 2.5F) < 1e-6F, "快照 line_width 应匹配通道配置");
    require(snapshot.channels[0].totalSamples == 5, "历史长度应裁剪到 5");
    const auto latest = buffer.latestTime();
    require(latest.has_value() && std::abs(*latest - 7.0) < 1e-12, "最新时间应来自裁剪后的尾部样本");
    require(buffer.dataRevision() == appendRevision, "只读快照不应推进波形数据版本");

    const auto envelope = buffer.buildEnvelope(0, 3.0, 7.0, 2);
    require(!envelope.points.empty(), "降采样包络不应为空");
    require(envelope.sourceSampleCount == 5, "可视区样本数应匹配");
    require(envelope.points[0].sampleCount >= 1, "包络点应记录桶内样本数");
}

void test_plot_history_limit_zero_keeps_all_samples()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.setViewConfig(protoscope::plot::ViewConfig{.historyLimit = 0});
    buffer.configureChannels(1);
    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .source = "zero-limit",
                              .samples = {{0.0, 1.0}, {1.0, 2.0}},
                          }),
            "追加采样应触发裁剪流程");

    const auto snapshot = buffer.snapshot(0.0, 1.0);
    require(snapshot.channels.size() == 1, "通道仍应存在");
    require(snapshot.channels.front().totalSamples == 2, "history_limit 为 0 时应保留完整历史");
    require(snapshot.channels.front().samples != nullptr, "完整历史应暴露样本指针");
    require(snapshot.channels.front().samples[0].time == 0.0 && snapshot.channels.front().samples[1].time == 1.0,
            "保留点应覆盖完整输入顺序");
}

void test_plot_set_view_config_applies_history_limit_immediately()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    protoscope::plot::WaveAppendRequest request{.source = "reload-limit"};
    for (int i = 0; i < 10; ++i) {
        request.samples.push_back({.time = static_cast<double>(i), .value = static_cast<double>(i)});
    }
    require(buffer.append(0, request), "无上限历史样本应追加成功");

    buffer.setViewConfig(protoscope::plot::ViewConfig{.historyLimit = 4});

    const auto snapshot =
        buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(snapshot.channels.size() == 1, "即时裁剪后仍应保留通道");
    const auto& channel = snapshot.channels.front();
    require(channel.totalSamples == 4, "setViewConfig 应立即应用新的 historyLimit");
    require(channel.sampleIndexOffset == 6, "即时裁剪应记录已丢弃的全局样本偏移");
    require(channel.samples != nullptr, "即时裁剪后的样本指针不能为空");
    require(std::abs(channel.samples[0].time - 6.0) < 1e-12, "即时裁剪应保留尾部窗口起点");
    require(std::abs(channel.samples[3].time - 9.0) < 1e-12, "即时裁剪应保留尾部窗口终点");
}

void test_plot_time_reset_clears_history_by_default()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V"});
    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .source = "run-1",
                              .samples = {{.time = 0.0, .value = 1.0}, {.time = 1.0, .value = 2.0}},
                          }),
            "第一轮样本应追加成功");
    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .source = "run-2",
                              .samples = {{.time = 0.0, .value = 10.0}, {.time = 1.0, .value = 20.0}},
                          }),
            "第二轮从 t=0 开始应清空旧历史后追加成功");

    const auto snapshot =
        buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(snapshot.channels.size() == 1, "应保留新一轮通道");
    require(snapshot.channels.front().totalSamples == 2, "默认策略下第二轮不应被旧历史挡住");
    require(snapshot.channels.front().samples != nullptr, "应能读取新一轮样本");
    require(std::abs(snapshot.channels.front().samples[0].value - 10.0) < 1e-12, "第一轮旧样本应被清掉");
    require(snapshot.source == "run-2", "清空后应保留第二轮 source");
    require(snapshot.channels.front().label == "CH1", "重开采集不应丢通道配置");
}

void test_plot_time_reset_can_continue_history()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.setResetHistoryOnTimeReset(false);
    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .samples = {{.time = 0.0, .value = 1.0}, {.time = 1.0, .value = 2.0}},
                          }),
            "第一轮样本应追加成功");
    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .samples = {{.time = 0.0, .value = 10.0}, {.time = 1.0, .value = 20.0}},
                          }),
            "继续累加策略下第二轮样本应追加成功");

    const auto snapshot =
        buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(snapshot.channels.size() == 1, "应保留通道");
    require(snapshot.channels.front().totalSamples == 4, "继续累加策略应保留两轮历史");
    require(snapshot.channels.front().samples != nullptr, "应能读取累加后的样本");
    require(std::abs(snapshot.channels.front().samples[2].time - 2.0) < 1e-12, "第二轮时间应接到第一轮之后");
    require(std::abs(snapshot.channels.front().samples[3].time - 3.0) < 1e-12, "第二轮末尾应连续显示");
}

void test_wave_sample_frequency_visible_range_filters_by_sample_index()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .samples =
                                  {
                                      {.time = 100.0, .value = 0.0},
                                      {.time = 101.0, .value = 1.0},
                                      {.time = 102.0, .value = 2.0},
                                      {.time = 103.0, .value = 3.0},
                                      {.time = 104.0, .value = 4.0},
                                      {.time = 105.0, .value = 5.0},
                                  },
                          }),
            "非零脚本时间样本应追加成功");

    auto snapshot = buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    protoscope::plot::applySampleFrequencyVisibleRange(snapshot, 0.3, 0.3, 10.0);
    const auto displayData = protoscope::plot::buildDisplayData(snapshot, 10.0);
    require(displayData.axisSource == protoscope::plot::WaveTimeAxisSource::SampleFrequency, "应使用采样频率时间轴");
    require(displayData.channels.size() == 1, "应保留通道显示数据");
    require(displayData.channels.front().samples.size() == 3, "Fs 单点视口应保留左右邻接样本");
    require(std::abs(displayData.channels.front().samples.front().time - 0.2) < 1e-12,
            "Fs 单点视口应从左侧邻接样本开始");
    require(std::abs(displayData.channels.front().samples.back().time - 0.4) < 1e-12, "Fs 单点视口应保留右侧邻接样本");
}

void test_wave_snapshot_visible_range_keeps_adjacent_samples()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .samples =
                                  {
                                      {.time = 1.0, .value = 1.0},
                                      {.time = 2.0, .value = 2.0},
                                      {.time = 3.0, .value = 3.0},
                                      {.time = 4.0, .value = 4.0},
                                      {.time = 5.0, .value = 5.0},
                                  },
                          }),
            "脚本时间样本应追加成功");

    const auto snapshot = buffer.snapshot(3.0, 3.0);
    const auto displayData = protoscope::plot::buildDisplayData(snapshot, 0.0);
    require(displayData.axisSource == protoscope::plot::WaveTimeAxisSource::ScriptTime, "应使用脚本时间轴");
    require(displayData.channels.size() == 1, "应保留通道显示数据");
    require(displayData.channels.front().samples.size() == 3, "脚本时间单点视口应保留左右邻接样本");
    require(std::abs(displayData.channels.front().samples.front().time - 2.0) < 1e-12,
            "脚本时间单点视口应从左侧邻接样本开始");
    require(std::abs(displayData.channels.front().samples[1].time - 3.0) < 1e-12, "脚本时间单点视口应包含可见样本");
    require(std::abs(displayData.channels.front().samples.back().time - 4.0) < 1e-12,
            "脚本时间单点视口应保留右侧邻接样本");
}

void test_wave_visible_range_adjacent_samples_clamp_at_edges()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .samples =
                                  {
                                      {.time = 1.0, .value = 1.0},
                                      {.time = 2.0, .value = 2.0},
                                      {.time = 3.0, .value = 3.0},
                                  },
                          }),
            "边界样本应追加成功");

    const auto firstSnapshot = buffer.snapshot(1.0, 1.0);
    const auto firstDisplayData = protoscope::plot::buildDisplayData(firstSnapshot, 0.0);
    require(firstDisplayData.channels.size() == 1, "首样本视口应保留通道显示数据");
    require(firstDisplayData.channels.front().samples.size() == 2, "首样本视口只应保留存在的右侧邻接样本");
    require(std::abs(firstDisplayData.channels.front().samples.front().time - 1.0) < 1e-12, "首样本视口不应向左越界");
    require(std::abs(firstDisplayData.channels.front().samples.back().time - 2.0) < 1e-12,
            "首样本视口应保留右侧邻接样本");

    const auto lastSnapshot = buffer.snapshot(3.0, 3.0);
    const auto lastDisplayData = protoscope::plot::buildDisplayData(lastSnapshot, 0.0);
    require(lastDisplayData.channels.size() == 1, "末样本视口应保留通道显示数据");
    require(lastDisplayData.channels.front().samples.size() == 2, "末样本视口只应保留存在的左侧邻接样本");
    require(std::abs(lastDisplayData.channels.front().samples.front().time - 2.0) < 1e-12,
            "末样本视口应保留左侧邻接样本");
    require(std::abs(lastDisplayData.channels.front().samples.back().time - 3.0) < 1e-12, "末样本视口不应向右越界");
}

void test_wave_sample_frequency_preserves_trimmed_sample_offset()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.setViewConfig(protoscope::plot::ViewConfig{.historyLimit = 5});
    protoscope::plot::WaveAppendRequest request{.source = "trimmed-fs"};
    for (int i = 0; i < 10; ++i) {
        request.samples.push_back({.time = static_cast<double>(i), .value = static_cast<double>(i)});
    }
    require(buffer.append(0, request), "historyLimit 场景样本应追加成功");

    auto snapshot = buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(snapshot.channels.size() == 1, "裁剪后应保留通道");
    require(snapshot.channels.front().totalSamples == 5, "historyLimit 应只保留尾部 5 个样本");
    require(snapshot.channels.front().sampleIndexOffset == 5, "裁剪后应记录前 5 个样本已丢弃");

    protoscope::plot::applySampleFrequencyVisibleRange(snapshot, 0.5, 0.9, 10.0);
    const auto displayData = protoscope::plot::buildDisplayData(snapshot, 10.0);
    require(displayData.channels.size() == 1, "采样频率显示数据应保留通道");
    require(displayData.channels.front().samples.size() == 5, "采样频率窗口应使用全局样本序号筛选");
    require(std::abs(displayData.channels.front().samples.front().time - 0.5) < 1e-12,
            "裁剪后采样频率时间轴起点不应归零");
    require(std::abs(displayData.channels.front().samples.back().time - 0.9) < 1e-12,
            "裁剪后采样频率时间轴终点应保持全局样本时间");
}

void test_plot_limited_envelope_preserves_spikes()
{
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

void test_wave_layout_solver_clamps_without_overflow()
{
    const auto layout = protoscope::plot::solveWaveLayout(
        900.0F, 420.0F, 240.0F, 300.0F, 34.0F, false, 6.0F, 6.0F, 72.0F, 160.0F, 220.0F, 520.0F, 58.0F);
    require(layout.toolsWidth >= 220.0F && layout.toolsWidth <= 520.0F, "工具栏宽度应被夹取到允许范围");
    require(layout.overviewHeight >= 72.0F, "概览高度不应低于最小值");
    require(layout.mainHeight >= 160.0F, "主视图高度不应低于最小值");
    require(layout.overviewHeight + layout.mainHeight + 6.0F + 58.0F <= 420.0F + 1e-3F, "布局总高度不应溢出父窗口");

    const auto draggedOverview = protoscope::plot::solveWaveLayout(
        900.0F, 600.0F, 260.0F, 300.0F, 34.0F, false, 6.0F, 6.0F, 72.0F, 160.0F, 220.0F, 520.0F, 70.0F);
    require(std::abs(draggedOverview.overviewHeight - 260.0F) < 1e-3F, "概览高度应尊重用户拖拽值，不应被 35% 上限拉回");

    const auto splitterReserved = protoscope::plot::solveWaveLayout(
        230.0F, 420.0F, 120.0F, 280.0F, 34.0F, false, 6.0F, 6.0F, 72.0F, 160.0F, 220.0F, 520.0F, 58.0F);
    require(splitterReserved.toolsWidth <= 224.0F, "工具栏宽度应为内容区保留 splitter 空间");

    const auto compact = protoscope::plot::solveWaveLayout(
        320.0F, 120.0F, 160.0F, 300.0F, 34.0F, true, 6.0F, 6.0F, 72.0F, 160.0F, 220.0F, 520.0F, 40.0F);
    require(compact.toolsWidth == 34.0F, "折叠工具栏应使用折叠宽度");
    require(compact.overviewHeight + compact.mainHeight + 6.0F + 40.0F <= 120.0F + 1e-3F, "紧凑布局也不应产生纵向溢出");

    const auto drawerOverlayMode = protoscope::plot::solveWaveLayout(
        900.0F, 420.0F, 120.0F, 360.0F, 38.0F, true, 6.0F, 6.0F, 72.0F, 160.0F, 220.0F, 520.0F, 58.0F);
    require(drawerOverlayMode.toolsWidth == 38.0F, "右侧抽屉打开时布局仍应只预留窄边栏宽度");

    constexpr float splitAvailableHeight = 420.0F;
    constexpr float splitRowSpacing = 6.0F;
    constexpr float splitPreferredMinHeight = 120.0F;
    constexpr std::size_t splitMaxRowsWithoutScroll = 4U;
    for (std::size_t rowCount = 2; rowCount <= splitMaxRowsWithoutScroll; ++rowCount) {
        const float rowHeight = protoscope::plot::solveSplitWavePlotHeight(
            rowCount, splitAvailableHeight, splitRowSpacing, splitPreferredMinHeight, splitMaxRowsWithoutScroll);
        const float totalHeight = rowHeight * static_cast<float>(rowCount) +
                                  splitRowSpacing * static_cast<float>(rowCount - 1U);
        require(totalHeight <= splitAvailableHeight + 1e-3F, "4 行以内分屏图不应撑出纵向滚动条");
    }

    const std::size_t overflowingRowCount = splitMaxRowsWithoutScroll + 1U;
    const float overflowingRowHeight = protoscope::plot::solveSplitWavePlotHeight(overflowingRowCount,
                                                                                  splitAvailableHeight,
                                                                                  splitRowSpacing,
                                                                                  splitPreferredMinHeight,
                                                                                  splitMaxRowsWithoutScroll);
    const float overflowingTotalHeight =
        overflowingRowHeight * static_cast<float>(overflowingRowCount) +
        splitRowSpacing * static_cast<float>(overflowingRowCount - 1U);
    require(overflowingTotalHeight > splitAvailableHeight, "超过 4 行分屏图应允许内容溢出并交给滚动条处理");
}

void test_plot_low_density_envelope_keeps_single_value_line()
{
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

    require(buffer.append(0,
                          protoscope::plot::WaveAppendRequest{
                              .source = "low-density",
                              .samples =
                                  {
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

void test_plot_cursor_snap_and_delta()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V"});
    buffer.append(0,
                  protoscope::plot::WaveAppendRequest{
                      .source = "test",
                      .samples =
                          {
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

void test_plot_cursor_snap_by_time_and_measurement()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V"});
    buffer.append(0,
                  protoscope::plot::WaveAppendRequest{
                      .source = "test",
                      .samples =
                          {
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

void test_plot_measurement_dispersion_metrics()
{
    const std::vector<double> times{0.0, 1.0, 2.0, 3.0};
    const std::vector<double> values{1.0, 2.0, 3.0, 4.0};
    const auto measurement = protoscope::plot::makeMeasurementReadout(0, times, values);

    require(measurement.valid, "离散度测量应有效");
    require(std::abs(measurement.meanValue - 2.5) < 1e-9, "均值错误");
    require(std::abs(measurement.variance - 1.25) < 1e-9, "总体方差错误");
    require(std::abs(measurement.stddev - std::sqrt(1.25)) < 1e-9, "标准差错误");
    require(measurement.cv.has_value() && std::abs(*measurement.cv - std::sqrt(1.25) / 2.5) < 1e-9, "CV 错误");
    require(std::abs(measurement.mad - 1.0) < 1e-9, "平均绝对偏差错误");
    require(std::abs(measurement.medianValue - 2.0) < 1e-9, "最近秩中位数错误");
    require(std::abs(measurement.medianAbsDev - 1.0) < 1e-9, "中位数绝对偏差错误");
    require(std::abs(measurement.iqr - 2.0) < 1e-9, "四分位距错误");
    require(std::abs(measurement.p95Spread - 3.0) < 1e-9, "P95-P5 错误");
    require(std::abs(measurement.p95Value - 4.0) < 1e-9, "P95 最近秩错误");
    require(std::abs(measurement.p99Value - 4.0) < 1e-9, "P99 最近秩错误");

    const std::vector<double> zeroValue{0.0};
    const std::vector<double> zeroTime{0.0};
    const auto single = protoscope::plot::makeMeasurementReadout(0, zeroTime, zeroValue);
    require(single.valid, "单样本测量应有效");
    require(single.variance == 0.0 && single.stddev == 0.0, "单样本方差和标准差应为 0");
    require(!single.cv.has_value(), "mean 为 0 时 CV 应无效");
}

void test_plot_measurement_error_metrics()
{
    const std::vector<double> times{0.0, 1.0, 2.0, 3.0};
    const std::vector<double> values{1.0, 2.0, 3.0, 4.0};
    const std::vector<double> reference{0.0, 2.0, 4.0, 4.0};
    const auto measurement = protoscope::plot::makeMeasurementReadout(0, times, values, &reference);

    require(measurement.absoluteError.has_value() && std::abs(*measurement.absoluteError) < 1e-9, "绝对误差错误");
    require(measurement.relativeErrorPercent.has_value() && std::abs(*measurement.relativeErrorPercent) < 1e-9,
            "相对误差错误");
    require(measurement.meanError.has_value() && std::abs(*measurement.meanError) < 1e-9, "平均误差错误");
    require(measurement.mse.has_value() && std::abs(*measurement.mse - 0.5) < 1e-9, "MSE 错误");
    require(measurement.rmse.has_value() && std::abs(*measurement.rmse - std::sqrt(0.5)) < 1e-9, "RMSE 错误");
    require(measurement.mae.has_value() && std::abs(*measurement.mae - 0.5) < 1e-9, "MAE 错误");
    require(measurement.maxAbsError.has_value() && std::abs(*measurement.maxAbsError - 1.0) < 1e-9, "最大绝对误差错误");
    require(measurement.bias.has_value() && std::abs(*measurement.bias) < 1e-9, "bias 应等于平均误差");
}

void test_wave_cursor_smart_snap_edge()
{
    const auto displayData = makeDisplayData(
        {
            {.time = 0.0, .value = 0.0},
            {.time = 1.0, .value = 0.0},
            {.time = 1.1, .value = -10.0},
            {.time = 2.0, .value = -10.0},
        },
        {0.0, 0.0, 5.0, 5.0});

    const auto edge = protoscope::plot::findStrongestEdgeNearTime(displayData, 0, 1.02, 0.08);
    require(edge.has_value(), "智能吸附应找到附近最大跳变边沿");
    require(edge->sampleIndex == 2, "边沿吸附应记录跳变右侧样本索引");
    require(std::abs(edge->time - 1.05) < 1e-9, "边沿吸附应落到跳变中点");
    require(std::abs(edge->value - 5.0) < 1e-9, "边沿吸附文本值应取右侧样本真实值");
    require(std::abs(edge->displayValue + 5.0) < 1e-9, "边沿吸附锚点应取显示几何中点");

    const auto farEdge = protoscope::plot::findStrongestEdgeNearTime(displayData, 0, 0.5, 0.1);
    require(!farEdge.has_value(), "窗口外边沿不应被吸附");
}

void test_wave_cursor_smart_snap_extreme()
{
    const auto displayData = makeDisplayData(
        {
            {.time = 0.0, .value = 0.0},
            {.time = 1.0, .value = -4.0},
            {.time = 2.0, .value = 0.0},
            {.time = 3.0, .value = 6.0},
            {.time = 4.0, .value = 0.0},
            {.time = 5.0, .value = -2.0},
            {.time = 6.0, .value = 0.0},
        },
        {0.0, 2.0, 0.0, -3.0, 0.0, 1.0, 0.0});

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

void test_wave_cursor_nearest_waveform_extreme_policy_snaps_to_local_trough()
{
    const auto displayData = makeDisplayData(
        {
            {.time = 0.0, .value = 80.0},
            {.time = 1.0, .value = 70.0},
            {.time = 2.0, .value = 80.0},
        },
        {80.0, 70.0, 80.0});
    protoscope::plot::WaveViewState view;
    view.cursorExtremeSnapPolicy = protoscope::plot::WaveCursorExtremeSnapPolicy::NearestWaveform;
    const ImPlotRect limits(0.0, 2.0, 0.0, 100.0);

    const auto snap = protoscope::ui::findSmartCursorSnapByScope(displayData, view, 1.0, 70.5, limits, 0.4);
    require(snap.has_value(), "默认极值策略应在鼠标靠近波形谷值时吸附");
    require(snap->label == "Trough", "默认极值策略应返回局部谷值");
    require(snap->readout.sampleIndex == 1, "默认极值策略应命中靠近鼠标的谷值样本");
}

void test_wave_cursor_viewport_zone_extreme_policy_keeps_bottom_zone_behavior()
{
    const auto displayData = makeDisplayData(
        {
            {.time = 0.0, .value = 80.0},
            {.time = 1.0, .value = 70.0},
            {.time = 2.0, .value = 80.0},
        },
        {80.0, 70.0, 80.0});
    protoscope::plot::WaveViewState view;
    view.cursorExtremeSnapPolicy = protoscope::plot::WaveCursorExtremeSnapPolicy::ViewportZone;
    const ImPlotRect limits(0.0, 2.0, 0.0, 100.0);

    const auto awayFromBottom = protoscope::ui::findSmartCursorSnapByScope(displayData, view, 1.0, 70.5, limits, 0.4);
    require(!awayFromBottom.has_value(), "旧极值策略不应在主图底部区域外触发谷值吸附");

    const auto bottomZone = protoscope::ui::findSmartCursorSnapByScope(displayData, view, 1.0, 10.0, limits, 0.4);
    require(bottomZone.has_value(), "旧极值策略应保留底部区域触发谷值吸附");
    require(bottomZone->label == "Trough", "旧极值策略底部区域应返回谷值");
}

void test_wave_cursor_extreme_snap_falls_back_to_window_peak_with_transforms()
{
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

void test_wave_cursor_extreme_snap_falls_back_to_window_trough()
{
    const auto displayData = makeDisplayData(
        {
            {.time = 0.0, .value = 2.0},
            {.time = 1.0, .value = -3.0},
            {.time = 2.0, .value = -3.0},
            {.time = 3.0, .value = -1.0},
        },
        {2.0, -30.0, -30.0, -1.0});

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

void test_wave_cursor_smart_snap_fallback_to_nearest()
{
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

void test_wave_cursor_drag_time_uses_smart_snap()
{
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

void test_split_cursor_drag_id_namespace_is_unique()
{
    std::vector<int> ids;
    for (std::size_t channelIndex = 0; channelIndex < 16; ++channelIndex) {
        for (std::size_t cursorIndex = 0; cursorIndex < 2; ++cursorIndex) {
            const int id = protoscope::ui::splitCursorDragId(channelIndex, cursorIndex);
            require(id != static_cast<int>(100 + cursorIndex), "分屏游标 ID 不应复用 overlay 命名空间");
            require(std::find(ids.begin(), ids.end(), id) == ids.end(), "分屏多通道游标 ID 不应互相冲突");
            ids.push_back(id);
        }
    }
    require(protoscope::ui::splitCursorDragId(0, 0) == 300, "分屏游标 ID 起点应保持稳定");
    require(protoscope::ui::splitCursorDragId(1, 1) == 309, "分屏游标 ID 应按通道预留固定步长");
}

void test_split_cursor_snap_forced_channel_ignores_other_rows()
{
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({.label = "CH1", .unit = "V"});
    snapshot.channels.push_back({.label = "CH2", .unit = "V"});

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 0.0}},
        .actualValues = {0.0},
    });
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 100.0}},
        .actualValues = {100.0},
    });

    protoscope::plot::WaveViewState view;
    view.cursorSnapScope = protoscope::plot::WaveCursorSnapScope::AllChannels;
    const protoscope::ui::BitLaneLayout bitLayout;

    const auto unscoped =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, bitLayout, 1.0, 100.0, 0.2, 200.0);
    require(unscoped.has_value() && unscoped->channelIndex == 1, "未限制时应命中离鼠标 Y 最近的其它行");

    const auto scoped = protoscope::ui::findNearestCursorByScope(
        snapshot, displayData, view, bitLayout, 1.0, 100.0, 0.2, 200.0, false, 0);
    require(scoped.has_value() && scoped->channelIndex == 0, "分屏强制通道后不应跨行吸附");
}

void test_split_cursor_smart_snap_forced_channel_ignores_hidden_row_candidate()
{
    protoscope::plot::WaveDockState wave;
    wave.hiddenChannelLabels.push_back("CH2");

    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({.label = "CH1", .unit = "V"});
    snapshot.channels.push_back({.label = "CH2", .unit = "V"});

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples =
            {
                {.time = 0.0, .value = 0.0},
                {.time = 1.0, .value = 0.0},
                {.time = 1.1, .value = 10.0},
            },
        .actualValues = {0.0, 0.0, 10.0},
    });
    displayData.channels.push_back({
        .samples =
            {
                {.time = 0.0, .value = 0.0},
                {.time = 1.0, .value = 0.0},
                {.time = 1.1, .value = 100.0},
            },
        .actualValues = {0.0, 0.0, 100.0},
    });

    const auto visibleChannels = protoscope::ui::channelIndicesForDerivedViews(wave, snapshot);
    require(visibleChannels.size() == 1 && visibleChannels.front() == 0, "隐藏行不应进入分屏候选列表");

    protoscope::plot::WaveViewState view;
    view.cursorSnapScope = protoscope::plot::WaveCursorSnapScope::AllChannels;
    const ImPlotRect limits(0.0, 2.0, -10.0, 120.0);
    const protoscope::ui::BitLaneLayout bitLayout;

    const auto unscoped =
        protoscope::ui::findSmartCursorSnapByScope(snapshot, displayData, view, bitLayout, 1.02, 100.0, limits, 0.2);
    require(unscoped.has_value() && unscoped->readout.channelIndex == 1, "未限制时隐藏行的强边沿会被选中");

    const auto scoped = protoscope::ui::findSmartCursorSnapByScope(
        snapshot, displayData, view, bitLayout, 1.02, 100.0, limits, 0.2, visibleChannels.front());
    require(scoped.has_value() && scoped->readout.channelIndex == 0, "分屏强制当前可见行后不应选择隐藏行");
}

void test_plot_channel_scale_and_offset_apply_to_display_only()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .scale = -2.0, .offset = 1.5});
    buffer.append(0,
                  protoscope::plot::WaveAppendRequest{
                      .source = "test",
                      .samples =
                          {
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
    zeroScaleBuffer.append(0,
                           protoscope::plot::WaveAppendRequest{
                               .source = "test",
                               .samples =
                                   {
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

void test_plot_channel_bit_display_reaches_snapshot()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(
        0,
        {.label = "CH1", .unit = "raw", .bitDisplay = {.enabled = true, .firstBit = 2, .bitCount = 4, .yOffset = 1.5}});
    buffer.append(0,
                  protoscope::plot::WaveAppendRequest{
                      .source = "bit",
                      .samples = {{.time = 0.0, .value = 0.0}, {.time = 1.0, .value = 15.0}},
                  });

    const auto spec = buffer.channelSpec(0);
    require(spec.has_value(), "buffer 应保存 bit_display 通道配置");
    require(spec->bitDisplay.enabled, "ChannelSpec 应保留 bit_display.enabled");
    require(spec->bitDisplay.firstBit == 2, "ChannelSpec 应保留 bit_display.firstBit");
    require(spec->bitDisplay.bitCount == 4, "ChannelSpec 应保留 bit_display.bitCount");
    require(std::abs(spec->bitDisplay.yOffset - 1.5) < 1e-12, "ChannelSpec 应保留 bit_display.yOffset");

    const auto snapshot = buffer.snapshot(0.0, 1.0);
    require(snapshot.channels.size() == 1, "snapshot 应包含 bit_display 通道");
    require(snapshot.channels[0].bitDisplay.enabled, "snapshot 应保留 bit_display.enabled");
    require(snapshot.channels[0].bitDisplay.firstBit == 2, "snapshot 应保留 bit_display.firstBit");
    require(snapshot.channels[0].bitDisplay.bitCount == 4, "snapshot 应保留 bit_display.bitCount");
    require(std::abs(snapshot.channels[0].bitDisplay.yOffset - 1.5) < 1e-12, "snapshot 应保留 bit_display.yOffset");

    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.buffer.setChannelSpec(
        0,
        {.label = "CH1", .unit = "raw", .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 2, .yOffset = 3.0}});
    require(protoscope::ui::resetChannelBitYOffsetToZero(wave, 0), "双击 bit CH 应可将 y_offset 复位为 0");
    const auto resetSpec = wave.buffer.channelSpec(0);
    require(resetSpec.has_value(), "复位后 bit_display 通道配置应存在");
    require(std::abs(resetSpec->bitDisplay.yOffset) < 1e-12, "bit_display y_offset 应复位为 0");
}

void test_wave_bit_display_bounds_and_hidden_policy()
{
    protoscope::plot::WaveDockState wave;
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 2, .yOffset = 2.0},
    });

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 0.0, .value = 1000.0}, {.time = 1.0, .value = 2000.0}},
        .actualValues = {1000.0, 2000.0},
    });

    const auto bounds = protoscope::ui::boundsForDerivedViews(wave, snapshot, displayData, {0});
    require(bounds.valid, "bit_display bounds 应有效");
    const double expectedMin = 2.0 * protoscope::ui::bitDisplayLanePitch();
    require(std::abs(bounds.minValue - expectedMin) < 1e-12, "bit_display bounds 不应使用原始模拟最小值");
    const double expectedMax = 3.0 * protoscope::ui::bitDisplayLanePitch() + protoscope::ui::bitDisplayLaneHeight();
    require(std::abs(bounds.maxValue - expectedMax) < 1e-12, "bit_display bounds 应覆盖所有 bit lane");

    wave.hiddenChannelLabels.push_back("CH1");
    const auto visibleIndices = protoscope::ui::channelIndicesForDerivedViews(wave, snapshot);
    require(visibleIndices.empty(), "父通道隐藏后整组 bit 不应参与 derived views");
    const auto hiddenBounds = protoscope::ui::boundsForDerivedViews(wave, snapshot, displayData, visibleIndices);
    require(!hiddenBounds.valid, "隐藏 bit 父通道后 derived bounds 应为空");

    protoscope::plot::WaveDockState mixedWave;
    protoscope::plot::WaveSnapshot mixedSnapshot;
    mixedSnapshot.channels.push_back({
        .label = "BIT",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 2, .yOffset = 0.0},
    });
    mixedSnapshot.channels.push_back({
        .label = "CH2",
        .unit = "V",
    });
    protoscope::plot::WaveDisplayData mixedDisplayData;
    mixedDisplayData.channels.push_back({
        .samples = {{.time = 0.0, .value = 1000.0}, {.time = 1.0, .value = 2000.0}},
        .actualValues = {1000.0, 2000.0},
    });
    mixedDisplayData.channels.push_back({
        .samples = {{.time = 0.0, .value = -3.0}, {.time = 1.0, .value = 4.0}},
        .actualValues = {-3.0, 4.0},
    });
    const auto yAutoFitBounds =
        protoscope::ui::boundsForYAxisAutoFit(mixedWave, mixedSnapshot, mixedDisplayData, {0, 1});
    require(yAutoFitBounds.valid, "普通 Y fit bounds 应在存在普通通道时有效");
    require(std::abs(yAutoFitBounds.minValue + 3.0) < 1e-12, "普通 Y fit 不应受 bit 原始值影响");
    require(std::abs(yAutoFitBounds.maxValue - 4.0) < 1e-12, "普通 Y fit 不应受 bit 原始值影响");
}

void test_bit_cursor_only_snaps_to_transitions()
{
    std::vector<protoscope::plot::WaveSample> samples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 1.0},
        {.time = 2.0, .value = 1.0},
        {.time = 3.0, .value = 0.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = 4,
        .visibleBegin = 0,
        .visibleEnd = 4,
        .samples = samples.data(),
    });

    const ImPlotRect limits(0.0, 4.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits, plotPos, plotSize);
    require(!layout.lanes.empty(), "应至少生成一条 bit lane");

    const auto snapAtTransition = protoscope::ui::findNearestBitTransition(snapshot, layout, 1.05, 0.5, 0.2, 10.0);
    require(snapAtTransition.has_value(), "鼠标靠近跳变点时应有吸附");
    require(std::abs(snapAtTransition->time - 1.0) < 0.01, "应吸附到跳变时间而不是稳定电平采样点");

    const auto snapAtSteadyState = protoscope::ui::findNearestBitTransition(snapshot, layout, 2.0, 0.5, 0.1, 10.0);
    require(!snapAtSteadyState.has_value(), "稳定电平采样点不应被吸附");
}

void test_hidden_bit_lane_excluded_from_layout_hit_and_snap()
{
    std::vector<protoscope::plot::WaveSample> samples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 1.0},
        {.time = 2.0, .value = 0.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });

    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto hiddenLayout = protoscope::ui::buildBitLaneLayout(snapshot, {}, limits, plotPos, plotSize);
    require(hiddenLayout.lanes.empty(), "隐藏 bit_display 通道不应生成 lane");
    require(!protoscope::ui::findBitLaneAtPlotValue(hiddenLayout, 0.5, 10.0).has_value(),
            "隐藏 bit_display 通道不应参与 hit test");
    require(!protoscope::ui::findNearestBitTransition(snapshot, hiddenLayout, 1.0, 0.5, 0.5, 10.0).has_value(),
            "隐藏 bit_display 通道不应参与 bit snap");

    const auto visibleLayout = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits, plotPos, plotSize);
    require(visibleLayout.lanes.size() == 1, "可见 bit_display 通道应生成 lane");
    require(
        protoscope::ui::findNearestBitTransition(snapshot, visibleLayout, 1.0, visibleLayout.lanes[0].highY, 0.5, 10.0)
            .has_value(),
        "可见 bit_display 通道应允许吸附跳变");
}

void test_split_bit_cursor_forced_channel_still_snaps_to_transition()
{
    std::vector<protoscope::plot::WaveSample> bitSamples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 1.0},
        {.time = 2.0, .value = 1.0},
    };

    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({.label = "ANALOG", .unit = "V"});
    snapshot.channels.push_back({
        .label = "BIT",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = bitSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = bitSamples.size(),
        .samples = bitSamples.data(),
    });

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 100.0}},
        .actualValues = {100.0},
    });
    displayData.channels.push_back({});

    protoscope::plot::WaveViewState view;
    view.cursorSnapScope = protoscope::plot::WaveCursorSnapScope::AllChannels;
    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto bitLayout = protoscope::ui::buildBitLaneLayout(snapshot, {1}, limits, plotPos, plotSize);

    const auto snap = protoscope::ui::findNearestCursorByScope(
        snapshot, displayData, view, bitLayout, 1.05, bitLayout.lanes.front().highY, 0.2, 10.0, false, 1);
    require(snap.has_value() && snap->bit.has_value(), "分屏 bit 子图应继续使用 bit transition 吸附");
    require(snap->channelIndex == 1 && std::abs(snap->time - 1.0) < 0.01, "bit 子图不应吸附到其它模拟行");
}

void test_bit_measurement_cross_lane_still_outputs_dt_f()
{
    std::vector<protoscope::plot::WaveSample> samples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 1.0},
        {.time = 2.0, .value = 3.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 2, .yOffset = 0.0},
        .totalSamples = 3,
        .visibleBegin = 0,
        .visibleEnd = 3,
        .samples = samples.data(),
    });

    protoscope::plot::CursorReadout left{
        .valid = true,
        .channelIndex = 0,
        .sampleIndex = 1,
        .time = 1.0,
        .value = 1.0,
        .displayValue = 0.5,
        .bit =
            protoscope::plot::BitLaneReadout{
                .parentChannelIndex = 0,
                .bitIndex = 0,
                .laneIndex = 0,
                .value = true,
                .y = 0.5,
            },
    };
    protoscope::plot::CursorReadout right{
        .valid = true,
        .channelIndex = 0,
        .sampleIndex = 2,
        .time = 2.0,
        .value = 1.0,
        .displayValue = 1.5,
        .bit =
            protoscope::plot::BitLaneReadout{
                .parentChannelIndex = 0,
                .bitIndex = 1,
                .laneIndex = 1,
                .value = true,
                .y = 1.5,
            },
    };

    require(protoscope::ui::cursorPairUsesBitLanes({{left, right}}), "两个游标都带 bit 信息时应返回 true");
    require(left.bit->bitIndex != right.bit->bitIndex, "应允许 A/B 游标分别命中不同 bit lane");

    const auto measurement = protoscope::ui::makeBitIntervalMeasurement(left, right);
    require(measurement.valid, "bit 模式跨 lane 测量应有效");
    require(std::abs(measurement.duration - 1.0) < 0.01, "时间差应为 1.0 s");
}

void test_bit_cursor_cross_lane_refresh_uses_own_y_anchor()
{
    std::vector<protoscope::plot::WaveSample> samples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 1.0},
        {.time = 2.0, .value = 3.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 2, .yOffset = 0.0},
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({});
    protoscope::plot::WaveViewState view;
    view.activeBitLane = {.active = true, .parentChannelIndex = 0, .bitIndex = 0, .laneIndex = 0};

    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits, plotPos, plotSize);
    require(layout.lanes.size() == 2, "两条 bit lane 应同时存在");

    const auto bit0 = protoscope::ui::findNearestCursorByScope(
        snapshot, displayData, view, layout, 1.0, layout.lanes[0].highY, 0.2, 10.0);
    const auto bit1 = protoscope::ui::findNearestCursorByScope(
        snapshot, displayData, view, layout, 2.0, layout.lanes[1].highY, 0.2, 10.0);
    require(bit0.has_value() && bit0->bit.has_value(), "bit0 游标刷新应保留 bit readout");
    require(bit1.has_value() && bit1->bit.has_value(), "bit1 游标刷新应保留 bit readout");
    require(bit0->bit->laneIndex == 0 && bit1->bit->laneIndex == 1, "A/B 游标使用各自 Y 锚点时应停留在不同 bit lane");
    require(protoscope::ui::cursorPairUsesBitLanes({{bit0, bit1}}),
            "跨 lane 刷新后应继续触发 bit interval measurement");
    const auto measurement = protoscope::ui::makeBitIntervalMeasurement(*bit0, *bit1);
    require(measurement.valid && std::abs(measurement.duration - 1.0) < 0.01,
            "跨 lane bit interval 应继续输出 dt/T/f 所需 duration");
}

void test_bit_active_switches_measurement_mode()
{
    protoscope::plot::WaveViewState view;
    view.activeBitLane = {.active = true, .parentChannelIndex = 0, .bitIndex = 0, .laneIndex = 0};
    require(protoscope::ui::bitLaneMeasurementActive(view), "激活 bit lane 后应为 bit 测量模式");

    view.activeBitLane = {};
    require(!protoscope::ui::bitLaneMeasurementActive(view), "清空 bit lane 后应回到普通测量模式");

    protoscope::plot::CursorReadout left{.valid = true, .channelIndex = 0, .time = 0.0, .value = 1.0};
    protoscope::plot::CursorReadout right{.valid = true, .channelIndex = 0, .time = 1.0, .value = 2.0};
    require(!protoscope::ui::cursorPairUsesBitLanes({{left, right}}), "普通波形游标不应触发 bit 测量判断");
}

void test_invisible_active_bit_lane_falls_back_to_waveform_cursor()
{
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
    });
    snapshot.channels.push_back({
        .label = "CH2",
    });
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 0.0, .value = 10.0}, {.time = 1.0, .value = 20.0}},
        .actualValues = {10.0, 20.0},
    });
    displayData.channels.push_back({
        .samples = {{.time = 0.0, .value = -1.0}, {.time = 1.0, .value = 0.0}},
        .actualValues = {-1.0, 0.0},
    });
    protoscope::plot::WaveViewState view;
    view.activeBitLane = {.active = true, .parentChannelIndex = 0, .bitIndex = 0, .laneIndex = 0};

    const protoscope::ui::BitLaneLayout emptyLayout;
    require(!protoscope::ui::activeBitLaneVisible(view, emptyLayout), "active bit lane 不在当前 layout 中时应判定失效");
    const auto fallback =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, emptyLayout, 1.0, 0.0, 0.2, 10.0);
    require(fallback.has_value(), "active bit lane 失效后应允许普通波形游标刷新");
    require(!fallback->bit.has_value(), "回退普通路径后不应带 bit readout");
    require(fallback->channelIndex == 1, "bit display 通道不应以 raw waveform 作为回退候选");
    require(std::abs(fallback->displayValue) < 1e-9, "普通波形刷新应返回显示值");
}

void test_active_channel_cursor_time_fallback_rebinds_after_channel_switch()
{
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({.label = "OLD"});
    snapshot.channels.push_back({.label = "ACTIVE"});

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 0.0}},
        .actualValues = {0.0},
    });
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 100.0}},
        .actualValues = {100.0},
    });

    protoscope::plot::WaveViewState view;
    view.cursorSnapScope = protoscope::plot::WaveCursorSnapScope::ActiveChannel;
    view.measurementChannelIndex = 1;

    const protoscope::ui::BitLaneLayout emptyLayout;
    const auto withoutFallback =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, emptyLayout, 1.0, 0.0, 0.2, 0.2);
    require(!withoutFallback.has_value(), "旧通道 Y 锚点离新激活通道过远时，普通 Y+time 吸附应失败");

    const auto withFallback =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, emptyLayout, 1.0, 0.0, 0.2, 0.2, true);
    require(withFallback.has_value(), "允许时间兜底后应按原时间重绑定新激活通道");
    require(withFallback->channelIndex == 1, "时间兜底应命中新激活通道");
    require(std::abs(withFallback->value - 100.0) < 1e-9, "时间兜底应返回新激活通道读数");
}

void test_active_channel_cursor_time_fallback_restores_pair_measurement()
{
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({.label = "OLD"});
    snapshot.channels.push_back({.label = "ACTIVE"});

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 0.0}, {.time = 2.0, .value = 0.0}, {.time = 3.0, .value = 0.0}},
        .actualValues = {0.0, 0.0, 0.0},
    });
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 100.0}, {.time = 2.0, .value = 110.0}, {.time = 3.0, .value = 120.0}},
        .actualValues = {100.0, 110.0, 120.0},
    });

    protoscope::plot::WaveViewState view;
    view.cursorSnapScope = protoscope::plot::WaveCursorSnapScope::ActiveChannel;
    view.measurementChannelIndex = 1;

    const protoscope::ui::BitLaneLayout emptyLayout;
    const auto left =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, emptyLayout, 1.0, 0.0, 0.2, 0.2, true);
    const auto right =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, emptyLayout, 2.0, 0.0, 0.2, 0.2, true);

    require(left.has_value() && right.has_value(), "A/B 游标都应按时间兜底重绑定到新激活通道");
    require(left->channelIndex == 1 && right->channelIndex == 1, "A/B readout 都应属于新激活通道");

    const auto measurement = protoscope::ui::measureDisplayWindow(displayData, 1, left->time, right->time);
    require(measurement.valid, "A/B 重绑定后测量窗口应恢复有效统计");
    require(measurement.sampleCount == 2, "测量窗口应覆盖新激活通道的 A/B 区间样本");
    require(std::abs(measurement.peakToPeak - 10.0) < 1e-9, "测量统计应基于新激活通道数据");
}

void test_active_bit_lane_cursor_can_return_nearby_waveform()
{
    std::vector<protoscope::plot::WaveSample> bitSamples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 1.0},
        {.time = 2.0, .value = 0.0},
    };
    std::vector<protoscope::plot::WaveSample> waveSamples{
        {.time = 1.0, .value = 5.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "BITS",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = bitSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = bitSamples.size(),
        .samples = bitSamples.data(),
    });
    snapshot.channels.push_back({
        .label = "WAVE",
        .unit = "V",
        .totalSamples = waveSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = waveSamples.size(),
        .samples = waveSamples.data(),
    });

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = bitSamples,
        .actualValues = {0.0, 1.0, 0.0},
    });
    displayData.channels.push_back({
        .samples = waveSamples,
        .actualValues = {5.0},
    });

    protoscope::plot::WaveViewState view;
    view.activeBitLane = {.active = true, .parentChannelIndex = 0, .bitIndex = 0, .laneIndex = 0};

    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0, 1}, limits, plotPos, plotSize);
    require(protoscope::ui::activeBitLaneVisible(view, layout), "测试前 active bit lane 应可见");

    const auto readout =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, layout, 1.0, 5.0, 0.2, 0.2);
    require(readout.has_value(), "active bit lane 不应屏蔽附近普通波形候选");
    require(readout->channelIndex == 1, "游标应回到离鼠标最近的普通波形通道");
    require(!readout->bit.has_value(), "普通波形候选不应携带 bit readout");
}

void test_bit_display_cursor_excludes_same_channel_raw_waveform()
{
    std::vector<protoscope::plot::WaveSample> samples{
        {.time = 1.0, .value = 15.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "BITS",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 10.05}},
        .actualValues = {15.0},
    });

    protoscope::plot::WaveViewState view;
    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits, plotPos, plotSize);

    const auto readout =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, layout, 1.0, 10.05, 0.2, 0.2);
    require(!readout.has_value(), "bit display 通道不应以原始 waveform 参与普通游标候选");
}

void test_explicit_bit_readout_policy_keeps_waveform_when_bit_not_active()
{
    std::vector<protoscope::plot::WaveSample> bitSamples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 1.0},
    };
    std::vector<protoscope::plot::WaveSample> waveSamples{
        {.time = 1.0, .value = 4.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "BITS",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = bitSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = bitSamples.size(),
        .samples = bitSamples.data(),
    });
    snapshot.channels.push_back({
        .label = "WAVE",
        .totalSamples = waveSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = waveSamples.size(),
        .samples = waveSamples.data(),
    });

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = bitSamples,
        .actualValues = {0.0, 1.0},
    });
    displayData.channels.push_back({
        .samples = waveSamples,
        .actualValues = {4.0},
    });

    protoscope::plot::WaveViewState view;
    view.bitDisplayReadoutPolicy = protoscope::plot::WaveBitDisplayReadoutPolicy::ExplicitActivation;

    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0, 1}, limits, plotPos, plotSize);

    const auto readout =
        protoscope::ui::findNearestCursorByScope(snapshot, displayData, view, layout, 1.0, 4.0, 0.2, 0.2);
    require(readout.has_value(), "显式激活策略未激活 bit lane 时仍应允许普通波形候选");
    require(readout->channelIndex == 1 && !readout->bit.has_value(), "未激活 bit lane 不应抢普通波形游标");
}

void test_bit_layout_independent_of_axis_range()
{
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 2, .yOffset = 0.0},
    });

    const ImPlotRect limits1(0.0, 1.0, -10.0, 10.0);
    const ImPlotRect limits2(0.0, 1.0, -100.0, 100.0);
    const ImVec2 plotPos(40.0F, 30.0F);
    const ImVec2 plotSize(800.0F, 600.0F);

    const auto layout1 = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits1, plotPos, plotSize);
    const auto layout2 = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits2, plotPos, plotSize);

    require(layout1.lanes.size() == layout2.lanes.size(), "变化 Y 轴范围不应改变 bit lane 数量");
    for (std::size_t i = 0; i < layout1.lanes.size(); ++i) {
        require(std::abs(layout1.lanes[i].lowPixelY - layout2.lanes[i].lowPixelY) < 0.01F,
                "变化 Y 轴范围不应改变 bit lane 像素位置");
        require(std::abs(layout1.lanes[i].highPixelY - layout2.lanes[i].highPixelY) < 0.01F,
                "变化 Y 轴范围不应改变 bit lane 像素位置");
    }
}

void test_bit_layout_multiple_channels_with_y_offset()
{
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 2, .yOffset = 0.0},
    });
    snapshot.channels.push_back({
        .label = "CH2",
        .bitDisplay = {.enabled = true, .firstBit = 1, .bitCount = 2, .yOffset = 0.0},
    });

    const ImPlotRect limits(0.0, 1.0, -1.0, 1.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 400.0F);

    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0, 1}, limits, plotPos, plotSize);
    require(layout.lanes.size() == 4, "两个父通道各 2 bit 应生成 4 条 lane");

    require(layout.lanes[0].parentChannelIndex == 0, "第 0 条 lane 应属于 CH1");
    require(layout.lanes[2].parentChannelIndex == 1, "第 2 条 lane 应属于 CH2");
    require(layout.lanes[1].bitIndex == 1 && layout.lanes[2].bitIndex == 1, "两个 CH 应按绝对 bit index 对齐");
    require(std::abs(layout.lanes[1].centerPixelY - layout.lanes[2].centerPixelY) < 0.5F,
            "相同 bit index 的不同 CH lane 应叠加");

    for (std::size_t i = 0; i < 2; ++i) {
        for (std::size_t j = i + 1; j < 2; ++j) {
            if (layout.lanes[i].parentChannelIndex == layout.lanes[j].parentChannelIndex) {
                require(!(std::abs(layout.lanes[i].centerPixelY - layout.lanes[j].centerPixelY) < 0.5F),
                        "同一父通道内不同 bit lane 的 Y 不应重合");
            }
        }
    }

    snapshot.channels[1].bitDisplay.yOffset = 1.0;
    const auto offsetLayout = protoscope::ui::buildBitLaneLayout(snapshot, {0, 1}, limits, plotPos, plotSize);
    require(offsetLayout.lanes.size() == 4, "y_offset 不应改变 lane 数量");
    const double offsetDelta = offsetLayout.lanes[2].centerPixelY - offsetLayout.lanes[1].centerPixelY;
    require(std::abs(offsetDelta - static_cast<double>(offsetLayout.lanes[1].lanePixelPitch)) < 0.5,
            "bit_display y_offset 应保留为叠加 lane 的手动像素偏移");
}

void test_bit_lane_display_label_uses_simplified_bit_text()
{
    require(protoscope::ui::bitLaneDisplayLabel(0) == "bit 0", "bit lane 标签 0 应使用简化文本");
    require(protoscope::ui::bitLaneDisplayLabel(15) == "bit 15", "bit lane 标签 15 应使用简化文本");
}

void test_bit_layout_16_and_8_bit_channels_share_absolute_rows()
{
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH16",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 16, .yOffset = 0.0},
    });
    snapshot.channels.push_back({
        .label = "CH8",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 8, .yOffset = 0.0},
    });

    const ImPlotRect limits(0.0, 1.0, -1.0, 1.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 400.0F);

    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0, 1}, limits, plotPos, plotSize);
    require(layout.lanes.size() == 24, "16-bit CH + 8-bit CH 应保留每个父通道的 lane entry");

    std::vector<bool> rowSeen(16, false);
    std::vector<std::size_t> bitCounts(16, 0);
    for (const auto& lane : layout.lanes) {
        require(lane.bitIndex < 16, "混合 bit display 不应生成超过 16 的 bit index");
        require(lane.rowIndex < 16, "混合 bit display 应只占用 16 个唯一行");
        require(lane.rowIndex == lane.bitIndex, "first_bit=0 时 rowIndex 应与 bitIndex 对齐");
        rowSeen[lane.rowIndex] = true;
        ++bitCounts[lane.bitIndex];
    }
    for (std::size_t bitIndex = 0; bitIndex < 16; ++bitIndex) {
        require(rowSeen[bitIndex], "16-bit CH 应覆盖 bit 0..15 的唯一行");
        require(bitCounts[bitIndex] == (bitIndex < 8 ? 2U : 1U), "重叠 bit 应共享行但保留各自 lane entry");
    }

    for (std::size_t bitIndex = 0; bitIndex < 8; ++bitIndex) {
        const auto first =
            std::ranges::find_if(layout.lanes, [bitIndex](const protoscope::ui::BitLaneLayoutEntry& lane) {
                return lane.parentChannelIndex == 0 && lane.bitIndex == bitIndex;
            });
        const auto second =
            std::ranges::find_if(layout.lanes, [bitIndex](const protoscope::ui::BitLaneLayoutEntry& lane) {
                return lane.parentChannelIndex == 1 && lane.bitIndex == bitIndex;
            });
        require(first != layout.lanes.end() && second != layout.lanes.end(), "bit 0..7 应同时包含两个父通道 lane");
        require(first->rowIndex == second->rowIndex, "bit 0..7 的两个父通道 lane 应共享 rowIndex");
        require(std::abs(first->centerPixelY - second->centerPixelY) < 0.5F, "bit 0..7 的两个父通道 lane 应纵向重叠");
    }
}

void test_plot_snapshot_without_stats_keeps_ranges_and_samples()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .ratio = 2.0, .scale = 3.0, .offset = -1.0});
    buffer.append(0,
                  protoscope::plot::WaveAppendRequest{
                      .source = "test",
                      .samples =
                          {
                              {.time = 0.0, .value = 1.0},
                              {.time = 0.5, .value = 2.0},
                              {.time = 1.0, .value = 3.0},
                          },
                  });

    const auto lightweight = buffer.snapshot(0.0, 1.0, false);
    require(lightweight.channels.size() == 1, "轻量快照应保留通道");
    require(lightweight.channels[0].samples != nullptr, "轻量快照应保留原始样本指针");
    require(lightweight.channels[0].visibleBegin == 0, "轻量快照应保留可视起点");
    require(lightweight.channels[0].visibleEnd == 3, "轻量快照应保留可视终点");
    require(lightweight.channels[0].stats.totalSamples == 3, "轻量快照应保留总样本数");
    require(lightweight.channels[0].stats.visibleSamples == 3, "轻量快照应保留可视样本数");
    require(std::abs(lightweight.channels[0].stats.sampleRateHz - 2.0) < 1e-12, "轻量快照应保留采样率摘要");
    require(lightweight.channels[0].stats.minValue == 0.0 && lightweight.channels[0].stats.maxValue == 0.0,
            "轻量快照不应计算 min/max 统计");

    const auto display = protoscope::plot::buildDisplayData(lightweight, 0.0);
    require(display.channels.size() == 1, "轻量快照仍应可构建显示数据");
    require(std::abs(display.channels[0].samples[1].value - 9.0) < 1e-12, "轻量快照构建显示数据时仍应应用通道变换");

    const auto full = buffer.snapshot(0.0, 1.0);
    require(std::abs(full.channels[0].stats.minValue - 3.0) < 1e-12, "默认快照仍应计算 min 统计");
    require(std::abs(full.channels[0].stats.maxValue - 15.0) < 1e-12, "默认快照仍应计算 max 统计");
}

void test_plot_build_display_data_into_reuses_storage_and_matches_output()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    buffer.configureChannels(1);
    buffer.setChannelSpec(0, {.label = "CH1", .unit = "V", .ratio = 2.0, .scale = 3.0, .offset = -1.0});
    buffer.append(0,
                  protoscope::plot::WaveAppendRequest{
                      .source = "test",
                      .samples =
                          {
                              {.time = 0.0, .value = 1.0},
                              {.time = 0.5, .value = 2.0},
                              {.time = 1.0, .value = 3.0},
                          },
                  });

    const auto snapshot = buffer.snapshot(0.0, 1.0);
    const auto expected = protoscope::plot::buildDisplayData(snapshot, 10.0);
    protoscope::plot::WaveDisplayData reused;
    protoscope::plot::buildDisplayDataInto(snapshot, 10.0, reused);
    require(reused.axisSource == protoscope::plot::WaveTimeAxisSource::SampleFrequency, "采样频率轴应保持一致");
    require(reused.channels.size() == expected.channels.size(), "复用构建应保留通道数量");
    require(reused.channels[0].samples.size() == expected.channels[0].samples.size(), "复用构建应保留样本数量");
    require(std::abs(reused.channels[0].samples[1].time - expected.channels[0].samples[1].time) < 1e-12,
            "复用构建的采样频率时间应一致");
    require(std::abs(reused.channels[0].samples[1].value - expected.channels[0].samples[1].value) < 1e-12,
            "复用构建的显示值应一致");
    require(std::abs(reused.channels[0].actualValues[1] - expected.channels[0].actualValues[1]) < 1e-12,
            "复用构建的真实值应一致");

    const auto capacityBefore = reused.channels[0].samples.capacity();
    protoscope::plot::buildDisplayDataInto(snapshot, 0.0, reused);
    require(reused.channels[0].samples.capacity() >= capacityBefore, "复用构建不应缩掉已分配样本容量");
    require(reused.axisSource == protoscope::plot::WaveTimeAxisSource::ScriptTime, "无采样频率时应继续使用脚本时间轴");
    require(std::abs(reused.channels[0].samples[2].time - 1.0) < 1e-12, "脚本时间轴应保留原始时间");
}

void test_plot_channel_ratio_and_formula_modes()
{
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
    buffer.append(0,
                  protoscope::plot::WaveAppendRequest{
                      .source = "test",
                      .samples =
                          {
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

void test_plot_channel_transform_updates_are_isolated()
{
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

void test_plot_cursor_snap_scope_selection()
{
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

void test_plot_hover_readout_ignores_hidden_channels()
{
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.resize(2);
    displayData.channels[0].samples = {
        {.time = 1.0, .value = 10.0},
    };
    displayData.channels[1].samples = {
        {.time = 1.02, .value = 10.1},
    };

    const std::vector<std::size_t> onlyFirstVisible{0};
    const auto visibleOnly =
        protoscope::plot::findNearestDisplayPointInChannels(displayData, onlyFirstVisible, 1.02, 10.1, 0.1, 0.2);
    require(visibleOnly.has_value(), "悬停吸附应在可见通道内继续命中候选");
    require(visibleOnly->channelIndex == 0, "隐藏通道不应参与悬停吸附评分");

    const std::vector<std::size_t> noVisibleChannels{};
    const auto allHidden =
        protoscope::plot::findNearestDisplayPointInChannels(displayData, noVisibleChannels, 1.02, 10.1, 0.1, 0.2);
    require(!allHidden.has_value(), "所有候选通道隐藏时悬停吸附应为空");
}

void test_bit_hover_readout_tracks_steady_level()
{
    std::vector<protoscope::plot::WaveSample> samples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 1.0},
        {.time = 2.0, .value = 1.0},
        {.time = 3.0, .value = 0.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH2",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = samples,
        .actualValues = {0.0, 1.0, 1.0, 0.0},
    });

    const ImPlotRect limits(0.0, 4.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits, plotPos, plotSize);
    require(!layout.lanes.empty(), "应生成 bit hover lane");

    const auto high = protoscope::ui::findHoverReadout(
        snapshot, displayData, {0}, layout, 2.4, layout.lanes[0].centerY, 0.2, 0.2, true);
    require(high.has_value(), "高电平平台区间应有 bit hover 读数");
    require(high->kind == protoscope::ui::HoverReadoutKind::BitLane, "平台区间 hover 应识别为 bit lane");
    require(high->readout.bit.has_value() && high->readout.bit->value, "高电平平台应读取为 1");
    require(std::abs(high->readout.time - 2.4) < 1e-12, "bit hover annotation 应锚定鼠标当前时间");
    require(std::abs(high->readout.displayValue - layout.lanes[0].highY) < 1e-12, "bit hover 高电平应锚定 highY");

    const auto low = protoscope::ui::findHoverReadout(
        snapshot, displayData, {0}, layout, 0.5, layout.lanes[0].centerY, 0.2, 0.2, true);
    require(low.has_value(), "低电平平台区间应有 bit hover 读数");
    require(low->readout.bit.has_value() && !low->readout.bit->value, "低电平平台应读取为 0");
    require(std::abs(low->readout.displayValue - layout.lanes[0].lowY) < 1e-12, "bit hover 低电平应锚定 lowY");
}

void test_bit_hover_readout_uses_sample_frequency_time_axis()
{
    std::vector<protoscope::plot::WaveSample> sourceSamples{
        {.time = 0.0, .value = 0.0},
        {.time = 1.0, .value = 0.0},
        {.time = 2.0, .value = 4.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 2, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = sourceSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = sourceSamples.size(),
        .samples = sourceSamples.data(),
    });
    protoscope::plot::WaveDisplayData displayData;
    displayData.axisSource = protoscope::plot::WaveTimeAxisSource::SampleFrequency;
    displayData.timeUnit = "s";
    displayData.channels.push_back({
        .samples =
            {
                {.time = 0.0, .value = 0.0},
                {.time = 0.001, .value = 0.0},
                {.time = 0.002, .value = 4.0},
            },
        .actualValues = {0.0, 0.0, 4.0},
    });

    const ImPlotRect limits(0.0, 0.003, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits, plotPos, plotSize);
    require(!layout.lanes.empty(), "采样频率时间轴应生成 bit lane");

    const auto readout = protoscope::ui::findHoverReadout(
        snapshot, displayData, {0}, layout, 0.0024, layout.lanes[0].centerY, 0.001, 0.2, true);
    require(readout.has_value(), "采样频率时间轴平台区间应有 bit hover 读数");
    require(readout->readout.bit.has_value() && readout->readout.bit->value, "应按显示时间轴映射到源样本 bit 值");
    require(readout->readout.sampleIndex == 2, "bit hover 应映射回对应源样本索引");
}

void test_bit_hover_readout_excludes_same_channel_raw_waveform()
{
    std::vector<protoscope::plot::WaveSample> samples{
        {.time = 1.0, .value = 15.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 10.05}},
        .actualValues = {15.0},
    });

    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0}, limits, plotPos, plotSize);

    const auto readout =
        protoscope::ui::findHoverReadout(snapshot, displayData, {0}, layout, 1.0, 10.05, 0.2, 0.2, true);
    require(!readout.has_value(), "bit display 通道不应以原始 waveform 参与 hover 候选");
}

void test_bit_hover_readout_uses_nearest_display_shape_in_mixed_view()
{
    std::vector<protoscope::plot::WaveSample> bitSamples{
        {.time = 1.0, .value = 15.0},
    };
    std::vector<protoscope::plot::WaveSample> waveSamples{
        {.time = 1.0, .value = 0.0},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "BITS",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = bitSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = bitSamples.size(),
        .samples = bitSamples.data(),
    });
    snapshot.channels.push_back({
        .label = "WAVE",
        .unit = "V",
        .totalSamples = waveSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = waveSamples.size(),
        .samples = waveSamples.data(),
    });

    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0, 1}, limits, plotPos, plotSize);
    require(!layout.lanes.empty(), "混合场景应生成 bit lane");

    waveSamples[0].value = layout.lanes[0].centerY;
    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 15.0}},
        .actualValues = {15.0},
    });
    displayData.channels.push_back({
        .samples = waveSamples,
        .actualValues = {waveSamples[0].value},
    });

    const auto preferred = protoscope::ui::findHoverReadout(
        snapshot, displayData, {0, 1}, layout, 1.0, layout.lanes[0].centerY, 0.2, 0.2, true);
    require(preferred.has_value(), "混合场景靠近普通波形时应有 hover");
    require(preferred->kind == protoscope::ui::HoverReadoutKind::Waveform, "靠近普通波形时应命中 waveform");
    require(preferred->readout.channelIndex == 1, "混合场景应命中普通波形通道");

    const auto bitNearest = protoscope::ui::findHoverReadout(
        snapshot, displayData, {0, 1}, layout, 1.0, layout.lanes[0].highY, 0.2, 0.2, true);
    require(bitNearest.has_value(), "混合场景靠近 bit lane 时应有 hover");
    require(bitNearest->kind == protoscope::ui::HoverReadoutKind::BitLane, "靠近 bit lane 时应命中 bit readout");
}

void test_bit_hover_readout_falls_back_to_waveform_outside_lane()
{
    std::vector<protoscope::plot::WaveSample> bitSamples{
        {.time = 1.0, .value = 15.0},
    };
    std::vector<protoscope::plot::WaveSample> waveSamples{
        {.time = 1.0, .value = 10.2},
    };
    protoscope::plot::WaveSnapshot snapshot;
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "raw",
        .bitDisplay = {.enabled = true, .firstBit = 0, .bitCount = 1, .yOffset = 0.0},
        .totalSamples = bitSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = bitSamples.size(),
        .samples = bitSamples.data(),
    });
    snapshot.channels.push_back({
        .label = "CH2",
        .unit = "V",
        .totalSamples = waveSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = waveSamples.size(),
        .samples = waveSamples.data(),
    });

    protoscope::plot::WaveDisplayData displayData;
    displayData.channels.push_back({
        .samples = {{.time = 1.0, .value = 10.05}},
        .actualValues = {15.0},
    });
    displayData.channels.push_back({
        .samples = waveSamples,
        .actualValues = {10.2},
    });

    const ImPlotRect limits(0.0, 2.0, -1.0, 2.0);
    const ImVec2 plotPos(0.0F, 0.0F);
    const ImVec2 plotSize(400.0F, 200.0F);
    const auto layout = protoscope::ui::buildBitLaneLayout(snapshot, {0, 1}, limits, plotPos, plotSize);

    const auto readout =
        protoscope::ui::findHoverReadout(snapshot, displayData, {0, 1}, layout, 1.0, 10.05, 0.2, 1.0, false);
    require(readout.has_value(), "鼠标不在 bit lane 内时应回退普通波形 hover");
    require(readout->kind == protoscope::ui::HoverReadoutKind::Waveform, "回退读数应是普通波形");
    require(readout->readout.channelIndex == 1, "bit-display 通道不应以原始 y 值抢占普通 hover");
}

void test_plot_limited_envelope_edges()
{
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

void test_wave_frequency_parse_and_axis_mapping()
{
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

void test_wave_display_data_uses_visible_window_only()
{
    using protoscope::plot::WaveSample;
    std::vector<WaveSample> samples;
    for (int index = 0; index < 100; ++index) {
        samples.push_back({.time = static_cast<double>(index) * 0.5, .value = static_cast<double>(index)});
    }

    protoscope::plot::WaveSnapshot snapshot{};
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "V",
        .scale = 1.0,
        .offset = 0.0,
        .totalSamples = samples.size(),
        .visibleBegin = 40,
        .visibleEnd = 45,
        .samples = samples.data(),
    });

    const auto mapped = protoscope::plot::buildDisplayData(snapshot, 0.0);
    require(mapped.axisSource == protoscope::plot::WaveTimeAxisSource::ScriptTime, "可视窗口脚本时间应保持有效");
    require(mapped.channels.size() == 1, "显示数据应保留通道");
    require(mapped.channels[0].samples.size() == 5, "显示数据只应包含可视窗口采样");
    require(mapped.channels[0].actualValues.size() == 5, "实际值只应包含可视窗口采样");
    require(std::abs(mapped.channels[0].samples.front().time - 20.0) < 1e-12, "显示数据应从窗口起点开始");
    require(std::abs(mapped.channels[0].samples.back().value - 44.0) < 1e-12, "显示数据应止于窗口终点前");

    const auto frequencyMapped = protoscope::plot::buildDisplayData(snapshot, 10.0);
    require(std::abs(frequencyMapped.channels[0].samples.front().time - 4.0) < 1e-12, "频率时间轴应保留全局样本序号");
}

void test_wave_main_render_data_uses_viewport_window()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.buffer.append(0,
                       protoscope::plot::WaveAppendRequest{
                           .source = "edge",
                           .samples =
                               {
                                   {.time = 0.0, .value = 0.0},
                                   {.time = 10.0, .value = 1.0},
                                   {.time = 20.0, .value = 0.0},
                               },
                       });

    wave.view.initialized = true;
    wave.view.autoFollowLatest = false;
    wave.view.defaultViewportPending = false;
    wave.view.viewMinTime = 9.0;
    wave.view.viewMaxTime = 11.0;
    wave.view.visibleDuration = 2.0;
    wave.view.centerTime = 10.0;

    const auto frame = protoscope::ui::prepareWaveFrame(wave, 800.0F);

    require(frame.displayData != nullptr, "主视图交互数据源不能为空");
    require(frame.renderDisplayData != nullptr, "主视图渲染数据源不能为空");
    require(frame.displayData->channels.front().samples.size() == 3, "主视图数据应保留视口样本及左右邻接样本");
    require(frame.renderDisplayData == frame.displayData, "主图渲染应复用当前视口显示缓存");
    require(frame.renderDisplayData->channels.front().samples.size() == 3, "渲染数据应携带当前视口的邻接样本");
    require(frame.overviewDisplayData != nullptr, "概览数据源不能为空");
    require(frame.overviewDisplayData->channels.front().samples.size() >= 3, "概览仍应保留完整历史的降采样视图");

    std::size_t sourceSampleCount = 0;
    const auto envelope = protoscope::ui::buildDisplayEnvelope(frame.renderDisplayData->channels.front().samples,
                                                               wave.view.viewMinTime,
                                                               wave.view.viewMaxTime,
                                                               32,
                                                               &sourceSampleCount);
    require(sourceSampleCount == 1, "包络统计仍应只统计视口内样本");
    require(envelope.size() == 3, "主图包络应基于当前视口缓存及邻接样本构建");
    require(std::abs(envelope.front().time - 0.0) < 1e-12, "主图包络应保留左侧邻接样本");
    require(std::abs(envelope[1].time - 10.0) < 1e-12, "主图包络应包含当前视口样本");
    require(std::abs(envelope.back().time - 20.0) < 1e-12, "主图包络应保留右侧邻接样本");
}

void test_wave_peak_detect_downsample_orders_bucket_points()
{
    const std::vector<protoscope::plot::WaveSample> samples{
        {.time = 0.0, .value = 0.5},
        {.time = 1.0, .value = 2.0},
        {.time = 2.0, .value = -1.0},
        {.time = 3.0, .value = 0.25},
    };

    std::size_t sourceSampleCount = 0;
    const auto trace = protoscope::ui::buildPeakDetectDownsample(samples, 0.0, 3.0, 4, &sourceSampleCount);

    require(sourceSampleCount == samples.size(), "peak-detect 应统计可视区内原始样本数");
    require(trace.size() == 4, "单桶同时存在首点、极值和末点时应输出 4 个代表点");
    require(std::abs(trace[0].time - 0.0) < 1e-12 && std::abs(trace[0].value - 0.5) < 1e-12,
            "peak-detect 首点应保持真实样本位置");
    require(std::abs(trace[1].time - 1.0) < 1e-12 && std::abs(trace[1].value - 2.0) < 1e-12,
            "peak-detect 极大值应按真实样本顺序插入");
    require(std::abs(trace[2].time - 2.0) < 1e-12 && std::abs(trace[2].value + 1.0) < 1e-12,
            "peak-detect 极小值应按真实样本顺序插入");
    require(std::abs(trace[3].time - 3.0) < 1e-12 && std::abs(trace[3].value - 0.25) < 1e-12,
            "peak-detect 末点应保持真实样本位置");
}

void test_wave_peak_detect_downsample_respects_point_budget()
{
    std::vector<protoscope::plot::WaveSample> samples;
    samples.reserve(100);
    for (std::size_t index = 0; index < 100; ++index) {
        samples.push_back({
            .time = static_cast<double>(index),
            .value = index % 2 == 0 ? -1.0 : 1.0,
        });
    }

    std::size_t sourceSampleCount = 0;
    const auto trace = protoscope::ui::buildPeakDetectDownsample(samples, 0.0, 99.0, 20, &sourceSampleCount);

    require(sourceSampleCount == samples.size(), "peak-detect 预算测试应覆盖完整可视样本");
    require(!trace.empty(), "peak-detect 预算内仍应输出代表点");
    require(trace.size() <= 20, "peak-detect 输出点数不应超过渲染点预算");
    for (std::size_t index = 1; index < trace.size(); ++index) {
        require(trace[index - 1].time <= trace[index].time, "peak-detect 输出应保持原始时间顺序");
    }
}

void test_wave_render_mode_label_reports_each_path()
{
    protoscope::plot::WaveRenderStats stats;
    require(std::string_view(protoscope::ui::waveRenderModeLabel(stats)) == "无", "无渲染统计时模式应为空状态");

    stats.rawChannelCount = 1;
    require(std::string_view(protoscope::ui::waveRenderModeLabel(stats)) == "原始", "raw 通道应显示原始模式");

    stats = {};
    stats.peakDownsampleChannelCount = 1;
    require(std::string_view(protoscope::ui::waveRenderModeLabel(stats)) == "Peak降采样",
            "peak 通道应显示 Peak 降采样模式");

    stats = {};
    stats.envelopeDownsampleChannelCount = 1;
    require(std::string_view(protoscope::ui::waveRenderModeLabel(stats)) == "包络降采样",
            "envelope 通道应显示包络降采样模式");

    stats = {};
    stats.phosphorChannelCount = 1;
    require(std::string_view(protoscope::ui::waveRenderModeLabel(stats)) == "余辉",
            "phosphor 通道应显示余辉模式");

    stats = {};
    stats.bitLaneChannelCount = 1;
    require(std::string_view(protoscope::ui::waveRenderModeLabel(stats)) == "Bit", "bit 通道应显示 Bit 模式");

    stats.phosphorChannelCount = 1;
    require(std::string_view(protoscope::ui::waveRenderModeLabel(stats)) == "混合", "多路径统计应显示混合模式");
}

void test_wave_phosphor_stroke_style_uses_channel_style()
{
    protoscope::plot::ChannelView styledChannel;
    styledChannel.color = std::array<float, 4>{0.12F, 0.34F, 0.56F, 0.78F};
    styledChannel.lineWidth = 2.75F;

    const auto styled = protoscope::ui::wavePhosphorStrokeStyle(styledChannel, 3);
    require(std::abs(styled.color.x - 0.12F) < 1e-6F, "余辉颜色应使用通道 R 分量");
    require(std::abs(styled.color.y - 0.34F) < 1e-6F, "余辉颜色应使用通道 G 分量");
    require(std::abs(styled.color.z - 0.56F) < 1e-6F, "余辉颜色应使用通道 B 分量");
    require(std::abs(styled.color.w - 0.78F) < 1e-6F, "余辉颜色应使用通道 alpha 分量");
    require(std::abs(styled.lineWidth - 2.75F) < 1e-6F, "余辉线宽应使用通道 line_width");

    const protoscope::plot::ChannelView fallbackChannel;
    const auto fallback = protoscope::ui::wavePhosphorStrokeStyle(fallbackChannel, 1);
    const auto paletteColor = protoscope::ui::fallbackChannelColor(1);
    require(std::abs(fallback.color.x - paletteColor.x) < 1e-6F, "未配置颜色时余辉应使用默认调色板");
    require(std::abs(fallback.lineWidth - protoscope::plot::kDefaultChannelLineWidth) < 1e-6F,
            "未配置线宽时余辉应使用默认通道线宽");
}

void test_wave_main_render_data_uses_sample_frequency_viewport()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.buffer.append(0,
                       protoscope::plot::WaveAppendRequest{
                           .source = "edge",
                           .samples =
                               {
                                   {.time = 100.0, .value = 0.0},
                                   {.time = 101.0, .value = 1.0},
                                   {.time = 102.0, .value = 0.0},
                               },
                       });

    wave.view.initialized = true;
    wave.view.autoFollowLatest = false;
    wave.view.defaultViewportPending = false;
    wave.view.sampleFrequencyHz = 10.0;
    wave.view.viewMinTime = 0.09;
    wave.view.viewMaxTime = 0.11;
    wave.view.visibleDuration = 0.02;
    wave.view.centerTime = 0.1;

    const auto frame = protoscope::ui::prepareWaveFrame(wave, 800.0F);

    require(frame.displayData != nullptr, "采样频率模式交互数据源不能为空");
    require(frame.renderDisplayData != nullptr, "采样频率模式渲染数据源不能为空");
    require(frame.displayData->channels.front().samples.size() == 3, "采样频率交互数据应保留视口样本及左右邻接样本");
    require(std::abs(frame.displayData->channels.front().samples.front().time - 0.0) < 1e-12,
            "采样频率交互数据应保留左侧邻接样本");
    require(std::abs(frame.displayData->channels.front().samples[1].time - 0.1) < 1e-12,
            "采样频率交互时间应按全局样本序号换算");
    require(std::abs(frame.displayData->channels.front().samples.back().time - 0.2) < 1e-12,
            "采样频率交互数据应保留右侧邻接样本");
    require(frame.renderDisplayData == frame.displayData, "采样频率主图渲染应复用当前视口显示缓存");
    require(frame.renderDisplayData->channels.front().samples.size() == 3, "采样频率渲染数据应携带邻接样本");
    require(frame.overviewDisplayData != nullptr, "采样频率概览数据源不能为空");
    require(std::abs(frame.overviewDisplayData->channels.front().samples.front().time - 0.0) < 1e-12,
            "概览数据起点应按全局样本序号换算");
    require(std::abs(frame.overviewDisplayData->channels.front().samples.back().time - 0.2) < 1e-12,
            "概览数据终点应按全局样本序号换算");
}

void test_wave_default_viewport_without_frequency_uses_time_scale()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.buffer.append(0,
                       protoscope::plot::WaveAppendRequest{
                           .source = "first-fit",
                           .samples =
                               {
                                   {.time = 10.0, .value = -2.0},
                                   {.time = 20.0, .value = 4.0},
                               },
                       });

    wave.view.initialized = true;
    wave.view.autoFollowLatest = false;
    wave.view.defaultViewportPending = true;
    wave.view.viewMinTime = 0.0;
    wave.view.viewMaxTime = 1.0;
    wave.view.visibleDuration = 2.5;
    wave.view.centerTime = 0.5;
    wave.view.viewMinValue = -3.0;
    wave.view.viewMaxValue = 7.0;

    const auto frame = protoscope::ui::prepareWaveFrame(wave, 800.0F);

    require(!wave.view.defaultViewportPending, "默认视口应用后应清除 pending 标记");
    require(!wave.view.autoFollowLatest, "默认视口不应强制恢复自动跟随");
    require(std::abs(wave.view.viewMinTime - 17.5) < 1e-12, "无采样频率时应沿用 time_scale 窗口长度");
    require(std::abs(wave.view.viewMaxTime - 20.0) < 1e-12, "无采样频率时默认窗口应右对齐最新脚本时间");
    require(std::abs(wave.view.viewMinValue + 3.0) < 1e-12, "默认视口不应按首批数据重算 Y 下限");
    require(std::abs(wave.view.viewMaxValue - 7.0) < 1e-12, "默认视口不应按首批数据重算 Y 上限");
    require(frame.displayData != nullptr, "默认视口后本帧显示数据指针不能为空");
}

void test_wave_default_viewport_uses_sample_frequency_budget()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    protoscope::plot::WaveAppendRequest request{.source = "default-frequency-budget"};
    for (int index = 0; index < 2000; ++index) {
        request.samples.push_back({.time = 100.0 + static_cast<double>(index), .value = static_cast<double>(index)});
    }
    require(wave.buffer.append(0, request), "采样频率默认视口样本应追加成功");

    wave.view.initialized = true;
    wave.view.autoFollowLatest = false;
    wave.view.defaultViewportPending = true;
    wave.view.sampleFrequencyHz = 1000.0;
    wave.view.viewMinTime = 10.0;
    wave.view.viewMaxTime = 11.0;
    wave.view.visibleDuration = 1.0;
    wave.view.centerTime = 10.5;
    wave.view.viewMinValue = -2.0;
    wave.view.viewMaxValue = 2.0;

    const auto frame = protoscope::ui::prepareWaveFrame(wave, 800.0F);

    require(!wave.view.defaultViewportPending, "采样频率默认视口应用后应清除 pending 标记");
    require(!wave.view.autoFollowLatest, "采样频率默认视口后仍应保留暂停跟随");
    require(std::abs(wave.view.visibleDuration - 0.8) < 1e-12,
            "默认 X duration 应使用 pointsPerChannel / sample_frequency_hz");
    require(std::abs(wave.view.viewMaxTime - 1.999) < 1e-12, "默认 X 右边界应对齐最新全局样本时间");
    require(std::abs(wave.view.viewMinTime - 1.199) < 1e-12, "默认 X 左边界应按预算窗口回推");
    require(std::abs(wave.view.viewMinValue + 2.0) < 1e-12, "采样频率默认视口不应覆盖 Y 下限");
    require(std::abs(wave.view.viewMaxValue - 2.0) < 1e-12, "采样频率默认视口不应覆盖 Y 上限");
    require(frame.displayData != nullptr, "采样频率默认视口后本帧显示数据不能为空");
    require(std::abs(frame.displayData->channels.front().samples.back().time - 1.999) < 1e-12,
            "采样频率默认视口后本帧显示时间应按 sampleIndex / Hz 换算");
}

void test_wave_default_viewport_duration_tracks_render_budget()
{
    const auto makeWave = [](std::size_t channelCount) {
        protoscope::plot::WaveDockState wave;
        wave.buffer.configureChannels(channelCount);
        for (std::size_t channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
            protoscope::plot::WaveAppendRequest request{.source = "default-budget"};
            for (int index = 0; index < 5000; ++index) {
                request.samples.push_back(
                    {.time = static_cast<double>(index), .value = static_cast<double>(index + channelIndex)});
            }
            require(wave.buffer.append(channelIndex, request), "预算测试样本应追加成功");
        }
        wave.view.initialized = true;
        wave.view.autoFollowLatest = false;
        wave.view.defaultViewportPending = true;
        wave.view.sampleFrequencyHz = 100.0;
        wave.view.visibleDuration = 1.0;
        return wave;
    };

    auto widthLimited = makeWave(1);
    static_cast<void>(protoscope::ui::prepareWaveFrame(widthLimited, 300.0F));
    require(std::abs(widthLimited.view.visibleDuration - 3.0) < 1e-12, "可用宽度应限制默认 X duration");

    auto pointConfigLimited = makeWave(1);
    pointConfigLimited.view.maxRenderPointsPerChannel = 125;
    static_cast<void>(protoscope::ui::prepareWaveFrame(pointConfigLimited, 800.0F));
    require(std::abs(pointConfigLimited.view.visibleDuration - 1.25) < 1e-12,
            "max_render_points_per_channel 应限制默认 X duration");

    auto vertexLimited = makeWave(2);
    vertexLimited.view.maxRenderPointsPerChannel = 1000;
    vertexLimited.view.maxRenderVertices = 3200;
    static_cast<void>(protoscope::ui::prepareWaveFrame(vertexLimited, 1000.0F));
    require(std::abs(vertexLimited.view.visibleDuration - 1.0) < 1e-12,
            "max_render_vertices 和通道数应限制默认 X duration");
}

void test_wave_default_viewport_preserves_configured_y_range()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.buffer.append(0,
                       protoscope::plot::WaveAppendRequest{
                           .source = "default-y-range",
                           .samples =
                               {
                                   {.time = 0.0, .value = -100.0},
                                   {.time = 1.0, .value = 100.0},
                               },
                       });

    wave.view.initialized = true;
    wave.view.autoFollowLatest = false;
    wave.view.defaultViewportPending = true;
    wave.view.sampleFrequencyHz = 10.0;
    wave.view.visibleDuration = 1.0;
    wave.view.manualVerticalMin = -5.0;
    wave.view.manualVerticalMax = 6.0;
    wave.view.viewMinValue = -5.0;
    wave.view.viewMaxValue = 6.0;

    static_cast<void>(protoscope::ui::prepareWaveFrame(wave, 800.0F));

    require(std::abs(wave.view.manualVerticalMin + 5.0) < 1e-12, "默认视口不应改写手动 Y 下限");
    require(std::abs(wave.view.manualVerticalMax - 6.0) < 1e-12, "默认视口不应改写手动 Y 上限");
    require(std::abs(wave.view.viewMinValue + 5.0) < 1e-12, "首批数据不应覆盖当前 Y 下限");
    require(std::abs(wave.view.viewMaxValue - 6.0) < 1e-12, "首批数据不应覆盖当前 Y 上限");
}

void test_wave_sample_frequency_auto_follow_preserves_trimmed_offset()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.setViewConfig(protoscope::plot::ViewConfig{.historyLimit = 5});
    wave.buffer.configureChannels(1);
    protoscope::plot::WaveAppendRequest request{.source = "trimmed-autofollow"};
    for (int i = 0; i < 10; ++i) {
        request.samples.push_back({.time = static_cast<double>(i), .value = static_cast<double>(i)});
    }
    require(wave.buffer.append(0, request), "自动跟随场景样本应追加成功");

    wave.view.initialized = true;
    wave.view.autoFollowLatest = true;
    wave.view.defaultViewportPending = false;
    wave.view.sampleFrequencyHz = 10.0;
    wave.view.visibleDuration = 0.2;
    wave.view.viewMinTime = 0.0;
    wave.view.viewMaxTime = 0.2;
    wave.view.centerTime = 0.1;

    const auto frame = protoscope::ui::prepareWaveFrame(wave, 800.0F);

    require(frame.displayData != nullptr, "裁剪后采样频率自动跟随数据源不能为空");
    require(std::abs(wave.view.viewMaxTime - 0.9) < 1e-12, "自动跟随应对齐最新全局样本时间");
    require(std::abs(wave.view.viewMinTime - 0.7) < 1e-12, "自动跟随窗口起点应保留全局样本偏移");
    require(frame.displayData->channels.front().samples.size() == 4, "自动跟随窗口应显示最新全局时间窗口及左邻接样本");
    require(std::abs(frame.displayData->channels.front().samples.front().time - 0.6) < 1e-12,
            "自动跟随显示起点应保留左侧邻接全局样本");
    require(std::abs(frame.displayData->channels.front().samples.back().time - 0.9) < 1e-12,
            "自动跟随显示终点应使用最新全局样本时间");
}

void test_wave_max_total_samples_trim_refreshes_cached_frame()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    protoscope::plot::WaveAppendRequest request{.source = "max-total-cache"};
    for (int i = 0; i < 10; ++i) {
        request.samples.push_back({.time = static_cast<double>(i), .value = static_cast<double>(i)});
    }
    require(wave.buffer.append(0, request), "max_total_samples 场景样本应追加成功");

    wave.view.initialized = true;
    wave.view.autoFollowLatest = false;
    wave.view.defaultViewportPending = false;
    wave.view.sampleFrequencyHz = 10.0;
    wave.view.viewMinTime = 0.0;
    wave.view.viewMaxTime = 0.9;
    wave.view.visibleDuration = 0.9;
    wave.view.centerTime = 0.45;

    const auto firstFrame = protoscope::ui::prepareWaveFrame(wave, 800.0F);
    require(firstFrame.fullSnapshot != nullptr, "初始全量快照不能为空");
    require(firstFrame.fullSnapshot->channels.front().totalSamples == 10, "初始缓存应包含 10 个样本");

    const auto previousRevision = wave.buffer.dataRevision();
    wave.buffer.setMaxTotalSamples(4);
    require(wave.buffer.dataRevision() == previousRevision + 1, "max_total_samples 真实裁剪后应推进数据版本");

    const auto frame = protoscope::ui::prepareWaveFrame(wave, 800.0F);
    require(frame.fullSnapshot != nullptr, "裁剪后全量快照不能为空");
    require(frame.fullSnapshot->channels.size() == 1, "裁剪后应保留通道");
    require(frame.fullSnapshot->channels.front().totalSamples == 4, "全量快照缓存应刷新为尾部 4 个样本");
    require(frame.fullSnapshot->channels.front().sampleIndexOffset == 6, "全量快照应记录已裁剪的全局样本偏移");
    require(frame.displayData != nullptr, "裁剪后显示数据不能为空");
    require(frame.displayData->channels.front().samples.size() == 4, "显示缓存应随裁剪刷新为 4 个样本");
    require(std::abs(frame.displayData->channels.front().samples.front().time - 0.6) < 1e-12,
            "采样频率时间轴起点应保留全局样本序号");
    require(std::abs(frame.displayData->channels.front().samples.back().time - 0.9) < 1e-12,
            "采样频率时间轴终点应保留最新全局样本序号");
}

void test_wave_max_total_samples_noop_preserves_revision()
{
    protoscope::plot::OscilloscopeBuffer buffer;
    protoscope::plot::WaveAppendRequest request{.source = "max-total-noop"};
    for (int i = 0; i < 3; ++i) {
        request.samples.push_back({.time = static_cast<double>(i), .value = static_cast<double>(i)});
    }
    require(buffer.append(0, request), "边界场景样本应追加成功");

    const auto revisionBeforeLargeLimit = buffer.dataRevision();
    buffer.setMaxTotalSamples(10);
    require(buffer.dataRevision() == revisionBeforeLargeLimit, "未超出 max_total_samples 时不应推进数据版本");
    auto snapshot = buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(snapshot.channels.front().totalSamples == 3, "未超限时样本数不应变化");
    require(snapshot.channels.front().sampleIndexOffset == 0, "未超限时全局样本偏移不应变化");

    buffer.setMaxTotalSamples(0);
    require(buffer.dataRevision() == revisionBeforeLargeLimit, "max_total_samples=0 不应裁剪或推进数据版本");
    snapshot = buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    require(snapshot.channels.front().totalSamples == 3, "max_total_samples=0 应保留全部样本");
    require(snapshot.channels.front().sampleIndexOffset == 0, "max_total_samples=0 不应改变全局样本偏移");
}

void test_wave_overview_display_data_is_budgeted()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(2);
    std::vector<protoscope::plot::WaveSample> samples;
    samples.reserve(10000);
    for (std::size_t index = 0; index < 10000; ++index) {
        samples.push_back({.time = static_cast<double>(index), .value = std::sin(static_cast<double>(index) * 0.01)});
    }
    wave.buffer.append(0, protoscope::plot::WaveAppendRequest{.source = "perf", .samples = samples});
    wave.buffer.append(1, protoscope::plot::WaveAppendRequest{.source = "perf", .samples = samples});
    wave.view.initialized = true;
    wave.view.autoFollowLatest = false;
    wave.view.defaultViewportPending = false;
    wave.view.viewMinTime = 9000.0;
    wave.view.viewMaxTime = 9010.0;
    wave.view.visibleDuration = 10.0;
    wave.view.centerTime = 9005.0;
    wave.view.overviewMaxSamples = 32;

    const auto frame = protoscope::ui::prepareWaveFrame(wave, 800.0F);

    require(frame.renderDisplayData == frame.displayData, "主图渲染应避免回退到完整历史数据");
    require(frame.displayData->channels.front().samples.size() < 10000, "当前视口缓存不应包含全历史样本");
    require(frame.overviewDisplayData != nullptr, "概览缓存不能为空");
    for (const auto& channel : frame.overviewDisplayData->channels) {
        require(channel.samples.size() <= wave.view.overviewMaxSamples * 2,
                "概览显示数据应按 overview_max_samples 预算降采样");
    }
    require(wave.cachedOverviewKeyValid, "概览缓存键应在 prepare 后有效");
}

void test_wave_overview_bounds_use_full_history_window()
{
    std::vector<protoscope::plot::WaveSample> samples;
    samples.reserve(20);
    for (std::size_t index = 0; index < 20; ++index) {
        samples.push_back({.time = static_cast<double>(index), .value = static_cast<double>(index)});
    }
    const std::vector<protoscope::plot::WaveSample> hiddenSamples{
        {.time = -100.0, .value = -50.0},
        {.time = 100.0, .value = 50.0},
    };

    protoscope::plot::WaveSnapshot fullSnapshot{};
    fullSnapshot.channels.push_back({
        .label = "CH1",
        .unit = "V",
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });
    fullSnapshot.channels.push_back({
        .label = "Hidden",
        .unit = "V",
        .totalSamples = hiddenSamples.size(),
        .visibleBegin = 0,
        .visibleEnd = hiddenSamples.size(),
        .samples = hiddenSamples.data(),
    });

    auto mainSnapshot = fullSnapshot;
    mainSnapshot.channels[0].visibleBegin = 15;
    mainSnapshot.channels[0].visibleEnd = samples.size();
    mainSnapshot.channels[1].visibleBegin = 0;
    mainSnapshot.channels[1].visibleEnd = 0;

    const auto mainDisplay = protoscope::plot::buildDisplayData(mainSnapshot, 0.0);
    const auto overviewDisplay = protoscope::plot::buildDisplayData(fullSnapshot, 0.0);
    const auto mainBounds = protoscope::plot::computeDisplayBoundsForChannels(mainDisplay, {0}, 0.001);
    const auto overviewBounds = protoscope::plot::computeDisplayBoundsForChannels(overviewDisplay, {0}, 0.001);
    require(mainBounds.valid && overviewBounds.valid, "主视图和概览 bounds 都应有效");
    require(std::abs(mainBounds.minTime - 15.0) < 1e-12, "主视图 bounds 应只覆盖当前窗口起点");
    require(std::abs(mainBounds.maxTime - 19.0) < 1e-12, "主视图 bounds 应只覆盖当前窗口终点");
    require(std::abs(overviewBounds.minTime - 0.0) < 1e-12, "概览 bounds 应覆盖完整历史起点");
    require(std::abs(overviewBounds.maxTime - 19.0) < 1e-12, "概览 bounds 应覆盖完整历史终点");

    auto frequencyMainSnapshot = fullSnapshot;
    protoscope::plot::applySampleFrequencyVisibleRange(frequencyMainSnapshot, 1.5, 1.9, 10.0);
    const auto frequencyMainDisplay = protoscope::plot::buildDisplayData(frequencyMainSnapshot, 10.0);
    const auto frequencyOverviewDisplay = protoscope::plot::buildDisplayData(fullSnapshot, 10.0);
    const auto frequencyMainBounds =
        protoscope::plot::computeDisplayBoundsForChannels(frequencyMainDisplay, {0}, 0.001);
    const auto frequencyOverviewBounds =
        protoscope::plot::computeDisplayBoundsForChannels(frequencyOverviewDisplay, {0}, 0.001);
    require(std::abs(frequencyMainBounds.minTime - 1.4) < 1e-12, "Fs 主视图 bounds 应包含左侧邻接样本");
    require(std::abs(frequencyMainBounds.maxTime - 1.9) < 1e-12, "Fs 主视图应按当前窗口换算终点");
    require(std::abs(frequencyOverviewBounds.minTime - 0.0) < 1e-12, "Fs 概览应按全样本序号换算起点");
    require(std::abs(frequencyOverviewBounds.maxTime - 1.9) < 1e-12, "Fs 概览应按全样本序号换算终点");

    const auto visibleOnlyBounds = protoscope::plot::computeDisplayBoundsForChannels(overviewDisplay, {0}, 0.001);
    const auto includeHiddenBounds = protoscope::plot::computeDisplayBounds(overviewDisplay, 0.001);
    require(std::abs(visibleOnlyBounds.minValue - 0.0) < 1e-12, "隐藏通道不应参与概览 Y 下限");
    require(std::abs(visibleOnlyBounds.maxValue - 19.0) < 1e-12, "隐藏通道不应参与概览 Y 上限");
    require(includeHiddenBounds.minValue < visibleOnlyBounds.minValue, "包含隐藏通道时应能看到隐藏通道下限");
    require(includeHiddenBounds.maxValue > visibleOnlyBounds.maxValue, "包含隐藏通道时应能看到隐藏通道上限");
}

void test_wave_x_axis_double_click_bounds_selects_full_history()
{
    const protoscope::plot::WaveDataBounds visibleWindow{
        .minTime = 5.0,
        .maxTime = 6.0,
        .minValue = -1.0,
        .maxValue = 1.0,
        .minStep = 0.1,
        .valid = true,
    };
    const protoscope::plot::WaveDataBounds fullHistory{
        .minTime = 1.0,
        .maxTime = 10.0,
        .minValue = -2.0,
        .maxValue = 2.0,
        .minStep = 0.1,
        .valid = true,
    };
    const protoscope::plot::WaveDataBounds invalidFullHistory{
        .valid = false,
    };

    const auto& defaultBounds = protoscope::plot::selectXAxisDoubleClickBounds(
        protoscope::plot::WaveXAxisDoubleClickAction::FitFullHistory, visibleWindow, fullHistory);
    require(std::abs(defaultBounds.minTime - 1.0) < 1e-12, "默认 X 轴双击应选择全历史起点");
    require(std::abs(defaultBounds.maxTime - 10.0) < 1e-12, "默认 X 轴双击应选择全历史终点");

    const auto& visibleBounds = protoscope::plot::selectXAxisDoubleClickBounds(
        protoscope::plot::WaveXAxisDoubleClickAction::FitVisibleWindow, visibleWindow, fullHistory);
    require(std::abs(visibleBounds.minTime - 5.0) < 1e-12, "旧行为应保留当前窗口起点");
    require(std::abs(visibleBounds.maxTime - 6.0) < 1e-12, "旧行为应保留当前窗口终点");

    const auto& fallbackBounds = protoscope::plot::selectXAxisDoubleClickBounds(
        protoscope::plot::WaveXAxisDoubleClickAction::FitFullHistory, visibleWindow, invalidFullHistory);
    require(std::abs(fallbackBounds.minTime - 5.0) < 1e-12, "全历史无效时应回退当前窗口起点");
    require(std::abs(fallbackBounds.maxTime - 6.0) < 1e-12, "全历史无效时应回退当前窗口终点");
}

void test_wave_fft_detects_50hz_and_150hz_components()
{
    constexpr double sampleFrequencyHz = 1024.0;
    constexpr std::size_t pointCount = 1024;
    std::vector<protoscope::plot::WaveSample> samples;
    samples.reserve(pointCount);
    for (std::size_t index = 0; index < pointCount; ++index) {
        const double time = static_cast<double>(index) / sampleFrequencyHz;
        const double value = std::sin(2.0 * 3.14159265358979323846 * 50.0 * time) +
                             0.5 * std::sin(2.0 * 3.14159265358979323846 * 150.0 * time);
        samples.push_back({.time = time, .value = value});
    }

    protoscope::plot::WaveSnapshot snapshot{};
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "V",
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });
    const auto displayData = protoscope::plot::buildDisplayData(snapshot, sampleFrequencyHz);
    const protoscope::plot::WaveFftConfig config{
        .enabled = true,
        .pointCount = protoscope::plot::WaveFftPointCount::N1024,
        .window = protoscope::plot::WaveFftWindow::Rectangular,
    };

    const auto frame = protoscope::plot::buildWaveFftFrame(
        snapshot, displayData, config, std::vector<std::uint8_t>{1}, 0.0, 1.0, sampleFrequencyHz);
    require(frame.valid, "FFT 帧应计算成功");
    require(std::abs(frame.frequencyResolutionHz - 1.0) < 1e-12, "1024Hz/1024 点应得到 1Hz/bin");
    require(frame.channels.size() == 1 && frame.channels[0].valid, "CH1 应有有效频谱");
    const auto& bins = frame.channels[0].bins;
    require(bins.size() == 513, "实数 FFT 应输出 N/2+1 个频点");
    require(std::abs(bins[50].frequencyHz - 50.0) < 1e-12, "第 50 个频点应对应 50Hz");
    require(std::abs(bins[150].frequencyHz - 150.0) < 1e-12, "第 150 个频点应对应 150Hz");
    require(bins[50].magnitude > 0.9 && bins[50].magnitude < 1.1, "50Hz 主分量幅值应接近 1");
    require(bins[150].magnitude > 0.4 && bins[150].magnitude < 0.6, "150Hz 分量幅值应接近 0.5");
    require(std::isfinite(bins[50].phaseDegrees), "50Hz 频点应保留相角");
    const auto readout = protoscope::plot::findNearestFftBin(frame, 0, 50.2);
    require(readout.has_value(), "FFT 游标应能吸附到最近频点");
    require(std::abs(readout->frequencyHz - 50.0) < 1e-12, "50.2Hz 应吸附到 50Hz bin");
    require(frame.channels[0].fundamental.has_value(), "应自动检测到基波");
    require(std::abs(frame.channels[0].fundamental->frequencyHz - 50.0) < 1e-12, "自动基波应为 50Hz");
}

void test_wave_fft_visible_samples_supports_non_power_of_two()
{
    constexpr double sampleFrequencyHz = 1000.0;
    constexpr std::size_t pointCount = 1000;
    std::vector<protoscope::plot::WaveSample> samples;
    samples.reserve(pointCount);
    for (std::size_t index = 0; index < pointCount; ++index) {
        const double time = static_cast<double>(index) / sampleFrequencyHz;
        const double value = std::sin(2.0 * 3.14159265358979323846 * 50.0 * time) +
                             0.5 * std::sin(2.0 * 3.14159265358979323846 * 150.0 * time);
        samples.push_back({.time = time, .value = value});
    }

    protoscope::plot::WaveSnapshot snapshot{};
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "V",
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });
    const auto displayData = protoscope::plot::buildDisplayData(snapshot, sampleFrequencyHz);
    const protoscope::plot::WaveFftConfig config{
        .enabled = true,
        .pointCount = protoscope::plot::WaveFftPointCount::VisibleSamples,
        .window = protoscope::plot::WaveFftWindow::Rectangular,
    };

    const auto frame = protoscope::plot::buildWaveFftFrame(
        snapshot, displayData, config, std::vector<std::uint8_t>{1}, 0.0, 1.0, sampleFrequencyHz);
    require(frame.valid, "非 2^n 可视样本 FFT 应计算成功");
    require(frame.pointCount == 1000, "VisibleSamples 应吃完整 1000 点");
    require(std::abs(frame.frequencyResolutionHz - 1.0) < 1e-12, "1000Hz/1000 点应得到 1Hz/bin");
    require(frame.channels[0].bins.size() == 501, "1000 点实数 FFT 应输出 501 个频点");
    require(frame.channels[0].bins[50].magnitude > 0.9, "50Hz 峰值应保留");
    require(frame.channels[0].bins[150].magnitude > 0.4, "150Hz 峰值应保留");
}

void test_wave_fft_manual_point_count_supports_non_power_of_two()
{
    constexpr double sampleFrequencyHz = 1500.0;
    constexpr std::size_t sampleCount = 1000;
    std::vector<protoscope::plot::WaveSample> samples;
    samples.reserve(sampleCount);
    for (std::size_t index = 0; index < sampleCount; ++index) {
        const double time = static_cast<double>(index) / sampleFrequencyHz;
        const double value = std::sin(2.0 * 3.14159265358979323846 * 60.0 * time);
        samples.push_back({.time = time, .value = value});
    }

    protoscope::plot::WaveSnapshot snapshot{};
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "V",
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });
    const auto displayData = protoscope::plot::buildDisplayData(snapshot, sampleFrequencyHz);
    const protoscope::plot::WaveFftConfig config{
        .enabled = true,
        .pointCount = protoscope::plot::WaveFftPointCount::Manual,
        .window = protoscope::plot::WaveFftWindow::Rectangular,
        .manualPointCount = 750,
    };

    const auto frame = protoscope::plot::buildWaveFftFrame(
        snapshot, displayData, config, std::vector<std::uint8_t>{1}, 0.0, 1.0, sampleFrequencyHz);
    require(frame.valid, "手动非 2^n 点数 FFT 应计算成功");
    require(frame.pointCount == 750, "Manual 应强制使用手动点数");
    require(frame.channels[0].usedSampleCount == 750, "Manual 应只使用最近的手动 N 点");
    require(std::abs(frame.frequencyResolutionHz - 2.0) < 1e-12, "1500Hz/750 点应得到 2Hz/bin");
    require(frame.channels[0].bins.size() == 376, "750 点实数 FFT 应输出 376 个频点");
    require(frame.channels[0].bins[30].magnitude > 0.9, "60Hz 峰值应保留");
}

void test_wave_fft_cursor_window_resolves_point_counts_and_duration()
{
    using protoscope::plot::WaveFftPointCount;

    protoscope::plot::WaveFftConfig config{
        .enabled = true,
        .pointCount = WaveFftPointCount::N1024,
    };
    auto window = protoscope::plot::resolveWaveFftCursorWindow(config, 4096, 2048.0, 10.0);
    require(window.has_value(), "固定 N 和有效 Fs 应生成游标 FFT 输入窗口");
    require(window->pointCount == 1024, "固定 N1024 应解析为 1024 点");
    require(std::abs(window->durationSeconds - 0.5) < 1e-12, "固定 N 游标间隔应等于 N/Fs");
    require(std::abs(window->minTime - 9.5) < 1e-12 && std::abs(window->maxTime - 10.0) < 1e-12,
            "固定 N 游标窗口应以右游标为锚回退 N/Fs");

    config.pointCount = WaveFftPointCount::Manual;
    config.manualPointCount = 750;
    window = protoscope::plot::resolveWaveFftCursorWindow(config, 1000, 1500.0, 3.0);
    require(window.has_value(), "Manual 点数应生成游标 FFT 输入窗口");
    require(window->pointCount == 750, "Manual 应保留用户指定点数");
    require(std::abs(window->durationSeconds - 0.5) < 1e-12, "Manual 游标间隔应等于 N/Fs");

    config.pointCount = WaveFftPointCount::Auto;
    config.autoMaxPointCount = 4096;
    window = protoscope::plot::resolveWaveFftCursorWindow(config, 3000, 1000.0, 8.0);
    require(window.has_value(), "Auto 点数应按当前可见样本解析窗口");
    require(window->pointCount == 2048, "Auto 应选择不超过可见样本和上限的最大 2^n");
    require(std::abs(window->durationSeconds - 2.048) < 1e-12, "Auto 游标间隔应跟随解析出的 N");

    config.pointCount = WaveFftPointCount::VisibleSamples;
    window = protoscope::plot::resolveWaveFftCursorWindow(config, 1000, 1000.0, 2.0);
    require(window.has_value(), "VisibleSamples 应使用当前可见样本数生成窗口");
    require(window->pointCount == 1000, "VisibleSamples 应保留非 2^n 可见样本数");
    require(std::abs(window->durationSeconds - 1.0) < 1e-12, "VisibleSamples 游标间隔应等于可见样本数/Fs");

    require(!protoscope::plot::resolveWaveFftCursorWindow(config, 1000, 0.0, 2.0).has_value(),
            "采样频率无效时不应强制移动游标窗口");
}

void test_wave_fft_fit_viewport_resets_frequency_and_value_ranges()
{
    constexpr double sampleFrequencyHz = 1000.0;
    constexpr std::size_t pointCount = 1000;
    std::vector<protoscope::plot::WaveSample> samples;
    samples.reserve(pointCount);
    for (std::size_t index = 0; index < pointCount; ++index) {
        const double time = static_cast<double>(index) / sampleFrequencyHz;
        const double value = std::sin(2.0 * 3.14159265358979323846 * 50.0 * time);
        samples.push_back({.time = time, .value = value});
    }

    protoscope::plot::WaveSnapshot snapshot{};
    snapshot.channels.push_back({
        .label = "CH1",
        .unit = "V",
        .totalSamples = samples.size(),
        .visibleBegin = 0,
        .visibleEnd = samples.size(),
        .samples = samples.data(),
    });
    const auto displayData = protoscope::plot::buildDisplayData(snapshot, sampleFrequencyHz);
    const protoscope::plot::WaveFftConfig config{
        .enabled = true,
        .pointCount = protoscope::plot::WaveFftPointCount::VisibleSamples,
        .window = protoscope::plot::WaveFftWindow::Rectangular,
    };
    const auto frame = protoscope::plot::buildWaveFftFrame(
        snapshot, displayData, config, std::vector<std::uint8_t>{1}, 0.0, 1.0, sampleFrequencyHz);
    const auto viewport = protoscope::plot::makeFftFitViewport(frame);
    require(frame.valid, "FFT 帧应计算成功");
    require(std::abs(viewport.frequencyMin) < 1e-12, "显示全部频谱应从 0Hz 开始");
    require(std::abs(viewport.frequencyMax - frame.maxFrequencyHz) < 1e-12, "显示全部频谱应恢复到 Nyquist");
    require(viewport.magnitudeMin < frame.minDisplayMagnitude, "显示全部频谱应给幅值下限留 padding");
    require(viewport.magnitudeMax > frame.maxDisplayMagnitude, "显示全部频谱应给幅值上限留 padding");
    require(std::abs(viewport.phaseMin + 180.0) < 1e-12, "显示全部频谱应恢复相位下限");
    require(std::abs(viewport.phaseMax - 180.0) < 1e-12, "显示全部频谱应恢复相位上限");
}

void test_wave_viewport_zoom_modes_and_clamp()
{
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

    const auto xy =
        protoscope::plot::zoomViewport(viewport, protoscope::plot::WaveZoomMode::XY, 1.0, 4.0, 0.0, bounds, 0.1, true);
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

void test_wave_overview_viewport_normalize()
{
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

void test_wave_cursor_position_in_viewport()
{
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

void test_wave_cursor_interval_text_by_axis()
{
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

    const auto scriptTime =
        protoscope::plot::makeCursorIntervalText(left, right, protoscope::plot::WaveTimeAxisSource::ScriptTime, "ms");
    require(scriptTime.valid, "脚本时间轴游标间隔应有效");
    require(scriptTime.showFrequency, "脚本时间轴应显示倒数频率");
    require(std::abs(scriptTime.frequencyHz - 0.25) < 1e-12, "脚本时间轴频率计算错误");

    const auto sampledTime = protoscope::plot::makeCursorIntervalText(
        left, right, protoscope::plot::WaveTimeAxisSource::SampleFrequency, "s");
    require(sampledTime.valid, "采样频率时间轴游标间隔应有效");
    require(sampledTime.showFrequency, "采样频率时间轴应显示倒数频率");
    require(std::abs(sampledTime.frequencyHz - 0.25) < 1e-12, "采样频率时间轴频率计算错误");
}

void test_wave_cursor_interval_lock()
{
    double right = 3.0;
    protoscope::plot::lockCursorInterval(1.0, right, 2.0, true);
    require(std::abs(right - 3.0) < 1e-12, "移动左游标时右游标应保持锁定间隔");

    double left = 1.0;
    protoscope::plot::lockCursorInterval(5.0, left, 2.0, false);
    require(std::abs(left - 3.0) < 1e-12, "移动右游标时左游标应保持锁定间隔");
}

void test_wave_channel_card_width_modes()
{
    const double fixedWidth = protoscope::plot::resolveChannelCardWidth(
        protoscope::plot::WaveChannelCardWidthMode::Fixed, 128.0, 0.22, 1000.0);
    require(std::abs(fixedWidth - 128.0) < 1e-12, "固定模式应直接使用配置宽度");

    const double adaptiveWidth = protoscope::plot::resolveChannelCardWidth(
        protoscope::plot::WaveChannelCardWidthMode::Adaptive, 128.0, 0.22, 1000.0);
    require(std::abs(adaptiveWidth - 220.0) < 1e-12, "自适应模式应按比例计算并保留上限夹取");
}

void test_wave_vertical_auto_fit_multiplier()
{
    const auto negativeRange = protoscope::plot::makeVerticalAutoFitRange(-10.0, 5.0, 1.2);
    require(std::abs(negativeRange.minValue + 12.0) < 1e-12, "负向范围 Auto Fit 下限应乘以系数");
    require(std::abs(negativeRange.maxValue - 12.0) < 1e-12, "负向范围 Auto Fit 上限应乘以系数");

    const auto positiveRange = protoscope::plot::makeVerticalAutoFitRange(2.0, 3.0, 1.2);
    require(std::abs(positiveRange.minValue + 3.6) < 1e-12, "正向范围 Auto Fit 下限应围绕 0 对称");
    require(std::abs(positiveRange.maxValue - 3.6) < 1e-12, "正向范围 Auto Fit 上限应围绕 0 对称");
}

void test_wave_visible_channel_bounds_ignore_hidden_channels()
{
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

void test_wave_hidden_channel_policy_defaults_to_visible_only()
{
    protoscope::config::GuiWaveConfig config;
    protoscope::plot::WaveViewState view;

    require(config.hiddenChannelPolicy == protoscope::plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews,
            "配置默认隐藏 CH 策略应只让可见通道参与派生视图");
    require(view.hiddenChannelPolicy == protoscope::plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews,
            "运行态默认隐藏 CH 策略应只让可见通道参与派生视图");
}

void test_wave_grid_division_readout_conversions()
{
    const protoscope::plot::ChannelSpec spec{
        .unit = "V",
        .ratio = 0.5,
        .scale = -2.0,
        .offset = 100.0,
    };
    const double displayPerDiv = protoscope::plot::waveDisplayValuePerDivision(-2.0, 6.0);
    require(std::abs(displayPerDiv - 1.0) < 1e-12, "显示值每格应等于 Y 范围除以 8");

    const auto display =
        protoscope::plot::waveChannelValuePerDivision(displayPerDiv,
                                                      spec,
                                                      protoscope::plot::WaveDisplayFormula::OffsetThenScale,
                                                      protoscope::plot::WaveGridDivisionReadoutMode::DisplayValue);
    require(display.has_value() && std::abs(*display - 1.0) < 1e-12, "display_value 应直接使用显示值每格");

    const auto actual =
        protoscope::plot::waveChannelValuePerDivision(displayPerDiv,
                                                      spec,
                                                      protoscope::plot::WaveDisplayFormula::OffsetThenScale,
                                                      protoscope::plot::WaveGridDivisionReadoutMode::ActualValue);
    require(actual.has_value() && std::abs(*actual - 0.5) < 1e-12, "actual_value 应除以 abs(scale)");

    const auto raw =
        protoscope::plot::waveChannelValuePerDivision(displayPerDiv,
                                                      spec,
                                                      protoscope::plot::WaveDisplayFormula::OffsetThenScale,
                                                      protoscope::plot::WaveGridDivisionReadoutMode::RawValue);
    require(raw.has_value() && std::abs(*raw - 1.0) < 1e-12, "raw_value 应除以 abs(scale * ratio)");
}

void test_wave_grid_division_readout_formula_offset_cancels()
{
    protoscope::plot::ChannelSpec spec{
        .ratio = 4.0,
        .scale = 0.5,
        .offset = -999.0,
    };
    const double displayPerDiv = 2.0;
    const auto offsetThenScale =
        protoscope::plot::waveChannelValuePerDivision(displayPerDiv,
                                                      spec,
                                                      protoscope::plot::WaveDisplayFormula::OffsetThenScale,
                                                      protoscope::plot::WaveGridDivisionReadoutMode::RawValue);
    const auto scaleThenOffset =
        protoscope::plot::waveChannelValuePerDivision(displayPerDiv,
                                                      spec,
                                                      protoscope::plot::WaveDisplayFormula::ScaleThenOffset,
                                                      protoscope::plot::WaveGridDivisionReadoutMode::RawValue);
    require(offsetThenScale.has_value() && scaleThenOffset.has_value(), "两种公式都应可计算 raw 每格");
    require(std::abs(*offsetThenScale - *scaleThenOffset) < 1e-12, "offset 不应污染每格差值换算");

    spec.scale = 0.0;
    const auto actualNa =
        protoscope::plot::waveChannelValuePerDivision(displayPerDiv,
                                                      spec,
                                                      protoscope::plot::WaveDisplayFormula::OffsetThenScale,
                                                      protoscope::plot::WaveGridDivisionReadoutMode::ActualValue);
    require(!actualNa.has_value(), "scale 为 0 时 actual_value 每格应为 n/a");

    spec.scale = 1.0;
    spec.ratio = 0.0;
    const auto rawNa =
        protoscope::plot::waveChannelValuePerDivision(displayPerDiv,
                                                      spec,
                                                      protoscope::plot::WaveDisplayFormula::ScaleThenOffset,
                                                      protoscope::plot::WaveGridDivisionReadoutMode::RawValue);
    require(!rawNa.has_value(), "ratio 为 0 时 raw_value 每格应为 n/a");
}

void test_wave_status_overlay_items_only_show_non_default_states()
{
    protoscope::plot::WaveViewState view;
    require(protoscope::ui::buildWaveStatusOverlayItems(view).empty(), "默认波形状态不应显示图内状态标签");

    view.autoFollowLatest = false;
    view.fft.enabled = true;
    view.zoomSelectionActive = true;
    view.lockVerticalRange = true;
    view.showCursors = false;
    view.showHoverReadout = false;

    const auto items = protoscope::ui::buildWaveStatusOverlayItems(view);
    require(items.size() == 6, "非默认波形状态应生成对应图内状态标签");
    require(items[0].label == std::string_view("暂停跟随"), "暂停跟随状态应排在第一位");
    require(items[1].label == std::string_view("FFT"), "FFT 状态应排在第二位");
    require(items[2].label == std::string_view("框选"), "框选状态应排在第三位");
    require(items[3].label == std::string_view("纵轴锁定"), "纵轴锁定状态应排在第四位");
    require(items[4].label == std::string_view("游标隐藏"), "游标隐藏状态应排在第五位");
    require(items[5].label == std::string_view("读数隐藏"), "悬停读数隐藏状态应排在第六位");

    view.zoomSelectionActive = false;
    view.zoomSelectionDragging = true;
    const auto draggingItems = protoscope::ui::buildWaveStatusOverlayItems(view);
    require(draggingItems[2].label == std::string_view("框选"), "框选拖动中也应保留框选状态标签");
}

void test_wave_phosphor_trigger_detection_interpolates_edges()
{
    const std::vector<protoscope::plot::WaveSample> risingSamples{
        {.time = 0.0, .value = 0.0},
        {.time = 2.0, .value = 10.0},
        {.time = 3.0, .value = 8.0},
    };
    const auto rising = protoscope::ui::findWavePhosphorTriggers(
        risingSamples, 0.0, 3.0, protoscope::plot::WavePhosphorTriggerEdge::Rising, 5.0);
    require(rising.size() == 1U, "上升沿触发应只命中一次");
    require(std::abs(rising.front().time - 1.0) < 1e-12, "上升沿触发时间应按阈值线性插值");

    const std::vector<protoscope::plot::WaveSample> fallingSamples{
        {.time = 0.0, .value = 8.0},
        {.time = 4.0, .value = 2.0},
        {.time = 6.0, .value = -2.0},
    };
    const auto falling = protoscope::ui::findWavePhosphorTriggers(
        fallingSamples, 0.0, 6.0, protoscope::plot::WavePhosphorTriggerEdge::Falling, 0.0);
    require(falling.size() == 1U, "下降沿触发应只命中一次");
    require(std::abs(falling.front().time - 5.0) < 1e-12, "下降沿触发时间应按阈值线性插值");
}

void test_wave_phosphor_trigger_window_aligns_to_fixed_x()
{
    const auto window = protoscope::ui::makeWavePhosphorTriggerWindow(10.0, 100.0, 4.0, 0.25);

    require(std::abs(window.sourceMinTime - 9.0) < 1e-12, "触发窗口源起点应按触发位置向左回溯");
    require(std::abs(window.sourceMaxTime - 13.0) < 1e-12, "触发窗口源终点应覆盖一个可视时长");
    require(std::abs(protoscope::ui::alignWavePhosphorSampleTime(window, 10.0) - 101.0) < 1e-12,
            "触发点应对齐到目标视窗固定 x 位置");
    require(std::abs(protoscope::ui::alignWavePhosphorSampleTime(window, 12.0) - 103.0) < 1e-12,
            "触发窗口内样本应保持相对时间偏移");
}

void test_wave_phosphor_non_follow_mode_freezes()
{
    protoscope::plot::WaveViewState view;
    view.phosphorEnabled = true;
    view.autoFollowLatest = true;
    require(protoscope::ui::wavePhosphorShouldAdvance(view), "跟随模式下 Phosphor 应推进");

    view.autoFollowLatest = false;
    require(!protoscope::ui::wavePhosphorShouldAdvance(view), "非跟随模式下 Phosphor 应冻结");

    view.autoFollowLatest = true;
    view.phosphorEnabled = false;
    require(!protoscope::ui::wavePhosphorShouldAdvance(view), "关闭 Phosphor 时不应扫描或衰减");
}

void test_wave_channel_reset_all_uses_protocol_default()
{
    auto wave = makeChannelResetWave();

    require(protoscope::plot::resetChannelConfigToDefault(
                wave, 0, protoscope::plot::WaveChannelDoubleClickAction::ResetAll),
            "恢复全部默认应成功");
    const auto spec = wave.buffer.channelSpec(0);
    require(spec.has_value(), "恢复全部默认后通道配置仍应存在");
    require(spec->label == "CH1", "恢复全部默认应恢复标签");
    require(std::abs(spec->ratio - 2.0) < 1e-12, "恢复全部默认应恢复 ratio");
    require(std::abs(spec->scale - 1.5) < 1e-12, "恢复全部默认应恢复 scale");
    require(std::abs(spec->offset + 0.25) < 1e-12, "恢复全部默认应恢复 offset");
    require(std::abs(spec->bitDisplay.yOffset - 1.0) < 1e-12, "恢复全部默认应恢复 bit y_offset");
    require(!wave.channelOverrides[0].labelOverridden, "恢复全部默认应清除 label override");
    require(!wave.channelOverrides[0].ratioOverridden, "恢复全部默认应清除 ratio override");
    require(!wave.channelOverrides[0].scaleOverridden, "恢复全部默认应清除 scale override");
    require(!wave.channelOverrides[0].offsetOverridden, "恢复全部默认应清除 offset override");
    require(!wave.channelOverrides[0].bitYOffsetOverridden, "恢复全部默认应清除 bit y_offset override");
}

void test_wave_channel_reset_scale_offset_preserves_label_and_ratio()
{
    auto wave = makeChannelResetWave();

    require(protoscope::plot::resetChannelConfigToDefault(
                wave, 0, protoscope::plot::WaveChannelDoubleClickAction::ResetScaleOffset),
            "恢复 scale/offset 默认应成功");
    const auto spec = wave.buffer.channelSpec(0);
    require(spec.has_value(), "恢复 scale/offset 后通道配置仍应存在");
    require(spec->label == "Renamed", "恢复 scale/offset 不应修改标签");
    require(std::abs(spec->ratio - 3.0) < 1e-12, "恢复 scale/offset 不应修改 ratio");
    require(std::abs(spec->scale - 1.5) < 1e-12, "恢复 scale/offset 应恢复 scale");
    require(std::abs(spec->offset + 0.25) < 1e-12, "恢复 scale/offset 应恢复 offset");
    require(std::abs(spec->bitDisplay.yOffset - 3.0) < 1e-12, "恢复 scale/offset 不应修改 bit y_offset");
    require(wave.channelOverrides[0].labelOverridden, "恢复 scale/offset 应保留 label override");
    require(wave.channelOverrides[0].ratioOverridden, "恢复 scale/offset 应保留 ratio override");
    require(!wave.channelOverrides[0].scaleOverridden, "恢复 scale/offset 应清除 scale override");
    require(!wave.channelOverrides[0].offsetOverridden, "恢复 scale/offset 应清除 offset override");
    require(wave.channelOverrides[0].bitYOffsetOverridden, "恢复 scale/offset 应保留 bit y_offset override");
}

void test_wave_channel_reset_scale_preserves_offset()
{
    auto wave = makeChannelResetWave();

    require(protoscope::plot::resetChannelConfigToDefault(
                wave, 0, protoscope::plot::WaveChannelDoubleClickAction::ResetScale),
            "恢复 scale 默认应成功");
    const auto spec = wave.buffer.channelSpec(0);
    require(spec.has_value(), "恢复 scale 后通道配置仍应存在");
    require(std::abs(spec->scale - 1.5) < 1e-12, "恢复 scale 应恢复 scale");
    require(std::abs(spec->offset - 10.0) < 1e-12, "恢复 scale 不应修改 offset");
    require(std::abs(spec->bitDisplay.yOffset - 3.0) < 1e-12, "恢复 scale 不应修改 bit y_offset");
    require(!wave.channelOverrides[0].scaleOverridden, "恢复 scale 应清除 scale override");
    require(wave.channelOverrides[0].offsetOverridden, "恢复 scale 应保留 offset override");
    require(wave.channelOverrides[0].bitYOffsetOverridden, "恢复 scale 应保留 bit y_offset override");
}

void test_wave_offset_reset_uses_protocol_default_only()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.defaultChannelSpecs.push_back({
        .label = "CH1",
        .unit = "V",
        .ratio = 2.0,
        .scale = 1.5,
        .offset = -0.25,
    });
    wave.buffer.setChannelSpec(0,
                               {
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

void test_wave_reset_one_channel_view_settings_only_resets_target()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(2);
    wave.defaultChannelSpecs = {
        {.label = "CH1", .unit = "V", .color = std::array<float, 4>{0.1F, 0.2F, 0.3F, 1.0F}},
        {.label = "CH2", .unit = "A", .scale = 2.0, .color = std::array<float, 4>{0.4F, 0.5F, 0.6F, 1.0F}},
    };
    wave.buffer.setChannelSpec(0, {.label = "Renamed1", .unit = "V", .scale = 5.0});
    wave.buffer.setChannelSpec(1, {.label = "Renamed2", .unit = "A", .scale = 6.0});
    wave.channelOverrides.resize(2);
    wave.channelOverrides[0].labelOverridden = true;
    wave.channelOverrides[0].scaleOverridden = true;
    wave.channelOverrides[0].colorOverridden = true;
    wave.channelOverrides[0].color = std::array<float, 4>{0.9F, 0.8F, 0.7F, 1.0F};
    wave.channelOverrides[1].labelOverridden = true;
    wave.channelOverrides[1].scaleOverridden = true;
    wave.hiddenChannelLabels = {"Renamed1", "Renamed2"};

    require(protoscope::plot::resetOneChannelViewSettings(wave, 0), "恢复单通道显示设置应成功");
    const auto spec0 = wave.buffer.channelSpec(0);
    const auto spec1 = wave.buffer.channelSpec(1);
    require(spec0.has_value() && spec0->label == "CH1" && spec0->scale == 1.0, "目标通道应恢复 Lua 默认值");
    require(spec1.has_value() && spec1->label == "Renamed2" && spec1->scale == 6.0, "非目标通道不应被恢复");
    require(!wave.channelOverrides[0].labelOverridden && !wave.channelOverrides[0].colorOverridden,
            "目标通道覆盖项应清空");
    require(wave.channelOverrides[1].labelOverridden && wave.channelOverrides[1].scaleOverridden,
            "非目标通道覆盖项应保留");
    require(wave.hiddenChannelLabels.size() == 1 && wave.hiddenChannelLabels[0] == "Renamed2",
            "恢复单通道应只让目标通道重新可见");
}

void test_wave_reset_all_channel_view_settings_preserves_samples()
{
    protoscope::plot::WaveDockState wave;
    wave.buffer.configureChannels(1);
    wave.defaultChannelSpecs = {{.label = "CH1", .unit = "V", .scale = 1.5}};
    wave.buffer.setChannelSpec(0, {.label = "Renamed", .unit = "V", .scale = 6.0});
    wave.buffer.append(0, {.samples = {{0.0, 1.0}, {1.0, 2.0}}});
    wave.channelOverrides.resize(1);
    wave.channelOverrides[0].labelOverridden = true;
    wave.channelOverrides[0].scaleOverridden = true;
    wave.hiddenChannelLabels = {"Renamed"};
    wave.legendOverlay.expanded = true;
    wave.legendOverlay.offsetX = 42.0F;
    wave.legendOverlay.offsetY = 64.0F;

    require(protoscope::plot::resetAllChannelViewSettings(wave), "恢复全部通道显示设置应成功");
    const auto spec = wave.buffer.channelSpec(0);
    const auto snapshot = wave.buffer.snapshot(0.0, 1.0);
    require(spec.has_value() && spec->label == "CH1" && spec->scale == 1.5, "全部恢复应恢复通道规格");
    require(snapshot.channels.size() == 1 && snapshot.channels[0].totalSamples == 2, "全部恢复不应清空波形数据");
    require(wave.channelOverrides.empty(), "全部恢复应清空覆盖项");
    require(wave.hiddenChannelLabels.empty(), "全部恢复应恢复所有通道可见");
    require(!wave.legendOverlay.expanded && wave.legendOverlay.offsetX == 8.0F && wave.legendOverlay.offsetY == 8.0F,
            "全部恢复应重置图例 overlay 状态");
}

void test_wave_mouse_y_offset_drag_mode_gate()
{
    using protoscope::plot::WaveMouseYOffsetDragMode;

    require(protoscope::ui::allowsMouseYOffsetDrag(WaveMouseYOffsetDragMode::Direct, false),
            "direct 模式不按 Shift 也应允许鼠标写回 Y 偏移");
    require(protoscope::ui::allowsMouseYOffsetDrag(WaveMouseYOffsetDragMode::Direct, true),
            "direct 模式按住 Shift 仍应允许鼠标写回 Y 偏移");
    require(!protoscope::ui::allowsMouseYOffsetDrag(WaveMouseYOffsetDragMode::Shift, false),
            "shift 模式不按 Shift 不应允许鼠标写回 Y 偏移");
    require(protoscope::ui::allowsMouseYOffsetDrag(WaveMouseYOffsetDragMode::Shift, true),
            "shift 模式按住 Shift 应允许鼠标写回 Y 偏移");
    require(!protoscope::ui::allowsMouseYOffsetDrag(WaveMouseYOffsetDragMode::Disabled, false),
            "disabled 模式不按 Shift 不应允许鼠标写回 Y 偏移");
    require(!protoscope::ui::allowsMouseYOffsetDrag(WaveMouseYOffsetDragMode::Disabled, true),
            "disabled 模式按住 Shift 也不应允许鼠标写回 Y 偏移");
}

void test_raw_capture_file_roundtrip()
{
    const ScopedTempFile tempFile("protoscope-roundtrip");
    const auto& tempPath = tempFile.path();

    const protoscope::plot::RawCaptureFileData capture{
        .protocolName = "default_protocol",
        .protocolDir = "protocols/templates/default_protocol",
        .sampleFrequencyHz = 4096.0,
        .capturedAtMs = 123456789,
        .payload = {0x01, 0x02, 0x7F, 0x00, 0x41},
        .events = {},
    };

    std::string error;
    require(protoscope::plot::writeRawCaptureFile(tempPath, capture, error), "psraw 写入应成功");
    const auto loaded = protoscope::plot::readRawCaptureFile(tempPath, error);
    if (!loaded.has_value()) {
        throw std::runtime_error("psraw 读回应成功: " + error);
    }
    require(loaded->protocolName == capture.protocolName, "psraw 应保留协议名");
    require(loaded->protocolDir == capture.protocolDir, "psraw 应保留协议目录");
    require(loaded->sampleFrequencyHz == capture.sampleFrequencyHz, "psraw 应保留采样频率");
    require(loaded->capturedAtMs == capture.capturedAtMs, "psraw 应保留采集时间");
    require(loaded->payload == capture.payload, "psraw 应保留原始 payload");
}

void test_raw_capture_file_plot_setup_roundtrip()
{
    const ScopedTempFile tempFile("protoscope-plot-setup-roundtrip");
    const auto& tempPath = tempFile.path();

    protoscope::plot::RawCaptureEvent setupEvent;
    setupEvent.type = protoscope::plot::RawCaptureEventType::PlotSetup;
    setupEvent.timestampMs = 1234;
    setupEvent.plotSetup.source = "温度曲线";
    setupEvent.plotSetup.resetHistory = true;
    setupEvent.plotSetup.channels = {
        {.label = "温度A",
         .unit = "℃",
         .ratio = 0.5,
         .scale = 2.0,
         .offset = -1.0,
         .color = std::array<float, 4>{1.0F, 0.25F, 0.0F, 1.0F},
         .lineWidth = std::optional<float>{2.75F},
         .bitDisplay = {.enabled = true, .firstBit = 4, .bitCount = 8, .yOffset = 1.25}},
        {.label = "压力B", .unit = "kPa", .ratio = 1.5, .scale = 3.0, .offset = 4.0},
    };
    setupEvent.plotSetup.view.timeScale = 0.25;
    setupEvent.plotSetup.view.timeUnit = "ms";
    setupEvent.plotSetup.view.verticalMin = -10.0;
    setupEvent.plotSetup.view.verticalMax = 80.0;
    setupEvent.plotSetup.view.verticalUnit = "℃";
    setupEvent.plotSetup.view.historyLimit = 4096;

    const protoscope::plot::RawCaptureFileData capture{
        .protocolName = "plot_setup_protocol",
        .protocolDir = "tests/fixtures/protocols/plot_setup_protocol",
        .sampleFrequencyHz = 1000.0,
        .capturedAtMs = 100,
        .payload = {0x01, 0x02},
        .events = {setupEvent,
                   {.type = protoscope::plot::RawCaptureEventType::RxBytes,
                    .timestampMs = 1235,
                    .bytes = {0x01, 0x02},
                    .profile = {},
                    .plotSetup = {}}},
    };

    std::string error;
    require(protoscope::plot::writeRawCaptureFile(tempPath, capture, error), "plot_setup psraw 写入应成功");
    std::ifstream in(tempPath, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    require(bytes.find("version: 3\n") != std::string::npos, "新 psraw 应写出 v3");
    require(bytes.find("event: plot_setup\n") != std::string::npos, "psraw 应包含 plot_setup 事件");
    require(bytes.find("channel.0.line_width: 2.75\n") != std::string::npos, "psraw 应写出显式 line_width");
    require(bytes.find("channel.1.line_width: none\n") != std::string::npos, "psraw 应写出默认 line_width 标记");
    require(bytes.find("channel.0.bit_display.enabled: true\n") != std::string::npos,
            "psraw 应写出 bit_display.enabled");
    require(bytes.find("channel.0.bit_display.first_bit: 4\n") != std::string::npos,
            "psraw 应写出 bit_display.first_bit");
    require(bytes.find("channel.0.bit_display.bit_count: 8\n") != std::string::npos,
            "psraw 应写出 bit_display.bit_count");
    require(bytes.find("channel.0.bit_display.y_offset: 1.25\n") != std::string::npos,
            "psraw 应写出 bit_display.y_offset");

    const auto loaded = protoscope::plot::readRawCaptureFile(tempPath, error);
    if (!loaded.has_value()) {
        throw std::runtime_error("plot_setup psraw 读回应成功: " + error);
    }
    require(loaded->events.size() == 2, "plot_setup roundtrip 应保留事件数量");
    const auto& loadedSetup = loaded->events.front().plotSetup;
    require(loaded->events.front().type == protoscope::plot::RawCaptureEventType::PlotSetup, "首个事件应为 plot_setup");
    require(loadedSetup.source == setupEvent.plotSetup.source, "plot_setup 应保留 source");
    require(loadedSetup.resetHistory, "plot_setup 应保留 reset_history");
    require(loadedSetup.channels.size() == 2, "plot_setup 应保留通道数量");
    require(loadedSetup.channels[0].label == "温度A", "plot_setup 应保留 UTF-8 label");
    require(loadedSetup.channels[0].unit == "℃", "plot_setup 应保留 UTF-8 unit");
    require(std::abs(loadedSetup.channels[0].ratio - 0.5) < 1e-12, "plot_setup 应保留 ratio");
    require(std::abs(loadedSetup.channels[0].scale - 2.0) < 1e-12, "plot_setup 应保留 scale");
    require(std::abs(loadedSetup.channels[0].offset + 1.0) < 1e-12, "plot_setup 应保留 offset");
    require(loadedSetup.channels[0].color.has_value(), "plot_setup 应保留颜色");
    require(std::abs((*loadedSetup.channels[0].color)[1] - 0.25F) < 1e-6F, "plot_setup 应保留 RGBA 分量");
    require(loadedSetup.channels[0].lineWidth.has_value(), "plot_setup 应保留 line_width");
    require(std::abs(*loadedSetup.channels[0].lineWidth - 2.75F) < 1e-6F, "plot_setup line_width 数值错误");
    require(!loadedSetup.channels[1].lineWidth.has_value(), "plot_setup 应保留默认 line_width 为空");
    require(loadedSetup.channels[0].bitDisplay.enabled, "plot_setup 应保留 bit_display.enabled");
    require(loadedSetup.channels[0].bitDisplay.firstBit == 4, "plot_setup 应保留 bit_display.first_bit");
    require(loadedSetup.channels[0].bitDisplay.bitCount == 8, "plot_setup 应保留 bit_display.bit_count");
    require(std::abs(loadedSetup.channels[0].bitDisplay.yOffset - 1.25) < 1e-12,
            "plot_setup 应保留 bit_display.y_offset");
    require(!loadedSetup.channels[1].bitDisplay.enabled, "缺省 bit_display 应按 disabled 处理");
    require(std::abs(loadedSetup.view.timeScale - 0.25) < 1e-12, "plot_setup 应保留 time_scale");
    require(loadedSetup.view.timeUnit == "ms", "plot_setup 应保留 time_unit");
    require(std::abs(loadedSetup.view.verticalMin + 10.0) < 1e-12, "plot_setup 应保留 vertical_min");
    require(std::abs(loadedSetup.view.verticalMax - 80.0) < 1e-12, "plot_setup 应保留 vertical_max");
    require(loadedSetup.view.verticalUnit == "℃", "plot_setup 应保留 vertical_unit");
    require(loadedSetup.view.historyLimit == 4096U, "plot_setup 应保留 history_limit");
}

void test_raw_capture_file_plot_setup_rejects_bad_fields()
{
    const ScopedTempFile tempFile("protoscope-plot-setup-bad-fields");
    const auto& tempPath = tempFile.path();

    protoscope::plot::RawCaptureEvent setupEvent;
    setupEvent.type = protoscope::plot::RawCaptureEventType::PlotSetup;
    setupEvent.plotSetup.channels = {{.label = "CH1", .unit = "V"}};
    setupEvent.plotSetup.view.historyLimit = 8;
    const protoscope::plot::RawCaptureFileData capture{
        .protocolName = "plot_setup_protocol",
        .protocolDir = "tests/fixtures/protocols/plot_setup_protocol",
        .sampleFrequencyHz = 1000.0,
        .capturedAtMs = 100,
        .payload = {},
        .events = {setupEvent},
    };

    std::string error;
    require(protoscope::plot::writeRawCaptureFile(tempPath, capture, error), "plot_setup psraw 写入应成功");
    std::ifstream in(tempPath, std::ios::binary);
    const std::string validBytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const auto rebuildRawCaptureBytes = [](const std::string& eventStream) {
        std::string header = std::string("ProtoScopeRawCapture\n"
                                         "version: 3\n"
                                         "protocol_name: plot_setup_protocol\n"
                                         "protocol_dir: tests/fixtures/protocols/plot_setup_protocol\n"
                                         "sample_frequency_hz: 1000\n"
                                         "captured_at_ms: 100\n"
                                         "truncated: false\n"
                                         "payload_size: ") +
                             std::to_string(eventStream.size()) +
                             "\n"
                             "event_stream: true\n"
                             "\n";
        header.resize(4096, '\0');
        return header + eventStream;
    };

    auto broken = validBytes;
    const auto channelCountPos = broken.find("channel_count: 1\n");
    require(channelCountPos != std::string::npos, "测试文件应包含 channel_count");
    broken.replace(channelCountPos, std::string("channel_count: 1\n").size(), "channel_xount: 1\n");
    require(!protoscope::plot::decodeRawCaptureFile(broken, error).has_value(), "缺少 channel_count 应拒绝解析");

    broken = validBytes;
    const auto labelPos = broken.find("channel.0.label: ");
    require(labelPos != std::string::npos, "测试文件应包含 channel label");
    const auto labelEnd = broken.find('\n', labelPos);
    broken.replace(labelPos, labelEnd - labelPos, "channel.0.label: z");
    require(!protoscope::plot::decodeRawCaptureFile(broken, error).has_value(), "坏 hex label 应拒绝解析");

    broken = validBytes;
    const auto ratioPos = broken.find("channel.0.ratio: ");
    require(ratioPos != std::string::npos, "测试文件应包含 channel ratio");
    const auto ratioEnd = broken.find('\n', ratioPos);
    broken.replace(ratioPos, ratioEnd - ratioPos, "channel.0.ratio: nope");
    require(!protoscope::plot::decodeRawCaptureFile(broken, error).has_value(), "坏数值 ratio 应拒绝解析");

    broken = validBytes;
    const auto lineWidthPos = broken.find("channel.0.line_width: ");
    require(lineWidthPos != std::string::npos, "测试文件应包含 channel line_width");
    auto eventStream = validBytes.substr(4096);
    const auto eventLineWidthPos = eventStream.find("channel.0.line_width: ");
    require(eventLineWidthPos != std::string::npos, "测试事件流应包含 channel line_width");
    const auto eventLineWidthEnd = eventStream.find('\n', eventLineWidthPos);
    eventStream.erase(eventLineWidthPos, eventLineWidthEnd - eventLineWidthPos + 1);
    const auto missingLineWidth = protoscope::plot::decodeRawCaptureFile(rebuildRawCaptureBytes(eventStream), error);
    require(missingLineWidth.has_value(), "旧 psraw 缺少 line_width 字段时应兼容读取");
    require(!missingLineWidth->events.front().plotSetup.channels.front().lineWidth.has_value(),
            "缺少 line_width 字段时应保留默认样式");

    broken = validBytes;
    const auto badLineWidthPos = broken.find("channel.0.line_width: ");
    require(badLineWidthPos != std::string::npos, "测试文件应包含 channel line_width");
    const auto badLineWidthEnd = broken.find('\n', badLineWidthPos);
    broken.replace(badLineWidthPos, badLineWidthEnd - badLineWidthPos, "channel.0.line_width: nope");
    require(!protoscope::plot::decodeRawCaptureFile(broken, error).has_value(), "坏数值 line_width 应拒绝解析");
}

void test_raw_capture_file_v2_event_stream_still_reads()
{
    const std::string eventBytes = "event: rx_bytes\n"
                                   "timestamp_ms: 2\n"
                                   "size: 3\n"
                                   "\n"
                                   "abc";
    std::string header = std::string("ProtoScopeRawCapture\n"
                                     "version: 2\n"
                                     "protocol_name: default_protocol\n"
                                     "protocol_dir: protocols/templates/default_protocol\n"
                                     "sample_frequency_hz: 1024\n"
                                     "captured_at_ms: 1\n"
                                     "truncated: false\n"
                                     "payload_size: ") +
                         std::to_string(eventBytes.size()) +
                         "\n"
                         "event_stream: true\n"
                         "\n";
    header.resize(4096, '\0');
    std::string error;
    const auto parsed = protoscope::plot::decodeRawCaptureFile(header + eventBytes, error);
    if (!parsed.has_value()) {
        throw std::runtime_error("v2 psraw 事件流仍应可读: " + error);
    }
    require(parsed->payload == std::vector<std::uint8_t>({'a', 'b', 'c'}), "v2 psraw 应保留 rx bytes");
}

void test_raw_capture_file_rejects_size_mismatch()
{
    const std::string broken = "ProtoScopeRawCapture\n"
                               "version: 2\n"
                               "protocol_name: default_protocol\n"
                               "protocol_dir: protocols/templates/default_protocol\n"
                               "sample_frequency_hz: 1024\n"
                               "payload_size: 5\n"
                               "captured_at_ms: 1\n"
                               "event_stream: true\n"
                               "\n"
                               "abc";
    std::string error;
    const auto parsed = protoscope::plot::decodeRawCaptureFile(broken, error);
    require(!parsed.has_value(), "raw_size 不匹配时应拒绝解析");
}

void test_raw_capture_file_requires_protocol_fields()
{
    const std::string broken = "ProtoScopeRawCapture\n"
                               "version: 2\n"
                               "sample_frequency_hz: 1024\n"
                               "payload_size: 3\n"
                               "captured_at_ms: 1\n"
                               "event_stream: true\n"
                               "\n"
                               "abc";
    std::string error;
    const auto parsed = protoscope::plot::decodeRawCaptureFile(broken, error);
    require(!parsed.has_value(), "缺少 protocol 字段时应拒绝解析");
}

void test_raw_capture_file_rejects_trailing_bytes()
{
    const ScopedTempFile tempFile("protoscope-trailing-bytes");
    const auto& tempPath = tempFile.path();
    const protoscope::plot::RawCaptureFileData capture{
        .protocolName = "default_protocol",
        .protocolDir = "protocols/templates/default_protocol",
        .sampleFrequencyHz = 4096.0,
        .capturedAtMs = 123,
        .payload = {0x01, 0x02},
        .events = {},
    };

    std::string error;
    require(protoscope::plot::writeRawCaptureFile(tempPath, capture, error), "psraw 写入应成功");
    std::ifstream in(tempPath, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    auto dirty = bytes;
    dirty.append("junk");
    const auto parsed = protoscope::plot::decodeRawCaptureFile(dirty, error);
    require(!parsed.has_value(), "payload 后存在尾随脏字节时应拒绝解析");
}

void test_raw_capture_file_rejects_profile_set_without_length()
{
    const ScopedTempFile tempFile("protoscope-profile-missing-length");
    const auto& tempPath = tempFile.path();
    protoscope::plot::RawCaptureFileData capture{
        .protocolName = "runtime_profile_stream",
        .protocolDir = "tests/fixtures/protocols/runtime_profile_stream",
        .sampleFrequencyHz = 4096.0,
        .capturedAtMs = 123,
        .payload = {},
        .events = {},
    };
    capture.events.push_back(protoscope::plot::RawCaptureEvent{
        .type = protoscope::plot::RawCaptureEventType::ProfileSet,
        .timestampMs = 123,
        .bytes = {},
        .profile = {.frameName = "dynamic_profile", .length = 8, .channelMap = {1, 0}},
        .plotSetup = {},
    });

    std::string error;
    require(protoscope::plot::writeRawCaptureFile(tempPath, capture, error), "profile_set psraw 写入应成功");
    std::ifstream in(tempPath, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    const auto pos = bytes.find("length: 8\n");
    require(pos != std::string::npos, "测试文件应包含 profile_set length 字段");
    bytes.replace(pos, std::string("length: 8\n").size(), "leng_x: 8\n");
    const auto parsed = protoscope::plot::decodeRawCaptureFile(bytes, error);
    require(!parsed.has_value(), "profile_set 缺少 length 时应拒绝解析");
}

void test_session_package_roundtrip_preserves_binary_entries()
{
    protoscope::session::SessionPackageData package{
        .createdAtMs = 123456,
        .entries =
            {
                {.name = "manifest.txt", .bytes = {'o', 'k', '\n'}},
                {.name = "capture.psraw", .bytes = {0x00, 0x01, static_cast<std::uint8_t>(0xFF), '\n'}},
            },
    };

    std::string error;
    const auto encoded = protoscope::session::encodeSessionPackage(package);
    const auto decoded = protoscope::session::decodeSessionPackage(encoded, error);
    if (!decoded.has_value()) {
        throw std::runtime_error("会话包 roundtrip 应成功: " + error);
    }

    require(decoded->createdAtMs == package.createdAtMs, "会话包 created_at_ms 应保留");
    const auto* manifest = protoscope::session::findSessionPackageEntry(*decoded, "manifest.txt");
    const auto* capture = protoscope::session::findSessionPackageEntry(*decoded, "capture.psraw");
    require(manifest != nullptr && manifest->bytes == package.entries[0].bytes, "manifest 条目应保留");
    require(capture != nullptr && capture->bytes == package.entries[1].bytes, "二进制 capture 条目应保留");
}

void test_session_package_rejects_truncated_entry()
{
    protoscope::session::SessionPackageData package{
        .createdAtMs = 1,
        .entries = {{.name = "capture.psraw", .bytes = {0x01, 0x02, 0x03}}},
    };

    auto encoded = protoscope::session::encodeSessionPackage(package);
    encoded.resize(encoded.size() - std::string("\nendentry\n").size());

    std::string error;
    const auto decoded = protoscope::session::decodeSessionPackage(encoded, error);
    require(!decoded.has_value(), "截断会话包应拒绝解析");
    require(!error.empty(), "截断会话包应给出错误信息");
}

void test_session_package_rejects_excessive_entry_count()
{
    const std::string bytes = "ProtoScopeSessionPackage\n"
                              "version: 1\n"
                              "created_at_ms: 1\n"
                              "entries: 999999999\n";

    std::string error;
    const auto decoded = protoscope::session::decodeSessionPackage(bytes, error);
    require(!decoded.has_value(), "entries 过大的会话包应拒绝解析");
    require(error.find("entries") != std::string::npos, "entries 过大错误应指向 entries 字段");
}
