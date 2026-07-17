#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace protoscope::ui {

void drawOverviewWindow(plot::WaveViewState& view,
                        const plot::ViewConfig& config,
                        const plot::WaveSnapshot& fullSnapshot,
                        const plot::WaveDisplayData& displayData,
                        const plot::WaveDataBounds& displayBounds,
                        const std::vector<std::size_t>& channelIndices,
                        const RenderBudget& renderBudget)
{
    if (fullSnapshot.channels.empty()) {
        return;
    }

    double overviewMinTime = displayBounds.valid ? displayBounds.minTime : std::numeric_limits<double>::infinity();
    double overviewMaxTime = displayBounds.valid ? displayBounds.maxTime : -std::numeric_limits<double>::infinity();
    double overviewMinValue = displayBounds.valid ? displayBounds.minValue : std::numeric_limits<double>::infinity();
    double overviewMaxValue = displayBounds.valid ? displayBounds.maxValue : -std::numeric_limits<double>::infinity();
    if (!displayBounds.valid) {
        for (const std::size_t channelIndex : channelIndices) {
            if (channelIndex >= fullSnapshot.channels.size()) {
                continue;
            }
            const auto& channel = fullSnapshot.channels[channelIndex];
            if (channel.totalSamples == 0 || channel.samples == nullptr) {
                continue;
            }
            const auto& first = channel.samples[0];
            const auto& last = channel.samples[channel.totalSamples - 1];
            overviewMinTime = (std::min)(overviewMinTime, first.time);
            overviewMaxTime = (std::max)(overviewMaxTime, last.time);
            if (channel.stats.visibleSamples > 0) {
                overviewMinValue = (std::min)(overviewMinValue, channel.stats.minValue);
                overviewMaxValue = (std::max)(overviewMaxValue, channel.stats.maxValue);
            }
        }
    }
    if (!std::isfinite(overviewMinTime) || !std::isfinite(overviewMaxTime) || overviewMinTime >= overviewMaxTime) {
        return;
    }
    if (!std::isfinite(overviewMinValue) || !std::isfinite(overviewMaxValue) || overviewMinValue >= overviewMaxValue) {
        overviewMinValue = config.verticalMin;
        overviewMaxValue = config.verticalMax;
    }

    const ImPlotFlags plotFlags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText |
                                  ImPlotFlags_NoMenus | ImPlotFlags_NoFrame;
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    // 概览图需要跟随 splitter 压缩，避免 ImPlot 默认 150px 最小高度撑住内部绘图区。
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotMinSize, ImVec2(64.0F, 24.0F));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(2.0F, 2.0F));
    if (ImPlot::BeginPlot("##wave_overview", ImVec2(-1.0F, -1.0F), plotFlags)) {
        constexpr ImPlotAxisFlags axisFlags =
            ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoDecorations;
        ImPlot::SetupAxis(ImAxis_X1, nullptr, axisFlags);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, axisFlags);
        ImPlot::SetupAxisLimits(ImAxis_X1, overviewMinTime, overviewMaxTime, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, overviewMinValue, overviewMaxValue, ImPlotCond_Always);

        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const std::size_t pixelWidth = static_cast<std::size_t>((std::max)(contentWidth, 64.0F));
        const auto overviewMaxSamples = view.adaptiveOverviewMaxSamples.value_or(view.overviewMaxSamples);
        const std::size_t overviewPointLimit =
            overviewMaxSamples > 0
                ? (std::min)({pixelWidth, renderBudget.pointsPerChannel, overviewMaxSamples})
                : (std::min)(pixelWidth, renderBudget.pointsPerChannel);
        for (const std::size_t channelIndex : channelIndices) {
            if (channelIndex >= fullSnapshot.channels.size() || channelIndex >= displayData.channels.size()) {
                continue;
            }
            auto overview = buildDisplayEnvelope(
                displayData.channels[channelIndex].samples, overviewMinTime, overviewMaxTime, overviewPointLimit);
            if (overview.empty()) {
                continue;
            }
            PlotGetterPayload payload{.points = overview.data()};
            const auto color = withAlpha(channelColor(fullSnapshot.channels[channelIndex], channelIndex), 0.65F);
            ImPlotSpec spec{};
            spec.LineColor = color;
            spec.LineWeight = 1.0F;
            spec.Flags = ImPlotItemFlags_NoLegend | ImPlotItemFlags_NoFit;
            const auto minItemLabel = std::string(fullSnapshot.channels[channelIndex].label) +
                                      " overview min##wave_channel_overview_min_" + std::to_string(channelIndex);
            const auto maxItemLabel = std::string(fullSnapshot.channels[channelIndex].label) +
                                      " overview max##wave_channel_overview_max_" + std::to_string(channelIndex);
            ImPlot::PlotLineG(minItemLabel.c_str(),
                              reinterpret_cast<ImPlotGetter>(&envelopeLineMinGetter),
                              &payload,
                              static_cast<int>(overview.size()),
                              spec);
            ImPlot::PlotLineG(maxItemLabel.c_str(),
                              reinterpret_cast<ImPlotGetter>(&envelopeLineMaxGetter),
                              &payload,
                              static_cast<int>(overview.size()),
                              spec);
        }

        double rectMinTime = view.viewMinTime;
        double rectMaxTime = view.viewMaxTime;
        double rectMinValue = overviewMinValue;
        double rectMaxValue = overviewMaxValue;
        bool rectHovered = false;
        bool rectHeld = false;
        const plot::WaveDataBounds overviewBounds{
            .minTime = overviewMinTime,
            .maxTime = overviewMaxTime,
            .minValue = overviewMinValue,
            .maxValue = overviewMaxValue,
            .minStep = minVisibleTimeSpan,
            .valid = true,
        };
        if (ImPlot::DragRect(300,
                             &rectMinTime,
                             &rectMinValue,
                             &rectMaxTime,
                             &rectMaxValue,
                             ImVec4(1.0F, 0.85F, 0.2F, 0.35F),
                             ImPlotDragToolFlags_NoFit,
                             nullptr,
                             &rectHovered,
                             &rectHeld)) {
            const auto normalized = plot::normalizeOverviewViewport({.minTime = rectMinTime,
                                                                     .maxTime = rectMaxTime,
                                                                     .minValue = view.viewMinValue,
                                                                     .maxValue = view.viewMaxValue},
                                                                    overviewBounds,
                                                                    minVisibleTimeSpan);
            view.viewMinTime = normalized.minTime;
            view.viewMaxTime = normalized.maxTime;
            view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, minVisibleTimeSpan);
            view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
            applyAutoFollowPausePolicy(view, WaveViewportAutoFollowPolicy::OverviewDrag);
            view.forceNextMainPlotLimits = true;
        }
        const auto mousePlotPos = ImPlot::GetPlotMousePos();
        const ImVec2 rectMinPixel = ImPlot::PlotToPixels((std::min)(rectMinTime, rectMaxTime), overviewMaxValue);
        const ImVec2 rectMaxPixel = ImPlot::PlotToPixels((std::max)(rectMinTime, rectMaxTime), overviewMinValue);
        const ImVec2 mousePixel = ImGui::GetMousePos();
        constexpr float kDragEdgePadding = 8.0F;
        const bool mouseInsideWindowBody =
            ImPlot::IsPlotHovered() && mousePixel.x > rectMinPixel.x + kDragEdgePadding &&
            mousePixel.x < rectMaxPixel.x - kDragEdgePadding && mousePixel.y > rectMinPixel.y + kDragEdgePadding &&
            mousePixel.y < rectMaxPixel.y - kDragEdgePadding;
        if (mouseInsideWindowBody && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.overviewWindowDragging = true;
            view.overviewDragLastTime = mousePlotPos.x;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            view.overviewWindowDragging = false;
        }
        if (view.overviewWindowDragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const double deltaTime = mousePlotPos.x - view.overviewDragLastTime;
            const auto moved =
                plot::moveViewportByDelta(currentViewport(view), deltaTime, overviewBounds, minVisibleTimeSpan);
            applyViewport(view, moved, WaveViewportAutoFollowPolicy::OverviewDrag);
            view.overviewDragLastTime = mousePlotPos.x;
        }
        if ((rectHovered || rectHeld) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            applyAutoFollowPausePolicy(view, WaveViewportAutoFollowPolicy::OverviewDrag);
        }
        const auto& io = ImGui::GetIO();
        if (ImPlot::IsPlotHovered() && io.MouseWheel != 0.0F) {
            const auto mousePos = ImPlot::GetPlotMousePos();
            const double centerTime =
                std::isfinite(mousePos.x) ? mousePos.x : 0.5 * (view.viewMinTime + view.viewMaxTime);
            const auto zoomed = plot::zoomViewport(currentViewport(view),
                                                   plot::WaveZoomMode::XOnly,
                                                   io.MouseWheel,
                                                   centerTime,
                                                   0.0,
                                                   overviewBounds,
                                                   minVisibleTimeSpan,
                                                   true);
            applyViewport(view, zoomed, WaveViewportAutoFollowPolicy::UserInteraction);
        }
        for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
            const auto& cursor = view.cursors[cursorIndex];
            if (!cursor.enabled) {
                continue;
            }
            double lineTime = cursor.time;
            const bool highlighted = lineTime >= view.viewMinTime && lineTime <= view.viewMaxTime;
            ImPlot::DragLineX(static_cast<int>(400 + cursorIndex),
                              &lineTime,
                              highlighted ? ImVec4(1.0F, 0.95F, 0.2F, 0.95F) : ImVec4(1.0F, 1.0F, 1.0F, 0.35F),
                              highlighted ? 1.3F : 1.0F,
                              ImPlotDragToolFlags_NoInputs | ImPlotDragToolFlags_NoFit);
        }
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar(2);
}

} // namespace protoscope::ui
