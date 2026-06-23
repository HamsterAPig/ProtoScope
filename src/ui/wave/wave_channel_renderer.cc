#include "wave_render_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
{
    return value == 0 ? fallback : value;
}

std::size_t estimateVerticesPerPoint(const bool glowEnabled)
{
    return glowEnabled ? 16 : 6;
}

RenderBudget makeRenderBudget(const plot::WaveViewState& view,
                              const std::size_t channelCount,
                              std::size_t pixelWidth,
                              const bool glowEnabled)
{
    const std::size_t safeChannelCount = (std::max)(std::size_t{1}, channelCount);
    const std::size_t estimatedVerticesPerPoint = estimateVerticesPerPoint(glowEnabled);
    const std::size_t configuredPointLimit = clampRenderConfig(view.maxRenderPointsPerChannel, 1200);
    const std::size_t configuredVertexLimit = clampRenderConfig(view.maxRenderVertices, 60000);
    const std::size_t pointsByVertexBudget =
        (std::max)(std::size_t{1}, configuredVertexLimit / (safeChannelCount * estimatedVerticesPerPoint));
    // 核心流程：每通道最终点数同时受像素宽度、用户配置和 16-bit 顶点预算约束，避免单帧 DrawList 溢出。
    const std::size_t pointsPerChannel =
        (std::max)(std::size_t{1}, (std::min)({pixelWidth, configuredPointLimit, pointsByVertexBudget}));
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

ImVec4 fallbackChannelColor(const std::size_t channelIndex)
{
    constexpr std::array<ImVec4, 8> kPalette{{
        {0.216F, 0.886F, 0.478F, 1.0F},
        {0.200F, 0.780F, 1.000F, 1.0F},
        {1.000F, 0.761F, 0.278F, 1.0F},
        {0.714F, 0.427F, 1.000F, 1.0F},
        {1.000F, 0.365F, 0.365F, 1.0F},
        {0.333F, 0.914F, 0.886F, 1.0F},
        {1.000F, 0.561F, 0.220F, 1.0F},
        {0.620F, 0.800F, 1.000F, 1.0F},
    }};
    return kPalette[channelIndex % kPalette.size()];
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
{
    return spec.enabled && spec.bitCount > 0 && spec.firstBit + spec.bitCount <= plot::kMaxBitDisplayCount;
}

std::uint64_t rawBitsFromSampleValue(const double value)
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

bool rawBitEnabled(const double value, const std::size_t bitIndex)
{
    if (bitIndex >= plot::kMaxBitDisplayCount) {
        return false;
    }
    return ((rawBitsFromSampleValue(value) >> bitIndex) & 1ULL) != 0ULL;
}

std::string bitLaneDisplayLabel(const std::size_t bitIndex)
{
    return "bit " + std::to_string(bitIndex);
}

double bitDisplayLanePitch()
{
    return 1.25;
}

double bitDisplayLaneHeight()
{
    return 0.75;
}

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
        minValue = (std::min)(minValue, laneBase);
        maxValue = (std::max)(maxValue, laneBase + bitDisplayLaneHeight());
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

namespace {

    float sanitizedGlowIntensity(const double glowIntensity)
    {
        if (!std::isfinite(glowIntensity)) {
            return 1.0F;
        }
        return static_cast<float>((std::clamp)(glowIntensity, 0.0, 3.0));
    }

    struct GlowStyle {
        float coreWidth{1.0F};
        float innerWidth{3.0F};
        float outerWidth{5.0F};
        float coreAlpha{1.0F};
        float innerAlpha{0.22F};
        float outerAlpha{0.10F};
    };

    GlowStyle makeGlowStyle(const double glowIntensity, const float lineWidth)
    {
        const float coreLineWidth = plot::sanitizeChannelLineWidth(lineWidth);
        const float intensity = sanitizedGlowIntensity(glowIntensity);
        return {
            .coreWidth = coreLineWidth,
            .innerWidth = (std::min)(coreLineWidth + 2.0F, 4.5F),
            .outerWidth = (std::min)(coreLineWidth + 4.0F, 7.0F),
            .coreAlpha = 1.0F,
            .innerAlpha = (std::clamp)(0.18F * intensity, 0.0F, 0.55F),
            .outerAlpha = (std::clamp)(0.08F * intensity, 0.0F, 0.35F),
        };
    }

    void drawGlowLine(ImDrawList* drawList, const ImVec2& from, const ImVec2& to, const ImVec4& color, GlowStyle style)
    {
        drawList->AddLine(from, to, ImGui::ColorConvertFloat4ToU32(withAlpha(color, style.outerAlpha)), style.outerWidth);
        drawList->AddLine(from, to, ImGui::ColorConvertFloat4ToU32(withAlpha(color, style.innerAlpha)), style.innerWidth);
        drawList->AddLine(from, to, ImGui::ColorConvertFloat4ToU32(withAlpha(color, style.coreAlpha)), style.coreWidth);
    }

} // namespace

void renderGlowEnvelope(const std::vector<plot::EnvelopePoint>& points,
                        const ImVec4& color,
                        const double glowIntensity,
                        const float lineWidth)
{
    if (points.empty()) {
        return;
    }

    const GlowStyle style = makeGlowStyle(glowIntensity, lineWidth);
    const GlowStyle connectorStyle{
        .coreWidth = (std::max)(style.coreWidth, 1.2F),
        .innerWidth = style.innerWidth,
        .outerWidth = (std::min)(style.coreWidth + 3.5F, 5.0F),
        .coreAlpha = 0.78F,
        .innerAlpha = style.innerAlpha,
        .outerAlpha = style.outerAlpha,
    };
    auto* drawList = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    bool hasPrevMid = false;
    ImVec2 prevMid{};
    for (const auto& [time, minValue, maxValue, sampleCount] : points) {
        static_cast<void>(sampleCount);
        const ImVec2 minPos = ImPlot::PlotToPixels(time, minValue);
        const ImVec2 maxPos = ImPlot::PlotToPixels(time, maxValue);
        const auto midPos = ImVec2(minPos.x, 0.5F * (minPos.y + maxPos.y));

        drawGlowLine(drawList, minPos, maxPos, color, style);
        drawList->AddCircleFilled(
            midPos, 1.5F, ImGui::ColorConvertFloat4ToU32(withAlpha(color, (std::min)(1.0F, color.w))));

        if (hasPrevMid) {
            drawGlowLine(drawList, prevMid, midPos, color, connectorStyle);
        }
        hasPrevMid = true;
        prevMid = midPos;
    }

    ImPlot::PopPlotClipRect();
}

void renderGlowSamples(const plot::WaveSample* samples,
                       const std::size_t sampleCount,
                       const ImVec4& color,
                       const double glowIntensity,
                       const float lineWidth)
{
    if (samples == nullptr || sampleCount == 0) {
        return;
    }

    const GlowStyle style = makeGlowStyle(glowIntensity, lineWidth);
    auto* drawList = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    if (sampleCount == 1) {
        const ImVec2 pos = ImPlot::PlotToPixels(samples[0].time, samples[0].value);
        drawList->AddCircleFilled(pos, 1.5F, ImGui::ColorConvertFloat4ToU32(color));
        ImPlot::PopPlotClipRect();
        return;
    }
    ImVec2 previous = ImPlot::PlotToPixels(samples[0].time, samples[0].value);
    for (std::size_t index = 1; index < sampleCount; ++index) {
        const ImVec2 current = ImPlot::PlotToPixels(samples[index].time, samples[index].value);
        drawGlowLine(drawList, previous, current, color, style);
        previous = current;
    }
    ImPlot::PopPlotClipRect();
}

} // namespace protoscope::ui
