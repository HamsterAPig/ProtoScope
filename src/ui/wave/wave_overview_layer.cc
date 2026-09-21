#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace protoscope::ui {

void normalizeOverviewEnvelope(std::vector<plot::EnvelopePoint>& envelope)
{
    std::erase_if(envelope, [](const auto& point) {
        return !std::isfinite(point.time) || !std::isfinite(point.minValue) || !std::isfinite(point.maxValue);
    });
    if (envelope.empty()) {
        return;
    }
    double low = std::numeric_limits<double>::infinity();
    double high = -low;
    for (const auto& point : envelope) {
        low = (std::min)(low, point.minValue);
        high = (std::max)(high, point.maxValue);
    }
    // 核心逻辑：只归一化本次绘制副本，半跨度计算避免大幅值正负相减溢出。
    const double center = low * 0.5 + high * 0.5;
    const double halfSpan = high * 0.5 - low * 0.5;
    for (auto& point : envelope) {
        point.minValue = halfSpan > 0.0 ? (point.minValue * 0.5 - center * 0.5) / halfSpan * 2.0 : 0.0;
        point.maxValue = halfSpan > 0.0 ? (point.maxValue * 0.5 - center * 0.5) / halfSpan * 2.0 : 0.0;
    }
}

const plot::WaveDockState::OverviewRenderEntry& cachedOverviewChannel(
    plot::WaveDockState& wave, const plot::ChannelView& channel, std::size_t channelIndex,
    plot::WaveTimeAxisSource axis, double minTime, double maxTime, std::size_t width, std::size_t budget)
{
    if (wave.overviewRenderCache.size() <= channelIndex) wave.overviewRenderCache.resize(channelIndex + 1);
    auto& entry = wave.overviewRenderCache[channelIndex];
    const plot::WaveDockState::OverviewRenderKey key{
        wave.buffer.analysisRevision(), wave.buffer.historyEpoch(), channelIndex, width, budget, axis,
        wave.view.sampleFrequencyHz, minTime, maxTime, channel.ratio, channel.scale, channel.offset,
        wave.buffer.viewConfig().displayFormula, wave.view.overviewNormalizeChannels,
        wave.view.downsampleMode};
    if (entry.valid && entry.key == key) return entry;
    entry = {};
    entry.key = key;
    const plot::WaveQueryView query(channel, axis, key.frequency, key.formula);
    const auto [begin, end] = query.range(minTime, maxTime, false);
    if (end - begin <= budget) {
        for (auto i = begin; i < end; ++i) entry.trace.push_back(query.sample(i));
    } else {
        entry.envelope = query.timeEnvelope(minTime, maxTime, (std::min)(width, budget));
    }
    if (key.normalize) {
        double low = std::numeric_limits<double>::infinity(), high = -low;
        for (const auto& point : entry.trace) {
            if (!std::isfinite(point.value)) continue;
            low = (std::min)(low, point.value);
            high = (std::max)(high, point.value);
        }
        for (const auto& bucket : entry.envelope) {
            low = (std::min)(low, bucket.minValue);
            high = (std::max)(high, bucket.maxValue);
        }
        const auto center = low * 0.5 + high * 0.5;
        const auto halfSpan = high * 0.5 - low * 0.5;
        const auto normalize = [&](double value) {
            return halfSpan > 0 ? (value * 0.5 - center * 0.5) / halfSpan * 2 : 0.0;
        };
        // 归一化只写缓存内的绘制副本，原始样本和显示变换保持原值。
        for (auto& point : entry.trace) point.value = normalize(point.value);
        for (auto& bucket : entry.envelope) {
            bucket.minValue = normalize(bucket.minValue);
            bucket.maxValue = normalize(bucket.maxValue);
        }
    }
    entry.valid = true;
    ++wave.overviewQueryCount;
    return entry;
}

void drawOverviewWindow(plot::WaveDockState& wave,
                        const plot::ViewConfig& config,
                        const plot::WaveSnapshot& fullSnapshot,
                        const plot::WaveDisplayData& displayData,
                        const plot::WaveDataBounds& displayBounds,
                        const std::vector<std::size_t>& channelIndices,
                        const RenderBudget& renderBudget)
{
    auto& view = wave.view;
    if (fullSnapshot.channels.empty()) {
        return;
    }

    // 先筛选可见通道，再稳定地把 Bit 放到底层；折线与包络共用此顺序。
    std::vector<std::size_t> overviewChannels;
    overviewChannels.reserve(channelIndices.size());
    for (const auto index : channelIndices) {
        if (index < fullSnapshot.channels.size() && index < displayData.channels.size() &&
            !channelHiddenByLegendState(wave, index) &&
            (view.overviewShowBitChannels || !fullSnapshot.channels[index].bitDisplay.enabled)) {
            overviewChannels.push_back(index);
        }
    }
    std::stable_partition(overviewChannels.begin(), overviewChannels.end(), [&](std::size_t index) {
        return fullSnapshot.channels[index].bitDisplay.enabled;
    });
    const auto valueBounds =
        plot::computeDisplayBoundsForChannels(displayData, overviewChannels, view.minVisibleTimeSpan);
    // 时间范围独立于绘制筛选，全部 Bit 被隐藏时仍保留全历史窗口导航。
    double overviewMinTime = displayBounds.valid ? displayBounds.minTime : std::numeric_limits<double>::infinity();
    double overviewMaxTime = displayBounds.valid ? displayBounds.maxTime : -std::numeric_limits<double>::infinity();
    double overviewMinValue = valueBounds.valid ? valueBounds.minValue : std::numeric_limits<double>::infinity();
    double overviewMaxValue = valueBounds.valid ? valueBounds.maxValue : -std::numeric_limits<double>::infinity();
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
            if (!valueBounds.valid && channel.stats.visibleSamples > 0 &&
                (view.overviewShowBitChannels || !channel.bitDisplay.enabled)) {
                overviewMinValue = (std::min)(overviewMinValue, channel.stats.minValue);
                overviewMaxValue = (std::max)(overviewMaxValue, channel.stats.maxValue);
            }
        }
    }
    if (!std::isfinite(overviewMinTime) || !std::isfinite(overviewMaxTime) || overviewMinTime >= overviewMaxTime) {
        return;
    }
    if (!std::isfinite(overviewMinValue) || !std::isfinite(overviewMaxValue)) {
        overviewMinValue = config.verticalMin;
        overviewMaxValue = config.verticalMax;
    } else if (overviewMinValue >= overviewMaxValue) {
        const auto padding = (std::max)(1.0, std::abs(overviewMinValue) * 0.05);
        overviewMinValue -= padding;
        overviewMaxValue += padding;
    }
    if (view.overviewNormalizeChannels) {
        overviewMinValue = -1.0;
        overviewMaxValue = 1.0;
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

        const std::size_t pixelWidth = static_cast<std::size_t>((std::max)(ImPlot::GetPlotSize().x, 1.0F));
        const auto overviewMaxSamples = view.adaptiveOverviewMaxSamples.value_or(view.overviewMaxSamples);
        const std::size_t overviewPointLimit =
            overviewMaxSamples > 0
                ? (std::min)({pixelWidth, renderBudget.pointsPerChannel, overviewMaxSamples})
                : (std::min)(pixelWidth, renderBudget.pointsPerChannel);
        std::vector<plot::OverviewColor> drawnColors;
        for (const std::size_t channelIndex : overviewChannels) {
            const auto& overview = cachedOverviewChannel(wave, fullSnapshot.channels[channelIndex], channelIndex,
                displayData.axisSource, overviewMinTime, overviewMaxTime, pixelWidth, overviewPointLimit);
            const auto color = withAlpha(channelColor(fullSnapshot.channels[channelIndex], channelIndex), 0.65F);
            if (!overview.trace.empty() || !overview.envelope.empty())
                drawnColors.push_back({color.x, color.y, color.z, color.w});
            if (!overview.trace.empty()) {
                WaveSampleGetterPayload payload{.samples = overview.trace.data()};
                ImPlotSpec spec{};
                spec.LineColor = color;
                spec.LineWeight = 1.0F;
                spec.Flags = ImPlotItemFlags_NoLegend | ImPlotItemFlags_NoFit;
                const auto label = "##wave_overview_trace_" + std::to_string(channelIndex);
                ImPlot::PlotLineG(label.c_str(), reinterpret_cast<ImPlotGetter>(&waveSampleGetter),
                                  &payload, static_cast<int>(overview.trace.size()), spec);
            }
            ImPlot::PushPlotClipRect();
            auto* drawList = ImPlot::GetPlotDrawList();
            for (const auto& bucket : overview.envelope) {
                const auto a = ImPlot::PlotToPixels(bucket.beginTime, bucket.minValue);
                const auto b = ImPlot::PlotToPixels(bucket.endTime, bucket.maxValue);
                // 桶只表示这段时间内的幅值覆盖范围，不把桶中心或代表点连接成假周期。
                drawList->AddRectFilled(ImVec2(a.x, (std::min)(a.y, b.y)),
                    ImVec2((std::max)(a.x + 1.0F, b.x), (std::max)((std::min)(a.y, b.y) + 1.0F, (std::max)(a.y, b.y))),
                    ImGui::ColorConvertFloat4ToU32(color));
            }
            ImPlot::PopPlotClipRect();
        }

        const auto background = ImPlot::GetStyleColorVec4(ImPlotCol_PlotBg);
        const auto window = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const auto effectiveBackground = plot::compositeOverviewColor(
            {background.x, background.y, background.z, background.w}, {window.x, window.y, window.z, 1});
        const auto rectangleRgb = wave.overviewColorCache.resolve(drawnColors, effectiveBackground);
        const auto rectangleColor = plot::overviewRgb(rectangleRgb);
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
                             ImVec4(float(rectangleColor.r), float(rectangleColor.g), float(rectangleColor.b), 1.0F),
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
                              withAlpha(measurementCursorColor(cursorIndex), highlighted ? 0.95F : 0.35F),
                              2.0F,
                              ImPlotDragToolFlags_NoInputs | ImPlotDragToolFlags_NoFit);
        }
        ImPlot::EndPlot();
    }
    ImPlot::PopStyleVar(2);
}

} // namespace protoscope::ui
