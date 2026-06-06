#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>

namespace protoscope::ui {

namespace {

    plot::WaveDockState::RenderEnvelopeCacheKey makeRenderEnvelopeCacheKey(
        const plot::WaveDockState& wave,
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
            entry.envelope = buildDisplayEnvelope(samples, limits.X.Min, limits.X.Max, pointLimit, &entry.sourceSampleCount);
            entry.key = key;
            entry.valid = true;
        }
        if (sourceSampleCount != nullptr) {
            *sourceSampleCount = entry.sourceSampleCount;
        }
        return entry.envelope;
    }

} // namespace

void renderWaveChannels(plot::WaveDockState& wave,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const RenderBudget& renderBudget,
                        const ImPlotRect& limits,
                        std::vector<std::size_t>& visibleChannelIndices)
{
    auto& view = wave.view;
    visibleChannelIndices.clear();
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto& channel = snapshot.channels[channelIndex];
        const auto& channelSamples = displayData.channels[channelIndex].samples;
        const ImVec4 color = channelColor(channel, channelIndex);
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

        if (sourceSampleCount <= downsampleThreshold) {
            auto begin =
                std::lower_bound(channelSamples.begin(),
                                 channelSamples.end(),
                                 limits.X.Min,
                                 [](const plot::WaveSample& sample, double value) { return sample.time < value; });
            auto end =
                std::upper_bound(channelSamples.begin(),
                                 channelSamples.end(),
                                 limits.X.Max,
                                 [](double value, const plot::WaveSample& sample) { return value < sample.time; });
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
            spec.LineWeight = 1.5F;
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
        legendSpec.LineWeight = 1.5F;
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
            renderPhosphorEnvelope(envelope, color, limits.X.Max, view.persistenceWindow, view.glowIntensity);
        } else {
            renderEnvelopeAsBars(envelope, color);
        }
    }
}

void handleHoverReadout(plot::WaveViewState& view,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const std::vector<std::size_t>& visibleChannelIndices,
                        const ImPlotPoint& mousePos,
                        double timeSnapDistance,
                        double valueSnapDistance)
{
    if (!ImPlot::IsPlotHovered() || !view.showHoverReadout || visibleChannelIndices.empty()) {
        return;
    }
    auto hovered = plot::findNearestDisplayPointInChannels(
        displayData, visibleChannelIndices, mousePos.x, mousePos.y, timeSnapDistance, valueSnapDistance);
    if (!hovered.has_value() || hovered->channelIndex >= snapshot.channels.size()) {
        return;
    }
    const auto& hoveredChannel = snapshot.channels[hovered->channelIndex];
    ImPlot::Annotation(hovered->time,
                       hovered->displayValue,
                       ImVec4(1.0F, 1.0F, 0.2F, 1.0F),
                       ImVec2(12.0F, -12.0F),
                       true,
                       "%s t=%s y=%.6g %s",
                       hoveredChannel.label.c_str(),
                       formatMetricText(hovered->time, displayData.timeUnit.c_str()).c_str(),
                       hovered->value,
                       hoveredChannel.unit.c_str());
    if (view.showCursors && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        view.measurementChannelIndex = hovered->channelIndex;
    }
}

bool handlePlotCursors(plot::WaveViewState& view,
                       const plot::WaveSnapshot& snapshot,
                       const plot::WaveDisplayData& displayData,
                       const ImPlotPoint& mousePos,
                       const ImPlotRect& limits,
                       double timeSnapDistance,
                       double smartSnapDistance,
                       std::array<std::optional<plot::CursorReadout>, 2>& cursorReadouts)
{
    if (!view.showCursors) {
        return false;
    }
    clampActiveChannel(view, snapshot.channels.size());

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
            auto smartSnapTarget =
                findSmartCursorSnapByScope(displayData, view, dragTime, mousePos.y, limits, smartSnapDistance);
            if (smartSnapTarget.has_value()) {
                smartSnap = smartSnapTarget->readout;
                snapLabel = smartSnapTarget->label;
            }
        }
        cursor.time = held ? plot::applyCursorDragSnap(dragTime, smartSnap) : dragTime;
        if (held && !view.cursorIntervalLocked && view.cursors[0].enabled && view.cursors[1].enabled) {
            view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
        }

        auto best = findNearestDisplayByScope(displayData, view, cursor.time, timeSnapDistance);
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
        cursor.value = best->value;
        if (held && view.cursorIntervalLocked && view.lockedCursorInterval > 0.0) {
            auto& pairedCursor = view.cursors[cursorIndex == 0 ? 1 : 0];
            plot::lockCursorInterval(cursor.time, pairedCursor.time, view.lockedCursorInterval, cursorIndex == 0);
        }
        if (held) {
            best->time = cursor.time;
        }
        cursorReadouts[cursorIndex] = best;
        if (held || hovered || cursor.pinned) {
            drawCursorAnnotation(
                cursorIndex, *best, snapshot.channels[best->channelIndex], displayData.timeUnit, snapLabel);
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
    const auto derivedBounds = boundsForDerivedViews(wave, displayData, derivedChannelIndices);
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
    if (!zoomSelectionMode) {
        viewportChangedThisFrame =
            handleMainPlotAxisDoubleClick(view, derivedBounds, fullHistoryBounds) || handleMainPlotZoom(view, mousePos);
    }
    // 悬停读数必须跟随 ImPlot 图例隐藏状态，只对真实可见波形做吸附。
    std::vector<std::size_t> visibleChannelIndices;
    renderWaveChannels(wave, frame.snapshot, renderDisplayData, frame.renderBudget, limits, visibleChannelIndices);
    syncLegendVisibilityState(wave, frame.snapshot);
    const auto fitChannelIndices = excludesLegendHiddenChannels(view)
                                       ? channelIndicesForDerivedViews(wave, frame.snapshot)
                                       : visibleChannelIndicesForFit(frame.snapshot);
    viewportChangedThisFrame =
        applyFitVisibleWaveforms(view, displayData, fitChannelIndices) || viewportChangedThisFrame;
    const auto zoomSelectionResult = handleMainPlotZoomSelection(view, wave.suppressZoomSelectionEscapeThisFrame);
    viewportChangedThisFrame = zoomSelectionResult.viewportChanged || viewportChangedThisFrame;
    viewportChangedThisFrame = applyPendingVerticalAutoFitOverride(view, derivedBounds) || viewportChangedThisFrame;
    const bool offsetReset =
        !zoomSelectionResult.consumed &&
        handleActiveWaveformDoubleClickOffsetReset(wave, displayData, mousePos, timeSnapDistance, valueSnapDistance);
    const bool blockPlotInteractions = zoomSelectionResult.consumed || offsetReset;
    if (!blockPlotInteractions) {
        handleHoverReadout(
            view, frame.snapshot, displayData, visibleChannelIndices, mousePos, timeSnapDistance, valueSnapDistance);
        viewportChangedThisFrame =
            handleOscilloscopeChannelInteractions(wave, displayData, mousePos, timeSnapDistance, valueSnapDistance) ||
            viewportChangedThisFrame;
    }

    const bool anyCursorHeld = blockPlotInteractions ? false
                                                     : handlePlotCursors(view,
                                                                         frame.snapshot,
                                                                         displayData,
                                                                         mousePos,
                                                                         limits,
                                                                         timeSnapDistance,
                                                                         smartSnapDistance,
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
    drawMeasurementOverlay(view, frame.snapshot, displayData, result);

    ImPlot::EndPlot();
    inputMap = savedInputMap;
    if (!view.showAxisLabels) {
        ImPlot::PopStyleVar(2);
    }
    return result;
}

} // namespace protoscope::ui
