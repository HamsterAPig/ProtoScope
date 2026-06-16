#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
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
                                                                           std::size_t pointLimit)
    {
        return {
            .dataRevision = wave.displayDataRevision,
            .sampleFrequencyHz = wave.view.sampleFrequencyHz,
            .visibleMinTime = limits.X.Min,
            .visibleMaxTime = limits.X.Max,
            .channelIndex = channelIndex,
            .pointLimit = pointLimit,
            .sampleCount = samples.size(),
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
        const auto key = makeRenderEnvelopeCacheKey(wave, channel, channelIndex, samples, limits, pointLimit);
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

    std::uint64_t rawBitsFromSampleValue(double value)
    {
        if (!std::isfinite(value)) {
            return 0;
        }
        const double truncated = std::trunc(value);
        if (truncated <= 0.0) {
            return 0;
        }
        const double maxValue = static_cast<double>((std::numeric_limits<std::uint64_t>::max)());
        if (truncated >= maxValue) {
            return (std::numeric_limits<std::uint64_t>::max)();
        }
        return static_cast<std::uint64_t>(truncated);
    }

    bool rawBitEnabled(double value, std::size_t bitIndex)
    {
        if (bitIndex >= plot::kMaxBitDisplayCount) {
            return false;
        }
        return ((rawBitsFromSampleValue(value) >> bitIndex) & 1ULL) != 0ULL;
    }

    plot::WaveDockState::BitRenderCacheKey makeBitRenderCacheKey(const plot::WaveDockState& wave,
                                                                 std::size_t channelIndex,
                                                                 const ImPlotRect& limits,
                                                                 const plot::BitDisplaySpec& spec,
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
        const auto key =
            makeBitRenderCacheKey(wave, channelIndex, limits, channel.bitDisplay, plotPixelWidth, vertexBudget);
        if (!entry.valid || !(entry.key == key)) {
            entry.valid = true;
            entry.key = key;
            buildBitRenderCacheEntry(entry, channel, displayChannel, bitLayout, channelIndex, limits, vertexBudget);
        }
        return entry;
    }

    void drawBitLaneLabels(const plot::ChannelView& channel,
                           std::size_t channelIndex,
                           const BitLaneLayout& bitLayout,
                           const ImPlotRect& limits,
                           ImU32 textColor)
    {
        auto* drawList = ImPlot::GetPlotDrawList();
        if (drawList == nullptr) {
            return;
        }
        const ImVec2 plotPos = ImPlot::GetPlotPos();
        for (std::size_t laneIndex = 0; laneIndex < channel.bitDisplay.bitCount; ++laneIndex) {
            const std::size_t bitIndex = channel.bitDisplay.firstBit + laneIndex;
            double centerValue = 0.0;
            for (const auto& layoutLane : bitLayout.lanes) {
                if (layoutLane.parentChannelIndex == channelIndex && layoutLane.laneIndex == laneIndex) {
                    centerValue = layoutLane.centerY;
                    break;
                }
            }
            const ImVec2 lanePixel = ImPlot::PlotToPixels(limits.X.Min, centerValue);
            const std::string label = channel.label + "." + std::to_string(bitIndex);
            drawList->AddText(
                ImVec2(plotPos.x + 6.0F, lanePixel.y - ImGui::GetTextLineHeight() * 0.5F), textColor, label.c_str());
        }
    }

    void drawBitRenderLanes(const plot::ChannelView& channel,
                            std::size_t channelIndex,
                            const plot::WaveDockState::BitRenderCacheEntry& entry,
                            const BitLaneLayout& bitLayout,
                            const ImVec4& color,
                            float lineWidth,
                            const ImPlotRect& limits)
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
        drawBitLaneLabels(channel, channelIndex, bitLayout, limits, labelColor);
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
            drawBitRenderLanes(channel, channelIndex, entry, outBitLayout, color, lineWidth, limits);
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
            view.lastRenderPointCount += rawVisibleCount;

            // 核心流程：低密度视图直接绘制原始点，避免桶包络把单条波形误画成双边界。
            WaveSampleGetterPayload payload{.samples = &(*begin)};
            ImPlotSpec spec{};
            spec.LineColor = color;
            spec.LineWeight = lineWidth;
            applySavedLegendVisibility(wave, channel.label);
            ImPlot::PlotLineG(
                channel.label.c_str(), &waveSampleGetter, &payload, static_cast<int>(rawVisibleCount), spec);
            if (!currentPlotItemVisible(channel.label)) {
                continue;
            }
            visibleChannelIndices.push_back(channelIndex);
            view.lastRenderSourceSampleCount += sourceSampleCount;
            if (view.showPointsWhenSparse) {
                ImPlotSpec pointSpec{};
                pointSpec.Marker = ImPlotMarker_Circle;
                pointSpec.MarkerSize = 2.5F;
                pointSpec.MarkerFillColor = color;
                pointSpec.MarkerLineColor = color;
                pointSpec.LineWeight = 0.0F;
                ImPlot::PlotScatterG((channel.label + " samples").c_str(),
                                     &waveSampleGetter,
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
        view.lastRenderSourceSampleCount += sourceSampleCount;
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
        view.lastRenderPointCount += envelope.size();
        if (view.phosphorGlowEnabled) {
            renderPhosphorEnvelope(
                envelope, color, limits.X.Max, view.persistenceWindow, view.glowIntensity, lineWidth);
        } else {
            renderEnvelopeAsBars(envelope, color, lineWidth);
        }
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
    const auto hovered = findHoverReadout(
        snapshot,
        displayData,
        visibleChannelIndices,
        bitLayout,
        mousePos.x,
        mousePos.y,
        timeSnapDistance,
        valueSnapDistance,
        view.preferWaveformHoverReadout);
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

    double cursorSearchAnchorY(const plot::WaveCursorState& cursor,
                               const std::optional<plot::CursorReadout>& previousReadout,
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
        return cursor.value;
    }

} // namespace

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
        ImPlot::DragLineX(static_cast<int>(100 + cursorIndex),
                          &dragTime,
                          ImVec4(cursorIndex == 0 ? 0.2F : 1.0F, 0.9F, 0.3F, 1.0F),
                          1.5F,
                          dragFlags,
                          &clicked,
                          &hovered,
                          &held);
        anyCursorHeld = anyCursorHeld || held;
        if (held && smartSnapActive) {
            // 核心流程：先用 DragLineX 写入的鼠标时间查吸附，再回写游标时间，配合 Delayed 让绘制使用受约束位置。
            auto smartSnapTarget = findSmartCursorSnapByScope(
                snapshot, displayData, view, bitLayout, dragTime, mousePos.y, limits, smartSnapDistance);
            if (smartSnapTarget.has_value()) {
                smartSnap = smartSnapTarget->readout;
                snapLabel = smartSnapTarget->label;
            }
        }
        cursor.time = held ? plot::applyCursorDragSnap(dragTime, smartSnap) : dragTime;
        if (held && !view.cursorIntervalLocked && view.cursors[0].enabled && view.cursors[1].enabled) {
            view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
        }

        const double searchY = cursorSearchAnchorY(cursor, cursorReadouts[cursorIndex], mousePos.y, held);
        auto best = findNearestCursorByScope(
            snapshot, displayData, view, bitLayout, cursor.time, searchY, timeSnapDistance, valueSnapDistance);
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
                                   "C%zu %s%s.%zu %s\nvalue %d",
                                   cursorIndex + 1,
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

PlotRenderResult drawOscilloscopePlot(plot::WaveDockState& wave, const WaveFrameData& frame)
{
    PlotRenderResult result;
    if (frame.fullSnapshot == nullptr || frame.displayData == nullptr || frame.fullSnapshot->channels.empty()) {
        ImGui::TextUnformatted("Lua 尚未通过 proto.plot.setup / proto.plot.push 提供波形数据。");
        return result;
    }
    auto& view = wave.view;

    if (!view.showAxisLabels) {
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10.0F, 10.0F));
        ImPlot::PushStyleVar(ImPlotStyleVar_LabelPadding, ImVec2(8.0F, 6.0F));
    }
    auto& inputMap = ImPlot::GetInputMap();
    const auto savedInputMap = inputMap;
    if (view.controlMode == plot::WaveControlMode::Oscilloscope) {
        inputMap.PanMod = ImGuiMod_Ctrl;
        inputMap.Fit = ImGuiMouseButton_Middle;
        inputMap.ZoomMod = ImGuiMod_Ctrl;
    }
    if (!ImPlot::BeginPlot("##oscilloscope", ImVec2(-1.0F, -1.0F), ImPlotFlags_NoLegend)) {
        inputMap = savedInputMap;
        if (!view.showAxisLabels) {
            ImPlot::PopStyleVar(2);
        }
        return result;
    }

    result.plotRendered = true;
    const auto& displayData = *frame.displayData;
    const auto& renderDisplayData = frame.renderDisplayData != nullptr ? *frame.renderDisplayData : displayData;
    const auto derivedChannelIndices = channelIndicesForDerivedViews(wave, frame.snapshot);
    const auto derivedBounds = boundsForDerivedViews(wave, frame.snapshot, displayData, derivedChannelIndices);
    const auto yAutoFitBounds = boundsForYAxisAutoFit(wave, frame.snapshot, displayData, derivedChannelIndices);
    auto fullHistoryBounds = derivedBounds;
    if (frame.fullSnapshot != nullptr && frame.overviewDisplayData != nullptr) {
        const auto fullHistoryChannelIndices = channelIndicesForDerivedViews(wave, *frame.fullSnapshot);
        // 核心流程：X 轴双击默认查看当前内存保留的完整历史，复用概览缓存避免额外复制全量样本。
        fullHistoryBounds = plot::computeDisplayBoundsForChannels(
            *frame.overviewDisplayData, fullHistoryChannelIndices, (std::max)(view.minVisibleTimeSpan, 1e-6));
    }
    applyMainPlotAxesAndLimits(view, frame.snapshot, displayData);

    const ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
    const ImPlotRect limits = ImPlot::GetPlotLimits();
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
    if (view.activeBitLane.active && !activeBitLaneVisible(view, bitLayout)) {
        view.activeBitLane = {};
    }
    syncLegendVisibilityState(wave, frame.snapshot);
    const auto fitChannelIndices = excludesLegendHiddenChannels(view)
                                       ? channelIndicesForDerivedViews(wave, frame.snapshot)
                                       : visibleChannelIndicesForFit(frame.snapshot);
    viewportChangedThisFrame =
        applyFitVisibleWaveforms(view, displayData, fitChannelIndices) || viewportChangedThisFrame;
    const auto zoomSelectionResult = handleMainPlotZoomSelection(view, wave.suppressZoomSelectionEscapeThisFrame);
    viewportChangedThisFrame = zoomSelectionResult.viewportChanged || viewportChangedThisFrame;
    if (!axisDoubleClickConsumed) {
        viewportChangedThisFrame =
            applyPendingVerticalAutoFitOverride(view, yAutoFitBounds) || viewportChangedThisFrame;
    }
    const bool offsetReset =
        !zoomSelectionResult.consumed &&
        handleActiveWaveformDoubleClickOffsetReset(
            wave, frame.snapshot, bitLayout, displayData, mousePos, timeSnapDistance, valueSnapDistance);
    const bool blockPlotInteractions = zoomSelectionResult.consumed || offsetReset;
    if (!blockPlotInteractions) {
        handleHoverReadout(view,
                           frame.snapshot,
                           displayData,
                           visibleChannelIndices,
                           bitLayout,
                           mousePos,
                           timeSnapDistance,
                           valueSnapDistance);
        viewportChangedThisFrame = handleOscilloscopeChannelInteractions(wave,
                                                                         frame.snapshot,
                                                                         displayData,
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
                                                                         displayData,
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
    inputMap = savedInputMap;
    if (!view.showAxisLabels) {
        ImPlot::PopStyleVar(2);
    }
    return result;
}

} // namespace protoscope::ui
