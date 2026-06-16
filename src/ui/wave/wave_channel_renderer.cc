#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace protoscope::ui {

ImPlotPoint envelopeLineMinGetter(const int index, const void* data)
{
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].minValue};
}

ImPlotPoint envelopeLineMaxGetter(const int index, const void* data)
{
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].maxValue};
}

ImPlotPoint waveSampleGetter(const int index, const void* data)
{
    const auto* payload = static_cast<const WaveSampleGetterPayload*>(data);
    return ImPlotPoint{payload->samples[index].time, payload->samples[index].value};
}

std::size_t clampRenderConfig(const std::size_t value, const std::size_t fallback)
{ return value == 0 ? fallback : value; }

std::size_t estimateVerticesPerPoint(const bool phosphorGlowEnabled)
{ return phosphorGlowEnabled ? 16 : 6; }

RenderBudget makeRenderBudget(const plot::WaveViewState& view,
                              const std::size_t channelCount,
                              std::size_t pixelWidth,
                              const bool phosphorGlowEnabled)
{
    const std::size_t safeChannelCount = (std::max) (std::size_t{1}, channelCount);
    const std::size_t estimatedVerticesPerPoint = estimateVerticesPerPoint(phosphorGlowEnabled);
    const std::size_t configuredPointLimit = clampRenderConfig(view.maxRenderPointsPerChannel, 1200);
    const std::size_t configuredVertexLimit = clampRenderConfig(view.maxRenderVertices, 60000);
    const std::size_t pointsByVertexBudget =
        (std::max) (std::size_t{1}, configuredVertexLimit / (safeChannelCount * estimatedVerticesPerPoint));
    // 核心流程：每通道最终点数同时受像素宽度、用户配置和 16-bit 顶点预算约束，避免单帧 DrawList 溢出。
    const std::size_t pointsPerChannel =
        (std::max) (std::size_t{1}, (std::min) ({pixelWidth, configuredPointLimit, pointsByVertexBudget}));
    return RenderBudget{.pointsPerChannel = pointsPerChannel, .estimatedVerticesPerPoint = estimatedVerticesPerPoint};
}

void renderEnvelopeAsBars(const std::vector<plot::EnvelopePoint>& points, const ImVec4& color, const float lineWidth)
{
    if (points.empty()) {
        return;
    }
    const float coreLineWidth = plot::sanitizeChannelLineWidth(lineWidth);
    auto* drawList = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(color);
    for (const auto& point : points) {
        const ImVec2 minPos = ImPlot::PlotToPixels(point.time, point.minValue);
        const ImVec2 maxPos = ImPlot::PlotToPixels(point.time, point.maxValue);
        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y), lineColor, coreLineWidth);
    }
    ImPlot::PopPlotClipRect();
}

ImVec4 withAlpha(ImVec4 color, const float alphaScale)
{
    color.w *= alphaScale;
    return color;
}

float phosphorFade(const double latestTime, const double pointTime, const double persistenceWindow)
{
    if (persistenceWindow <= 1e-12) {
        return 1.0F;
    }
    const double age = (std::max) (0.0, latestTime - pointTime);
    const double fade = 1.0 - age / persistenceWindow;
    return static_cast<float>((std::clamp) (fade, 0.08, 1.0));
}

float densityStrength(const std::size_t sampleCount)
{
    const double strength = std::log2(static_cast<double>(sampleCount) + 1.0) / 4.0;
    return static_cast<float>((std::clamp) (0.35 + strength, 0.35, 1.0));
}

ImVec4 fallbackChannelColor(const std::size_t channelIndex)
{
    return {0.15F + 0.25F * static_cast<float>(channelIndex % 3),
            0.75F,
            0.35F + 0.2F * static_cast<float>((channelIndex + 1) % 3),
            1.0F};
}

ImVec4 channelColor(const plot::ChannelSpec& spec, const std::size_t channelIndex)
{
    if (!spec.color.has_value()) {
        return fallbackChannelColor(channelIndex);
    }
    return {(*spec.color)[0], (*spec.color)[1], (*spec.color)[2], (*spec.color)[3]};
}

ImVec4 channelColor(const plot::ChannelView& channel, const std::size_t channelIndex)
{
    if (!channel.color.has_value()) {
        return fallbackChannelColor(channelIndex);
    }
    return {(*channel.color)[0], (*channel.color)[1], (*channel.color)[2], (*channel.color)[3]};
}

bool bitDisplayEnabled(const plot::BitDisplaySpec& spec)
{ return spec.enabled && spec.bitCount > 0 && spec.firstBit + spec.bitCount <= plot::kMaxBitDisplayCount; }

double bitDisplayLanePitch()
{ return 1.25; }

double bitDisplayLaneHeight()
{ return 0.75; }

std::vector<std::size_t> bitDisplayRowsForChannels(const plot::WaveSnapshot& snapshot,
                                                   const std::vector<std::size_t>& channelIndices)
{
    std::vector<std::size_t> rows;
    for (const std::size_t channelIndex : channelIndices) {
        if (channelIndex >= snapshot.channels.size()) {
            continue;
        }
        const auto& bitDisplay = snapshot.channels[channelIndex].bitDisplay;
        if (!bitDisplayEnabled(bitDisplay)) {
            continue;
        }
        rows.reserve(rows.size() + bitDisplay.bitCount);
        for (std::size_t laneIndex = 0; laneIndex < bitDisplay.bitCount; ++laneIndex) {
            rows.push_back(bitDisplay.firstBit + laneIndex);
        }
    }
    std::ranges::sort(rows);
    rows.erase(std::ranges::unique(rows).begin(), rows.end());
    return rows;
}

double bitDisplayGroupBase(const plot::WaveSnapshot& snapshot, const std::size_t channelIndex)
{
    if (channelIndex >= snapshot.channels.size()) {
        return 0.0;
    }
    std::vector<std::size_t> channelIndices;
    channelIndices.reserve(snapshot.channels.size());
    for (std::size_t index = 0; index < snapshot.channels.size(); ++index) {
        channelIndices.push_back(index);
    }
    const auto rows = bitDisplayRowsForChannels(snapshot, channelIndices);
    const auto bitIndex = snapshot.channels[channelIndex].bitDisplay.firstBit;
    const auto row = std::ranges::lower_bound(rows, bitIndex);
    if (row == rows.end() || *row != bitIndex) {
        return 0.0;
    }
    return static_cast<double>(std::distance(rows.begin(), row)) * bitDisplayLanePitch();
}

plot::WaveValueRange bitDisplayValueRange(const plot::WaveSnapshot& snapshot,
                                          const std::size_t channelIndex,
                                          const plot::BitDisplaySpec& spec)
{
    if (!bitDisplayEnabled(spec)) {
        return {};
    }
    static_cast<void>(channelIndex);
    std::vector<std::size_t> channelIndices;
    channelIndices.reserve(snapshot.channels.size());
    for (std::size_t index = 0; index < snapshot.channels.size(); ++index) {
        channelIndices.push_back(index);
    }
    const auto rows = bitDisplayRowsForChannels(snapshot, channelIndices);
    double minValue = std::numeric_limits<double>::infinity();
    double maxValue = -std::numeric_limits<double>::infinity();
    for (std::size_t laneIndex = 0; laneIndex < spec.bitCount; ++laneIndex) {
        const std::size_t bitIndex = spec.firstBit + laneIndex;
        const auto row = std::ranges::lower_bound(rows, bitIndex);
        if (row == rows.end() || *row != bitIndex) {
            continue;
        }
        const double laneBase =
            (static_cast<double>(std::distance(rows.begin(), row)) + spec.yOffset) * bitDisplayLanePitch();
        minValue = (std::min) (minValue, laneBase);
        maxValue = (std::max) (maxValue, laneBase + bitDisplayLaneHeight());
    }
    if (!std::isfinite(minValue) || !std::isfinite(maxValue)) {
        return {};
    }
    return {.minValue = minValue, .maxValue = maxValue};
}

namespace {

    double plotYFromPixel(const ImPlotRect& limits, const ImVec2& plotPos, const ImVec2& plotSize, const float pixelY)
    {
        if (plotSize.y <= 1.0F) {
            return 0.5 * (limits.Y.Min + limits.Y.Max);
        }
        const auto normalized = static_cast<double>((pixelY - plotPos.y) / plotSize.y);
        return limits.Y.Max - normalized * (limits.Y.Max - limits.Y.Min);
    }

} // namespace

BitLaneLayout buildBitLaneLayout(const plot::WaveSnapshot& snapshot,
                                 const std::vector<std::size_t>& visibleChannelIndices,
                                 const ImPlotRect& limits,
                                 const ImVec2& plotPos,
                                 const ImVec2& plotSize)
{
    BitLaneLayout layout;
    std::vector<std::size_t> bitChannels;
    bitChannels.reserve(visibleChannelIndices.size());
    for (const std::size_t channelIndex : visibleChannelIndices) {
        if (channelIndex >= snapshot.channels.size()) {
            continue;
        }
        if (const auto& channel = snapshot.channels[channelIndex]; !bitDisplayEnabled(channel.bitDisplay)) {
            continue;
        }
        bitChannels.push_back(channelIndex);
    }
    const auto bitRows = bitDisplayRowsForChannels(snapshot, bitChannels);
    if (bitChannels.empty() || bitRows.empty() || plotSize.y <= 1.0F) {
        return layout;
    }

    constexpr float kLowHighMarginRatio = 0.18F;
    const float lanePitch = plotSize.y / static_cast<float>(bitRows.size());
    const float lanePadding = lanePitch * kLowHighMarginRatio;

    std::size_t totalLaneCount = 0;
    for (const std::size_t channelIndex : bitChannels) {
        totalLaneCount += snapshot.channels[channelIndex].bitDisplay.bitCount;
    }
    layout.lanes.reserve(totalLaneCount);
    for (const std::size_t channelIndex : bitChannels) {
        const auto& channel = snapshot.channels[channelIndex];
        const float yOffsetPixels = static_cast<float>(channel.bitDisplay.yOffset) * lanePitch;
        for (std::size_t laneIndex = 0; laneIndex < channel.bitDisplay.bitCount; ++laneIndex) {
            const std::size_t bitIndex = channel.bitDisplay.firstBit + laneIndex;
            const auto row = std::ranges::lower_bound(bitRows, bitIndex);
            if (row == bitRows.end() || *row != bitIndex) {
                continue;
            }
            const auto rowIndex = static_cast<std::size_t>(std::distance(bitRows.begin(), row));
            const float laneTop = plotPos.y + static_cast<float>(rowIndex) * lanePitch + yOffsetPixels;
            const float lowPixel = laneTop + lanePitch - lanePadding;
            const float highPixel = laneTop + lanePadding;
            const float centerPixel = 0.5F * (lowPixel + highPixel);
            const double lowY = plotYFromPixel(limits, plotPos, plotSize, lowPixel);
            const double highY = plotYFromPixel(limits, plotPos, plotSize, highPixel);
            layout.lanes.push_back({
                .parentChannelIndex = channelIndex,
                .bitIndex = bitIndex,
                .laneIndex = laneIndex,
                .rowIndex = rowIndex,
                .lowY = lowY,
                .highY = highY,
                .centerY = 0.5 * (lowY + highY),
                .lowPixelY = lowPixel,
                .highPixelY = highPixel,
                .centerPixelY = centerPixel,
                .lanePixelPitch = lanePitch,
            });
        }
    }
    return layout;
}

std::optional<BitLaneHit> findBitLaneAtPlotValue(const BitLaneLayout& layout,
                                                 const double plotY,
                                                 const double maxDistance)
{
    std::optional<BitLaneHit> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    if (!std::isfinite(plotY) || !std::isfinite(maxDistance) || maxDistance < 0.0) {
        return std::nullopt;
    }
    for (const auto& lane : layout.lanes) {
        const double minY = (std::min) (lane.lowY, lane.highY);
        const double maxY = (std::max) (lane.lowY, lane.highY);
        double distance = 0.0;
        if (plotY < minY) {
            distance = minY - plotY;
        } else if (plotY > maxY) {
            distance = plotY - maxY;
        }
        if (distance > maxDistance) {
            continue;
        }
        const double centerDistance = std::abs(plotY - lane.centerY);
        if (const double score = distance * distance + centerDistance * centerDistance * 0.01;
            !best.has_value() || score < bestDistance) {
            bestDistance = score;
            best = BitLaneHit{.lane = lane, .distance = distance};
        }
    }
    return best;
}

std::optional<std::size_t> findBitDisplayChannelAtValue(const plot::WaveDockState& wave,
                                                        const plot::WaveSnapshot& snapshot,
                                                        const double value,
                                                        const double maxDistance)
{
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto& channel = snapshot.channels[channelIndex];
        if (!bitDisplayEnabled(channel.bitDisplay) || channelHiddenByLegendState(wave, channel.label)) {
            continue;
        }
        if (const auto [minValue, maxValue] = bitDisplayValueRange(snapshot, channelIndex, channel.bitDisplay);
            value >= minValue - maxDistance && value <= maxValue + maxDistance) {
            return channelIndex;
        }
    }
    return std::nullopt;
}

void renderPhosphorEnvelope(const std::vector<plot::EnvelopePoint>& points,
                            const ImVec4& color,
                            const double latestTime,
                            const double persistenceWindow,
                            const double glowIntensity,
                            const float lineWidth)
{
    if (points.empty()) {
        return;
    }

    const float coreLineWidth = plot::sanitizeChannelLineWidth(lineWidth);
    const float innerGlowWidth = (std::min) (coreLineWidth + 2.0F, 4.5F);
    const float outerGlowWidth = (std::min) (coreLineWidth + 4.0F, 7.0F);
    const float connectorCoreWidth = (std::max) (coreLineWidth, 1.2F);
    const float connectorGlowWidth = (std::min) (coreLineWidth + 3.5F, 5.0F);
    auto* drawList = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    bool hasPrevMid = false;
    ImVec2 prevMid{};
    float prevAlpha = 0.0F;
    for (const auto& [time, minValue, maxValue, sampleCount] : points) {
        const float fade = phosphorFade(latestTime, time, persistenceWindow);
        const float density = densityStrength(sampleCount);
        const float alpha = static_cast<float>((std::clamp) (fade * density * glowIntensity, 0.05, 1.0));

        const ImVec2 minPos = ImPlot::PlotToPixels(time, minValue);
        const ImVec2 maxPos = ImPlot::PlotToPixels(time, maxValue);
        const auto midPos = ImVec2(minPos.x, 0.5F * (minPos.y + maxPos.y));

        drawList->AddLine(ImVec2(minPos.x, minPos.y),
                          ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.12F)),
                          outerGlowWidth);
        drawList->AddLine(ImVec2(minPos.x, minPos.y),
                          ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.28F)),
                          innerGlowWidth);
        drawList->AddLine(ImVec2(minPos.x, minPos.y),
                          ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.9F)),
                          coreLineWidth);
        drawList->AddCircleFilled(
            midPos, 1.5F + 1.5F * alpha, ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.85F)));

        if (hasPrevMid) {
            const float lineAlpha = (std::min) (prevAlpha, alpha);
            drawList->AddLine(prevMid,
                              midPos,
                              ImGui::ColorConvertFloat4ToU32(withAlpha(color, lineAlpha * 0.18F)),
                              connectorGlowWidth);
            drawList->AddLine(prevMid,
                              midPos,
                              ImGui::ColorConvertFloat4ToU32(withAlpha(color, lineAlpha * 0.75F)),
                              connectorCoreWidth);
        }
        hasPrevMid = true;
        prevMid = midPos;
        prevAlpha = alpha;
    }

    ImPlot::PopPlotClipRect();
}

} // namespace protoscope::ui
