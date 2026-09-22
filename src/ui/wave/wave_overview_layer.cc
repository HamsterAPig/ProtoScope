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

void releaseOverviewDrag(plot::WaveDockState& wave)
{
    const auto id = wave.view.overviewDrag.activeId;
    if (id && ImGui::GetCurrentContext() && ImGui::GetActiveID() == id) ImGui::ClearActiveID();
    wave.view.overviewDrag = {};
    wave.view.overviewWindowDragging = false;
    wave.overviewColorCache.dragging = false;
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
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::GetIO().AppFocusLost)
        releaseOverviewDrag(wave);
    if (fullSnapshot.channels.empty()) {
        releaseOverviewDrag(wave);
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
        releaseOverviewDrag(wave);
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
                                  ImPlotFlags_NoMenus | ImPlotFlags_NoFrame | ImPlotFlags_NoBoxSelect;
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    // 概览图需要跟随 splitter 压缩，避免 ImPlot 默认 150px 最小高度撑住内部绘图区。
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotMinSize, ImVec2(64.0F, 24.0F));
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(2.0F, 2.0F));
    ImPlot::PushStyleColor(ImPlotCol_PlotBg, activeWaveStyleTokens().plotBackground);
    if (ImPlot::BeginPlot("##wave_overview", ImVec2(-1.0F, -1.0F), plotFlags)) {
        constexpr ImPlotAxisFlags axisFlags =
            ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoMenus | ImPlotAxisFlags_NoDecorations | ImPlotAxisFlags_Lock;
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
        const auto background = activeWaveStyleTokens().plotBackground;
        const auto window = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
        const auto effectiveBackground = plot::compositeOverviewColor(
            {background.x, background.y, background.z, background.w}, {window.x, window.y, window.z, 1});
        const auto settings = overviewSelectionStyle(view.overviewSelection);
        auto& colorCache = wave.overviewColorCache;
        colorCache.dragging = view.overviewWindowDragging;
        std::uint64_t visibility = view.overviewNormalizeChannels ? 1 : 0;
        std::vector<plot::OverviewColor> drawnColors;
        for (const auto index : overviewChannels) {
            visibility = visibility * 1099511628211ULL + index + 1;
            const auto color = displayColor(channelColor(fullSnapshot.channels[index], index), background, .65F);
            drawnColors.push_back({color.x, color.y, color.z, color.w});
        }
        const bool invalidated = colorCache.revision != activeThemeRevision() || colorCache.visibility != visibility ||
            colorCache.background != effectiveBackground || colorCache.style != settings ||
            colorCache.channels != drawnColors;
        const bool sampleNow = colorCache.needsEvaluation(ImGui::GetTime(), invalidated);
        plot::OverviewColorRaster raster(effectiveBackground);
        const auto normX = [&](double t) { return (t - overviewMinTime) / (overviewMaxTime - overviewMinTime); };
        const auto normY = [&](double v) { return (v - overviewMinValue) / (overviewMaxValue - overviewMinValue); };
        std::size_t drawnIndex = 0;
        for (const std::size_t channelIndex : overviewChannels) {
            const auto& overview = cachedOverviewChannel(wave, fullSnapshot.channels[channelIndex], channelIndex,
                displayData.axisSource, overviewMinTime, overviewMaxTime, pixelWidth, overviewPointLimit);
            const auto sampledColor = drawnColors[drawnIndex++];
            const ImVec4 color(float(sampledColor.r), float(sampledColor.g), float(sampledColor.b), float(sampledColor.a));
            if (sampleNow) {
                for (std::size_t i = 1; i < overview.trace.size(); ++i) {
                    const auto& a = overview.trace[i - 1];
                    const auto& b = overview.trace[i];
                    raster.line(normX(a.time), normY(a.value), normX(b.time), normY(b.value), sampledColor);
                }
                for (const auto& bucket : overview.envelope)
                    raster.rectangle(normX(bucket.beginTime), normY(bucket.minValue),
                                     normX(bucket.endTime), normY(bucket.maxValue), sampledColor);
            }
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

        const auto rectangleColor = colorCache.resolveSamples(drawnColors, effectiveBackground, raster.pixels,
            settings, activeThemeRevision(), visibility, ImGui::GetTime(), ImGui::GetIO().DeltaTime);
        const auto pos = ImPlot::GetPlotPos();
        const auto size = ImPlot::GetPlotSize();
        const float dpi = ImGui::GetWindowViewport()->DpiScale;
        // 命中和绘制只使用这一个最终矩形，最小宽度仅用于视觉而不回写时间。
        const auto visualRect = [&] {
            float left = std::clamp(ImPlot::PlotToPixels(view.viewMinTime, overviewMaxValue).x,
                                    pos.x + 2 * dpi, pos.x + size.x - 2 * dpi);
            float right = std::clamp(ImPlot::PlotToPixels(view.viewMaxTime, overviewMinValue).x,
                                     pos.x + 2 * dpi, pos.x + size.x - 2 * dpi);
            if (right - left < 4 * dpi) {
                const float center = std::clamp((left + right) * .5F, pos.x + 4 * dpi, pos.x + size.x - 4 * dpi);
                left = center - 2 * dpi;
                right = center + 2 * dpi;
            }
            return ImRect(ImVec2(left, pos.y + 2 * dpi), ImVec2(right, pos.y + size.y - 2 * dpi));
        };
        auto visual = visualRect();
        auto& drag = view.overviewDrag;
        const auto mouse = ImGui::GetMousePos();
        const ImRect plotRect(pos, ImVec2(pos.x + size.x, pos.y + size.y));
        const float centerY = pos.y + size.y * .5F;
        const bool narrow = visual.GetWidth() <= 16 * dpi;
        int target = 0;
        if (plotRect.Contains(mouse) && !ImGui::GetIO().AppFocusLost &&
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
            // 极窄框只在中段把手捕获边缘，框体上下仍能平移；重叠容差按最近边互斥分区。
            const bool edgeBand = !narrow || std::abs(mouse.y - centerY) <= 7 * dpi;
            const auto dl = std::abs(mouse.x - visual.Min.x), dr = std::abs(mouse.x - visual.Max.x);
            if (edgeBand && (std::min)(dl, dr) <= 5 * dpi) target = dl <= dr ? 1 : 2;
            else if (visual.Contains(mouse)) target = 3;
        }
        ImGui::PushID("overview_viewport");
        const ImGuiID id = ImGui::GetID("capture");
        // ImPlot 的背景项允许覆盖；这里只接受当帧在目标内的按下，移入不劫持。
        if (!drag.activeId && target && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            (ImGui::GetActiveID() == 0 || ImGui::GetActiveID() == GImPlot->CurrentPlot->ID)) {
            ImGui::SetActiveID(id, ImGui::GetCurrentWindow());
            GImGui->ActiveIdMouseButton = ImGuiMouseButton_Left;
            ImGui::SetKeyOwner(ImGuiKey_MouseLeft, id);
            ImGui::FocusWindow(ImGui::GetCurrentWindow());
            drag = {id, target, view.viewMinTime, view.viewMaxTime, mouse.x,
                    (overviewMaxTime - overviewMinTime) / size.x};
        }
        ImGui::PopID();
        if (drag.activeId && ImGui::GetActiveID() != drag.activeId) releaseOverviewDrag(wave);
        if (drag.activeId) ImGui::KeepAliveID(drag.activeId);
        bool changed = false;
        double rectMinTime = view.viewMinTime, rectMaxTime = view.viewMaxTime;
        if (drag.activeId && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && drag.target != 3) {
            if (drag.target == 1) rectMinTime = overviewMinTime;
            else rectMaxTime = overviewMaxTime;
            changed = true;
            releaseOverviewDrag(wave);
        } else if (drag.activeId && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const double delta = (mouse.x - drag.mouseX) * drag.timePerPixel;
            rectMinTime = drag.minTime + (drag.target != 2 ? delta : 0);
            rectMaxTime = drag.maxTime + (drag.target != 1 ? delta : 0);
            if (drag.target == 1) rectMinTime = (std::max)(overviewMinTime, (std::min)(rectMinTime, rectMaxTime - minVisibleTimeSpan));
            if (drag.target == 2) rectMaxTime = (std::min)(overviewMaxTime, (std::max)(rectMaxTime, rectMinTime + minVisibleTimeSpan));
            changed = true;
        }
        view.overviewWindowDragging = drag.activeId != 0;
        colorCache.dragging = view.overviewWindowDragging;
        if (target || drag.activeId) ImGui::SetMouseCursor(
            (drag.activeId ? drag.target : target) == 3 ? ImGuiMouseCursor_ResizeAll : ImGuiMouseCursor_ResizeEW);
        const plot::WaveDataBounds overviewBounds{
            .minTime = overviewMinTime,
            .maxTime = overviewMaxTime,
            .minValue = overviewMinValue,
            .maxValue = overviewMaxValue,
            .minStep = minVisibleTimeSpan,
            .valid = true,
        };
        if (changed) {
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
        visual = visualRect();
        const auto visualMin = visual.Min, visualMax = visual.Max;
        const auto left = visual.Min.x, right = visual.Max.x;
        const ImVec4 border(float(rectangleColor.r), float(rectangleColor.g), float(rectangleColor.b), 1.F);
        const auto guard = plot::overviewLuminance(rectangleColor) > .35
            ? IM_COL32(0, 0, 0, 255) : IM_COL32(255, 255, 255, 255);
        auto* selectionDraw = ImPlot::GetPlotDrawList();
        ImPlot::PushPlotClipRect();
        selectionDraw->AddRectFilled(visualMin, visualMax, ImGui::ColorConvertFloat4ToU32(
            ImVec4(border.x, border.y, border.z, float(rectangleColor.a))));
        selectionDraw->AddRect(visualMin, visualMax, guard, 0, 0, 4.F);
        selectionDraw->AddRect(visualMin, visualMax, ImGui::ColorConvertFloat4ToU32(border), 0, 0, 2.F);
        for (const auto x : {left, right}) {
            selectionDraw->AddLine(ImVec2(x, centerY - 5 * dpi), ImVec2(x, centerY + 5 * dpi), guard, 6.F * dpi);
            selectionDraw->AddLine(ImVec2(x, centerY - 5 * dpi), ImVec2(x, centerY + 5 * dpi),
                                  ImGui::ColorConvertFloat4ToU32(border), 3.F * dpi);
        }
        ImPlot::PopPlotClipRect();
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
            if (!view.cursorColors.resolve(cursorIndex, cursorIndex).graphicsPass)
                drawCursorGuard(lineTime, measurementCursorColor(view, cursorIndex));
            ImPlot::DragLineX(static_cast<int>(400 + cursorIndex),
                              &lineTime,
                              measurementCursorColor(view, cursorIndex),
                              2.0F,
                              ImPlotDragToolFlags_NoInputs | ImPlotDragToolFlags_NoFit);
        }
        // T 是独立时间标记，概览沿用同一身份色和虚线，不与触发线混用。
        ImPlot::PushPlotClipRect();
        for (const auto& cursor : view.auxiliaryCursors.items) {
            const float x = ImPlot::PlotToPixels(cursor.time, overviewMaxValue).x;
            if (x < pos.x || x > pos.x + size.x) continue;
            const auto color = ImGui::ColorConvertFloat4ToU32(auxiliaryCursorColor(view, cursor));
            for (float y = pos.y; y < pos.y + size.y; y += 10 * dpi) {
                if (!view.cursorColors.resolve(cursor.id + 2, cursor.colorIndex + 2).graphicsPass)
                    selectionDraw->AddLine(ImVec2(x, y), ImVec2(x, (std::min)(y + 6 * dpi, pos.y + size.y)),
                        cursorGuardColor(auxiliaryCursorColor(view, cursor)), 4 * dpi);
                selectionDraw->AddLine(ImVec2(x, y), ImVec2(x, (std::min)(y + 6 * dpi, pos.y + size.y)), color, 2 * dpi);
            }
        }
        ImPlot::PopPlotClipRect();
        ImPlot::EndPlot();
    } else {
        releaseOverviewDrag(wave);
    }
    ImPlot::PopStyleVar(2);
    ImPlot::PopStyleColor();
}

} // namespace protoscope::ui
