#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace protoscope::ui {

namespace {

    bool hasSampleFrequencyTimebase(const plot::WaveViewState& view)
    {
        return view.sampleFrequencyHz > 0.0 && std::isfinite(view.sampleFrequencyHz);
    }

    std::optional<double> latestDisplayTime(const plot::WaveSnapshot& snapshot, double sampleFrequencyHz)
    {
        if (sampleFrequencyHz <= 0.0 || !std::isfinite(sampleFrequencyHz)) {
            return std::nullopt;
        }
        std::optional<std::size_t> latestSampleIndex;
        for (const auto& channel : snapshot.channels) {
            if (channel.totalSamples == 0) {
                continue;
            }
            const auto candidate = channel.sampleIndexOffset + channel.totalSamples - 1;
            if (!latestSampleIndex.has_value() || candidate > *latestSampleIndex) {
                latestSampleIndex = candidate;
            }
        }
        if (!latestSampleIndex.has_value()) {
            return std::nullopt;
        }
        return static_cast<double>(*latestSampleIndex) / sampleFrequencyHz;
    }

    std::size_t fftReferenceChannelIndex(const plot::WaveDockState& wave, std::size_t channelCount)
    {
        if (channelCount == 0) {
            return 0;
        }
        const auto measurementChannel = (std::min)(wave.view.measurementChannelIndex, channelCount - 1);
        if (measurementChannel < wave.fftChannelEnabled.size() && wave.fftChannelEnabled[measurementChannel] != 0) {
            return measurementChannel;
        }
        for (std::size_t channelIndex = 0; channelIndex < wave.fftChannelEnabled.size(); ++channelIndex) {
            if (wave.fftChannelEnabled[channelIndex] != 0 && channelIndex < channelCount) {
                return channelIndex;
            }
        }
        return measurementChannel;
    }

    std::size_t fftReferenceVisibleSampleCount(const plot::WaveDockState& wave,
                                               const plot::WaveDisplayData& displayData)
    {
        if (displayData.channels.empty()) {
            return 0;
        }
        const std::size_t channelIndex = fftReferenceChannelIndex(wave, displayData.channels.size());
        const auto& channel = displayData.channels[channelIndex];
        return (std::min)(channel.samples.size(), channel.actualValues.size());
    }

    void syncCursorSplitFftWindow(plot::WaveDockState& wave,
                                  const plot::WaveDisplayData& displayData,
                                  std::optional<double> latestTime)
    {
        auto& view = wave.view;
        if (!view.fft.enabled || view.fft.displayMode != plot::WaveFftDisplayMode::CursorSplit) {
            return;
        }

        const auto window = plot::resolveWaveFftCursorWindow(
            view.fft, fftReferenceVisibleSampleCount(wave, displayData), view.sampleFrequencyHz, view.cursors[1].time);
        if (!window.has_value()) {
            return;
        }

        double leftTime = view.cursors[0].time;
        double rightTime = view.cursors[1].time;
        if (view.autoFollowLatest && latestTime.has_value()) {
            rightTime = *latestTime;
            leftTime = rightTime - window->durationSeconds;
            view.lastCursorFftAnchorIndex = 1;
        } else if (view.lastCursorFftAnchorIndex == 0) {
            leftTime = view.cursors[0].time;
            rightTime = leftTime + window->durationSeconds;
        } else {
            rightTime = view.cursors[1].time;
            leftTime = rightTime - window->durationSeconds;
        }

        view.cursors[0].time = leftTime;
        view.cursors[1].time = rightTime;
        view.lockedCursorInterval = window->durationSeconds;
        view.fftSourceMinTime = (std::min)(leftTime, rightTime);
        view.fftSourceMaxTime = (std::max)(leftTime, rightTime);
        view.fftSourceWindowValid = true;
    }

    void hashCombine(std::size_t& seed, std::size_t value)
    {
        seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6U) + (seed >> 2U);
    }

    plot::WaveDockState::DisplayDataCacheKey makeDisplayDataCacheKey(const plot::WaveSnapshot& snapshot,
                                                                     const plot::WaveViewState& view,
                                                                     std::uint64_t dataRevision)
    {
        std::size_t rangeHash = 0;
        for (const auto& channel : snapshot.channels) {
            hashCombine(rangeHash, channel.visibleBegin);
            hashCombine(rangeHash, channel.visibleEnd);
            hashCombine(rangeHash, channel.totalSamples);
            hashCombine(rangeHash, channel.sampleIndexOffset);
        }
        return {
            .dataRevision = dataRevision,
            .sampleFrequencyHz = view.sampleFrequencyHz,
            .viewMinTime = view.viewMinTime,
            .viewMaxTime = view.viewMaxTime,
            .channelCount = snapshot.channels.size(),
            .displayFormula = snapshot.config.displayFormula,
            .rangeHash = rangeHash,
        };
    }

    plot::WaveDockState::OverviewDisplayDataCacheKey makeOverviewDisplayDataCacheKey(const plot::WaveSnapshot& snapshot,
                                                                                     const plot::WaveViewState& view,
                                                                                     std::uint64_t dataRevision,
                                                                                     std::size_t pointLimit)
    {
        const auto displayKey = makeDisplayDataCacheKey(snapshot, view, dataRevision);
        return {
            .dataRevision = dataRevision,
            .sampleFrequencyHz = view.sampleFrequencyHz,
            .channelCount = displayKey.channelCount,
            .displayFormula = displayKey.displayFormula,
            .rangeHash = displayKey.rangeHash,
            .pointLimit = pointLimit,
        };
    }

    bool channelScriptTimeUsable(const plot::ChannelView& channel, std::size_t begin, std::size_t end)
    {
        if (channel.samples == nullptr || begin >= end) {
            return false;
        }
        double previous = channel.samples[begin].time;
        if (!std::isfinite(previous)) {
            return false;
        }
        for (std::size_t index = begin + 1; index < end; ++index) {
            const double current = channel.samples[index].time;
            if (!std::isfinite(current) || current <= previous) {
                return false;
            }
            previous = current;
        }
        return true;
    }

    std::pair<std::size_t, std::size_t> visibleSampleRange(const plot::ChannelView& channel)
    {
        if (channel.samples == nullptr || channel.totalSamples == 0) {
            return {0, 0};
        }
        const std::size_t begin = (std::min)(channel.visibleBegin, channel.totalSamples);
        std::size_t end = (std::min)(channel.visibleEnd, channel.totalSamples);
        if (end < begin) {
            end = begin;
        }
        return {begin, end};
    }

    plot::WaveTimeAxisSource overviewAxisSource(const plot::WaveSnapshot& snapshot, double sampleFrequencyHz)
    {
        if (sampleFrequencyHz > 0.0 && std::isfinite(sampleFrequencyHz)) {
            return plot::WaveTimeAxisSource::SampleFrequency;
        }
        for (const auto& channel : snapshot.channels) {
            const auto [begin, end] = visibleSampleRange(channel);
            if (channelScriptTimeUsable(channel, begin, end)) {
                return plot::WaveTimeAxisSource::ScriptTime;
            }
        }
        return plot::WaveTimeAxisSource::SampleIndex;
    }

    double overviewSampleTime(const plot::WaveSample& sample,
                              std::size_t sampleIndexOffset,
                              std::size_t sampleIndex,
                              plot::WaveTimeAxisSource axisSource,
                              double sampleFrequencyHz)
    {
        const std::size_t globalSampleIndex = sampleIndexOffset + sampleIndex;
        if (axisSource == plot::WaveTimeAxisSource::SampleFrequency) {
            return static_cast<double>(globalSampleIndex) / sampleFrequencyHz;
        }
        if (axisSource == plot::WaveTimeAxisSource::ScriptTime) {
            return sample.time;
        }
        return static_cast<double>(globalSampleIndex);
    }

    void buildOverviewDisplayDataInto(const plot::WaveSnapshot& snapshot,
                                      double sampleFrequencyHz,
                                      std::size_t pointLimit,
                                      plot::WaveDisplayData& data)
    {
        data.channels.resize(snapshot.channels.size());
        for (auto& channel : data.channels) {
            channel.samples.clear();
            channel.actualValues.clear();
        }
        const auto axisSource = overviewAxisSource(snapshot, sampleFrequencyHz);
        data.axisSource = axisSource;
        data.timeUnit = axisSource == plot::WaveTimeAxisSource::SampleFrequency
                            ? "s"
                            : (axisSource == plot::WaveTimeAxisSource::ScriptTime
                                   ? (snapshot.config.timeUnit.empty() ? "s" : snapshot.config.timeUnit)
                                   : "sample");
        if (pointLimit == 0) {
            return;
        }

        for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
            const auto& channel = snapshot.channels[channelIndex];
            auto& displayChannel = data.channels[channelIndex];
            const auto [begin, end] = visibleSampleRange(channel);
            if (channel.samples == nullptr || begin >= end) {
                continue;
            }
            const std::size_t visibleCount = end - begin;
            const std::size_t bucketCount = (std::min)(pointLimit, visibleCount);
            displayChannel.samples.reserve(bucketCount * 2);
            displayChannel.actualValues.reserve(bucketCount * 2);
            const bool offsetThenScale = snapshot.config.displayFormula == plot::WaveDisplayFormula::OffsetThenScale;
            for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
                const std::size_t bucketBegin = begin + bucket * visibleCount / bucketCount;
                const std::size_t bucketEnd = begin + (bucket + 1) * visibleCount / bucketCount;
                double minValue = std::numeric_limits<double>::infinity();
                double maxValue = -std::numeric_limits<double>::infinity();
                double minActualValue = std::numeric_limits<double>::infinity();
                double maxActualValue = -std::numeric_limits<double>::infinity();
                double timeSum = 0.0;
                std::size_t count = 0;
                for (std::size_t sampleIndex = bucketBegin; sampleIndex < bucketEnd; ++sampleIndex) {
                    const auto& sample = channel.samples[sampleIndex];
                    const double actualValue = sample.value * channel.ratio;
                    const double displayValue = offsetThenScale ? (actualValue + channel.offset) * channel.scale
                                                                : actualValue * channel.scale + channel.offset;
                    minValue = (std::min)(minValue, displayValue);
                    maxValue = (std::max)(maxValue, displayValue);
                    minActualValue = (std::min)(minActualValue, actualValue);
                    maxActualValue = (std::max)(maxActualValue, actualValue);
                    timeSum += overviewSampleTime(
                        sample, channel.sampleIndexOffset, sampleIndex, axisSource, sampleFrequencyHz);
                    ++count;
                }
                if (count == 0) {
                    continue;
                }
                const double bucketTime = timeSum / static_cast<double>(count);
                displayChannel.samples.push_back({.time = bucketTime, .value = minValue});
                displayChannel.actualValues.push_back(minActualValue);
                if (maxValue != minValue) {
                    displayChannel.samples.push_back({.time = bucketTime, .value = maxValue});
                    displayChannel.actualValues.push_back(maxActualValue);
                }
            }
        }
    }

    void clampViewportLowerBoundToZero(plot::WaveViewState& view)
    {
        if (view.viewMinTime >= 0.0) {
            return;
        }
        view.viewMinTime = 0.0;
        view.viewMaxTime = (std::max)(view.viewMaxTime, view.viewMinTime + view.visibleDuration);
        view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    }

    bool applyDefaultViewportIfPending(plot::WaveDockState& wave,
                                       const RenderBudget& renderBudget,
                                       std::optional<double> latestTime,
                                       double minVisibleTimeSpan)
    {
        auto& view = wave.view;
        if (!view.defaultViewportPending) {
            return false;
        }

        // 核心流程：启动/reset 后默认只确定 X 窗口；Y 轴沿用配置或用户当前范围，不被首批数据覆盖。
        if (hasSampleFrequencyTimebase(view)) {
            const double budgetDuration = static_cast<double>(renderBudget.pointsPerChannel) / view.sampleFrequencyHz;
            view.visibleDuration = (std::max)(budgetDuration, minVisibleTimeSpan);
        } else {
            view.visibleDuration = (std::max)(view.visibleDuration, minVisibleTimeSpan);
        }

        if (latestTime.has_value()) {
            view.viewMaxTime = *latestTime;
            view.viewMinTime = view.viewMaxTime - view.visibleDuration;
            clampViewportLowerBoundToZero(view);
        } else {
            view.viewMinTime = 0.0;
            view.viewMaxTime = view.visibleDuration;
        }
        view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
        view.forceNextMainPlotLimits = true;
        view.defaultViewportPending = false;
        wave.cachedDisplayKeyValid = false;
        return true;
    }

} // namespace

void initializeWaveViewIfNeeded(plot::WaveViewState& view)
{
    if (view.initialized) {
        return;
    }
    view.visibleDuration = (std::max)(view.visibleDuration, (std::max)(view.minVisibleTimeSpan, 1e-6));
    const double halfDuration = view.visibleDuration * 0.5;
    view.viewMinTime = view.centerTime - halfDuration;
    view.viewMaxTime = view.centerTime + halfDuration;
    view.viewMinValue = view.manualVerticalMin;
    view.viewMaxValue = view.manualVerticalMax;
    clampViewportLowerBoundToZero(view);
    view.initialized = true;
}

WaveFrameData prepareWaveFrame(plot::WaveDockState& wave, float availableWidth)
{
    auto& view = wave.view;
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);

    WaveFrameData frame;
    const auto dataRevision = wave.buffer.dataRevision();
    if (wave.displayDataRevision != dataRevision || wave.displayDataSampleFrequencyHz != view.sampleFrequencyHz) {
        // 核心流程：全量快照只保留通道元数据和原始样本指针，显示缓存按当前窗口单独构建，避免高速采样时反复复制全历史。
        wave.cachedFullSnapshot = wave.buffer.snapshot(
            -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), false);
        wave.displayDataRevision = dataRevision;
        wave.displayDataSampleFrequencyHz = view.sampleFrequencyHz;
        wave.cachedDisplayKeyValid = false;
        wave.cachedOverviewKeyValid = false;
        wave.renderEnvelopeCache.clear();
        wave.cachedFftKeyValid = false;
    }

    frame.fullSnapshot = &wave.cachedFullSnapshot;
    view.lastRenderPointCount = 0;
    view.lastRenderSourceSampleCount = 0;
    view.lastRenderStats = {};
    view.visibleDuration = (std::max)(view.visibleDuration, minVisibleTimeSpan);
    const std::size_t contentPixelWidth = static_cast<std::size_t>((std::max)(availableWidth, 64.0F));
    frame.renderBudget = makeRenderBudget(view,
                                          wave.cachedFullSnapshot.channels.size(),
                                          contentPixelWidth,
                                          view.glowEnabled);
    const auto latestTime = hasSampleFrequencyTimebase(view)
                                ? latestDisplayTime(wave.cachedFullSnapshot, view.sampleFrequencyHz)
                                : wave.buffer.latestTime();
    const bool defaultViewportApplied =
        applyDefaultViewportIfPending(wave, frame.renderBudget, latestTime, minVisibleTimeSpan);
    if (!defaultViewportApplied && latestTime.has_value() && view.autoFollowLatest) {
        view.viewMaxTime = *latestTime;
        view.viewMinTime = view.viewMaxTime - view.visibleDuration;
        clampViewportLowerBoundToZero(view);
        view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    }

    const auto refreshMainDisplayFrame = [&]() {
        if (hasSampleFrequencyTimebase(view)) {
            frame.snapshot = wave.cachedFullSnapshot;
            plot::applySampleFrequencyVisibleRange(
                frame.snapshot, view.viewMinTime, view.viewMaxTime, view.sampleFrequencyHz);
        } else {
            frame.snapshot = wave.buffer.snapshot(view.viewMinTime, view.viewMaxTime);
        }
        const auto displayKey = makeDisplayDataCacheKey(frame.snapshot, view, dataRevision);
        if (!wave.cachedDisplayKeyValid || !(wave.cachedDisplayKey == displayKey)) {
            // 核心流程：主显示窗口完全未变时复用上一帧显示数据和边界，避免 UI 空转重复构建。
            plot::buildDisplayDataInto(frame.snapshot, view.sampleFrequencyHz, wave.cachedDisplayData);
            wave.cachedDisplayBounds = plot::computeDisplayBounds(wave.cachedDisplayData, minVisibleTimeSpan);
            wave.cachedDisplayKey = displayKey;
            wave.cachedDisplayKeyValid = true;
        }
        frame.displayData = &wave.cachedDisplayData;
        frame.renderDisplayData = &wave.cachedDisplayData;
        frame.displayBounds = wave.cachedDisplayBounds;
        view.timeAxisSource = frame.displayData->axisSource;
    };
    refreshMainDisplayFrame();

    const std::size_t overviewPixelBudget = contentPixelWidth;
    const std::size_t overviewPointLimit =
        view.overviewMaxSamples > 0
            ? (std::min)({overviewPixelBudget, frame.renderBudget.pointsPerChannel, view.overviewMaxSamples})
            : (std::min)(overviewPixelBudget, frame.renderBudget.pointsPerChannel);
    const auto overviewKey = makeOverviewDisplayDataCacheKey(
        wave.cachedFullSnapshot, view, dataRevision, (std::max)(overviewPointLimit, std::size_t{1}));
    if (!wave.cachedOverviewKeyValid || !(wave.cachedOverviewKey == overviewKey)) {
        // 核心流程：概览只保留按像素预算压缩后的完整历史包络点，避免每次数据变更复制全历史显示样本。
        buildOverviewDisplayDataInto(
            wave.cachedFullSnapshot, view.sampleFrequencyHz, overviewKey.pointLimit, wave.cachedOverviewDisplayData);
        wave.cachedOverviewKey = overviewKey;
        wave.cachedOverviewKeyValid = true;
    }
    frame.overviewDisplayData = &wave.cachedOverviewDisplayData;

    if (view.fft.enabled) {
        if (wave.fftChannelEnabled.size() < wave.cachedFullSnapshot.channels.size()) {
            const auto oldSize = wave.fftChannelEnabled.size();
            wave.fftChannelEnabled.resize(wave.cachedFullSnapshot.channels.size(), 0);
            if (oldSize == 0 && !wave.fftChannelEnabled.empty()) {
                const auto preferredChannel =
                    (std::min)(view.measurementChannelIndex, wave.fftChannelEnabled.size() - 1);
                wave.fftChannelEnabled[preferredChannel] = 1;
            }
        }
        syncCursorSplitFftWindow(wave, *frame.displayData, latestTime);
        if (!view.fftSourceWindowValid) {
            view.fftSourceMinTime = view.viewMinTime;
            view.fftSourceMaxTime = view.viewMaxTime;
            view.fftSourceWindowValid = true;
            view.fftViewportInitialized = false;
            wave.cachedFftKeyValid = false;
        }
        const plot::WaveFftCacheKey key{
            .dataRevision = dataRevision,
            .viewMinTime = view.fftSourceMinTime,
            .viewMaxTime = view.fftSourceMaxTime,
            .sampleFrequencyHz = view.sampleFrequencyHz,
            .config = view.fft,
            .channelEnabled = wave.fftChannelEnabled,
        };
        if (!wave.cachedFftKeyValid || !(wave.cachedFftKey == key)) {
            // 核心流程：FFT 输入窗口与频域视口分离，频域缩放不会反向改变待分析的时域样本。
            auto fftSnapshot = hasSampleFrequencyTimebase(view)
                                   ? wave.cachedFullSnapshot
                                   : wave.buffer.snapshot(view.fftSourceMinTime, view.fftSourceMaxTime);
            if (hasSampleFrequencyTimebase(view)) {
                plot::applySampleFrequencyVisibleRange(
                    fftSnapshot, view.fftSourceMinTime, view.fftSourceMaxTime, view.sampleFrequencyHz);
            }
            const auto fftDisplayData = plot::buildDisplayData(fftSnapshot, view.sampleFrequencyHz);
            wave.cachedFftFrame = plot::buildWaveFftFrame(fftSnapshot,
                                                          fftDisplayData,
                                                          view.fft,
                                                          wave.fftChannelEnabled,
                                                          view.fftSourceMinTime,
                                                          view.fftSourceMaxTime,
                                                          view.sampleFrequencyHz);
            wave.cachedFftKey = key;
            wave.cachedFftKeyValid = true;
        }
        frame.fftFrame = &wave.cachedFftFrame;
    } else {
        view.fftSourceWindowValid = false;
        view.fftViewportInitialized = false;
        wave.cachedFftKeyValid = false;
        wave.cachedFftFrame = {};
        frame.fftFrame = &wave.cachedFftFrame;
    }

    return frame;
}

void drawCursorToolbar(plot::WaveViewState& view,
                       const plot::ViewConfig& config,
                       const plot::WaveDisplayData& displayData)
{
    (void) config;
    ImGui::Text("时间轴: %s (%s)", axisSourceName(view.timeAxisSource), displayData.timeUnit.c_str());
    if (!view.showCursors || displayData.channels.empty()) {
        return;
    }
}

void placeCursorInViewport(plot::WaveViewState& view,
                           const plot::ViewConfig& config,
                           const plot::WaveDisplayData& displayData,
                           std::size_t cursorIndex,
                           double ratio)
{
    if (displayData.channels.empty()) {
        return;
    }
    clampActiveChannel(view, displayData.channels.size());
    const double cursorSnapDistance = (std::max)(view.viewMaxTime - view.viewMinTime, config.timeScale) / 80.0;
    auto& cursor = view.cursors[cursorIndex];
    cursor.enabled = true;
    cursor.time = plot::cursorTimeInViewport(currentViewport(view), ratio);
    cursor.channelIndex = view.measurementChannelIndex;
    const auto best = findNearestDisplayByScope(displayData, view, cursor.time, cursorSnapDistance);
    if (!best.has_value()) {
        return;
    }
    cursor.time = best->time;
    cursor.value = best->value;
    cursor.channelIndex = best->channelIndex;
}

void placeCursorPairInViewport(plot::WaveViewState& view,
                               const plot::ViewConfig& config,
                               const plot::WaveDisplayData& displayData)
{
    // 核心流程：双游标快捷定位只移动游标，不改变当前视窗，便于快速重取测量区间。
    placeCursorInViewport(view, config, displayData, 0, 0.4);
    placeCursorInViewport(view, config, displayData, 1, 0.6);
    view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
}

void applyMainPlotAxesAndLimits(plot::WaveViewState& view,
                                const plot::WaveSnapshot& snapshot,
                                const plot::WaveDisplayData& displayData)
{
    static_cast<void>(snapshot);
    constexpr ImPlotAxisFlags xAxisFlags = ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoGridLines;
    constexpr ImPlotAxisFlags yAxisFlags = ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoLabel |
                                           ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks |
                                           ImPlotAxisFlags_NoTickLabels;
    const char* xAxisLabel = nullptr;
    if (view.showAxisLabels) {
        xAxisLabel = displayData.timeUnit == "sample" ? "Sample" : "Time";
    }
    ImPlot::SetupAxis(ImAxis_X1, xAxisLabel, xAxisFlags);
    ImPlot::SetupAxis(ImAxis_Y1, nullptr, yAxisFlags);
    const bool forceMainPlotLimits = view.forceNextMainPlotLimits;
    ImPlot::SetupAxisLimits(ImAxis_X1,
                            view.viewMinTime,
                            view.viewMaxTime,
                            (view.autoFollowLatest || forceMainPlotLimits) ? ImPlotCond_Always : ImPlotCond_Once);
    ImPlot::SetupAxisTicks(
        ImAxis_X1, view.viewMinTime, view.viewMaxTime, plot::kWaveGridMajorXDivisions + 1, nullptr, false);
    view.forceNextMainPlotLimits = false;
    if (view.lockVerticalRange) {
        ImPlot::SetupAxisLimits(ImAxis_Y1, view.manualVerticalMin, view.manualVerticalMax, ImPlotCond_Always);
    } else {
        ImPlot::SetupAxisLimits(
            ImAxis_Y1, view.viewMinValue, view.viewMaxValue, forceMainPlotLimits ? ImPlotCond_Always : ImPlotCond_Once);
    }
}

bool handleMainPlotZoom(plot::WaveViewState& view, const ImPlotPoint& mousePos)
{
    const auto& io = ImGui::GetIO();
    if (io.MouseWheel == 0.0F ||
        (!ImPlot::IsPlotHovered() && !ImPlot::IsAxisHovered(ImAxis_X1) && !ImPlot::IsAxisHovered(ImAxis_Y1))) {
        return false;
    }

    plot::WaveZoomMode zoomMode =
        view.controlMode == plot::WaveControlMode::Oscilloscope ? plot::WaveZoomMode::XOnly : plot::WaveZoomMode::XY;
    if (ImPlot::IsAxisHovered(ImAxis_X1)) {
        zoomMode = plot::WaveZoomMode::XOnly;
    } else if (ImPlot::IsAxisHovered(ImAxis_Y1)) {
        if (view.controlMode == plot::WaveControlMode::Oscilloscope) {
            return false;
        }
        zoomMode = plot::WaveZoomMode::YOnly;
    }
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    const plot::WaveDataBounds bounds{
        .minTime = -std::numeric_limits<double>::infinity(),
        .maxTime = std::numeric_limits<double>::infinity(),
        .minValue = -std::numeric_limits<double>::infinity(),
        .maxValue = std::numeric_limits<double>::infinity(),
        .minStep = minVisibleTimeSpan,
        .valid = false,
    };
    const auto zoomed = plot::zoomViewport(
        currentViewport(view), zoomMode, io.MouseWheel, mousePos.x, mousePos.y, bounds, minVisibleTimeSpan, false);
    applyViewport(view, zoomed);
    return true;
}

} // namespace protoscope::ui
