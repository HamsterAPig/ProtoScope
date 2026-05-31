#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>

namespace protoscope::ui {

ImPlotPoint envelopeLineMinGetter(int index, void* data) {
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].minValue};
}

ImPlotPoint envelopeLineMaxGetter(int index, void* data) {
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].maxValue};
}

ImPlotPoint waveSampleGetter(int index, void* data) {
    const auto* payload = static_cast<const WaveSampleGetterPayload*>(data);
    return ImPlotPoint{payload->samples[index].time, payload->samples[index].value};
}

std::size_t clampRenderConfig(std::size_t value, std::size_t fallback) {
    return value == 0 ? fallback : value;
}

std::size_t estimateVerticesPerPoint(bool phosphorGlowEnabled) {
    return phosphorGlowEnabled ? 16 : 6;
}

RenderBudget makeRenderBudget(const plot::WaveViewState& view,
                              std::size_t channelCount,
                              std::size_t pixelWidth,
                              bool phosphorGlowEnabled) {
    const std::size_t safeChannelCount = (std::max)(std::size_t{1}, channelCount);
    const std::size_t estimatedVerticesPerPoint = estimateVerticesPerPoint(phosphorGlowEnabled);
    const std::size_t configuredPointLimit = clampRenderConfig(view.maxRenderPointsPerChannel, 1200);
    const std::size_t configuredVertexLimit = clampRenderConfig(view.maxRenderVertices, 60000);
    const std::size_t pointsByVertexBudget = (std::max)(
        std::size_t{1}, configuredVertexLimit / (safeChannelCount * estimatedVerticesPerPoint));
    // 核心流程：每通道最终点数同时受像素宽度、用户配置和 16-bit 顶点预算约束，避免单帧 DrawList 溢出。
    const std::size_t pointsPerChannel = (std::max)(
        std::size_t{1},
        (std::min)({pixelWidth, configuredPointLimit, pointsByVertexBudget}));
    return RenderBudget{.pointsPerChannel = pointsPerChannel, .estimatedVerticesPerPoint = estimatedVerticesPerPoint};
}

void renderEnvelopeAsBars(const std::vector<plot::EnvelopePoint>& points, const ImVec4& color) {
    if (points.empty()) {
        return;
    }
    auto* drawList = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(color);
    for (const auto& point : points) {
        const ImVec2 minPos = ImPlot::PlotToPixels(point.time, point.minValue);
        const ImVec2 maxPos = ImPlot::PlotToPixels(point.time, point.maxValue);
        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y), lineColor, 1.0F);
    }
    ImPlot::PopPlotClipRect();
}

ImVec4 withAlpha(ImVec4 color, float alphaScale) {
    color.w *= alphaScale;
    return color;
}

float phosphorFade(double latestTime, double pointTime, double persistenceWindow) {
    if (persistenceWindow <= 1e-12) {
        return 1.0F;
    }
    const double age = (std::max)(0.0, latestTime - pointTime);
    const double fade = 1.0 - age / persistenceWindow;
    return static_cast<float>((std::clamp)(fade, 0.08, 1.0));
}

float densityStrength(std::size_t sampleCount) {
    const double strength = std::log2(static_cast<double>(sampleCount) + 1.0) / 4.0;
    return static_cast<float>((std::clamp)(0.35 + strength, 0.35, 1.0));
}

ImVec4 fallbackChannelColor(std::size_t channelIndex) {
    return ImVec4(0.15F + 0.25F * static_cast<float>(channelIndex % 3),
                  0.75F,
                  0.35F + 0.2F * static_cast<float>((channelIndex + 1) % 3),
                  1.0F);
}

ImVec4 channelColor(const plot::ChannelSpec& spec, std::size_t channelIndex) {
    if (!spec.color.has_value()) {
        return fallbackChannelColor(channelIndex);
    }
    return ImVec4((*spec.color)[0], (*spec.color)[1], (*spec.color)[2], (*spec.color)[3]);
}

ImVec4 channelColor(const plot::ChannelView& channel, std::size_t channelIndex) {
    if (!channel.color.has_value()) {
        return fallbackChannelColor(channelIndex);
    }
    return ImVec4((*channel.color)[0], (*channel.color)[1], (*channel.color)[2], (*channel.color)[3]);
}

void renderPhosphorEnvelope(const std::vector<plot::EnvelopePoint>& points,
                            const ImVec4& color,
                            double latestTime,
                            double persistenceWindow,
                            double glowIntensity) {
    if (points.empty()) {
        return;
    }

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

        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.12F)), 7.0F);
        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.28F)), 3.0F);
        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.9F)), 1.0F);
        drawList->AddCircleFilled(midPos, 1.5F + 1.5F * alpha,
                                  ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.85F)));

        if (hasPrevMid) {
            const float lineAlpha = (std::min)(prevAlpha, alpha);
            drawList->AddLine(prevMid, midPos,
                              ImGui::ColorConvertFloat4ToU32(withAlpha(color, lineAlpha * 0.18F)), 5.0F);
            drawList->AddLine(prevMid, midPos,
                              ImGui::ColorConvertFloat4ToU32(withAlpha(color, lineAlpha * 0.75F)), 1.2F);
        }
        hasPrevMid = true;
        prevMid = midPos;
        prevAlpha = alpha;
    }

    ImPlot::PopPlotClipRect();
}

} // namespace protoscope::ui
