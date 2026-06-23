#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

namespace protoscope::ui {

std::vector<WaveStatusOverlayItem> buildWaveStatusOverlayItems(const plot::WaveViewState& view)
{
    std::vector<WaveStatusOverlayItem> items;
    if (!view.autoFollowLatest) {
        items.push_back({"暂停跟随"});
    }
    if (view.fft.enabled) {
        items.push_back({"FFT"});
    }
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
    return items;
}

namespace {

    struct StatusOverlayLayout {
        std::vector<ImVec2> offsets;
        std::vector<ImVec2> sizes;
        ImVec2 size{0.0F, 0.0F};
    };

    StatusOverlayLayout makeStatusOverlayLayout(const std::vector<WaveStatusOverlayItem>& items, float maxWidth)
    {
        StatusOverlayLayout layout;
        constexpr float chipPadX = 7.0F;
        constexpr float chipPadY = 3.0F;
        constexpr float gapX = 4.0F;
        constexpr float gapY = 4.0F;

        float x = 0.0F;
        float y = 0.0F;
        float rowHeight = 0.0F;
        for (const auto& item : items) {
            const ImVec2 textSize = ImGui::CalcTextSize(item.label.data(), item.label.data() + item.label.size());
            const ImVec2 chipSize(textSize.x + chipPadX * 2.0F, textSize.y + chipPadY * 2.0F);
            float itemX = x > 0.0F ? x + gapX : x;
            if (x > 0.0F && itemX + chipSize.x > maxWidth) {
                layout.size.x = (std::max)(layout.size.x, x);
                x = 0.0F;
                y += rowHeight + gapY;
                rowHeight = 0.0F;
                itemX = 0.0F;
            }
            layout.offsets.push_back(ImVec2(itemX, y));
            layout.sizes.push_back(chipSize);
            x = itemX + chipSize.x;
            rowHeight = (std::max)(rowHeight, chipSize.y);
        }
        layout.size.x = (std::max)(layout.size.x, x);
        layout.size.y = y + rowHeight;
        return layout;
    }

    std::size_t countSamplesInOverlayRect(const plot::WaveDisplayData& displayData,
                                          const std::vector<std::size_t>& channelIndices,
                                          const ImVec2& rectMin,
                                          const ImVec2& rectMax,
                                          std::size_t stopAfter)
    {
        const ImPlotPoint plotA = ImPlot::PixelsToPlot(rectMin);
        const ImPlotPoint plotB = ImPlot::PixelsToPlot(rectMax);
        const double minTime = (std::min)(plotA.x, plotB.x);
        const double maxTime = (std::max)(plotA.x, plotB.x);
        const double minValue = (std::min)(plotA.y, plotB.y);
        const double maxValue = (std::max)(plotA.y, plotB.y);

        std::size_t count = 0;
        for (const std::size_t channelIndex : channelIndices) {
            if (channelIndex >= displayData.channels.size()) {
                continue;
            }
            for (const auto& sample : displayData.channels[channelIndex].samples) {
                if (sample.time >= minTime && sample.time <= maxTime && sample.value >= minValue &&
                    sample.value <= maxValue) {
                    ++count;
                    if (count > stopAfter) {
                        return count;
                    }
                }
            }
        }
        return count;
    }

    ImVec2 chooseStatusOverlayPosition(const plot::WaveDisplayData& displayData,
                                       const std::vector<std::size_t>& channelIndices,
                                       const ImVec2& plotPos,
                                       const ImVec2& plotSize,
                                       const ImVec2& overlaySize,
                                       float margin)
    {
        const float left = plotPos.x + margin;
        const float top = plotPos.y + margin;
        const float right = (std::max)(left, plotPos.x + plotSize.x - margin - overlaySize.x);
        const float bottom = (std::max)(top, plotPos.y + plotSize.y - margin - overlaySize.y);
        const std::array<ImVec2, 4> candidates{
            ImVec2(right, top),
            ImVec2(left, top),
            ImVec2(right, bottom),
            ImVec2(left, bottom),
        };

        ImVec2 best = candidates.front();
        std::size_t bestScore = std::numeric_limits<std::size_t>::max();
        for (const ImVec2& candidate : candidates) {
            const ImVec2 candidateMax(candidate.x + overlaySize.x, candidate.y + overlaySize.y);
            const std::size_t score =
                countSamplesInOverlayRect(displayData, channelIndices, candidate, candidateMax, bestScore);
            if (score < bestScore) {
                bestScore = score;
                best = candidate;
            }
        }
        return best;
    }

} // namespace

int splitCursorDragId(const std::size_t channelIndex, const std::size_t cursorIndex)
{
    return static_cast<int>(300U + channelIndex * 8U + cursorIndex);
}

void drawWaveStatusOverlay(const plot::WaveViewState& view,
                           const plot::WaveDisplayData* displayData,
                           const std::vector<std::size_t>* channelIndices)
{
    const auto items = buildWaveStatusOverlayItems(view);
    if (items.empty()) {
        return;
    }

    const ImVec2 plotPos = ImPlot::GetPlotPos();
    const ImVec2 plotSize = ImPlot::GetPlotSize();
    constexpr float margin = 8.0F;
    const float maxWidth = (std::min)(360.0F, plotSize.x - margin * 2.0F);
    if (maxWidth <= 32.0F || plotSize.y <= margin * 2.0F) {
        return;
    }

    const StatusOverlayLayout layout = makeStatusOverlayLayout(items, maxWidth);
    if (layout.size.x <= 0.0F || layout.size.y <= 0.0F) {
        return;
    }

    ImVec2 origin((std::max)(plotPos.x + margin, plotPos.x + plotSize.x - margin - layout.size.x), plotPos.y + margin);
    if (displayData != nullptr && channelIndices != nullptr && !channelIndices->empty()) {
        origin = chooseStatusOverlayPosition(*displayData, *channelIndices, plotPos, plotSize, layout.size, margin);
    }

    auto* drawList = ImPlot::GetPlotDrawList();
    const ImU32 chipBgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.04F, 0.045F, 0.05F, 0.68F));
    const ImU32 chipBorderColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0F, 1.0F, 1.0F, 0.18F));
    const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.92F, 0.94F, 0.98F, 0.95F));

    for (std::size_t index = 0; index < items.size(); ++index) {
        const ImVec2 chipMin(origin.x + layout.offsets[index].x, origin.y + layout.offsets[index].y);
        const ImVec2 chipMax(chipMin.x + layout.sizes[index].x, chipMin.y + layout.sizes[index].y);
        drawList->AddRectFilled(chipMin, chipMax, chipBgColor, 5.0F);
        drawList->AddRect(chipMin, chipMax, chipBorderColor, 5.0F);
        drawList->AddText(ImVec2(chipMin.x + 7.0F, chipMin.y + 3.0F),
                          textColor,
                          items[index].label.data(),
                          items[index].label.data() + items[index].label.size());
    }
}

namespace {

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

    std::size_t bitLaneLayoutFingerprint(const BitLaneLayout& bitLayout, const std::size_t channelIndex)
    {
        std::size_t hash = static_cast<std::size_t>(1469598103934665603ULL);
        const auto mix = [&hash](const std::size_t value) {
            hash ^= value + 0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
        };
        mix(bitLayout.lanes.size());
        for (const auto& lane : bitLayout.lanes) {
            if (lane.parentChannelIndex != channelIndex) {
                continue;
            }
            mix(lane.bitIndex);
            mix(lane.laneIndex);
            mix(lane.rowIndex);
        }
        return hash;
    }

    plot::WaveDockState::BitRenderCacheKey makeBitRenderCacheKey(const plot::WaveDockState& wave,
                                                                 std::size_t channelIndex,
                                                                 const ImPlotRect& limits,
                                                                 const plot::BitDisplaySpec& spec,
                                                                 const BitLaneLayout& bitLayout,
                                                                 std::size_t plotPixelWidth,
                                                                 std::size_t vertexBudget)
    {
        return {
            .dataRevision = wave.displayDataRevision,
            .channelIndex = channelIndex,
            .visibleMinTime = limits.X.Min,
            .visibleMaxTime = limits.X.Max,
            .visibleMinValue = limits.Y.Min,
            .visibleMaxValue = limits.Y.Max,
            .sampleFrequencyHz = wave.view.sampleFrequencyHz,
            .firstBit = spec.firstBit,
            .bitCount = spec.bitCount,
            .yOffset = spec.yOffset,
            .plotPixelWidth = plotPixelWidth,
            .plotPixelHeight = static_cast<std::size_t>((std::max)(ImPlot::GetPlotSize().y, 1.0F)),
            .layoutFingerprint = bitLaneLayoutFingerprint(bitLayout, channelIndex),
            .vertexBudget = vertexBudget,
        };
    }

    void appendBitRenderPoint(std::vector<plot::WaveSample>& lane, plot::WaveSample point, std::size_t maxPointsPerLane)
    {
        if (lane.size() < maxPointsPerLane) {
            lane.push_back(point);
            return;
        }
        if (!lane.empty()) {
            // 核心流程：预算耗尽时保留最新状态，避免极高频 bit 跳变把 draw list 顶爆。
            lane.back() = point;
        }
    }

    void buildBitRenderCacheEntry(plot::WaveDockState::BitRenderCacheEntry& entry,
                                  const plot::ChannelView& channel,
                                  const plot::WaveDisplayChannel& displayChannel,
                                  const BitLaneLayout& bitLayout,
                                  std::size_t channelIndex,
                                  const ImPlotRect& limits,
                                  std::size_t vertexBudget)
    {
        entry.lanes.assign(channel.bitDisplay.bitCount, {});
        entry.sourceSampleCount = 0;
        if (!bitDisplayEnabled(channel.bitDisplay) || channel.samples == nullptr || displayChannel.samples.empty()) {
            return;
        }

        const std::size_t begin = (std::min)(channel.visibleBegin, channel.totalSamples);
        const std::size_t end = (std::min)(channel.visibleEnd, channel.totalSamples);
        const std::size_t sourceCount = begin < end ? end - begin : 0;
        const std::size_t sampleCount = (std::min)(sourceCount, displayChannel.samples.size());
        entry.sourceSampleCount = sampleCount;
        if (sampleCount == 0) {
            return;
        }

        const std::size_t maxPointsPerLane =
            (std::max<std::size_t>) (2, vertexBudget / (std::max<std::size_t>) (channel.bitDisplay.bitCount, 1));
        for (std::size_t laneIndex = 0; laneIndex < channel.bitDisplay.bitCount; ++laneIndex) {
            const std::size_t bitIndex = channel.bitDisplay.firstBit + laneIndex;
            auto& lane = entry.lanes[laneIndex];
            lane.reserve((std::min<std::size_t>) (sampleCount * 2U, maxPointsPerLane));

            double lowY = 0.0;
            double highY = 1.0;
            for (const auto& layoutLane : bitLayout.lanes) {
                if (layoutLane.parentChannelIndex == channelIndex && layoutLane.laneIndex == laneIndex) {
                    lowY = layoutLane.lowY;
                    highY = layoutLane.highY;
                    break;
                }
            }
            bool previousState = rawBitEnabled(channel.samples[begin].value, bitIndex);
            double previousY = previousState ? highY : lowY;
            const double firstTime = displayChannel.samples.front().time;
            double lastTime = firstTime;
            appendBitRenderPoint(lane, {.time = firstTime, .value = previousY}, maxPointsPerLane);

            for (std::size_t sampleIndex = 1; sampleIndex < sampleCount; ++sampleIndex) {
                const auto& displaySample = displayChannel.samples[sampleIndex];
                const bool currentState = rawBitEnabled(channel.samples[begin + sampleIndex].value, bitIndex);
                if (currentState == previousState) {
                    lastTime = displaySample.time;
                    continue;
                }

                appendBitRenderPoint(lane, {.time = displaySample.time, .value = previousY}, maxPointsPerLane);
                previousState = currentState;
                previousY = previousState ? highY : lowY;
                appendBitRenderPoint(lane, {.time = displaySample.time, .value = previousY}, maxPointsPerLane);
                lastTime = displaySample.time;
            }

            if (std::abs(lastTime - firstTime) <= 1e-12 && limits.X.Max > firstTime) {
                lastTime = limits.X.Max;
            }
            appendBitRenderPoint(lane, {.time = lastTime, .value = previousY}, maxPointsPerLane);
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

    void drawBitLaneLabels(const BitLaneLayout& bitLayout, const ImPlotRect& limits, ImU32 textColor)
    {
        auto* drawList = ImPlot::GetPlotDrawList();
        if (drawList == nullptr) {
            return;
        }
        const ImVec2 plotPos = ImPlot::GetPlotPos();
        std::vector<std::size_t> labeledRows;
        labeledRows.reserve(bitLayout.lanes.size());
        for (const auto& layoutLane : bitLayout.lanes) {
            if (std::ranges::find(labeledRows, layoutLane.rowIndex) != labeledRows.end()) {
                continue;
            }
            labeledRows.push_back(layoutLane.rowIndex);
            const ImVec2 lanePixel = ImPlot::PlotToPixels(limits.X.Min, layoutLane.centerY);
            const std::string label = bitLaneDisplayLabel(layoutLane.bitIndex);
            drawList->AddText(
                ImVec2(plotPos.x + 6.0F, lanePixel.y - ImGui::GetTextLineHeight() * 0.5F), textColor, label.c_str());
        }
    }

    void drawBitRenderLanes(const plot::WaveDockState::BitRenderCacheEntry& entry, const ImVec4& color, float lineWidth)
    {
        auto* drawList = ImPlot::GetPlotDrawList();
        if (drawList == nullptr) {
            return;
        }
        const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(withAlpha(color, 0.9F));
        const ImU32 labelColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.84F, 0.88F, 0.92F, 0.72F));
        for (const auto& lane : entry.lanes) {
            if (lane.size() < 2) {
                continue;
            }
            for (std::size_t pointIndex = 1; pointIndex < lane.size(); ++pointIndex) {
                const ImVec2 from = ImPlot::PlotToPixels(lane[pointIndex - 1].time, lane[pointIndex - 1].value);
                const ImVec2 to = ImPlot::PlotToPixels(lane[pointIndex].time, lane[pointIndex].value);
                drawList->AddLine(from, to, lineColor, lineWidth);
            }
        }
    }

    struct StackedDisplayData {
        plot::WaveDisplayData data;
        std::vector<double> channelBaseY;
        plot::WaveDataBounds bounds{};
    };

    StackedDisplayData makeStackedDisplayData(const plot::WaveDockState& wave,
                                              const plot::WaveSnapshot& snapshot,
                                              const plot::WaveDisplayData& source)
    {
        StackedDisplayData result;
        result.data = source;
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
            auto& channel = result.data.channels[channelIndex];
            if (channel.samples.empty() || channelHiddenByLegendState(wave, snapshot.channels[channelIndex].label)) {
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
            const double span = (std::max)(maxValue - minValue, 1e-12);
            const double center = 0.5 * (minValue + maxValue);
            const double baseY = static_cast<double>(visibleRow) * 1.6;
            result.channelBaseY[channelIndex] = baseY;
            for (auto& sample : channel.samples) {
                sample.value = baseY + (sample.value - center) / span;
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
        const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.16F, 0.24F, 0.31F, 0.70F));
        const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.84F, 0.89F, 0.94F, 0.76F));
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
    view.lastRenderStats = {};
    view.lastRenderStats.lastRenderPointBudget = renderBudget.pointsPerChannel;
    view.lastRenderStats.lastDownsampleThreshold = static_cast<std::size_t>(
        std::ceil(static_cast<double>(renderBudget.pointsPerChannel) *
                  (std::max)(view.downsampleStartMultiplier, 1.0)));

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
        applySavedLegendVisibility(wave, channel.label);
        ImPlot::PlotDummy(channel.label.c_str(), legendSpec);
        if (currentPlotItemVisible(channel.label)) {
            bitChannelIndices.push_back(channelIndex);
            visibleChannelIndices.push_back(channelIndex);
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

        if (bitDisplayEnabled(channel.bitDisplay)) {
            if (!currentPlotItemVisible(channel.label)) {
                continue;
            }

            const ImVec2 plotSize = ImPlot::GetPlotSize();
            const auto plotPixelWidth = static_cast<std::size_t>((std::max)(plotSize.x, 1.0F));
            const std::size_t vertexBudget =
                (std::max<std::size_t>) (channel.bitDisplay.bitCount * 2U, renderBudget.pointsPerChannel * 2U);
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
            drawBitRenderLanes(entry, color, lineWidth);
            continue;
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
            applySavedLegendVisibility(wave, channel.label);
            ImPlot::PlotLineG(channel.label.c_str(),
                              reinterpret_cast<ImPlotGetter>(&waveSampleGetter),
                              &payload,
                              static_cast<int>(rawVisibleCount),
                              spec);
            if (!currentPlotItemVisible(channel.label)) {
                continue;
            }
            visibleChannelIndices.push_back(channelIndex);
            view.lastRenderPointCount += rawVisibleCount;
            view.lastRenderSourceSampleCount += sourceSampleCount;
            ++view.lastRenderStats.rawChannelCount;
            if (view.showPointsWhenSparse) {
                ImPlotSpec pointSpec{};
                pointSpec.Marker = ImPlotMarker_Circle;
                pointSpec.MarkerSize = 2.5F;
                pointSpec.MarkerFillColor = color;
                pointSpec.MarkerLineColor = color;
                pointSpec.LineWeight = 0.0F;
                ImPlot::PlotScatterG((channel.label + " samples").c_str(),
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
        applySavedLegendVisibility(wave, channel.label);
        ImPlot::PlotDummy(channel.label.c_str(), legendSpec);
        const bool legendVisible = currentPlotItemVisible(channel.label);
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
                ImPlot::PlotLineG((channel.label + " peak").c_str(),
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
        if (view.phosphorGlowEnabled) {
            renderPhosphorEnvelope(
                envelope, color, limits.X.Max, view.persistenceWindow, view.glowIntensity, lineWidth);
        } else {
            renderEnvelopeAsBars(envelope, color, lineWidth);
        }
    }
    if (!outBitLayout.lanes.empty()) {
        const ImU32 labelColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.84F, 0.88F, 0.92F, 0.72F));
        drawBitLaneLabels(outBitLayout, limits, labelColor);
    }
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
    if (!ImPlot::IsPlotHovered() || !view.showHoverReadout || visibleChannelIndices.empty()) {
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
                                          activeBitLaneVisible(view, bitLayout));
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
        if (view.showCursors && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.measurementChannelIndex = laneInfo.parentChannelIndex;
            view.activeBitLane = {
                .active = true,
                .parentChannelIndex = laneInfo.parentChannelIndex,
                .bitIndex = laneInfo.bitIndex,
                .laneIndex = laneInfo.laneIndex,
            };
        }
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
        view.activeBitLane = {};
    }
}

namespace {

    class ScopedImPlotInputMap {
    public:
        explicit ScopedImPlotInputMap(const plot::WaveControlMode controlMode)
            : inputMap_(ImPlot::GetInputMap()), savedInputMap_(inputMap_)
        {
            if (controlMode == plot::WaveControlMode::Oscilloscope) {
                inputMap_.PanMod = ImGuiMod_Ctrl;
                inputMap_.Fit = ImGuiMouseButton_Middle;
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
        return false;
    }
    clampActiveChannel(view, snapshot.channels.size());
    if (view.activeBitLane.active && !activeBitLaneVisible(view, bitLayout)) {
        view.activeBitLane = {};
    }

    const auto& io = ImGui::GetIO();
    bool anyCursorHeld = false;
    for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
        auto& cursor = view.cursors[cursorIndex];
        if (!cursor.enabled) {
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
        ImPlot::DragLineX(dragId,
                          &dragTime,
                          cursorColor,
                          (hovered || held) ? 2.0F : 1.0F,
                          dragFlags,
                          &clicked,
                          &hovered,
                          &held);
        anyCursorHeld = anyCursorHeld || held;
        if (splitChannelIndex.has_value() && !held && !hovered && !ImPlot::IsPlotHovered() &&
            (!cursor.pinned || cursor.channelIndex != *splitChannelIndex)) {
            continue;
        }
        if (held && view.fft.enabled && view.fft.displayMode == plot::WaveFftDisplayMode::CursorSplit) {
            view.lastCursorFftAnchorIndex = cursorIndex;
        }
        if (held && smartSnapActive) {
            // 核心流程：先用 DragLineX 写入的鼠标时间查吸附，再回写游标时间，配合 Delayed 让绘制使用受约束位置。
            auto smartSnapTarget = findSmartCursorSnapByScope(
                snapshot,
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

        const double searchY = cursorSearchAnchorY(cursor, cursorReadouts[cursorIndex], snapshot, mousePos.y, held);
        const bool allowActiveChannelTimeFallback =
            view.cursorSnapScope == plot::WaveCursorSnapScope::ActiveChannel && !held &&
            (cursor.channelIndex >= snapshot.channels.size() || cursor.channelIndex != view.measurementChannelIndex) &&
            view.measurementChannelIndex < snapshot.channels.size() &&
            !bitDisplayEnabled(snapshot.channels[view.measurementChannelIndex].bitDisplay);
        auto best = findNearestCursorByScope(
            snapshot,
            displayData,
            view,
            bitLayout,
            cursor.time,
            searchY,
            timeSnapDistance,
            valueSnapDistance,
            allowActiveChannelTimeFallback,
            splitChannelIndex);
        if (smartSnap.has_value()) {
            best = smartSnap;
        }
        if (!best.has_value()) {
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
        if (best->bit.has_value()) {
            view.activeBitLane = {
                .active = true,
                .parentChannelIndex = best->bit->parentChannelIndex,
                .bitIndex = best->bit->bitIndex,
                .laneIndex = best->bit->laneIndex,
            };
        } else if (!smartSnap.has_value()) {
            view.activeBitLane = {};
        }
        if (held || hovered || cursor.pinned) {
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
    return anyCursorHeld;
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

void drawDashedGridLine(ImDrawList* drawList,
                        const ImVec2& from,
                        const ImVec2& to,
                        ImU32 color,
                        float thickness,
                        float dashLength,
                        float gapLength)
{
    const ImVec2 delta(to.x - from.x, to.y - from.y);
    const float length = std::sqrt(delta.x * delta.x + delta.y * delta.y);
    if (drawList == nullptr || length <= 0.0F) {
        return;
    }
    const ImVec2 direction(delta.x / length, delta.y / length);
    float cursor = 0.0F;
    while (cursor < length) {
        const float next = (std::min)(cursor + dashLength, length);
        const ImVec2 segmentFrom(from.x + direction.x * cursor, from.y + direction.y * cursor);
        const ImVec2 segmentTo(from.x + direction.x * next, from.y + direction.y * next);
        drawList->AddLine(segmentFrom, segmentTo, color, thickness);
        cursor = next + gapLength;
    }
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
    const ImU32 minorColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.071F, 0.106F, 0.141F, 0.35F));
    const ImU32 majorColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.114F, 0.169F, 0.220F, 0.60F));
    const ImU32 centerColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.90F, 0.96F, 1.0F, 0.42F));
    const double xStep = (limits.X.Max - limits.X.Min) / static_cast<double>(plot::kWaveGridMajorXDivisions);
    const double yStep = (limits.Y.Max - limits.Y.Min) / static_cast<double>(plot::kWaveGridMajorYDivisions);
    const int xMinorCount = plot::kWaveGridMajorXDivisions * plot::kWaveGridMinorDivisionsPerMajor;
    const int yMinorCount = plot::kWaveGridMajorYDivisions * plot::kWaveGridMinorDivisionsPerMajor;

    ImPlot::PushPlotClipRect();
    for (int index = 1; index < xMinorCount; ++index) {
        if (index % plot::kWaveGridMinorDivisionsPerMajor == 0) {
            continue;
        }
        const double x = limits.X.Min +
                         (limits.X.Max - limits.X.Min) * static_cast<double>(index) / static_cast<double>(xMinorCount);
        drawDashedGridLine(drawList,
                           ImPlot::PlotToPixels(x, limits.Y.Min),
                           ImPlot::PlotToPixels(x, limits.Y.Max),
                           minorColor,
                           1.0F,
                           3.0F,
                           5.0F);
    }
    for (int index = 1; index < yMinorCount; ++index) {
        if (index % plot::kWaveGridMinorDivisionsPerMajor == 0) {
            continue;
        }
        const double y = limits.Y.Min +
                         (limits.Y.Max - limits.Y.Min) * static_cast<double>(index) / static_cast<double>(yMinorCount);
        drawDashedGridLine(drawList,
                           ImPlot::PlotToPixels(limits.X.Min, y),
                           ImPlot::PlotToPixels(limits.X.Max, y),
                           minorColor,
                           1.0F,
                           3.0F,
                           5.0F);
    }
    for (int index = 0; index <= plot::kWaveGridMajorXDivisions; ++index) {
        const double x = limits.X.Min + xStep * static_cast<double>(index);
        const bool center = index == plot::kWaveGridMajorXDivisions / 2;
        drawList->AddLine(ImPlot::PlotToPixels(x, limits.Y.Min),
                          ImPlot::PlotToPixels(x, limits.Y.Max),
                          center ? centerColor : majorColor,
                          center ? 1.4F : 1.0F);
    }
    for (int index = 0; index <= plot::kWaveGridMajorYDivisions; ++index) {
        const double y = limits.Y.Min + yStep * static_cast<double>(index);
        const bool center = index == plot::kWaveGridMajorYDivisions / 2;
        drawList->AddLine(ImPlot::PlotToPixels(limits.X.Min, y),
                          ImPlot::PlotToPixels(limits.X.Max, y),
                          center ? centerColor : majorColor,
                          center ? 1.4F : 1.0F);
    }
    ImPlot::PopPlotClipRect();
}

std::optional<plot::CursorReadout> findSplitBitCursorReadout(const plot::WaveSnapshot& snapshot,
                                                             const plot::WaveDisplayData& displayData,
                                                             const plot::WaveViewState& view,
                                                             std::size_t channelIndex,
                                                             double time,
                                                             double maxTimeDistance)
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
    const auto& samples = displayChannel.samples;
    if (samples.empty()) {
        return std::nullopt;
    }

    const auto lower = std::lower_bound(
        samples.begin(), samples.end(), time, [](const plot::WaveSample& sample, double value) {
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
    if (view.activeBitLane.active && view.activeBitLane.parentChannelIndex == channelIndex &&
        view.activeBitLane.laneIndex < sourceChannel.bitDisplay.bitCount) {
        laneIndex = view.activeBitLane.laneIndex;
        bitIndex = sourceChannel.bitDisplay.firstBit + laneIndex;
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

void updateSplitMeasurementResult(const plot::WaveViewState& view,
                                  const plot::WaveDisplayData& displayData,
                                  PlotRenderResult& result,
                                  std::optional<std::size_t> measurementChannelOverride = std::nullopt)
{
    result.bitMeasurementActive = false;
    result.measurement.reset();
    if (!view.showCursors || !result.cursorReadouts[0].has_value() || !result.cursorReadouts[1].has_value()) {
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
    result.measurement = measureDisplayWindow(displayData,
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
        result.cursorReadouts[cursorIndex].reset();
        if (!view.cursors[cursorIndex].enabled || channelIndex >= snapshot.channels.size() ||
            channelIndex >= displayData.channels.size()) {
            continue;
        }
        if (bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
            result.cursorReadouts[cursorIndex] = findSplitBitCursorReadout(
                snapshot, displayData, view, channelIndex, view.cursors[cursorIndex].time, maxTimeDistance);
        } else {
            result.cursorReadouts[cursorIndex] =
                plot::findNearestDisplayByTime(
                    displayData, channelIndex, view.cursors[cursorIndex].time, maxTimeDistance);
        }
    }
}

PlotRenderResult drawSplitOscilloscopePlots(plot::WaveDockState& wave, const WaveFrameData& frame)
{
    PlotRenderResult result;
    if (frame.displayData == nullptr || frame.fullSnapshot == nullptr || frame.fullSnapshot->channels.empty()) {
        ImGui::TextUnformatted("Lua 尚未通过 proto.plot.setup / proto.plot.push 提供波形数据。");
        return result;
    }

    auto& view = wave.view;
    const auto& snapshot = frame.snapshot;
    const auto& displayData = *frame.displayData;
    std::vector<std::size_t> visibleChannels = channelIndicesForDerivedViews(wave, snapshot);
    if (visibleChannels.empty()) {
        ImGui::TextUnformatted("所有通道已隐藏。");
        return result;
    }

    const ImVec2 splitPos = ImGui::GetCursorScreenPos();
    const ImVec2 splitSize = ImGui::GetContentRegionAvail();
    const bool mouseInsideSplitRegion = ImGui::IsMouseHoveringRect(
        splitPos, ImVec2(splitPos.x + splitSize.x, splitPos.y + splitSize.y), false);
    ScopedImPlotInputMap inputMapGuard(view.controlMode);

    bool viewportChangedThisFrame = false;
    bool anyCursorHeld = false;
    bool userInteractingInAnySplitPlot = false;
    ImGui::BeginChild("##wave_split_scroll", ImVec2(-1.0F, -1.0F), false);
    const float plotHeight = plot::solveSplitWavePlotHeight(visibleChannels.size(),
                                                            ImGui::GetContentRegionAvail().y,
                                                            ImGui::GetStyle().ItemSpacing.y,
                                                            120.0F,
                                                            4U);
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotMinSize, ImVec2(64.0F, 24.0F));
    for (std::size_t rowIndex = 0; rowIndex < visibleChannels.size(); ++rowIndex) {
        const std::size_t channelIndex = visibleChannels[rowIndex];
        if (channelIndex >= snapshot.channels.size() || channelIndex >= displayData.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[channelIndex];
        const auto& samples = displayData.channels[channelIndex].samples;
        if (samples.empty()) {
            continue;
        }

        ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4(0.043F, 0.067F, 0.094F, 1.0F));
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
            ImPlot::SetupAxis(
                ImAxis_X1,
                xAxisLabel,
                bottomRow ? xFlags : (xFlags | ImPlotAxisFlags_NoTickLabels));
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
                double minValue = std::numeric_limits<double>::infinity();
                double maxValue = -std::numeric_limits<double>::infinity();
                for (const auto& sample : samples) {
                    if (sample.time < view.viewMinTime || sample.time > view.viewMaxTime) {
                        continue;
                    }
                    minValue = (std::min)(minValue, sample.value);
                    maxValue = (std::max)(maxValue, sample.value);
                }
                if (!std::isfinite(minValue) || !std::isfinite(maxValue) || std::abs(maxValue - minValue) <= 1e-12) {
                    minValue = channel.stats.minValue - 1.0;
                    maxValue = channel.stats.maxValue + 1.0;
                }
                const auto range = plot::makeVerticalAutoFitRange(minValue, maxValue, view.verticalAutoFitMultiplier);
                ImPlot::SetupAxisLimits(ImAxis_Y1, range.minValue, range.maxValue, ImPlotCond_Always);
            }

            const ImPlotRect limits = ImPlot::GetPlotLimits();
            drawOscilloscopeGrid(limits);
            const ImVec4 color = channelColor(channel, channelIndex);
            BitLaneLayout bitLayout;
            if (bitChannel) {
                const std::vector<std::size_t> bitChannelIndices{channelIndex};
                bitLayout =
                    buildBitLaneLayout(snapshot, bitChannelIndices, limits, ImPlot::GetPlotPos(), ImPlot::GetPlotSize());
                const ImVec2 plotSize = ImPlot::GetPlotSize();
                const auto plotPixelWidth = static_cast<std::size_t>((std::max)(plotSize.x, 1.0F));
                const std::size_t vertexBudget = (std::max<std::size_t>)(
                    channel.bitDisplay.bitCount * 2U, frame.renderBudget.pointsPerChannel * 2U);
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
                drawBitRenderLanes(entry, color, plot::resolveChannelLineWidth(channel));
                const ImU32 labelColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.84F, 0.88F, 0.92F, 0.72F));
                drawBitLaneLabels(bitLayout, limits, labelColor);
            } else {
                WaveSampleGetterPayload payload{.samples = samples.data()};
                ImPlotSpec spec{};
                spec.LineColor = color;
                spec.LineWeight = plot::resolveChannelLineWidth(channel);
                ImPlot::PlotLineG(channel.label.c_str(),
                                  reinterpret_cast<ImPlotGetter>(&waveSampleGetter),
                                  &payload,
                                  static_cast<int>(samples.size()),
                                  spec);
            }

            auto* drawList = ImPlot::GetPlotDrawList();
            const ImVec2 plotPos = ImPlot::GetPlotPos();
            drawList->AddText(ImVec2(plotPos.x + 8.0F, plotPos.y + 6.0F),
                              ImGui::ColorConvertFloat4ToU32(ImVec4(0.90F, 0.94F, 0.98F, 0.86F)),
                              ("CH" + std::to_string(channelIndex + 1U) + "  " + channel.label).c_str());

            const ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            const double visibleTimeWidth = std::abs(limits.X.Max - limits.X.Min);
            const double timeSnapDistance = visibleTimeWidth / 80.0;
            const double smartSnapDistance = (std::max)(timeSnapDistance, visibleTimeWidth * 0.02);
            const double valueSnapDistance = (limits.Y.Max - limits.Y.Min) / 30.0;
            const std::vector<std::size_t> splitChannelIndices{channelIndex};
            const bool plotHovered = ImPlot::IsPlotHovered();
            const bool splitZoomChanged = handleMainPlotZoom(view, mousePos);
            viewportChangedThisFrame = splitZoomChanged || viewportChangedThisFrame;
            if (splitZoomChanged) {
                view.forceNextMainPlotLimits = true;
            }
            handleHoverReadout(view,
                               snapshot,
                               displayData,
                               splitChannelIndices,
                               bitLayout,
                               mousePos,
                               timeSnapDistance,
                               valueSnapDistance);
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
            const bool rowCursorHeld = handleSplitPlotCursors(view,
                                                              snapshot,
                                                              displayData,
                                                              interactionContext,
                                                              result.cursorReadouts);
            anyCursorHeld = rowCursorHeld || anyCursorHeld;
            if (plotHovered || (!mouseInsideSplitRegion && channelIndex == view.measurementChannelIndex)) {
                const double maxCursorReadoutDistance =
                    (std::max)(view.viewMaxTime - view.viewMinTime, view.minVisibleTimeSpan) / 80.0;
                updateSplitCursorReadoutsForChannel(
                    snapshot, displayData, view, channelIndex, maxCursorReadoutDistance, result);
                updateSplitMeasurementResult(view, displayData, result, channelIndex);
                drawMeasurementOverlay(view, snapshot, displayData, result);
            }

            const ImPlotRect updatedLimits = ImPlot::GetPlotLimits();
            if (!viewportChangedThisFrame && (ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1))) {
                recordMainPlotLimits(view, updatedLimits);
            }
            // 核心流程：ImPlot 交互查询必须在当前子图 EndPlot 前完成，避免分屏结束后访问空 active plot。
            userInteractingInAnySplitPlot = plotInteractionActive(rowCursorHeld) || userInteractingInAnySplitPlot;
            ImPlot::EndPlot();
        }
        ImPlot::PopStyleColor();
    }
    ImPlot::PopStyleVar();
    if (!viewportChangedThisFrame) {
        view.forceNextMainPlotLimits = false;
    }
    ImGui::EndChild();
    drawChannelLegendOverlay(wave, snapshot, splitPos, splitSize);

    if (userInteractingInAnySplitPlot && view.pauseAutoFollowOnInteraction) {
        view.autoFollowLatest = false;
    }

    if (view.showCursors) {
        const double maxDistance = (std::max)(view.viewMaxTime - view.viewMinTime, view.minVisibleTimeSpan) / 80.0;
        for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
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
                    snapshot, displayData, view, channelIndex, view.cursors[cursorIndex].time, maxDistance);
            } else if (channelIndex < displayData.channels.size()) {
                result.cursorReadouts[cursorIndex] =
                    plot::findNearestDisplayByTime(
                        displayData, channelIndex, view.cursors[cursorIndex].time, maxDistance);
            }
        }
        updateSplitMeasurementResult(view, displayData, result);
    }
    result.plotRendered = true;
    return result;
}

PlotRenderResult drawOscilloscopePlot(plot::WaveDockState& wave, const WaveFrameData& frame)
{
    PlotRenderResult result;
    if (frame.fullSnapshot == nullptr || frame.displayData == nullptr || frame.fullSnapshot->channels.empty()) {
        ImGui::TextUnformatted("Lua 尚未通过 proto.plot.setup / proto.plot.push 提供波形数据。");
        return result;
    }
    auto& view = wave.view;
    if (view.viewMode == plot::WaveViewMode::Split) {
        return drawSplitOscilloscopePlots(wave, frame);
    }

    if (!view.showAxisLabels) {
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10.0F, 10.0F));
        ImPlot::PushStyleVar(ImPlotStyleVar_LabelPadding, ImVec2(8.0F, 6.0F));
    }
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, ImVec4(0.043F, 0.067F, 0.094F, 1.0F));
    auto& inputMap = ImPlot::GetInputMap();
    const auto savedInputMap = inputMap;
    if (view.controlMode == plot::WaveControlMode::Oscilloscope) {
        inputMap.PanMod = ImGuiMod_Ctrl;
        inputMap.Fit = ImGuiMouseButton_Middle;
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
    const auto& plotDisplayData = stackedDisplay.has_value() ? stackedDisplay->data : displayData;
    const auto& renderDisplayData = stackedDisplay.has_value() ? stackedDisplay->data : baseRenderDisplayData;
    const auto derivedChannelIndices = channelIndicesForDerivedViews(wave, frame.snapshot);
    const auto derivedBounds =
        stackedDisplay.has_value()
            ? stackedDisplay->bounds
            : boundsForDerivedViews(wave, frame.snapshot, plotDisplayData, derivedChannelIndices);
    const auto yAutoFitBounds =
        stackedDisplay.has_value()
            ? stackedDisplay->bounds
            : boundsForYAxisAutoFit(wave, frame.snapshot, plotDisplayData, derivedChannelIndices);
    auto fullHistoryBounds = derivedBounds;
    if (frame.fullSnapshot != nullptr && frame.overviewDisplayData != nullptr) {
        const auto fullHistoryChannelIndices = channelIndicesForDerivedViews(wave, *frame.fullSnapshot);
        // 核心流程：X 轴双击默认查看当前内存保留的完整历史，复用概览缓存避免额外复制全量样本。
        fullHistoryBounds =
            boundsForVisibleWaveforms(view, *frame.fullSnapshot, *frame.overviewDisplayData, fullHistoryChannelIndices);
    }
    if (stackedDisplay.has_value() && !view.lockVerticalRange) {
        view.viewMinValue = stackedDisplay->bounds.minValue;
        view.viewMaxValue = stackedDisplay->bounds.maxValue;
    }
    applyMainPlotAxesAndLimits(view, frame.snapshot, plotDisplayData);

    const ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
    const ImVec2 plotPos = ImPlot::GetPlotPos();
    const ImVec2 plotSize = ImPlot::GetPlotSize();
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    drawOscilloscopeGrid(limits);
    if (view.fft.enabled && view.fft.displayMode == plot::WaveFftDisplayMode::CursorSplit && view.showCursors &&
        view.cursors[0].enabled && view.cursors[1].enabled) {
        const double minTime = (std::min)(view.cursors[0].time, view.cursors[1].time);
        const double maxTime = (std::max)(view.cursors[0].time, view.cursors[1].time);
        if (maxTime > minTime) {
            const auto& rgba = view.cursorFftHighlightRgba;
            const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(ImVec4(rgba[0], rgba[1], rgba[2], rgba[3]));
            const ImVec2 pixelA = ImPlot::PlotToPixels(minTime, limits.Y.Min);
            const ImVec2 pixelB = ImPlot::PlotToPixels(maxTime, limits.Y.Max);
            ImPlot::GetPlotDrawList()->AddRectFilled(
                ImVec2((std::min)(pixelA.x, pixelB.x), (std::min)(pixelA.y, pixelB.y)),
                ImVec2((std::max)(pixelA.x, pixelB.x), (std::max)(pixelA.y, pixelB.y)),
                fillColor);
        }
    }
    const double visibleTimeWidth = std::abs(limits.X.Max - limits.X.Min);
    const double timeSnapDistance = visibleTimeWidth / 80.0;
    double smartSnapDistance = (std::max)(timeSnapDistance, visibleTimeWidth * 0.02);
    if (derivedBounds.valid) {
        smartSnapDistance = (std::max)(smartSnapDistance, derivedBounds.minStep * 2.0);
    }
    const double valueSnapDistance = (limits.Y.Max - limits.Y.Min) / 30.0;

    const bool zoomSelectionMode = view.zoomSelectionActive || view.zoomSelectionDragging;
    bool viewportChangedThisFrame = false;
    bool axisDoubleClickConsumed = false;
    if (!zoomSelectionMode) {
        axisDoubleClickConsumed =
            handleMainPlotAxisDoubleClick(view, frame.snapshot, derivedBounds, fullHistoryBounds, yAutoFitBounds);
        viewportChangedThisFrame = axisDoubleClickConsumed || handleMainPlotZoom(view, mousePos);
    }
    // 悬停读数必须跟随 ImPlot 图例隐藏状态，只对真实可见波形做吸附。
    std::vector<std::size_t> visibleChannelIndices;
    BitLaneLayout bitLayout;
    renderWaveChannels(
        wave, frame.snapshot, renderDisplayData, frame.renderBudget, limits, visibleChannelIndices, bitLayout);
    if (stackedDisplay.has_value()) {
        drawStackedChannelGuides(frame.snapshot, stackedDisplay->channelBaseY);
    }
    if (view.activeBitLane.active && !activeBitLaneVisible(view, bitLayout)) {
        view.activeBitLane = {};
    }
    syncLegendVisibilityState(wave, frame.snapshot);
    const auto fitChannelIndices = excludesLegendHiddenChannels(view)
                                       ? channelIndicesForDerivedViews(wave, frame.snapshot)
                                       : visibleChannelIndicesForFit(frame.snapshot);
    const auto& fitDisplayData = stackedDisplay.has_value() ? renderDisplayData : displayData;
    viewportChangedThisFrame =
        applyFitVisibleWaveforms(view, frame.snapshot, fitDisplayData, fitChannelIndices) || viewportChangedThisFrame;
    const auto zoomSelectionResult = handleMainPlotZoomSelection(view, wave.suppressZoomSelectionEscapeThisFrame);
    viewportChangedThisFrame = zoomSelectionResult.viewportChanged || viewportChangedThisFrame;
    if (!axisDoubleClickConsumed) {
        viewportChangedThisFrame =
            applyPendingVerticalAutoFitOverride(view, yAutoFitBounds) || viewportChangedThisFrame;
    }
    const bool offsetReset = !axisDoubleClickConsumed && !zoomSelectionResult.consumed &&
                             handleActiveWaveformDoubleClickOffsetReset(wave,
                                                                        frame.snapshot,
                                                                        bitLayout,
                                                                        plotDisplayData,
                                                                        visibleChannelIndices,
                                                                        mousePos,
                                                                        timeSnapDistance,
                                                                        valueSnapDistance);
    const bool blockPlotInteractions = zoomSelectionResult.consumed || offsetReset;
    if (!blockPlotInteractions) {
        handleHoverReadout(view,
                           frame.snapshot,
                           plotDisplayData,
                           visibleChannelIndices,
                           bitLayout,
                           mousePos,
                           timeSnapDistance,
                           valueSnapDistance);
        viewportChangedThisFrame = handleOscilloscopeChannelInteractions(wave,
                                                                         frame.snapshot,
                                                                         plotDisplayData,
                                                                         visibleChannelIndices,
                                                                         limits,
                                                                         mousePos,
                                                                         timeSnapDistance,
                                                                         valueSnapDistance) ||
                                   viewportChangedThisFrame;
    }

    const bool anyCursorHeld = blockPlotInteractions ? false
                                                     : handlePlotCursors(view,
                                                                         frame.snapshot,
                                                                         plotDisplayData,
                                                                         bitLayout,
                                                                         mousePos,
                                                                         limits,
                                                                         timeSnapDistance,
                                                                         smartSnapDistance,
                                                                         valueSnapDistance,
                                                                         result.cursorReadouts);
    const bool userInteracting = plotInteractionActive(anyCursorHeld);
    if (!viewportChangedThisFrame) {
        const ImPlotRect updatedLimits = ImPlot::GetPlotLimits();
        const bool limitsSynced = syncAutoFitAxisLimits(view, updatedLimits);
        if (userInteracting && !limitsSynced) {
            recordMainPlotLimits(view, updatedLimits);
        }
    }
    if (userInteracting && view.pauseAutoFollowOnInteraction) {
        view.autoFollowLatest = false;
    }

    if (view.showCursors && result.cursorReadouts[0].has_value() && result.cursorReadouts[1].has_value()) {
        const auto intervalText = plot::makeCursorIntervalText(
            *result.cursorReadouts[0], *result.cursorReadouts[1], displayData.axisSource, displayData.timeUnit);
        drawCursorIntervalHint(*result.cursorReadouts[0], *result.cursorReadouts[1], intervalText, limits);
    }

    if (view.showCursors && result.cursorReadouts[0].has_value() && result.cursorReadouts[1].has_value()) {
        result.bitMeasurementActive =
            cursorPairUsesBitLanes(result.cursorReadouts) && activeBitLaneVisible(view, bitLayout);
        if (result.bitMeasurementActive) {
            result.measurement = makeBitIntervalMeasurement(*result.cursorReadouts[0], *result.cursorReadouts[1]);
        } else {
            const auto referenceChannelIndex = view.referenceMode == plot::WaveMeasurementReferenceMode::Channel
                                                   ? std::optional<std::size_t>(view.referenceChannelIndex)
                                                   : std::nullopt;
            const auto manualReferenceValue = view.referenceMode == plot::WaveMeasurementReferenceMode::ManualValue
                                                  ? std::optional<double>(view.manualReferenceValue)
                                                  : std::nullopt;
            result.measurement = measureDisplayWindow(displayData,
                                                      view.measurementChannelIndex,
                                                      result.cursorReadouts[0]->time,
                                                      result.cursorReadouts[1]->time,
                                                      referenceChannelIndex,
                                                      manualReferenceValue);
        }
    }
    drawMeasurementOverlay(view, frame.snapshot, displayData, result);
    drawWaveStatusOverlay(view, &renderDisplayData, &visibleChannelIndices);

    ImPlot::EndPlot();
    drawChannelLegendOverlay(wave, frame.snapshot, plotPos, plotSize);
    inputMap = savedInputMap;
    ImPlot::PopStyleColor();
    if (!view.showAxisLabels) {
        ImPlot::PopStyleVar(2);
    }
    return result;
}

} // namespace protoscope::ui
