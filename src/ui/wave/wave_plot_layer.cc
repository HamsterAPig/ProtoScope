#include "protoscope/ui/ui_theme.hpp"
#include "protoscope/ui/wave_status.hpp"

#include "wave_context.hpp"
#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <iterator>
#include <limits>
#include <optional>
#include <string>

namespace protoscope::ui {

std::vector<WaveStatusOverlayItem> buildWaveStatusOverlayItems(const plot::WaveViewState& view)
{
    std::vector<WaveStatusOverlayItem> items;
    if (!view.autoFollowLatest) {
        items.push_back({"暂停跟随"});
    }
    if (view.fft.enabled) {
        items.push_back({view.fftUpdatePending ? "FFT 待更新" : "FFT"});
    }
    if (view.measurementUpdatePending) items.push_back({"统计待更新"});
    if (view.zoomSelectionActive || view.zoomSelectionDragging) {
        items.push_back({"框选"});
    }
    if (view.lockVerticalRange) {
        items.push_back({"纵轴锁定"});
    }
    if (!view.showCursors) {
        items.push_back({"游标隐藏"});
    }
    if (!view.showHoverReadout) {
        items.push_back({"读数隐藏"});
    }
    if (view.showCursorIntersectionReadouts) {
        items.push_back({"交点读数"});
    }
    return items;
}

bool shouldDrawCursorReadoutAnnotation(bool held, bool pinned)
{
    return held || pinned;
}

int splitCursorDragId(const std::size_t channelIndex, const std::size_t cursorIndex)
{
    return static_cast<int>(300U + channelIndex * 8U + cursorIndex);
}

std::vector<plot::WaveSample> buildBitRenderLanePoints(const std::vector<plot::WaveSample>& displaySamples,
                                                       const plot::WaveSample* sourceSamples,
                                                       std::size_t sourceSampleCount,
                                                       std::size_t bitIndex,
                                                       double lowY,
                                                       double highY,
                                                       double fallbackMaxTime,
                                                       std::size_t maxPoints)
{
    const std::size_t sampleCount = (std::min)(sourceSampleCount, displaySamples.size());
    if (sampleCount == 0 || sourceSamples == nullptr || maxPoints == 0) {
        return {};
    }

    const auto stateY = [lowY, highY](bool state) { return state ? highY : lowY; };
    const auto appendPoint = [](std::vector<plot::WaveSample>& lane, double time, double value) {
        if (!lane.empty() && lane.back().time == time && lane.back().value == value) {
            return;
        }
        lane.push_back({.time = time, .value = value});
    };

    const bool firstState = rawBitEnabled(sourceSamples[0].value, bitIndex);
    const double firstTime = displaySamples.front().time;
    double lastTime = displaySamples[sampleCount - 1U].time;
    if (std::abs(lastTime - firstTime) <= 1e-12 && fallbackMaxTime > firstTime) {
        lastTime = fallbackMaxTime;
    }

    std::vector<plot::WaveSample> exact;
    exact.reserve((std::min<std::size_t>) (sampleCount * 2U, maxPoints + 1U));
    bool previousState = firstState;
    double previousY = stateY(previousState);
    appendPoint(exact, firstTime, previousY);
    for (std::size_t sampleIndex = 1; sampleIndex < sampleCount; ++sampleIndex) {
        const bool currentState = rawBitEnabled(sourceSamples[sampleIndex].value, bitIndex);
        if (currentState == previousState) {
            continue;
        }

        const double transitionTime = displaySamples[sampleIndex].time;
        appendPoint(exact, transitionTime, previousY);
        previousState = currentState;
        previousY = stateY(previousState);
        appendPoint(exact, transitionTime, previousY);
    }
    appendPoint(exact, lastTime, previousY);
    if (exact.size() <= maxPoints) {
        return exact;
    }

    // 旧折线接口无法表达活动区，超预算时不再返回伪造阶梯；实际绘图使用 digitalSegments。
    return {};
}

namespace {

    std::size_t bitGeometryBudget(const RenderBudget& budget, std::size_t bitCount)
    {
        const auto channelBudget = budget.pointsPerChannel * budget.estimatedVerticesPerPoint;
        const auto textBudget = bitCount * 40U * 4U;
        return (std::max)(bitCount * 8U, channelBudget > textBudget ? channelBudget - textBudget : 0U);
    }

    plot::WaveDockState::RenderEnvelopeCacheKey makeRenderEnvelopeCacheKey(const plot::WaveDockState& wave,
                                                                           const plot::ChannelView& channel,
                                                                           std::size_t channelIndex,
                                                                           const std::vector<plot::WaveSample>& samples,
                                                                           const ImPlotRect& limits,
                                                                           std::size_t pointLimit,
                                                                           bool peakDetectDownsample)
    {
        return {
            .dataRevision = wave.displayDataRevision,
            .sampleFrequencyHz = wave.view.sampleFrequencyHz,
            .visibleMinTime = limits.X.Min,
            .visibleMaxTime = limits.X.Max,
            .channelIndex = channelIndex,
            .pointLimit = pointLimit,
            .sampleCount = samples.size(),
            .peakDetectDownsample = peakDetectDownsample,
            .displayFormula = wave.cachedFullSnapshot.config.displayFormula,
            .ratio = channel.ratio,
            .scale = channel.scale,
            .offset = channel.offset,
        };
    }

    const std::vector<plot::EnvelopePoint>& cachedRenderEnvelope(plot::WaveDockState& wave,
                                                                 const plot::ChannelView& channel,
                                                                 std::size_t channelIndex,
                                                                 const std::vector<plot::WaveSample>& samples,
                                                                 const ImPlotRect& limits,
                                                                 std::size_t pointLimit,
                                                                 std::size_t* sourceSampleCount)
    {
        if (wave.renderEnvelopeCache.size() <= channelIndex) {
            wave.renderEnvelopeCache.resize(channelIndex + 1);
        }
        auto& entry = wave.renderEnvelopeCache[channelIndex];
        const auto key = makeRenderEnvelopeCacheKey(wave, channel, channelIndex, samples, limits, pointLimit, false);
        if (!entry.valid || !(entry.key == key)) {
            // 核心流程：视口、数据和显示变换都没变时复用上一帧包络，避免 UI 空转反复扫样本。
            entry.envelope =
                buildDisplayEnvelope(samples, limits.X.Min, limits.X.Max, pointLimit, &entry.sourceSampleCount);
            entry.key = key;
            entry.valid = true;
        }
        if (sourceSampleCount != nullptr) {
            *sourceSampleCount = entry.sourceSampleCount;
        }
        return entry.envelope;
    }

    const std::vector<plot::WaveSample>& cachedPeakDetectTrace(plot::WaveDockState& wave,
                                                               const plot::ChannelView& channel,
                                                               std::size_t channelIndex,
                                                               const std::vector<plot::WaveSample>& samples,
                                                               const ImPlotRect& limits,
                                                               std::size_t pointLimit,
                                                               std::size_t* sourceSampleCount)
    {
        if (wave.renderEnvelopeCache.size() <= channelIndex) {
            wave.renderEnvelopeCache.resize(channelIndex + 1);
        }
        auto& entry = wave.renderEnvelopeCache[channelIndex];
        const auto key = makeRenderEnvelopeCacheKey(wave, channel, channelIndex, samples, limits, pointLimit, true);
        if (!entry.valid || !(entry.key == key)) {
            // 核心流程：高密度主图默认改成示波器式 peak-detect 轨迹，保留极值但不再逐桶画竖线。
            entry.peakDetectTrace =
                buildPeakDetectDownsample(samples, limits.X.Min, limits.X.Max, pointLimit, &entry.sourceSampleCount);
            entry.key = key;
            entry.valid = true;
        }
        if (sourceSampleCount != nullptr) {
            *sourceSampleCount = entry.sourceSampleCount;
        }
        return entry.peakDetectTrace;
    }

    plot::WaveDockState::BitRenderCacheKey makeBitRenderCacheKey(const plot::WaveDockState& wave,
                                                                 std::size_t channelIndex,
                                                                 const ImPlotRect& limits,
                                                                 const plot::BitDisplaySpec& spec,
                                                                 const BitLaneLayout&,
                                                                 std::size_t plotPixelWidth,
                                                                 std::size_t vertexBudget)
    {
        return {
            .dataRevision = wave.buffer.analysisRevision(),
            .historyEpoch = wave.buffer.historyEpoch(),
            .axis = wave.view.timeAxisSource,
            .channelIndex = channelIndex,
            .visibleMinTime = limits.X.Min,
            .visibleMaxTime = limits.X.Max,
            .visibleMinValue = 0,
            .visibleMaxValue = 0,
            .sampleFrequencyHz = wave.view.sampleFrequencyHz,
            .firstBit = spec.firstBit,
            .bitCount = spec.bitCount,
            .yOffset = 0,
            .plotPixelWidth = plotPixelWidth,
            .plotPixelHeight = 0,
            .layoutFingerprint = 0,
            .vertexBudget = vertexBudget,
            .denseMode = wave.view.bitDenseRenderMode,
        };
    }

    void buildBitRenderCacheEntry(plot::WaveDockState::BitRenderCacheEntry& entry,
                                  const plot::ChannelView& channel,
                                  const plot::WaveDisplayChannel& displayChannel,
                                  const BitLaneLayout&,
                                  std::size_t,
                                  const ImPlotRect& limits,
                                  std::size_t vertexBudget)
    {
        entry.lanes.assign(channel.bitDisplay.bitCount, {});
        entry.sourceSampleCount = 0;
        if (!bitDisplayEnabled(channel.bitDisplay) || channel.samples == nullptr || displayChannel.samples.empty()) {
            return;
        }
        {
            const plot::WaveQueryView query(channel, entry.key.axis, entry.key.sampleFrequencyHz, displayChannel.formula);
            const auto [first, last] = query.range(limits.X.Min, limits.X.Max);
            entry.sourceSampleCount = last - first;
            // 每个线段保守预留八个抗锯齿顶点，活动矩形只需四个。
            const auto primitives = (std::max)(std::size_t{1}, vertexBudget / (8 * channel.bitDisplay.bitCount));
            for (std::size_t laneIndex = 0; laneIndex < entry.lanes.size(); ++laneIndex) {
                entry.lanes[laneIndex] = query.digitalSegments(limits.X.Min, limits.X.Max,
                    channel.bitDisplay.firstBit + laneIndex, primitives, entry.key.plotPixelWidth);
            }
        }
    }

    const plot::WaveDockState::BitRenderCacheEntry& cachedBitRenderEntry(plot::WaveDockState& wave,
                                                                         const plot::ChannelView& channel,
                                                                         const plot::WaveDisplayChannel& displayChannel,
                                                                         const BitLaneLayout& bitLayout,
                                                                         std::size_t channelIndex,
                                                                         const ImPlotRect& limits,
                                                                         std::size_t plotPixelWidth,
                                                                         std::size_t vertexBudget)
    {
        if (wave.bitRenderCache.size() <= channelIndex) {
            wave.bitRenderCache.resize(channelIndex + 1);
        }
        auto& entry = wave.bitRenderCache[channelIndex];
        const auto key = makeBitRenderCacheKey(
            wave, channelIndex, limits, channel.bitDisplay, bitLayout, plotPixelWidth, vertexBudget);
        if (!entry.valid || !(entry.key == key)) {
            entry.valid = true;
            entry.key = key;
            buildBitRenderCacheEntry(entry, channel, displayChannel, bitLayout, channelIndex, limits, vertexBudget);
        }
        return entry;
    }

    void drawBitLaneLabels(const plot::WaveDockState& wave, const BitLaneLayout& bitLayout,
                           const ImPlotRect& limits, ImU32 textColor)
    {
        auto* drawList = ImPlot::GetPlotDrawList();
        if (drawList == nullptr) {
            return;
        }
        const ImVec2 plotPos = ImPlot::GetPlotPos();
        ImPlot::PushPlotClipRect();
        std::vector<float> labeledCenters;
        for (const auto& layoutLane : bitLayout.lanes) {
            if (std::ranges::find(labeledCenters, layoutLane.centerPixelY) != labeledCenters.end()) {
                continue;
            }
            labeledCenters.push_back(layoutLane.centerPixelY);
            const ImVec2 lanePixel = ImPlot::PlotToPixels(limits.X.Min, layoutLane.centerY);
            std::string label;
            for (const auto& lane : bitLayout.lanes) {
                if (lane.centerPixelY != layoutLane.centerPixelY) continue;
                if (!label.empty()) label += "   ";
                label += "CH" + std::to_string(lane.parentChannelIndex + 1) + " " + bitLaneDisplayLabel(lane.bitIndex);
                const auto* cached = lane.parentChannelIndex < wave.bitCountCache.size()
                    ? &wave.bitCountCache[lane.parentChannelIndex] : nullptr;
                label += "  " + (cached && cached->valid && lane.laneIndex < cached->counts.size()
                    ? std::to_string(cached->counts[lane.laneIndex]) : "--") + " 次";
            }
            // 同行多通道合并排版，按轨道高度和可用宽度收缩字号，不注册鼠标命中区域。
            const auto textSize = ImGui::CalcTextSize(label.c_str());
            float labelPitch = layoutLane.lanePixelPitch;
            for (const auto& other : bitLayout.lanes) {
                const auto distance = std::abs(other.centerPixelY - layoutLane.centerPixelY);
                if (distance > 0.5F) labelPitch = (std::min)(labelPitch, distance);
            }
            const auto scale = (std::min)({1.0F, (std::max)(1.0F, labelPitch - 2.0F) / textSize.y,
                (std::max)(1.0F, ImPlot::GetPlotSize().x - 12.0F) / (std::max)(1.0F, textSize.x)});
            const ImVec2 labelPos(plotPos.x + 6.0F, lanePixel.y - textSize.y * scale * 0.5F);
            // 文字优先可读，背景只遮住字形区域，不参与命中测试或改变游标交互。
            drawList->AddRectFilled(ImVec2(labelPos.x - 2, labelPos.y),
                ImVec2(labelPos.x + textSize.x * scale + 2, labelPos.y + textSize.y * scale),
                ImGui::ColorConvertFloat4ToU32(withAlpha(activeWaveStyleTokens().plotBackground, 0.9F)));
            drawList->AddText(ImGui::GetFont(), ImGui::GetFontSize() * scale,
                labelPos, textColor, label.c_str());
        }
        ImPlot::PopPlotClipRect();
    }

    void drawBitRenderLanes(const plot::WaveDockState::BitRenderCacheEntry& entry, const ImVec4& color, float lineWidth,
                           const BitLaneLayout& layout)
    {
        auto* drawList = ImPlot::GetPlotDrawList();
        if (drawList == nullptr) {
            return;
        }
        const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(withAlpha(color, 0.9F));
        ImPlot::PushPlotClipRect();
        for (std::size_t laneIndex = 0; laneIndex < entry.lanes.size(); ++laneIndex) {
            const auto& lane = entry.lanes[laneIndex];
            const auto location = std::ranges::find_if(layout.lanes, [&](const auto& item) {
                return item.parentChannelIndex == entry.key.channelIndex && item.laneIndex == laneIndex;
            });
            if (location == layout.lanes.end()) continue;
            const auto y = [&](double state) { return state != 0 ? location->highY : location->lowY; };
            for (const auto& segment : lane) {
                if (segment.activity) {
                    const auto left = ImPlot::PlotToPixels(segment.beginTime, location->lowY);
                    const auto right = ImPlot::PlotToPixels(segment.endTime, location->highY);
                    drawList->AddRectFilled(ImVec2(left.x, (std::min)(left.y, right.y)),
                        ImVec2((std::max)(left.x + 1.0F, right.x), (std::max)(left.y, right.y)),
                        ImGui::ColorConvertFloat4ToU32(withAlpha(color, 0.25F)));
                } else {
                    drawList->AddLine(ImPlot::PlotToPixels(segment.beginTime, y(segment.firstState)),
                        ImPlot::PlotToPixels(segment.endTime, y(segment.lastState)), lineColor, lineWidth);
                }
            }
        }
        ImPlot::PopPlotClipRect();
    }

    struct StackedDisplayData {
        const plot::WaveDisplayData* data{nullptr};
        std::vector<double> channelBaseY;
        plot::WaveDataBounds bounds{};
    };

    StackedDisplayData makeStackedDisplayData(const plot::WaveDockState& wave,
                                              const plot::WaveSnapshot& snapshot,
                                              const plot::WaveDisplayData& source)
    {
        StackedDisplayData result;
        result.data = &source;
        result.channelBaseY.assign(source.channels.size(), std::numeric_limits<double>::quiet_NaN());
        result.bounds.minTime = std::numeric_limits<double>::infinity();
        result.bounds.maxTime = -std::numeric_limits<double>::infinity();
        result.bounds.minValue = std::numeric_limits<double>::infinity();
        result.bounds.maxValue = -std::numeric_limits<double>::infinity();
        result.bounds.minStep = (std::max)(wave.view.minVisibleTimeSpan, 1e-6);

        std::size_t visibleRow = 0;
        for (std::size_t channelIndex = 0;
             channelIndex < source.channels.size() && channelIndex < snapshot.channels.size();
             ++channelIndex) {
            const auto& channel = result.data->channels[channelIndex];
            if (channel.samples.empty() || channelHiddenByLegendState(wave, channelIndex)) {
                continue;
            }
            if (bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
                const auto bitRange =
                    bitDisplayValueRange(snapshot, channelIndex, snapshot.channels[channelIndex].bitDisplay);
                for (const auto& sample : channel.samples) {
                    result.bounds.minTime = (std::min)(result.bounds.minTime, sample.time);
                    result.bounds.maxTime = (std::max)(result.bounds.maxTime, sample.time);
                }
                result.bounds.minValue = (std::min)(result.bounds.minValue, bitRange.minValue);
                result.bounds.maxValue = (std::max)(result.bounds.maxValue, bitRange.maxValue);
                continue;
            }
            double minValue = std::numeric_limits<double>::infinity();
            double maxValue = -std::numeric_limits<double>::infinity();
            for (const auto& sample : channel.samples) {
                minValue = (std::min)(minValue, sample.value);
                maxValue = (std::max)(maxValue, sample.value);
                result.bounds.minTime = (std::min)(result.bounds.minTime, sample.time);
                result.bounds.maxTime = (std::max)(result.bounds.maxTime, sample.time);
            }
            const double baseY = static_cast<double>(visibleRow) * 1.6;
            result.channelBaseY[channelIndex] = baseY;
            for (auto& sample : channel.samples) {
                result.bounds.minValue = (std::min)(result.bounds.minValue, sample.value);
                result.bounds.maxValue = (std::max)(result.bounds.maxValue, sample.value);
            }
            ++visibleRow;
        }

        if (!std::isfinite(result.bounds.minTime) || !std::isfinite(result.bounds.maxTime)) {
            result.bounds.minTime = 0.0;
            result.bounds.maxTime = 1.0;
        }
        if (!std::isfinite(result.bounds.minValue) || !std::isfinite(result.bounds.maxValue)) {
            result.bounds.minValue = -1.0;
            result.bounds.maxValue = 1.0;
        } else {
            result.bounds.minValue -= 0.5;
            result.bounds.maxValue += 0.5;
        }
        result.bounds.valid = true;
        return result;
    }

    void drawStackedChannelGuides(const plot::WaveSnapshot& snapshot, const std::vector<double>& channelBaseY)
    {
        auto* drawList = ImPlot::GetPlotDrawList();
        if (drawList == nullptr) {
            return;
        }
        const ImPlotRect limits = ImPlot::GetPlotLimits();
        const auto& waveTokens = activeWaveStyleTokens();
        const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(waveTokens.channelSeparator);
        const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(waveTokens.channelLabel);
        const ImVec2 plotPos = ImPlot::GetPlotPos();
        for (std::size_t channelIndex = 0;
             channelIndex < channelBaseY.size() && channelIndex < snapshot.channels.size();
             ++channelIndex) {
            const double baseY = channelBaseY[channelIndex];
            if (!std::isfinite(baseY)) {
                continue;
            }
            drawList->AddLine(ImPlot::PlotToPixels(limits.X.Min, baseY - 0.8),
                              ImPlot::PlotToPixels(limits.X.Max, baseY - 0.8),
                              lineColor,
                              1.0F);
            const ImVec2 labelPos = ImPlot::PlotToPixels(limits.X.Min, baseY);
            const std::string label = "CH" + std::to_string(channelIndex + 1U);
            drawList->AddText(
                ImVec2(plotPos.x + 8.0F, labelPos.y - ImGui::GetTextLineHeight() * 0.5F), textColor, label.c_str());
        }
    }

} // namespace

void resetWaveRenderStats(plot::WaveViewState& view, const RenderBudget& renderBudget)
{
    view.lastRenderStats = {};
    view.lastRenderStats.lastRenderPointBudget = renderBudget.pointsPerChannel;
    view.lastRenderStats.lastDownsampleThreshold = static_cast<std::size_t>(std::ceil(
        static_cast<double>(renderBudget.pointsPerChannel) * (std::max)(view.downsampleStartMultiplier, 1.0)));
}

std::size_t visibleSampleCount(const std::vector<plot::WaveSample>& samples, const ImPlotRect& limits)
{
    const auto visibleBegin = std::lower_bound(
        samples.begin(), samples.end(), limits.X.Min, [](const plot::WaveSample& sample, double value) {
            return sample.time < value;
        });
    const auto visibleEnd = std::upper_bound(
        samples.begin(), samples.end(), limits.X.Max, [](double value, const plot::WaveSample& sample) {
            return value < sample.time;
        });
    return visibleBegin < visibleEnd ? static_cast<std::size_t>(std::distance(visibleBegin, visibleEnd)) : 0U;
}

std::vector<std::size_t> collectVisiblePhosphorAnalogChannels(const plot::WaveDockState& wave,
                                                              const plot::WaveSnapshot& snapshot,
                                                              const plot::WaveDisplayData& displayData,
                                                              const ImPlotRect& limits)
{
    std::vector<std::size_t> visibleAnalogChannels;
    visibleAnalogChannels.reserve(snapshot.channels.size());
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        if (channelIndex >= displayData.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[channelIndex];
        if (bitDisplayEnabled(channel.bitDisplay) || channelHiddenByLegendState(wave, channelIndex)) {
            continue;
        }
        if (visibleSampleCount(displayData.channels[channelIndex].samples, limits) > 0U) {
            visibleAnalogChannels.push_back(channelIndex);
        }
    }
    return visibleAnalogChannels;
}

void registerPhosphorAnalogChannels(plot::WaveDockState& wave,
                                    const plot::WaveSnapshot& snapshot,
                                    const plot::WaveDisplayData& displayData,
                                    const ImPlotRect& limits,
                                    const std::vector<std::size_t>& visibleAnalogChannels)
{
    auto& view = wave.view;
    for (const auto channelIndex : visibleAnalogChannels) {
        if (channelIndex >= snapshot.channels.size() || channelIndex >= displayData.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[channelIndex];
        const auto style = wavePhosphorStrokeStyle(channel, channelIndex);
        ImPlotSpec legendSpec{};
        legendSpec.LineColor = style.color;
        legendSpec.LineWeight = style.lineWidth;
        legendSpec.Flags = ImPlotItemFlags_NoFit;
        const auto itemLabel = waveChannelItemLabel(channel.label, channelIndex);
        applySavedLegendVisibility(wave, channelIndex);
        ImPlot::PlotDummy(itemLabel.c_str(), legendSpec);
        if (!currentPlotItemVisible(channel.label, channelIndex)) {
            continue;
        }
        const std::size_t sourceSampleCount = visibleSampleCount(displayData.channels[channelIndex].samples, limits);
        view.lastRenderPointCount += sourceSampleCount;
        view.lastRenderSourceSampleCount += sourceSampleCount;
        ++view.lastRenderStats.phosphorChannelCount;
    }
}

void updateBitTransitionCounts(plot::WaveDockState& wave, const plot::ChannelView& channel,
                                std::size_t channelIndex, plot::WaveTimeAxisSource axis,
                                double minTime, double maxTime)
{
    if (wave.bitCountCache.size() <= channelIndex) wave.bitCountCache.resize(channelIndex + 1);
    auto& entry = wave.bitCountCache[channelIndex];
    const plot::WaveDockState::BitCountCacheKey key{
        wave.buffer.analysisRevision(), wave.buffer.historyEpoch(), channelIndex,
        channel.bitDisplay.firstBit, channel.bitDisplay.bitCount, axis,
        wave.view.sampleFrequencyHz, minTime, maxTime};
    if (entry.key.epoch != key.epoch || entry.key.firstBit != key.firstBit || entry.key.bitCount != key.bitCount)
        entry = {};
    entry.pending = !entry.valid || !(entry.key == key);
    if (!entry.pending) return;
    // 查询入口统一冻结，动画结束后只对最终范围刷新；采集侧索引始终继续维护。
    if (wave.view.interactionActive || wave.view.viewportAnimation.active || wave.view.overviewWindowDragging ||
        (ImGui::GetCurrentContext() && (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
         ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right)))) return;
    const auto start = std::chrono::steady_clock::now();
    const plot::WaveQueryView query(channel, axis, key.frequency, wave.buffer.viewConfig().displayFormula);
    entry.counts.resize(key.bitCount);
    for (std::size_t lane = 0; lane < key.bitCount; ++lane) {
        entry.counts[lane] = query.bitTransitions(minTime, maxTime, key.firstBit + lane);
        ++wave.bitCountQueryCount;
    }
    entry.key = key;
    entry.valid = true;
    entry.pending = false;
    wave.lastBitCountQueryMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

WaveStatusSnapshot makeWaveStatusSnapshot(const plot::WaveDockState& wave, std::string context)
{
    const auto& view = wave.view;
    WaveStatusSnapshot snapshot;
    snapshot.context = std::move(context);
    snapshot.epoch = wave.buffer.historyEpoch();
    snapshot.fftEnabled = view.fft.enabled;
    const bool currentContext = wave.analysisEpoch == snapshot.epoch;
    snapshot.fftPending = currentContext && view.fftUpdatePending;
    snapshot.statisticsEnabled = view.showCursors && view.cursors[0].enabled && view.cursors[1].enabled &&
        wave.buffer.channelCount() > view.measurementChannelIndex &&
        (!view.fft.enabled || view.fft.displayMode == plot::WaveFftDisplayMode::CursorSplit);
    snapshot.statisticsPending = currentContext && view.measurementUpdatePending;
    if (snapshot.fftEnabled && currentContext) {
        if (!wave.fftDisplayError.empty()) snapshot.fftError = wave.fftDisplayError;
        else if (wave.cachedFftKeyValid && !wave.cachedFftFrame.valid)
            snapshot.fftError = wave.cachedFftFrame.message.empty() ? "当前视图无法计算 FFT" : wave.cachedFftFrame.message;
    }
    for (const auto& item : buildWaveStatusOverlayItems(view)) {
        if (item.label == "FFT" || item.label == "FFT 待更新" || item.label == "统计待更新") continue;
        if (!snapshot.modes.empty()) snapshot.modes += " | ";
        snapshot.modes += item.label;
    }
    return snapshot;
}

void renderBitWaveChannels(plot::WaveDockState& wave,
                           const plot::WaveSnapshot& snapshot,
                           const plot::WaveDisplayData& displayData,
                           const RenderBudget& renderBudget,
                           const ImPlotRect& limits,
                           std::vector<std::size_t>& visibleChannelIndices,
                           BitLaneLayout& outBitLayout)
{
    auto& view = wave.view;
    std::vector<std::size_t> bitChannelIndices;
    bitChannelIndices.reserve(snapshot.channels.size());
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto& channel = snapshot.channels[channelIndex];
        if (!bitDisplayEnabled(channel.bitDisplay)) {
            continue;
        }
        const ImVec4 color = channelColor(channel, channelIndex);
        const float lineWidth = plot::resolveChannelLineWidth(channel);
        ImPlotSpec legendSpec{};
        legendSpec.LineColor = color;
        legendSpec.LineWeight = lineWidth;
        legendSpec.Flags = ImPlotItemFlags_NoFit;
        const auto itemLabel = waveChannelItemLabel(channel.label, channelIndex);
        applySavedLegendVisibility(wave, channelIndex);
        ImPlot::PlotDummy(itemLabel.c_str(), legendSpec);
        if (currentPlotItemVisible(channel.label, channelIndex)) {
            bitChannelIndices.push_back(channelIndex);
            visibleChannelIndices.push_back(channelIndex);
            updateBitTransitionCounts(wave, channel, channelIndex, displayData.axisSource, limits.X.Min, limits.X.Max);
        }
    }
    if (!bitChannelIndices.empty()) {
        outBitLayout =
            buildBitLaneLayout(snapshot, bitChannelIndices, limits, ImPlot::GetPlotPos(), ImPlot::GetPlotSize());
    }
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        if (channelIndex >= displayData.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[channelIndex];
        const auto& channelSamples = displayData.channels[channelIndex].samples;
        const ImVec4 color = channelColor(channel, channelIndex);
        const float lineWidth = plot::resolveChannelLineWidth(channel);
        std::size_t sourceSampleCount = 0;
        const auto visibleBegin =
            std::lower_bound(channelSamples.begin(),
                             channelSamples.end(),
                             limits.X.Min,
                             [](const plot::WaveSample& sample, double value) { return sample.time < value; });
        const auto visibleEnd =
            std::upper_bound(channelSamples.begin(),
                             channelSamples.end(),
                             limits.X.Max,
                             [](double value, const plot::WaveSample& sample) { return value < sample.time; });
        if (visibleBegin < visibleEnd) {
            sourceSampleCount = static_cast<std::size_t>(std::distance(visibleBegin, visibleEnd));
        }
        if (sourceSampleCount == 0 && !bitDisplayEnabled(channel.bitDisplay)) {
            continue;
        }

        if (bitDisplayEnabled(channel.bitDisplay)) {
            if (!currentPlotItemVisible(channel.label, channelIndex)) {
                continue;
            }

            const ImVec2 plotSize = ImPlot::GetPlotSize();
            const auto plotPixelWidth = static_cast<std::size_t>((std::max)(plotSize.x, 1.0F));
            // 标签及最多二十位十进制计数优先预留，余量用于线段和活动带。
            const auto vertexBudget = bitGeometryBudget(renderBudget, channel.bitDisplay.bitCount);
            const auto& entry = cachedBitRenderEntry(wave,
                                                     channel,
                                                     displayData.channels[channelIndex],
                                                     outBitLayout,
                                                     channelIndex,
                                                     limits,
                                                     plotPixelWidth,
                                                     vertexBudget);
            std::size_t renderedPoints = 0;
            for (const auto& lane : entry.lanes) {
                renderedPoints += lane.size();
            }
            view.lastRenderSourceSampleCount += entry.sourceSampleCount;
            view.lastRenderPointCount += renderedPoints;
            ++view.lastRenderStats.bitLaneChannelCount;
            drawBitRenderLanes(entry, color, lineWidth, outBitLayout);
            continue;
        }
    }
}

void drawBitLaneLabelsIfNeeded(const plot::WaveDockState& wave, const BitLaneLayout& bitLayout, const ImPlotRect& limits)
{
    if (!bitLayout.lanes.empty()) {
        const ImU32 labelColor = ImGui::ColorConvertFloat4ToU32(activeWaveStyleTokens().bitLabel);
        drawBitLaneLabels(wave, bitLayout, limits, labelColor);
    }
}

void renderWaveChannels(plot::WaveDockState& wave,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const RenderBudget& renderBudget,
                        const ImPlotRect& limits,
                        std::vector<std::size_t>& visibleChannelIndices,
                        BitLaneLayout& outBitLayout)
{
    auto& view = wave.view;
    visibleChannelIndices.clear();
    outBitLayout = {};
    resetWaveRenderStats(view, renderBudget);
    renderBitWaveChannels(wave, snapshot, displayData, renderBudget, limits, visibleChannelIndices, outBitLayout);
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        if (channelIndex >= displayData.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[channelIndex];
        if (bitDisplayEnabled(channel.bitDisplay)) {
            continue;
        }
        const auto& channelSamples = displayData.channels[channelIndex].samples;
        const ImVec4 color = channelColor(channel, channelIndex);
        const float lineWidth = plot::resolveChannelLineWidth(channel);
        const double downsampleStartMultiplier = (std::max)(view.downsampleStartMultiplier, 1.0);
        const std::size_t downsampleThreshold = static_cast<std::size_t>(
            std::ceil(static_cast<double>(renderBudget.pointsPerChannel) * downsampleStartMultiplier));
        std::size_t sourceSampleCount = 0;
        const auto visibleBegin =
            std::lower_bound(channelSamples.begin(),
                             channelSamples.end(),
                             limits.X.Min,
                             [](const plot::WaveSample& sample, double value) { return sample.time < value; });
        const auto visibleEnd =
            std::upper_bound(channelSamples.begin(),
                             channelSamples.end(),
                             limits.X.Max,
                             [](double value, const plot::WaveSample& sample) { return value < sample.time; });
        if (visibleBegin < visibleEnd) {
            sourceSampleCount = static_cast<std::size_t>(std::distance(visibleBegin, visibleEnd));
        }
        if (sourceSampleCount == 0) {
            continue;
        }

        if (const auto& c = displayData.channels[channelIndex]; c.source) {
            const plot::WaveQueryView query(*c.source, c.axis, c.frequency, c.formula);
            const auto range = query.range(limits.X.Min, limits.X.Max, false);
            sourceSampleCount = range.second - range.first;
        }
        if (sourceSampleCount <= downsampleThreshold) {
            auto begin = visibleBegin;
            auto end = visibleEnd;
            if (begin != channelSamples.begin()) {
                --begin;
            }
            if (end != channelSamples.end()) {
                ++end;
            }
            if (begin >= end) {
                continue;
            }
            const std::size_t rawVisibleCount = static_cast<std::size_t>(std::distance(begin, end));

            // 核心流程：低密度视图直接绘制原始点，避免桶包络把单条波形误画成双边界。
            WaveSampleGetterPayload payload{.samples = &(*begin)};
            ImPlotSpec spec{};
            spec.LineColor = color;
            spec.LineWeight = lineWidth;
            const auto itemLabel = waveChannelItemLabel(channel.label, channelIndex);
            applySavedLegendVisibility(wave, channelIndex);
            ImPlot::PlotLineG(itemLabel.c_str(),
                              reinterpret_cast<ImPlotGetter>(&waveSampleGetter),
                              &payload,
                              static_cast<int>(rawVisibleCount),
                              spec);
            if (!currentPlotItemVisible(channel.label, channelIndex)) {
                continue;
            }
            visibleChannelIndices.push_back(channelIndex);
            view.lastRenderPointCount += rawVisibleCount;
            view.lastRenderSourceSampleCount += sourceSampleCount;
            ++view.lastRenderStats.rawChannelCount;
            if (view.glowEnabled) {
                renderGlowSamples(&(*begin), rawVisibleCount, color, view.glowIntensity, lineWidth);
            }
            if (view.showPointsWhenSparse) {
                ImPlotSpec pointSpec{};
                pointSpec.Marker = ImPlotMarker_Circle;
                pointSpec.MarkerSize = 2.5F;
                pointSpec.MarkerFillColor = color;
                pointSpec.MarkerLineColor = color;
                pointSpec.LineWeight = 0.0F;
                pointSpec.Flags = ImPlotItemFlags_NoLegend | ImPlotItemFlags_NoFit;
                const auto samplesItemLabel =
                    std::string(channel.label) + " samples##wave_channel_samples_" + std::to_string(channelIndex);
                ImPlot::PlotScatterG(samplesItemLabel.c_str(),
                                     reinterpret_cast<ImPlotGetter>(&waveSampleGetter),
                                     &payload,
                                     static_cast<int>(rawVisibleCount),
                                     pointSpec);
            }
            continue;
        }

        ImPlotSpec legendSpec{};
        legendSpec.LineColor = color;
        legendSpec.LineWeight = lineWidth;
        legendSpec.Flags = ImPlotItemFlags_NoFit;
        const auto itemLabel = waveChannelItemLabel(channel.label, channelIndex);
        applySavedLegendVisibility(wave, channelIndex);
        ImPlot::PlotDummy(itemLabel.c_str(), legendSpec);
        const bool legendVisible = currentPlotItemVisible(channel.label, channelIndex);
        if (!legendVisible && excludesLegendHiddenChannels(view)) {
            continue;
        }
        if (legendVisible) {
            visibleChannelIndices.push_back(channelIndex);
        }
        if (view.peakDetectDownsample) {
            const auto& trace = cachedPeakDetectTrace(wave,
                                                      channel,
                                                      channelIndex,
                                                      displayData.channels[channelIndex].samples,
                                                      limits,
                                                      renderBudget.pointsPerChannel,
                                                      &sourceSampleCount);
            if (trace.empty()) {
                continue;
            }
            if (legendVisible) {
                view.lastRenderSourceSampleCount += sourceSampleCount;
                view.lastRenderPointCount += trace.size();
                ++view.lastRenderStats.peakDownsampleChannelCount;
                WaveSampleGetterPayload payload{.samples = trace.data()};
                ImPlotSpec spec{};
                spec.LineColor = color;
                spec.LineWeight = lineWidth;
                spec.Flags = ImPlotItemFlags_NoLegend | ImPlotItemFlags_NoFit;
                if (view.glowEnabled) {
                    renderGlowSamples(trace.data(), trace.size(), color, view.glowIntensity, lineWidth);
                }
                const auto peakItemLabel =
                    std::string(channel.label) + " peak##wave_channel_peak_" + std::to_string(channelIndex);
                ImPlot::PlotLineG(peakItemLabel.c_str(),
                                  reinterpret_cast<ImPlotGetter>(&waveSampleGetter),
                                  &payload,
                                  static_cast<int>(trace.size()),
                                  spec);
            }
            continue;
        }

        const auto& envelope = cachedRenderEnvelope(wave,
                                                    channel,
                                                    channelIndex,
                                                    displayData.channels[channelIndex].samples,
                                                    limits,
                                                    renderBudget.pointsPerChannel,
                                                    &sourceSampleCount);
        if (envelope.empty()) {
            continue;
        }
        if (legendVisible) {
            view.lastRenderSourceSampleCount += sourceSampleCount;
            view.lastRenderPointCount += envelope.size();
            ++view.lastRenderStats.envelopeDownsampleChannelCount;
        }
        if (view.glowEnabled) {
            renderGlowEnvelope(envelope, color, view.glowIntensity, lineWidth);
        } else {
            renderEnvelopeAsBars(envelope, color, lineWidth);
        }
    }
    drawBitLaneLabelsIfNeeded(wave, outBitLayout, limits);
}

void handleHoverReadout(plot::WaveViewState& view,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const std::vector<std::size_t>& visibleChannelIndices,
                        const BitLaneLayout& bitLayout,
                        const ImPlotPoint& mousePos,
                        double timeSnapDistance,
                        double valueSnapDistance)
{
    if (!ImPlot::IsPlotHovered() || visibleChannelIndices.empty()) {
        return;
    }
    const auto hovered = findHoverReadout(snapshot,
                                          displayData,
                                          visibleChannelIndices,
                                          bitLayout,
                                          mousePos.x,
                                          mousePos.y,
                                          timeSnapDistance,
                                          valueSnapDistance,
                                          view.preferWaveformHoverReadout,
                                          view.bitDisplayReadoutPolicy,
                                          view.showHoverReadout);
    if (!hovered.has_value() || hovered->readout.channelIndex >= snapshot.channels.size()) {
        return;
    }

    const auto& readout = hovered->readout;
    const auto& hoveredChannel = snapshot.channels[readout.channelIndex];
    if (hovered->kind == HoverReadoutKind::BitLane && readout.bit.has_value()) {
        const auto& laneInfo = *readout.bit;
        ImPlot::Annotation(readout.time,
                           readout.displayValue,
                           ImVec4(1.0F, 1.0F, 0.2F, 1.0F),
                           ImVec2(12.0F, -12.0F),
                           true,
                           "%s.%zu = %s",
                           hoveredChannel.label.c_str(),
                           laneInfo.bitIndex,
                           laneInfo.value ? "1" : "0");
        return;
    }

    ImPlot::Annotation(readout.time,
                       readout.displayValue,
                       ImVec4(1.0F, 1.0F, 0.2F, 1.0F),
                       ImVec2(12.0F, -12.0F),
                       true,
                       "%s t=%s y=%.6g %s",
                       hoveredChannel.label.c_str(),
                       formatMetricText(readout.time, displayData.timeUnit.c_str()).c_str(),
                       readout.value,
                       hoveredChannel.unit.c_str());
    if (view.showCursors && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        view.measurementChannelIndex = readout.channelIndex;
    }
}

namespace {

    class ScopedImPlotInputMap {
    public:
        explicit ScopedImPlotInputMap(const plot::WaveControlMode controlMode)
            : inputMap_(ImPlot::GetInputMap()), savedInputMap_(inputMap_)
        {
            inputMap_.Fit = resolveMainPlotFitMouseButton(
                controlMode, ImGui::GetIO().KeyShift, savedInputMap_.Fit, ImGuiMouseButton_Middle);
            if (controlMode == plot::WaveControlMode::Oscilloscope) {
                inputMap_.PanMod = ImGuiMod_Ctrl;
                inputMap_.ZoomMod = ImGuiMod_Ctrl;
            }
        }

        ScopedImPlotInputMap(const ScopedImPlotInputMap&) = delete;
        ScopedImPlotInputMap& operator=(const ScopedImPlotInputMap&) = delete;

        ~ScopedImPlotInputMap() { inputMap_ = savedInputMap_; }

    private:
        ImPlotInputMap& inputMap_;
        ImPlotInputMap savedInputMap_;
    };

    struct SplitPlotInteractionContext {
        std::size_t channelIndex{0};
        bool plotHovered{false};
        ImPlotRect limits{};
        ImPlotPoint mousePos{};
        const BitLaneLayout* bitLayout{nullptr};
        double timeSnapDistance{0.0};
        double smartSnapDistance{0.0};
        double valueSnapDistance{0.0};
    };

    void toggleWaveFft(plot::WaveDockState& wave)
    {
        auto& view = wave.view;
        view.fft.enabled = !view.fft.enabled;
        if (view.fft.enabled) {
            view.fftSourceMinTime = view.viewMinTime;
            view.fftSourceMaxTime = view.viewMaxTime;
            view.fftSourceWindowValid = true;
            view.fftViewportInitialized = false;
        } else {
            view.fftSourceWindowValid = false;
            view.fftViewportInitialized = false;
        }
        wave.cachedFftKeyValid = false;
    }

    void drawMainPlotContextMenu(plot::WaveDockState& wave, WaveFrameState* frameState)
    {
        auto& view = wave.view;
        const ImVec2 rightDrag = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right);
        const bool rightDragged = std::hypot(rightDrag.x, rightDrag.y) > 4.0F;
        const bool canOpen = ImPlot::IsPlotHovered() && !view.zoomSelectionActive && !view.zoomSelectionDragging &&
                             !ImGui::IsAnyItemActive() && !ImGui::IsMouseDragging(ImGuiMouseButton_Right) &&
                             !rightDragged;
        if (canOpen && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("##wave_main_plot_context");
        }

        if (!ImGui::BeginPopup("##wave_main_plot_context")) {
            return;
        }

        if (ImGui::MenuItem("适配可见波形")) {
            view.fitVisibleWaveformsRequested = true;
        }
        if (ImGui::MenuItem("恢复自动跟随")) {
            view.autoFollowLatest = true;
            view.viewportAnimation.active = false;
        }
        if (ImGui::MenuItem(view.zoomSelectionActive ? "关闭框选放大" : "开启框选放大")) {
            view.zoomSelectionActive = !view.zoomSelectionActive;
            view.zoomSelectionDragging = false;
        }
        if (ImGui::MenuItem(view.cursors[0].enabled ? "隐藏 A 游标" : "显示 A 游标")) {
            view.cursors[0].enabled = !view.cursors[0].enabled;
        }
        if (ImGui::MenuItem(view.cursors[1].enabled ? "隐藏 B 游标" : "显示 B 游标")) {
            view.cursors[1].enabled = !view.cursors[1].enabled;
        }
        if (ImGui::MenuItem(view.fft.enabled ? "关闭 FFT" : "开启 FFT")) {
            toggleWaveFft(wave);
        }
        if (ImGui::MenuItem("清空当前波形历史") && frameState != nullptr) {
            frameState->resetHistoryRequested = true;
        }

        ImGui::EndPopup();
    }

    double cursorDisplayAnchorFromActualValue(const plot::ChannelView& spec,
                                              plot::WaveDisplayFormula formula,
                                              double actualValue)
    {
        if (formula == plot::WaveDisplayFormula::OffsetThenScale) {
            return (actualValue + spec.offset) * spec.scale;
        }
        return actualValue * spec.scale + spec.offset;
    }

    double cursorSearchAnchorY(const plot::WaveCursorState& cursor,
                               const std::optional<plot::CursorReadout>& previousReadout,
                               const plot::WaveSnapshot& snapshot,
                               double mouseY,
                               bool held)
    {
        if (held) {
            return mouseY;
        }
        if (previousReadout.has_value()) {
            if (previousReadout->bit.has_value() && std::isfinite(previousReadout->bit->y)) {
                return previousReadout->bit->y;
            }
            if (std::isfinite(previousReadout->displayValue)) {
                return previousReadout->displayValue;
            }
        }
        if (cursor.channelIndex < snapshot.channels.size() &&
            !bitDisplayEnabled(snapshot.channels[cursor.channelIndex].bitDisplay)) {
            // 核心流程：游标状态保存实际读数，重查波形点时必须换回当前显示坐标。
            return cursorDisplayAnchorFromActualValue(
                snapshot.channels[cursor.channelIndex], snapshot.config.displayFormula, cursor.value);
        }
        return cursor.value;
    }

} // namespace

bool handlePlotCursorsImpl(plot::WaveViewState& view,
                           const plot::WaveSnapshot& snapshot,
                           const plot::WaveDisplayData& displayData,
                           const BitLaneLayout& bitLayout,
                           const ImPlotPoint& mousePos,
                           const ImPlotRect& limits,
                           double timeSnapDistance,
                           double smartSnapDistance,
                           double valueSnapDistance,
                           std::array<std::optional<plot::CursorReadout>, 2>& cursorReadouts,
                           std::optional<std::size_t> splitChannelIndex)
{
    if (!view.showCursors) {
        if (!splitChannelIndex.has_value()) {
            view.measurementCursorReadoutRefreshPending = false;
        }
        return false;
    }
    clampActiveChannel(view, snapshot.channels.size());

    const auto& io = ImGui::GetIO();
    const bool timeRefreshPending = view.measurementCursorReadoutRefreshPending && !splitChannelIndex.has_value();
    bool anyCursorInteractionClaimed = false;
    for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
        auto& cursor = view.cursors[cursorIndex];
        if (!cursor.enabled) {
            cursorReadouts[cursorIndex].reset();
            continue;
        }
        std::optional<plot::CursorReadout> smartSnap;
        std::string_view snapLabel;
        const bool smartSnapActive = cursorSmartSnapActive(view, io);
        bool clicked = false;
        bool hovered = false;
        bool held = false;
        double dragTime = cursor.time;
        ImPlotDragToolFlags dragFlags = ImPlotDragToolFlags_NoFit;
        if (smartSnapActive) {
            dragFlags |= ImPlotDragToolFlags_Delayed;
        }
        const ImVec4 cursorColor =
            cursorIndex == 0 ? ImVec4(1.0F, 0.761F, 0.278F, 1.0F) : ImVec4(0.0F, 0.722F, 1.0F, 1.0F);
        // 核心流程：分屏每行必须使用独立 DragLine ID，避免同帧多个子图共享 ImPlot 状态。
        const int dragId = splitChannelIndex.has_value() ? splitCursorDragId(*splitChannelIndex, cursorIndex)
                                                         : static_cast<int>(100 + cursorIndex);
        ImPlot::DragLineX(
            dragId, &dragTime, cursorColor, (hovered || held) ? 2.0F : 1.0F, dragFlags, &clicked, &hovered, &held);
        anyCursorInteractionClaimed = anyCursorInteractionClaimed || clicked || held;
        if (splitChannelIndex.has_value() && !held && !hovered && !ImPlot::IsPlotHovered() &&
            (!cursor.pinned || cursor.channelIndex != *splitChannelIndex)) {
            continue;
        }
        if (held && view.fft.enabled && view.fft.displayMode == plot::WaveFftDisplayMode::CursorSplit) {
            view.lastCursorFftAnchorIndex = cursorIndex;
        }
        if (held && smartSnapActive) {
            // 核心流程：先用 DragLineX 写入的鼠标时间查吸附，再回写游标时间，配合 Delayed 让绘制使用受约束位置。
            auto smartSnapTarget = findSmartCursorSnapByScope(snapshot,
                                                              displayData,
                                                              view,
                                                              bitLayout,
                                                              dragTime,
                                                              mousePos.y,
                                                              limits,
                                                              smartSnapDistance,
                                                              splitChannelIndex);
            if (smartSnapTarget.has_value()) {
                smartSnap = smartSnapTarget->readout;
                snapLabel = smartSnapTarget->label;
            }
        }
        cursor.time = held ? plot::applyCursorDragSnap(dragTime, smartSnap) : dragTime;
        if (held && !view.cursorIntervalLocked && view.cursors[0].enabled && view.cursors[1].enabled) {
            view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
        }

        std::optional<plot::CursorReadout> best;
        if (timeRefreshPending && !held) {
            // 核心流程：跟随滚动后的读数刷新不再依赖旧 Y 值，优先按游标时间重新绑定采样点。
            best = findMeasurementCursorReadoutByTimeRefresh(displayData, view, cursor, timeSnapDistance);
        } else {
            const double searchY = cursorSearchAnchorY(cursor, cursorReadouts[cursorIndex], snapshot, mousePos.y, held);
            const bool allowActiveChannelTimeFallback =
                view.cursorSnapScope == plot::WaveCursorSnapScope::ActiveChannel && !held &&
                (cursor.channelIndex >= snapshot.channels.size() ||
                 cursor.channelIndex != view.measurementChannelIndex) &&
                view.measurementChannelIndex < snapshot.channels.size() &&
                !bitDisplayEnabled(snapshot.channels[view.measurementChannelIndex].bitDisplay);
            best = findNearestCursorByScope(snapshot,
                                            displayData,
                                            view,
                                            bitLayout,
                                            cursor.time,
                                            searchY,
                                            timeSnapDistance,
                                            valueSnapDistance,
                                            allowActiveChannelTimeFallback,
                                            splitChannelIndex);
        }
        if (smartSnap.has_value()) {
            best = smartSnap;
        }
        if (!best.has_value()) {
            cursorReadouts[cursorIndex].reset();
            continue;
        }
        // 核心流程：每帧都刷新游标读数；拖动中保留连续时间，避免采样点吸附导致抖动。
        cursor.channelIndex = best->channelIndex;
        if (!held || smartSnapActive) {
            cursor.time = best->time;
        }
        cursor.value = best->bit.has_value() ? best->displayValue : best->value;
        if (held && view.cursorIntervalLocked && view.lockedCursorInterval > 0.0) {
            auto& pairedCursor = view.cursors[cursorIndex == 0 ? 1 : 0];
            plot::lockCursorInterval(cursor.time, pairedCursor.time, view.lockedCursorInterval, cursorIndex == 0);
        }
        if (held) {
            best->time = cursor.time;
        }
        cursorReadouts[cursorIndex] = best;
        if (shouldDrawCursorReadoutAnnotation(held, cursor.pinned)) {
            if (best->bit.has_value()) {
                const auto& laneInfo = *best->bit;
                const auto& bitChannel = snapshot.channels[laneInfo.parentChannelIndex];
                const std::string snapText = snapLabel.empty() ? "" : std::string(snapLabel) + " ";
                const std::string timeText = formatMetricText(best->time, displayData.timeUnit.c_str());
                ImPlot::Annotation(best->time,
                                   best->displayValue,
                                   ImVec4(1.0F, 1.0F, 1.0F, 0.92F),
                                   ImVec2(10.0F, cursorIndex == 0 ? -18.0F : 18.0F),
                                   true,
                                   "%c %s%s.%zu %s\nvalue %d",
                                   cursorIndex == 0 ? 'A' : 'B',
                                   snapText.c_str(),
                                   bitChannel.label.c_str(),
                                   laneInfo.bitIndex,
                                   timeText.c_str(),
                                   laneInfo.value ? 1 : 0);
            } else {
                drawCursorAnnotation(
                    cursorIndex, *best, snapshot.channels[best->channelIndex], displayData.timeUnit, snapLabel);
            }
        }
    }
    if (timeRefreshPending) {
        view.measurementCursorReadoutRefreshPending = false;
    }
    return anyCursorInteractionClaimed;
}

bool handlePlotCursors(plot::WaveViewState& view,
                       const plot::WaveSnapshot& snapshot,
                       const plot::WaveDisplayData& displayData,
                       const BitLaneLayout& bitLayout,
                       const ImPlotPoint& mousePos,
                       const ImPlotRect& limits,
                       double timeSnapDistance,
                       double smartSnapDistance,
                       double valueSnapDistance,
                       std::array<std::optional<plot::CursorReadout>, 2>& cursorReadouts)
{
    return handlePlotCursorsImpl(view,
                                 snapshot,
                                 displayData,
                                 bitLayout,
                                 mousePos,
                                 limits,
                                 timeSnapDistance,
                                 smartSnapDistance,
                                 valueSnapDistance,
                                 cursorReadouts,
                                 std::nullopt);
}

bool handleSplitPlotCursors(plot::WaveViewState& view,
                            const plot::WaveSnapshot& snapshot,
                            const plot::WaveDisplayData& displayData,
                            const SplitPlotInteractionContext& context,
                            std::array<std::optional<plot::CursorReadout>, 2>& cursorReadouts)
{
    if (context.bitLayout == nullptr) {
        return false;
    }
    return handlePlotCursorsImpl(view,
                                 snapshot,
                                 displayData,
                                 *context.bitLayout,
                                 context.mousePos,
                                 context.limits,
                                 context.timeSnapDistance,
                                 context.smartSnapDistance,
                                 context.valueSnapDistance,
                                 cursorReadouts,
                                 context.channelIndex);
}

void drawOscilloscopeGrid(const ImPlotRect& limits)
{
    if (!std::isfinite(limits.X.Min) || !std::isfinite(limits.X.Max) || !std::isfinite(limits.Y.Min) ||
        !std::isfinite(limits.Y.Max) || limits.X.Max == limits.X.Min || limits.Y.Max == limits.Y.Min) {
        return;
    }

    auto* drawList = ImPlot::GetPlotDrawList();
    if (drawList == nullptr) {
        return;
    }
    const auto& waveTokens = activeWaveStyleTokens();
    const ImU32 tickColor = ImGui::ColorConvertFloat4ToU32(waveTokens.gridMinorTick);
    const ImU32 majorColor = ImGui::ColorConvertFloat4ToU32(waveTokens.gridMajor);
    const ImU32 centerColor = ImGui::ColorConvertFloat4ToU32(waveTokens.gridCenter);
    const double xStep = (limits.X.Max - limits.X.Min) / static_cast<double>(plot::kWaveGridMajorXDivisions);
    const double yStep = (limits.Y.Max - limits.Y.Min) / static_cast<double>(plot::kWaveGridMajorYDivisions);
    const int xMinorCount = plot::kWaveGridMajorXDivisions * plot::kWaveGridMinorDivisionsPerMajor;
    const int yMinorCount = plot::kWaveGridMajorYDivisions * plot::kWaveGridMinorDivisionsPerMajor;

    ImPlot::PushPlotClipRect();
    for (int index = 0; index <= plot::kWaveGridMajorXDivisions; ++index) {
        const double x = limits.X.Min + xStep * static_cast<double>(index);
        const bool center = index == plot::kWaveGridMajorXDivisions / 2;
        drawList->AddLine(ImPlot::PlotToPixels(x, limits.Y.Min),
                          ImPlot::PlotToPixels(x, limits.Y.Max),
                          center ? centerColor : majorColor,
                          center ? waveTokens.gridCenterWidth : waveTokens.gridMajorWidth);
    }
    for (int index = 0; index <= plot::kWaveGridMajorYDivisions; ++index) {
        const double y = limits.Y.Min + yStep * static_cast<double>(index);
        const bool center = index == plot::kWaveGridMajorYDivisions / 2;
        drawList->AddLine(ImPlot::PlotToPixels(limits.X.Min, y),
                          ImPlot::PlotToPixels(limits.X.Max, y),
                          center ? centerColor : majorColor,
                          center ? waveTokens.gridCenterWidth : waveTokens.gridMajorWidth);
    }
    // 核心绘制规则：不再贯穿次网格线，只在主网格线上用短刻度标出每大格五等分。
    for (int majorIndex = 0; majorIndex <= plot::kWaveGridMajorXDivisions; ++majorIndex) {
        const double x = limits.X.Min + xStep * static_cast<double>(majorIndex);
        for (int minorIndex = 1; minorIndex < yMinorCount; ++minorIndex) {
            if (minorIndex % plot::kWaveGridMinorDivisionsPerMajor == 0) {
                continue;
            }
            const double y = limits.Y.Min + (limits.Y.Max - limits.Y.Min) * static_cast<double>(minorIndex) /
                                                static_cast<double>(yMinorCount);
            const ImVec2 pixel = ImPlot::PlotToPixels(x, y);
            drawList->AddLine(ImVec2(pixel.x - waveTokens.gridMinorTickHalfLength, pixel.y),
                              ImVec2(pixel.x + waveTokens.gridMinorTickHalfLength, pixel.y),
                              tickColor,
                              waveTokens.gridMinorTickWidth);
        }
    }
    for (int majorIndex = 0; majorIndex <= plot::kWaveGridMajorYDivisions; ++majorIndex) {
        const double y = limits.Y.Min + yStep * static_cast<double>(majorIndex);
        for (int minorIndex = 1; minorIndex < xMinorCount; ++minorIndex) {
            if (minorIndex % plot::kWaveGridMinorDivisionsPerMajor == 0) {
                continue;
            }
            const double x = limits.X.Min + (limits.X.Max - limits.X.Min) * static_cast<double>(minorIndex) /
                                                static_cast<double>(xMinorCount);
            const ImVec2 pixel = ImPlot::PlotToPixels(x, y);
            drawList->AddLine(ImVec2(pixel.x, pixel.y - waveTokens.gridMinorTickHalfLength),
                              ImVec2(pixel.x, pixel.y + waveTokens.gridMinorTickHalfLength),
                              tickColor,
                              waveTokens.gridMinorTickWidth);
        }
    }
    ImPlot::PopPlotClipRect();
}

std::optional<plot::CursorReadout> findSplitBitCursorReadout(const plot::WaveSnapshot& snapshot,
                                                             const plot::WaveDisplayData& displayData,
                                                             std::size_t channelIndex,
                                                             double time,
                                                             double maxTimeDistance,
                                                             const std::optional<plot::CursorReadout>& preferredReadout)
{
    if (channelIndex >= snapshot.channels.size() || channelIndex >= displayData.channels.size() ||
        !std::isfinite(time) || !std::isfinite(maxTimeDistance) || maxTimeDistance < 0.0) {
        return std::nullopt;
    }
    const auto& sourceChannel = snapshot.channels[channelIndex];
    if (!bitDisplayEnabled(sourceChannel.bitDisplay)) {
        return std::nullopt;
    }
    const auto& displayChannel = displayData.channels[channelIndex];
    if (displayChannel.source) {
        plot::WaveDisplayData exact;
        exact.channels.resize(displayData.channels.size());
        exact.channels[channelIndex] = plot::extractDisplayWindow(displayChannel, time, time, true);
        auto exactSnapshot = snapshot;
        if (!exact.channels[channelIndex].sourceIndices.empty()) {
            exactSnapshot.channels[channelIndex].visibleBegin = exact.channels[channelIndex].sourceIndices.front();
            exactSnapshot.channels[channelIndex].visibleEnd = exact.channels[channelIndex].sourceIndices.back() + 1;
        }
        return findSplitBitCursorReadout(exactSnapshot, exact, channelIndex, time, maxTimeDistance, preferredReadout);
    }
    const auto& samples = displayChannel.samples;
    if (samples.empty()) {
        return std::nullopt;
    }

    const auto lower =
        std::lower_bound(samples.begin(), samples.end(), time, [](const plot::WaveSample& sample, double value) {
            return sample.time < value;
        });
    auto best = samples.begin();
    if (lower == samples.end()) {
        best = std::prev(samples.end());
    } else if (lower == samples.begin()) {
        best = lower;
    } else {
        const auto previous = std::prev(lower);
        best = std::abs(previous->time - time) <= std::abs(lower->time - time) ? previous : lower;
    }
    if (std::abs(best->time - time) > maxTimeDistance) {
        return std::nullopt;
    }

    std::size_t laneIndex = 0;
    std::size_t bitIndex = sourceChannel.bitDisplay.firstBit;
    if (preferredReadout.has_value() && preferredReadout->bit.has_value()) {
        const auto& preferredBit = *preferredReadout->bit;
        if (preferredBit.parentChannelIndex == channelIndex &&
            preferredBit.laneIndex < sourceChannel.bitDisplay.bitCount) {
            laneIndex = preferredBit.laneIndex;
            bitIndex = sourceChannel.bitDisplay.firstBit + laneIndex;
        }
    }

    double rawValue = best->value;
    std::size_t sourceSampleIndex = static_cast<std::size_t>(std::distance(samples.begin(), best));
    if (sourceChannel.samples != nullptr) {
        const std::size_t begin = (std::min)(sourceChannel.visibleBegin, sourceChannel.totalSamples);
        const std::size_t mappedSourceIndex = begin + sourceSampleIndex;
        if (mappedSourceIndex < sourceChannel.totalSamples) {
            sourceSampleIndex = mappedSourceIndex;
            rawValue = sourceChannel.samples[mappedSourceIndex].value;
        }
    } else if (sourceSampleIndex < displayChannel.actualValues.size()) {
        rawValue = displayChannel.actualValues[sourceSampleIndex];
    }

    const bool value = rawBitEnabled(rawValue, bitIndex);
    const double displayY = value ? 1.0 : 0.0;
    return plot::CursorReadout{
        .valid = true,
        .channelIndex = channelIndex,
        .sampleIndex = sourceSampleIndex,
        .time = best->time,
        .value = value ? 1.0 : 0.0,
        .displayValue = displayY,
        .bit =
            plot::BitLaneReadout{
                .parentChannelIndex = channelIndex,
                .bitIndex = bitIndex,
                .laneIndex = laneIndex,
                .value = value,
                .y = displayY,
            },
    };
}

std::optional<plot::MeasurementReadout> requestWaveMeasurement(plot::WaveDockState& wave,
    const plot::WaveDisplayData& display, std::size_t channel, double begin, double end,
    std::optional<std::size_t> reference, std::optional<double> manual)
{
    auto& view = wave.view;
    if (channel >= display.channels.size() || !std::isfinite(begin) || !std::isfinite(end)) {
        ++wave.measurementRequestGeneration;
        wave.measurementKeyValid = false;
        wave.measurementRequestActive = false;
        wave.cachedMeasurement.reset();
        view.measurementUpdatePending = false;
        return std::nullopt;
    }
    if (end < begin) std::swap(begin, end);
    const auto ratio = [&](std::size_t i) {
        return i < display.channels.size() && display.channels[i].source ? display.channels[i].source->ratio : 1.0;
    };
    const plot::WaveDockState::MeasurementKey key{channel, begin, end, reference, manual,
        view.sampleFrequencyHz, ratio(channel), reference ? ratio(*reference) : 1.0};
    // 区间或通道变化后立即作废缓存和在途结果，拖动期间只保留轻量读数。
    // 同一区间的持续采集仍沿用原有刷新调度，避免每帧追加数据使后台结果永久失效。
    if (!wave.measurementKeyValid || !(wave.measurementKey == key)) {
        wave.measurementKey = key;
        wave.measurementKeyValid = true;
        ++wave.measurementRequestGeneration;
        wave.measurementRequestActive = false;
        wave.measurementDataRevision = (std::numeric_limits<std::uint64_t>::max)();
        wave.cachedMeasurement.reset();
    }
    if (wave.analysisWorker) {
        if (auto output = wave.analysisWorker->takeMeasurement();
            output && output->generation == wave.measurementRequestGeneration) {
            wave.cachedMeasurement = std::move(output->result);
            wave.measurementRequestActive = false;
        }
    }
    view.measurementUpdatePending = wave.measurementRequestActive || !wave.cachedMeasurement.has_value() ||
        wave.measurementDataRevision != wave.buffer.analysisRevision();
    const bool interacting = view.interactionActive || ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
        ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (view.measurementUpdatePending && !interacting && !wave.measurementRequestActive) {
        const auto exact = plot::extractDisplayWindow(display.channels[channel], begin, end);
        plot::WaveMeasurementInput input{wave.measurementRequestGeneration, channel, {}, {}, {}};
        for (std::size_t i = 0; i < exact.samples.size(); ++i) {
            if (exact.samples[i].time < begin || exact.samples[i].time > end) continue;
            input.times.push_back(exact.samples[i].time);
            input.values.push_back(i < exact.actualValues.size() ? exact.actualValues[i] : exact.samples[i].value);
        }
        if (manual) input.reference.assign(input.values.size(), *manual);
        else if (reference && *reference < display.channels.size()) {
            const auto ref = plot::extractDisplayWindow(display.channels[*reference], begin, end);
            std::size_t i = 0;
            for (const auto time : input.times) {
                while (i < ref.samples.size() && ref.samples[i].time < time) ++i;
                if (i == ref.samples.size() || std::abs(ref.samples[i].time - time) > 1e-9) {
                    input.reference.clear();
                    break;
                }
                input.reference.push_back(i < ref.actualValues.size() ? ref.actualValues[i] : ref.samples[i].value);
            }
        }
        if (!wave.analysisWorker) wave.analysisWorker = std::make_shared<plot::WaveAnalysisWorker>();
        wave.analysisWorker->submit(std::move(input));
        ++wave.measurementSubmittedCount;
        wave.measurementRequestActive = true;
        wave.measurementDataRevision = wave.buffer.analysisRevision();
    }
    return wave.cachedMeasurement;
}

void updateSplitMeasurementResult(plot::WaveDockState& wave,
                                  const plot::WaveDisplayData& displayData,
                                  PlotRenderResult& result,
                                  std::optional<std::size_t> measurementChannelOverride = std::nullopt)
{
    const auto& view = wave.view;
    result.bitMeasurementActive = false;
    result.measurement.reset();
    if (!view.showCursors || !cursorPairHasCompleteReadouts(result.cursorReadouts)) {
        return;
    }

    if (cursorPairUsesBitLanes(result.cursorReadouts)) {
        result.bitMeasurementActive = true;
        result.measurement = makeBitIntervalMeasurement(*result.cursorReadouts[0], *result.cursorReadouts[1]);
        return;
    }

    const auto referenceChannelIndex = view.referenceMode == plot::WaveMeasurementReferenceMode::Channel
                                           ? std::optional<std::size_t>(view.referenceChannelIndex)
                                           : std::nullopt;
    const auto manualReferenceValue = view.referenceMode == plot::WaveMeasurementReferenceMode::ManualValue
                                          ? std::optional<double>(view.manualReferenceValue)
                                          : std::nullopt;
    result.measurement = requestWaveMeasurement(wave, displayData,
                                              measurementChannelOverride.value_or(view.measurementChannelIndex),
                                              result.cursorReadouts[0]->time,
                                              result.cursorReadouts[1]->time,
                                              referenceChannelIndex,
                                              manualReferenceValue);
}

void updateSplitCursorReadoutsForChannel(const plot::WaveSnapshot& snapshot,
                                         const plot::WaveDisplayData& displayData,
                                         const plot::WaveViewState& view,
                                         std::size_t channelIndex,
                                         double maxTimeDistance,
                                         PlotRenderResult& result)
{
    for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
        const auto preferredReadout = result.cursorReadouts[cursorIndex];
        result.cursorReadouts[cursorIndex].reset();
        if (!view.cursors[cursorIndex].enabled || channelIndex >= snapshot.channels.size() ||
            channelIndex >= displayData.channels.size()) {
            continue;
        }
        if (bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
            result.cursorReadouts[cursorIndex] = findSplitBitCursorReadout(
                snapshot,
                displayData,
                channelIndex,
                view.cursors[cursorIndex].time,
                maxTimeDistance,
                preferredReadout);
        } else {
            result.cursorReadouts[cursorIndex] = plot::findNearestDisplayByTime(
                displayData, channelIndex, view.cursors[cursorIndex].time, maxTimeDistance);
        }
    }
}

struct SplitPlotRowOutcome {
    bool cursorHeld{false};
    bool userInteracting{false};
    bool viewportChanged{false};
};

SplitPlotRowOutcome drawSplitChannelPlot(plot::WaveDockState& wave,
                                         const WaveFrameData& frame,
                                         const plot::WaveSnapshot& snapshot,
                                         const plot::WaveDisplayData& displayData,
                                         const WavePlotOverlayPolicy& overlayPolicy,
                                         WaveFrameState* frameState,
                                         const std::vector<std::size_t>& visibleChannels,
                                         std::size_t rowIndex,
                                         float plotHeight,
                                         bool mouseInsideSplitRegion,
                                         bool viewportChangedBeforeRow,
                                         PlotRenderResult& result)
{
    SplitPlotRowOutcome outcome;
    auto& view = wave.view;
    const std::size_t channelIndex = visibleChannels[rowIndex];
    if (channelIndex >= snapshot.channels.size() || channelIndex >= displayData.channels.size()) {
        return outcome;
    }
    const auto& channel = snapshot.channels[channelIndex];
    const auto& samples = displayData.channels[channelIndex].samples;
    if (samples.empty()) {
        return outcome;
    }

    ImPlot::PushStyleColor(ImPlotCol_PlotBg, activeWaveStyleTokens().plotBackground);
    const std::string plotId = "##wave_split_" + std::to_string(channelIndex);
    if (ImPlot::BeginPlot(plotId.c_str(), ImVec2(-1.0F, plotHeight), ImPlotFlags_NoLegend)) {
        constexpr ImPlotAxisFlags xFlags = ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoGridLines;
        constexpr ImPlotAxisFlags yFlags = ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoLabel |
                                           ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks |
                                           ImPlotAxisFlags_NoTickLabels;
        const bool bottomRow = rowIndex + 1U == visibleChannels.size();
        const char* xAxisLabel = nullptr;
        if (bottomRow && view.showAxisLabels) {
            xAxisLabel = displayData.timeUnit == "sample" ? "Sample" : "Time";
        }
        ImPlot::SetupAxis(ImAxis_X1, xAxisLabel, bottomRow ? xFlags : (xFlags | ImPlotAxisFlags_NoTickLabels));
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, yFlags);
        ImPlot::SetupAxisLimits(
            ImAxis_X1,
            view.viewMinTime,
            view.viewMaxTime,
            (view.autoFollowLatest || view.forceNextMainPlotLimits) ? ImPlotCond_Always : ImPlotCond_Once);

        const bool bitChannel = bitDisplayEnabled(channel.bitDisplay);
        if (bitChannel) {
            ImPlot::SetupAxisLimits(ImAxis_Y1, 0.0, 1.0, ImPlotCond_Always);
        } else {
            const auto baseline = currentViewport(view);
            ImPlot::SetupAxisLimits(ImAxis_Y1, baseline.minValue, baseline.maxValue,
                                    view.forceNextMainPlotLimits ? ImPlotCond_Always : ImPlotCond_Once);
        }

        const auto verticalBaseline = currentViewport(view);
        const ImPlotRect limits = ImPlot::GetPlotLimits();
        drawOscilloscopeGrid(limits);
        const ImVec4 color = channelColor(channel, channelIndex);
        BitLaneLayout bitLayout;
        if (bitChannel) {
            updateBitTransitionCounts(wave, channel, channelIndex, displayData.axisSource, limits.X.Min, limits.X.Max);
            const std::vector<std::size_t> bitChannelIndices{channelIndex};
            bitLayout =
                buildBitLaneLayout(snapshot, bitChannelIndices, limits, ImPlot::GetPlotPos(), ImPlot::GetPlotSize());
            const ImVec2 plotSize = ImPlot::GetPlotSize();
            const auto plotPixelWidth = static_cast<std::size_t>((std::max)(plotSize.x, 1.0F));
            const auto vertexBudget = bitGeometryBudget(frame.renderBudget, channel.bitDisplay.bitCount);
            const auto& entry = cachedBitRenderEntry(wave,
                                                     channel,
                                                     displayData.channels[channelIndex],
                                                     bitLayout,
                                                     channelIndex,
                                                     limits,
                                                     plotPixelWidth,
                                                     vertexBudget);
            std::size_t renderedPoints = 0;
            for (const auto& lane : entry.lanes) {
                renderedPoints += lane.size();
            }
            view.lastRenderSourceSampleCount += entry.sourceSampleCount;
            view.lastRenderPointCount += renderedPoints;
            drawBitRenderLanes(entry, color, plot::resolveChannelLineWidth(channel), bitLayout);
            const ImU32 labelColor = ImGui::ColorConvertFloat4ToU32(activeWaveStyleTokens().bitLabel);
            drawBitLaneLabels(wave, bitLayout, limits, labelColor);
        } else {
            const bool legacyEnvelope = !view.peakDetectDownsample &&
                channel.visibleEnd - channel.visibleBegin > frame.renderBudget.pointsPerChannel;
            if (legacyEnvelope) {
                const auto& envelope = cachedRenderEnvelope(wave, channel, channelIndex, samples, limits,
                                                             frame.renderBudget.pointsPerChannel, nullptr);
                if (view.glowEnabled) renderGlowEnvelope(envelope, color, view.glowIntensity, plot::resolveChannelLineWidth(channel));
                else renderEnvelopeAsBars(envelope, color, plot::resolveChannelLineWidth(channel));
            } else {
            WaveSampleGetterPayload payload{.samples = samples.data()};
            ImPlotSpec spec{};
            spec.LineColor = color;
            spec.LineWeight = plot::resolveChannelLineWidth(channel);
            const auto itemLabel = waveChannelItemLabel(channel.label, channelIndex);
            ImPlot::PlotLineG(itemLabel.c_str(),
                              reinterpret_cast<ImPlotGetter>(&waveSampleGetter),
                              &payload,
                              static_cast<int>(samples.size()),
                              spec);
            if (view.glowEnabled) {
                renderGlowSamples(samples.data(), samples.size(), color, view.glowIntensity, spec.LineWeight);
            }
            }
        }

        auto* drawList = ImPlot::GetPlotDrawList();
        const ImVec2 plotPos = ImPlot::GetPlotPos();
        const ImVec2 plotSize = ImPlot::GetPlotSize();
        if (!bitChannel) {
            drawList->AddText(ImVec2(plotPos.x + 8.0F, plotPos.y + 6.0F),
                              ImGui::ColorConvertFloat4ToU32(activeWaveStyleTokens().splitChannelLabel),
                              ("CH" + std::to_string(channelIndex + 1U) + "  " + channel.label).c_str());
        }

        const ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
        const double visibleTimeWidth = std::abs(limits.X.Max - limits.X.Min);
        const double timeSnapDistance = visibleTimeWidth / 80.0;
        const double smartSnapDistance = (std::max)(timeSnapDistance, visibleTimeWidth * 0.02);
        const double valueSnapDistance = (limits.Y.Max - limits.Y.Min) / 30.0;
        const std::vector<std::size_t> splitChannelIndices{channelIndex};
        const bool plotHovered = ImPlot::IsPlotHovered();
        handleWheelFineAdjustmentShortcut(view,
                                          ImGui::GetIO().KeyShift,
                                          ImGui::IsMouseClicked(ImGuiMouseButton_Middle),
                                          plotHovered,
                                          ImPlot::IsAxisHovered(ImAxis_X1),
                                          ImPlot::IsAxisHovered(ImAxis_Y1));
        outcome.viewportChanged = handleMainPlotZoom(view, mousePos);
        if (outcome.viewportChanged) {
            view.forceNextMainPlotLimits = true;
        }
        handleHoverReadout(
            view, snapshot, displayData, splitChannelIndices, bitLayout, mousePos, timeSnapDistance, valueSnapDistance);
        const SplitPlotInteractionContext interactionContext{
            .channelIndex = channelIndex,
            .plotHovered = plotHovered,
            .limits = limits,
            .mousePos = mousePos,
            .bitLayout = &bitLayout,
            .timeSnapDistance = timeSnapDistance,
            .smartSnapDistance = smartSnapDistance,
            .valueSnapDistance = valueSnapDistance,
        };
        outcome.cursorHeld =
            handleSplitPlotCursors(view, snapshot, displayData, interactionContext, result.cursorReadouts);
        if (plotHovered && !outcome.cursorHeld) {
            view.measurementChannelIndex = channelIndex;
        }
        outcome.viewportChanged = handleOscilloscopeChannelInteractions(
            wave, snapshot, displayData, splitChannelIndices, limits, mousePos,
            timeSnapDistance, valueSnapDistance, outcome.cursorHeld) || outcome.viewportChanged;
        if (!outcome.cursorHeld) {
            const auto selection = handleMainPlotZoomSelection(view, wave.suppressZoomSelectionEscapeThisFrame);
            outcome.viewportChanged = selection.viewportChanged || outcome.viewportChanged;
        }
        auto& splitYAxis = GImPlot->CurrentPlot->YAxis(0);
        if (!bitChannel && !view.lockVerticalRange &&
            (splitYAxis.FitThisFrame || splitYAxis.IsAutoFitting())) {
            fitChannelDisplayRange(wave, snapshot, channelIndex,
                                   (verticalBaseline.minValue + verticalBaseline.maxValue) * 0.5,
                                   (verticalBaseline.maxValue - verticalBaseline.minValue) /
                                       (std::max)(view.verticalAutoFitMultiplier, 1.0));
            splitYAxis.FitThisFrame = false;
            splitYAxis.SetRange(verticalBaseline.minValue, verticalBaseline.maxValue);
            outcome.viewportChanged = true;
        }
        const auto intersectionReadouts =
            collectCursorIntersectionReadouts(view, snapshot, displayData, splitChannelIndices, timeSnapDistance);
        drawCursorIntersectionReadouts(intersectionReadouts, snapshot);
        if (plotHovered || (!mouseInsideSplitRegion && channelIndex == view.measurementChannelIndex)) {
            const double maxCursorReadoutDistance =
                (std::max)(view.viewMaxTime - view.viewMinTime, view.minVisibleTimeSpan) / 80.0;
            updateSplitCursorReadoutsForChannel(
                snapshot, displayData, view, channelIndex, maxCursorReadoutDistance, result);
            updateSplitMeasurementResult(wave, displayData, result, channelIndex);
            result.measurementOverlay = {.pos = plotPos, .size = plotSize, .valid = true};
            if (overlayPolicy.drawMeasurementOverlay) {
                const auto placement =
                    resolveMeasurementOverlayPlacementSize(plotPos, plotSize, overlayPolicy.measurementSafeRightX);
                if (placement.visible) {
                    drawMeasurementOverlay(
                        view, snapshot, displayData, result, plotPos, placement.plotSize, ImPlot::GetPlotDrawList());
                }
            }
        }

        const ImPlotRect updatedLimits = ImPlot::GetPlotLimits();
        if (!viewportChangedBeforeRow && !outcome.viewportChanged &&
            (ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1))) {
            recordMainPlotLimits(view, updatedLimits);
        }
        // 核心流程：ImPlot 交互查询必须在当前子图 EndPlot 前完成，避免分屏结束后访问空 active plot。
        outcome.userInteracting = plotInteractionActive(outcome.cursorHeld);
        ImGui::PushID(static_cast<int>(channelIndex));
        drawMainPlotContextMenu(wave, frameState);
        ImGui::PopID();
        ImPlot::EndPlot();
        if (!bitChannel && !view.lockVerticalRange) {
            if (!outcome.viewportChanged && outcome.userInteracting) {
                view.viewMinValue = updatedLimits.Y.Min;
                view.viewMaxValue = updatedLimits.Y.Max;
            }
            outcome.viewportChanged =
                commitWaveVerticalViewport(wave, verticalBaseline, splitChannelIndices) || outcome.viewportChanged;
        }
    }
    ImPlot::PopStyleColor();
    return outcome;
}

PlotRenderResult drawSplitOscilloscopePlots(plot::WaveDockState& wave,
                                            const WaveFrameData& frame,
                                            const WavePlotOverlayPolicy& overlayPolicy,
                                            WaveFrameState* frameState)
{
    PlotRenderResult result;
    if (frame.displayData == nullptr || frame.fullSnapshot == nullptr || frame.fullSnapshot->channels.empty()) {
        ImGui::TextUnformatted("Lua 尚未通过 proto.plot.setup / proto.plot.push 提供波形数据。");
        return result;
    }

    auto& view = wave.view;
    result.cursorReadouts = view.lastCursorReadouts;
    if (view.phosphorEnabled) {
        view.lastRenderStats.phosphorBackendStatus = "Split 暂不支持";
    }
    const auto& snapshot = frame.snapshot;
    const auto& displayData = *frame.displayData;
    std::vector<std::size_t> visibleChannels = channelIndicesForDerivedViews(wave, snapshot);
    if (visibleChannels.empty()) {
        ImGui::TextUnformatted("所有通道已隐藏。");
        return result;
    }

    const ImVec2 splitPos = ImGui::GetCursorScreenPos();
    const ImVec2 splitSize = ImGui::GetContentRegionAvail();
    result.legendOverlay = {.pos = splitPos, .size = splitSize, .valid = true};
    const bool mouseInsideSplitRegion =
        ImGui::IsMouseHoveringRect(splitPos, ImVec2(splitPos.x + splitSize.x, splitPos.y + splitSize.y), false);
    ScopedImPlotInputMap inputMapGuard(view.controlMode);

    bool viewportChangedThisFrame = false;
    if (frame.overviewDisplayData != nullptr) {
        const auto fitChannelIndices = channelIndicesForDerivedViews(wave, *frame.fullSnapshot);
        viewportChangedThisFrame =
            applyFitVisibleWaveforms(wave, *frame.fullSnapshot, *frame.overviewDisplayData, fitChannelIndices);
    }
    bool anyCursorHeld = false;
    bool userInteractingInAnySplitPlot = false;
    ImGui::BeginChild("##wave_split_scroll", ImVec2(-1.0F, -1.0F), false);
    const float plotHeight = plot::solveSplitWavePlotHeight(
        visibleChannels.size(), ImGui::GetContentRegionAvail().y, ImGui::GetStyle().ItemSpacing.y, 120.0F, 4U);
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotMinSize, ImVec2(64.0F, 24.0F));
    for (std::size_t rowIndex = 0; rowIndex < visibleChannels.size(); ++rowIndex) {
        const auto row = drawSplitChannelPlot(wave,
                                              frame,
                                              snapshot,
                                              displayData,
                                              overlayPolicy,
                                              frameState,
                                              visibleChannels,
                                              rowIndex,
                                              plotHeight,
                                              mouseInsideSplitRegion,
                                              viewportChangedThisFrame,
                                              result);
        viewportChangedThisFrame = row.viewportChanged || viewportChangedThisFrame;
        anyCursorHeld = row.cursorHeld || anyCursorHeld;
        userInteractingInAnySplitPlot = row.userInteracting || userInteractingInAnySplitPlot;
    }
    ImPlot::PopStyleVar();
    if (!viewportChangedThisFrame) {
        view.forceNextMainPlotLimits = false;
    }
    ImGui::EndChild();
    if (overlayPolicy.drawLegendOverlay) {
        drawChannelLegendOverlay(wave, snapshot, splitPos, splitSize, ImGui::GetWindowViewport());
    }

    if (userInteractingInAnySplitPlot) {
        applyAutoFollowPausePolicy(view, WaveViewportAutoFollowPolicy::UserInteraction);
    }

    if (view.showCursors) {
        const double maxDistance = (std::max)(view.viewMaxTime - view.viewMinTime, view.minVisibleTimeSpan) / 80.0;
        for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
            const auto preferredReadout = result.cursorReadouts[cursorIndex];
            result.cursorReadouts[cursorIndex].reset();
            if (!view.cursors[cursorIndex].enabled) {
                continue;
            }
            const auto channelIndex = view.cursors[cursorIndex].channelIndex;
            if (std::ranges::find(visibleChannels, channelIndex) == visibleChannels.end()) {
                continue;
            }
            if (channelIndex < snapshot.channels.size() &&
                bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
                result.cursorReadouts[cursorIndex] = findSplitBitCursorReadout(
                    snapshot,
                    displayData,
                    channelIndex,
                    view.cursors[cursorIndex].time,
                    maxDistance,
                    preferredReadout);
            } else if (channelIndex < displayData.channels.size()) {
                result.cursorReadouts[cursorIndex] = plot::findNearestDisplayByTime(
                    displayData, channelIndex, view.cursors[cursorIndex].time, maxDistance);
            }
        }
        updateSplitMeasurementResult(wave, displayData, result);
    }
    view.measurementCursorReadoutRefreshPending = false;
    view.lastCursorReadouts = result.cursorReadouts;
    result.plotRendered = true;
    return result;
}

void drawCursorFftHighlight(const plot::WaveViewState& view, const ImPlotRect& limits)
{
    if (!view.fft.enabled || view.fft.displayMode != plot::WaveFftDisplayMode::CursorSplit || !view.showCursors ||
        !view.cursors[0].enabled || !view.cursors[1].enabled) {
        return;
    }

    const double minTime = (std::min)(view.cursors[0].time, view.cursors[1].time);
    const double maxTime = (std::max)(view.cursors[0].time, view.cursors[1].time);
    if (maxTime <= minTime) {
        return;
    }

    const auto& rgba = view.cursorFftHighlightRgba;
    const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
    const ImVec2 pixelA = ImPlot::PlotToPixels(minTime, limits.Y.Min);
    const ImVec2 pixelB = ImPlot::PlotToPixels(maxTime, limits.Y.Max);
    ImPlot::GetPlotDrawList()->AddRectFilled(ImVec2((std::min)(pixelA.x, pixelB.x), (std::min)(pixelA.y, pixelB.y)),
                                             ImVec2((std::max)(pixelA.x, pixelB.x), (std::max)(pixelA.y, pixelB.y)),
                                             fillColor);
}

void renderMainWaveContent(plot::WaveDockState& wave,
                           const plot::WaveSnapshot& snapshot,
                           const plot::WaveDisplayData& renderDisplayData,
                           const RenderBudget& renderBudget,
                           const ImPlotRect& limits,
                           std::vector<std::size_t>& visibleChannelIndices,
                           BitLaneLayout& bitLayout)
{
    auto& view = wave.view;
    if (view.phosphorEnabled) {
        resetWaveRenderStats(view, renderBudget);
        visibleChannelIndices = collectVisiblePhosphorAnalogChannels(wave, snapshot, renderDisplayData, limits);
        const bool phosphorRendered =
            renderWavePhosphor(view, snapshot, renderDisplayData, visibleChannelIndices, limits);
        if (phosphorRendered) {
            registerPhosphorAnalogChannels(wave, snapshot, renderDisplayData, limits, visibleChannelIndices);
            renderBitWaveChannels(
                wave, snapshot, renderDisplayData, renderBudget, limits, visibleChannelIndices, bitLayout);
            drawBitLaneLabelsIfNeeded(wave, bitLayout, limits);
            return;
        }

        const std::string phosphorStatus = view.lastRenderStats.phosphorBackendStatus;
        renderWaveChannels(wave, snapshot, renderDisplayData, renderBudget, limits, visibleChannelIndices, bitLayout);
        view.lastRenderStats.phosphorBackendStatus = phosphorStatus;
        return;
    }

    renderWaveChannels(wave, snapshot, renderDisplayData, renderBudget, limits, visibleChannelIndices, bitLayout);
}

void updateMainMeasurementResult(plot::WaveDockState& wave,
                                 const plot::WaveDisplayData& displayData,
                                 PlotRenderResult& result)
{
    const auto& view = wave.view;
    if (!view.showCursors || !result.cursorReadouts[0].has_value() || !result.cursorReadouts[1].has_value()) {
        return;
    }

    result.bitMeasurementActive = cursorPairUsesBitLanes(result.cursorReadouts);
    if (result.bitMeasurementActive) {
        result.measurement = makeBitIntervalMeasurement(*result.cursorReadouts[0], *result.cursorReadouts[1]);
        return;
    }

    const auto referenceChannelIndex = view.referenceMode == plot::WaveMeasurementReferenceMode::Channel
                                           ? std::optional<std::size_t>(view.referenceChannelIndex)
                                           : std::nullopt;
    const auto manualReferenceValue = view.referenceMode == plot::WaveMeasurementReferenceMode::ManualValue
                                          ? std::optional<double>(view.manualReferenceValue)
                                          : std::nullopt;
    result.measurement = requestWaveMeasurement(wave, displayData,
                                              view.measurementChannelIndex,
                                              result.cursorReadouts[0]->time,
                                              result.cursorReadouts[1]->time,
                                              referenceChannelIndex,
                                              manualReferenceValue);
}

PlotRenderResult drawOscilloscopePlot(plot::WaveDockState& wave,
                                      WaveFrameData& frame,
                                      const WavePlotOverlayPolicy& overlayPolicy,
                                      WaveFrameState* frameState)
{
    // 工具栏可能在 prepareWaveFrame 之后修改布局或参数，重建共享帧供主图与覆盖层共同使用。
    if (alignWaveLayoutChannels(wave) || !wave.cachedDisplayKeyValid) {
        frame = prepareWaveFrame(wave, ImGui::GetContentRegionAvail().x);
    }
    if (wave.view.fitVisibleWaveformsRequested && frame.fullSnapshot && frame.overviewDisplayData) {
        std::vector<std::size_t> visible;
        for (std::size_t i = 0; i < frame.fullSnapshot->channels.size(); ++i) {
            if (!channelHiddenByLegendState(wave, i)) {
                visible.push_back(i);
            }
        }
        applyFitVisibleWaveforms(wave, *frame.fullSnapshot, *frame.overviewDisplayData, visible);
        frame = prepareWaveFrame(wave, ImGui::GetContentRegionAvail().x);
    }
    PlotRenderResult result;
    if (frame.fullSnapshot == nullptr || frame.displayData == nullptr || frame.fullSnapshot->channels.empty()) {
        ImGui::TextUnformatted("Lua 尚未通过 proto.plot.setup / proto.plot.push 提供波形数据。");
        return result;
    }
    auto& view = wave.view;
    result.cursorReadouts = view.lastCursorReadouts;
    // 核心流程：分屏早退前也完成视图模式切换，确保离开堆叠时恢复普通模式 Y 范围。
    applyWaveViewModeVerticalRange(view, std::nullopt);
    if (view.viewMode == plot::WaveViewMode::Split) {
        result = drawSplitOscilloscopePlots(wave, frame, overlayPolicy, frameState);
        view.lastCursorReadouts = result.cursorReadouts;
        return result;
    }

    if (!view.showAxisLabels) {
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10.0F, 10.0F));
        ImPlot::PushStyleVar(ImPlotStyleVar_LabelPadding, ImVec2(8.0F, 6.0F));
    }
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, activeWaveStyleTokens().plotBackground);
    auto& inputMap = ImPlot::GetInputMap();
    const auto savedInputMap = inputMap;
    inputMap.Fit = resolveMainPlotFitMouseButton(
        view.controlMode, ImGui::GetIO().KeyShift, savedInputMap.Fit, ImGuiMouseButton_Middle);
    if (view.controlMode == plot::WaveControlMode::Oscilloscope) {
        inputMap.PanMod = ImGuiMod_Ctrl;
        inputMap.ZoomMod = ImGuiMod_Ctrl;
    }
    if (!ImPlot::BeginPlot("##oscilloscope", ImVec2(-1.0F, -1.0F), ImPlotFlags_NoLegend)) {
        inputMap = savedInputMap;
        ImPlot::PopStyleColor();
        if (!view.showAxisLabels) {
            ImPlot::PopStyleVar(2);
        }
        return result;
    }

    result.plotRendered = true;
    const auto& displayData = *frame.displayData;
    const auto& baseRenderDisplayData = frame.renderDisplayData != nullptr ? *frame.renderDisplayData : displayData;
    std::optional<StackedDisplayData> stackedDisplay;
    if (view.viewMode == plot::WaveViewMode::Stacked) {
        stackedDisplay = makeStackedDisplayData(wave, frame.snapshot, baseRenderDisplayData);
    }
    const auto& plotDisplayData = stackedDisplay.has_value() ? *stackedDisplay->data : displayData;
    const auto& renderDisplayData = stackedDisplay.has_value() ? *stackedDisplay->data : baseRenderDisplayData;
    const auto derivedChannelIndices = channelIndicesForDerivedViews(wave, frame.snapshot);
    const auto derivedBounds =
        stackedDisplay.has_value()
            ? stackedDisplay->bounds
            : boundsForDerivedViews(wave, frame.snapshot, plotDisplayData, derivedChannelIndices);
    auto fullHistoryBounds = derivedBounds;
    if (frame.fullSnapshot != nullptr && frame.overviewDisplayData != nullptr) {
        const auto fullHistoryChannelIndices = channelIndicesForDerivedViews(wave, *frame.fullSnapshot);
        // 核心流程：X 轴双击默认查看当前内存保留的完整历史，复用概览缓存避免额外复制全量样本。
        fullHistoryBounds =
            boundsForVisibleWaveforms(view, *frame.fullSnapshot, *frame.overviewDisplayData, fullHistoryChannelIndices);
    }
    const auto stackedVerticalBounds =
        stackedDisplay.has_value() ? std::optional<plot::WaveDataBounds>(stackedDisplay->bounds) : std::nullopt;
    applyWaveViewModeVerticalRange(view, stackedVerticalBounds);
    applyMainPlotAxesAndLimits(view, frame.snapshot, plotDisplayData);
    const auto verticalBaseline = currentViewport(view);

    const ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
    const ImVec2 plotPos = ImPlot::GetPlotPos();
    const ImVec2 plotSize = ImPlot::GetPlotSize();
    result.measurementOverlay = {.pos = plotPos, .size = plotSize, .valid = true};
    result.legendOverlay = {.pos = plotPos, .size = plotSize, .valid = true};
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    drawOscilloscopeGrid(limits);
    drawCursorFftHighlight(view, limits);
    const double visibleTimeWidth = std::abs(limits.X.Max - limits.X.Min);
    const double timeSnapDistance = visibleTimeWidth / 80.0;
    double smartSnapDistance = (std::max)(timeSnapDistance, visibleTimeWidth * 0.02);
    if (derivedBounds.valid) {
        smartSnapDistance = (std::max)(smartSnapDistance, derivedBounds.minStep * 2.0);
    }
    const double valueSnapDistance = (limits.Y.Max - limits.Y.Min) / 30.0;
    handleWheelFineAdjustmentShortcut(view,
                                      ImGui::GetIO().KeyShift,
                                      ImGui::IsMouseClicked(ImGuiMouseButton_Middle),
                                      ImPlot::IsPlotHovered(),
                                      ImPlot::IsAxisHovered(ImAxis_X1),
                                      ImPlot::IsAxisHovered(ImAxis_Y1));

    const bool zoomSelectionMode = view.zoomSelectionActive || view.zoomSelectionDragging;
    bool viewportChangedThisFrame = false;
    bool axisDoubleClickConsumed = false;
    if (!zoomSelectionMode) {
        viewportChangedThisFrame = handleMainPlotZoom(view, mousePos);
    }
    // 悬停读数必须跟随 ImPlot 图例隐藏状态，只对真实可见波形做吸附。
    std::vector<std::size_t> visibleChannelIndices;
    BitLaneLayout bitLayout;
    renderMainWaveContent(
        wave, frame.snapshot, renderDisplayData, frame.renderBudget, limits, visibleChannelIndices, bitLayout);
    if (stackedDisplay.has_value()) {
        drawStackedChannelGuides(frame.snapshot, stackedDisplay->channelBaseY);
    }
    syncLegendVisibilityState(wave, frame.snapshot);
    if (!zoomSelectionMode) {
        axisDoubleClickConsumed = handleMainPlotAxisDoubleClick(
            wave, frame.snapshot, derivedBounds, fullHistoryBounds, visibleChannelIndices);
        viewportChangedThisFrame = axisDoubleClickConsumed || viewportChangedThisFrame;
    }
    if (frame.overviewDisplayData != nullptr && frame.fullSnapshot != nullptr) {
        const auto fitChannelIndices = excludesLegendHiddenChannels(view)
                                           ? channelIndicesForDerivedViews(wave, *frame.fullSnapshot)
                                           : visibleChannelIndicesForFit(*frame.fullSnapshot);
        viewportChangedThisFrame =
            applyFitVisibleWaveforms(wave, *frame.fullSnapshot, *frame.overviewDisplayData, fitChannelIndices) ||
            viewportChangedThisFrame;
    }
    const auto zoomSelectionResult = handleMainPlotZoomSelection(view, wave.suppressZoomSelectionEscapeThisFrame);
    viewportChangedThisFrame = zoomSelectionResult.viewportChanged || viewportChangedThisFrame;
    if (!axisDoubleClickConsumed && GImPlot->CurrentPlot != nullptr) {
        auto& yAxis = GImPlot->CurrentPlot->YAxis(0);
        if (yAxis.FitThisFrame || yAxis.IsAutoFitting()) {
            viewportChangedThisFrame =
                applyYAxisSingleSideScaleToChannels(wave, frame.snapshot, visibleChannelIndices) ||
                viewportChangedThisFrame;
            yAxis.FitThisFrame = false;
            yAxis.SetRange(verticalBaseline.minValue, verticalBaseline.maxValue);
        }
    }
    bool cursorDragClaimed = false;
    if (!zoomSelectionResult.consumed) {
        cursorDragClaimed = handlePlotCursors(view,
                                              frame.snapshot,
                                              plotDisplayData,
                                              bitLayout,
                                              mousePos,
                                              limits,
                                              timeSnapDistance,
                                              smartSnapDistance,
                                              valueSnapDistance,
                                              result.cursorReadouts);
    }

    const bool offsetReset =
        !axisDoubleClickConsumed && !zoomSelectionResult.consumed && !cursorDragClaimed &&
        handleActiveWaveformDoubleClickOffsetReset(wave,
                                                   frame.snapshot,
                                                   plotDisplayData,
                                                   visibleChannelIndices,
                                                   mousePos,
                                                   timeSnapDistance,
                                                   valueSnapDistance);
    const bool blockPlotInteractions = zoomSelectionResult.consumed || offsetReset;
    if (!blockPlotInteractions) {
        if (!cursorDragClaimed) {
            handleHoverReadout(view,
                               frame.snapshot,
                               plotDisplayData,
                               visibleChannelIndices,
                               bitLayout,
                               mousePos,
                               timeSnapDistance,
                               valueSnapDistance);
        }
        viewportChangedThisFrame = handleOscilloscopeChannelInteractions(wave,
                                                                         frame.snapshot,
                                                                         plotDisplayData,
                                                                         visibleChannelIndices,
                                                                         limits,
                                                                         mousePos,
                                                                         timeSnapDistance,
                                                                         valueSnapDistance,
                                                                         cursorDragClaimed) ||
                                   viewportChangedThisFrame;
    }

    const auto intersectionReadouts = collectCursorIntersectionReadouts(
        view, frame.snapshot, plotDisplayData, visibleChannelIndices, timeSnapDistance);
    drawCursorIntersectionReadouts(intersectionReadouts, frame.snapshot);
    const bool userInteracting = plotInteractionActive(cursorDragClaimed);
    if (!viewportChangedThisFrame) {
        const ImPlotRect updatedLimits = ImPlot::GetPlotLimits();
        const bool limitsSynced = syncAutoFitAxisLimits(view, updatedLimits);
        if (userInteracting && !limitsSynced) {
            recordMainPlotLimits(view, updatedLimits);
        }
        if (userInteracting && !view.lockVerticalRange) {
            view.viewMinValue = updatedLimits.Y.Min;
            view.viewMaxValue = updatedLimits.Y.Max;
        }
    }
    if (userInteracting) {
        applyAutoFollowPausePolicy(view, WaveViewportAutoFollowPolicy::UserInteraction);
    }

    if (view.showCursors && view.cursors[0].enabled && view.cursors[1].enabled) {
        const auto intervalText = plot::makeCursorIntervalText(
            view.cursors[0].time, view.cursors[1].time, plotDisplayData.axisSource, plotDisplayData.timeUnit);
        drawCursorIntervalHint(view.cursors[0].time, view.cursors[1].time, intervalText, limits);
    }

    updateMainMeasurementResult(wave, displayData, result);
    view.lastCursorReadouts = result.cursorReadouts;
    auto* hostViewport = ImGui::GetWindowViewport();
    if (overlayPolicy.drawMeasurementOverlay) {
        const auto placement =
            resolveMeasurementOverlayPlacementSize(plotPos, plotSize, overlayPolicy.measurementSafeRightX);
        if (placement.visible) {
            drawMeasurementOverlay(
                view, frame.snapshot, displayData, result, plotPos, placement.plotSize, ImPlot::GetPlotDrawList());
        }
    }
    drawMainPlotContextMenu(wave, frameState);

    ImPlot::EndPlot();
    commitWaveVerticalViewport(wave, verticalBaseline, visibleChannelIndices);
    if (overlayPolicy.drawLegendOverlay) {
        drawChannelLegendOverlay(wave, frame.snapshot, plotPos, plotSize, hostViewport);
    }
    inputMap = savedInputMap;
    ImPlot::PopStyleColor();
    if (!view.showAxisLabels) {
        ImPlot::PopStyleVar(2);
    }
    return result;
}

} // namespace protoscope::ui
