#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace protoscope::ui {

ImPlotPoint envelopeLineMinGetter(int index, void* data)
{
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].minValue};
}

ImPlotPoint envelopeLineMaxGetter(int index, void* data)
{
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].maxValue};
}

ImPlotPoint waveSampleGetter(int index, void* data)
{
    const auto* payload = static_cast<const WaveSampleGetterPayload*>(data);
    return ImPlotPoint{payload->samples[index].time, payload->samples[index].value};
}

std::size_t clampRenderConfig(std::size_t value, std::size_t fallback)
{
    return value == 0 ? fallback : value;
}

std::size_t estimateVerticesPerPoint(bool phosphorGlowEnabled)
{
    return phosphorGlowEnabled ? 16 : 6;
}

RenderBudget makeRenderBudget(const plot::WaveViewState& view,
                              std::size_t channelCount,
                              std::size_t pixelWidth,
                              bool phosphorGlowEnabled)
{
    const std::size_t safeChannelCount = (std::max)(std::size_t{1}, channelCount);
    const std::size_t estimatedVerticesPerPoint = estimateVerticesPerPoint(phosphorGlowEnabled);
    const std::size_t configuredPointLimit = clampRenderConfig(view.maxRenderPointsPerChannel, 1200);
    const std::size_t configuredVertexLimit = clampRenderConfig(view.maxRenderVertices, 60000);
    const std::size_t pointsByVertexBudget =
        (std::max)(std::size_t{1}, configuredVertexLimit / (safeChannelCount * estimatedVerticesPerPoint));
    // 核心流程：每通道最终点数同时受像素宽度、用户配置和 16-bit 顶点预算约束，避免单帧 DrawList 溢出。
    const std::size_t pointsPerChannel =
        (std::max)(std::size_t{1}, (std::min)({pixelWidth, configuredPointLimit, pointsByVertexBudget}));
    return RenderBudget{.pointsPerChannel = pointsPerChannel, .estimatedVerticesPerPoint = estimatedVerticesPerPoint};
}

void renderEnvelopeAsBars(const std::vector<plot::EnvelopePoint>& points, const ImVec4& color, float lineWidth)
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

ImVec4 withAlpha(ImVec4 color, float alphaScale)
{
    color.w *= alphaScale;
    return color;
}

float phosphorFade(double latestTime, double pointTime, double persistenceWindow)
{
    if (persistenceWindow <= 1e-12) {
        return 1.0F;
    }
    const double age = (std::max)(0.0, latestTime - pointTime);
    const double fade = 1.0 - age / persistenceWindow;
    return static_cast<float>((std::clamp)(fade, 0.08, 1.0));
}

float densityStrength(std::size_t sampleCount)
{
    const double strength = std::log2(static_cast<double>(sampleCount) + 1.0) / 4.0;
    return static_cast<float>((std::clamp)(0.35 + strength, 0.35, 1.0));
}

ImVec4 fallbackChannelColor(std::size_t channelIndex)
{
    return ImVec4(0.15F + 0.25F * static_cast<float>(channelIndex % 3),
                  0.75F,
                  0.35F + 0.2F * static_cast<float>((channelIndex + 1) % 3),
                  1.0F);
}

ImVec4 channelColor(const plot::ChannelSpec& spec, std::size_t channelIndex)
{
    if (!spec.color.has_value()) {
        return fallbackChannelColor(channelIndex);
    }
    return ImVec4((*spec.color)[0], (*spec.color)[1], (*spec.color)[2], (*spec.color)[3]);
}

ImVec4 channelColor(const plot::ChannelView& channel, std::size_t channelIndex)
{
    if (!channel.color.has_value()) {
        return fallbackChannelColor(channelIndex);
    }
    return ImVec4((*channel.color)[0], (*channel.color)[1], (*channel.color)[2], (*channel.color)[3]);
}

bool bitDisplayEnabled(const plot::BitDisplaySpec& spec)
{
    return spec.enabled && spec.bitCount > 0 && spec.firstBit + spec.bitCount <= plot::kMaxBitDisplayCount;
}

double bitDisplayLanePitch()
{
    return 1.25;
}

double bitDisplayLaneHeight()
{
    return 0.75;
}

double bitDisplayGroupBase(const plot::WaveSnapshot& snapshot, std::size_t channelIndex)
{
    constexpr double kGroupGap = 0.75;
    double base = 0.0;
    const std::size_t limit = (std::min)(channelIndex, snapshot.channels.size());
    for (std::size_t index = 0; index < limit; ++index) {
        const auto& bitDisplay = snapshot.channels[index].bitDisplay;
        if (!bitDisplayEnabled(bitDisplay)) {
            continue;
        }
        base += static_cast<double>(bitDisplay.bitCount) * bitDisplayLanePitch() + kGroupGap;
    }
    return base;
}

plot::WaveValueRange bitDisplayValueRange(const plot::WaveSnapshot& snapshot,
                                          std::size_t channelIndex,
                                          const plot::BitDisplaySpec& spec)
{
    if (!bitDisplayEnabled(spec)) {
        return {};
    }
    const double minValue = bitDisplayGroupBase(snapshot, channelIndex) + spec.yOffset;
    const double maxValue = minValue + static_cast<double>(spec.bitCount - 1U) * bitDisplayLanePitch() +
                            bitDisplayLaneHeight();
    return {.minValue = minValue, .maxValue = maxValue};
}

namespace {

double plotYFromPixel(const ImPlotRect& limits, const ImVec2& plotPos, const ImVec2& plotSize, float pixelY)
{
    if (plotSize.y <= 1.0F) {
        return 0.5 * (limits.Y.Min + limits.Y.Max);
    }
    const double normalized = static_cast<double>((pixelY - plotPos.y) / plotSize.y);
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
    std::size_t totalLaneCount = 0;
    for (const std::size_t channelIndex : visibleChannelIndices) {
        if (channelIndex >= snapshot.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[channelIndex];
        if (!bitDisplayEnabled(channel.bitDisplay)) {
            continue;
        }
        bitChannels.push_back(channelIndex);
        totalLaneCount += channel.bitDisplay.bitCount;
    }
    if (bitChannels.empty() || totalLaneCount == 0 || plotSize.y <= 1.0F) {
        return layout;
    }

    constexpr float kGroupGapRatio = 0.35F;
    constexpr float kLowHighMarginRatio = 0.18F;
    const float groupGap = bitChannels.size() > 1 ? (plotSize.y * kGroupGapRatio) / static_cast<float>(totalLaneCount) : 0.0F;
    const float availableHeight = (std::max)(plotSize.y - groupGap * static_cast<float>(bitChannels.size() - 1U), 1.0F);
    const float lanePitch = availableHeight / static_cast<float>(totalLaneCount);
    const float lanePadding = lanePitch * kLowHighMarginRatio;

    layout.lanes.reserve(totalLaneCount);
    float cursorTop = plotPos.y;
    for (std::size_t groupIndex = 0; groupIndex < bitChannels.size(); ++groupIndex) {
        const std::size_t channelIndex = bitChannels[groupIndex];
        const auto& channel = snapshot.channels[channelIndex];
        const float yOffsetPixels = static_cast<float>(channel.bitDisplay.yOffset) * lanePitch;
        for (std::size_t laneIndex = 0; laneIndex < channel.bitDisplay.bitCount; ++laneIndex) {
            const float laneTop = cursorTop + static_cast<float>(laneIndex) * lanePitch + yOffsetPixels;
            const float lowPixel = laneTop + lanePitch - lanePadding;
            const float highPixel = laneTop + lanePadding;
            const float centerPixel = 0.5F * (lowPixel + highPixel);
            const double lowY = plotYFromPixel(limits, plotPos, plotSize, lowPixel);
            const double highY = plotYFromPixel(limits, plotPos, plotSize, highPixel);
            layout.lanes.push_back({
                .parentChannelIndex = channelIndex,
                .bitIndex = channel.bitDisplay.firstBit + laneIndex,
                .laneIndex = laneIndex,
                .lowY = lowY,
                .highY = highY,
                .centerY = 0.5 * (lowY + highY),
                .lowPixelY = lowPixel,
                .highPixelY = highPixel,
                .centerPixelY = centerPixel,
                .lanePixelPitch = lanePitch,
            });
        }
        cursorTop += static_cast<float>(channel.bitDisplay.bitCount) * lanePitch + groupGap;
    }
    return layout;
}

std::optional<BitLaneHit> findBitLaneAtPlotValue(const BitLaneLayout& layout, double plotY, double maxDistance)
{
    std::optional<BitLaneHit> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    if (!std::isfinite(plotY) || !std::isfinite(maxDistance) || maxDistance < 0.0) {
        return std::nullopt;
    }
    for (const auto& lane : layout.lanes) {
        const double minY = (std::min)(lane.lowY, lane.highY);
        const double maxY = (std::max)(lane.lowY, lane.highY);
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
        const double score = distance * distance + centerDistance * centerDistance * 0.01;
        if (!best.has_value() || score < bestDistance) {
            bestDistance = score;
            best = BitLaneHit{.lane = lane, .distance = distance};
        }
    }
    return best;
}

std::optional<std::size_t> findBitDisplayChannelAtValue(const plot::WaveDockState& wave,
                                                        const plot::WaveSnapshot& snapshot,
                                                        double value,
                                                        double maxDistance)
{
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto& channel = snapshot.channels[channelIndex];
        if (!bitDisplayEnabled(channel.bitDisplay) || channelHiddenByLegendState(wave, channel.label)) {
            continue;
        }
        const auto range = bitDisplayValueRange(snapshot, channelIndex, channel.bitDisplay);
        if (value >= range.minValue - maxDistance && value <= range.maxValue + maxDistance) {
            return channelIndex;
        }
    }
    return std::nullopt;
}

void renderPhosphorEnvelope(const std::vector<plot::EnvelopePoint>& points,
                            const ImVec4& color,
                            double latestTime,
                            double persistenceWindow,
                            double glowIntensity,
                            float lineWidth)
{
    if (points.empty()) {
        return;
    }

    const float coreLineWidth = plot::sanitizeChannelLineWidth(lineWidth);
    const float innerGlowWidth = (std::min)(coreLineWidth + 2.0F, 4.5F);
    const float outerGlowWidth = (std::min)(coreLineWidth + 4.0F, 7.0F);
    const float connectorCoreWidth = (std::max)(coreLineWidth, 1.2F);
    const float connectorGlowWidth = (std::min)(coreLineWidth + 3.5F, 5.0F);
    auto* drawList = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    bool hasPrevMid = false;
    ImVec2 prevMid{};
    float prevAlpha = 0.0F;
    for (const auto& point : points) {
        const float fade = phosphorFade(latestTime, point.time, persistenceWindow);
        const float density = densityStrength(point.sampleCount);
        const float alpha = static_cast<float>((std::clamp)(fade * density * glowIntensity, 0.05, 1.0));

        const ImVec2 minPos = ImPlot::PlotToPixels(point.time, point.minValue);
        const ImVec2 maxPos = ImPlot::PlotToPixels(point.time, point.maxValue);
        const ImVec2 midPos = ImVec2(minPos.x, 0.5F * (minPos.y + maxPos.y));

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
            const float lineAlpha = (std::min)(prevAlpha, alpha);
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
