#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace protoscope::ui {

double scaleFromInteractionFactor(double scale, double factor) {
    if (!std::isfinite(factor) || factor <= 0.0) {
        return scale;
    }
    const double direction = scale < 0.0 ? -1.0 : 1.0;
    double magnitude = std::abs(scale);
    if (magnitude <= 1e-12) {
        magnitude = 1.0;
    }
    return direction * magnitude * factor;
}

bool updateActiveChannelScale(plot::WaveDockState& wave, double factor) {
    const auto channelIndex = wave.view.measurementChannelIndex;
    const auto spec = wave.buffer.channelSpec(channelIndex);
    if (!spec.has_value()) {
        return false;
    }
    auto updated = *spec;
    updated.scale = scaleFromInteractionFactor(updated.scale, factor);
    applyChannelTransformOverride(wave, channelIndex, updated, channelDefaultSpec(wave, channelIndex, *spec));
    return true;
}

bool updateActiveChannelOffset(plot::WaveDockState& wave, double displayDelta) {
    const auto channelIndex = wave.view.measurementChannelIndex;
    const auto spec = wave.buffer.channelSpec(channelIndex);
    if (!spec.has_value()) {
        return false;
    }
    auto updated = *spec;
    updated.offset += offsetParameterDeltaFromDisplayDelta(updated, wave.view.displayFormula, displayDelta);
    applyChannelTransformOverride(wave, channelIndex, updated, channelDefaultSpec(wave, channelIndex, *spec));
    return true;
}

bool handleOscilloscopeChannelInteractions(plot::WaveDockState& wave,
                                           const plot::WaveDisplayData& displayData,
                                           const ImPlotPoint& mousePos,
                                           double timeSnapDistance,
                                           double valueSnapDistance) {
    auto& view = wave.view;
    if (view.controlMode != plot::WaveControlMode::Oscilloscope) {
        view.activeChannelOffsetDrag = false;
        view.activeChannelScaleDrag = false;
        return false;
    }

    const auto& io = ImGui::GetIO();
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        view.activeChannelOffsetDrag = false;
        view.activeChannelScaleDrag = false;
    }

    bool changed = false;
    if (ImPlot::IsAxisHovered(ImAxis_Y1) && io.MouseWheel != 0.0F) {
        changed = updateActiveChannelScale(wave, std::pow(1.1, io.MouseWheel)) || changed;
    }
    if (ImPlot::IsAxisHovered(ImAxis_Y1) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        view.activeChannelScaleDrag = true;
    }
    if (view.activeChannelScaleDrag && ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImPlot::IsAxisHovered(ImAxis_Y1)) {
        changed = updateActiveChannelScale(wave, std::exp(-static_cast<double>(io.MouseDelta.y) * 0.01)) || changed;
    }

    if (!ImPlot::IsPlotHovered()) {
        return changed;
    }

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const auto activePoint =
            plot::findNearestDisplayByTime(displayData, view.measurementChannelIndex, mousePos.x, timeSnapDistance);
        if (activePoint.has_value() && std::abs(activePoint->displayValue - mousePos.y) <= valueSnapDistance) {
            view.activeChannelOffsetDrag = true;
        }
    }

    if (!ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        return changed;
    }

    const ImVec2 mousePixel = ImGui::GetMousePos();
    const ImVec2 previousPixel{mousePixel.x - io.MouseDelta.x, mousePixel.y - io.MouseDelta.y};
    const ImPlotPoint currentPlot = ImPlot::PixelsToPlot(mousePixel);
    const ImPlotPoint previousPlot = ImPlot::PixelsToPlot(previousPixel);

    // 核心流程：新示波器模式下，左键拖动统一保留横向时间平移；
    // 只有命中当前激活波形时，纵向拖动才写回该通道 offset。
    auto viewport = currentViewport(view);
    viewport.minTime += previousPlot.x - currentPlot.x;
    viewport.maxTime += previousPlot.x - currentPlot.x;
    applyViewport(view, viewport);
    changed = true;

    if (view.activeChannelOffsetDrag) {
        changed = updateActiveChannelOffset(wave, currentPlot.y - previousPlot.y) || changed;
    }
    return changed;
}

bool applyPendingVerticalAutoFitOverride(plot::WaveViewState& view, const plot::WaveDataBounds& bounds) {
    if (GImPlot == nullptr || GImPlot->CurrentPlot == nullptr) {
        return false;
    }

    ImPlotPlot& currentPlot = *GImPlot->CurrentPlot;
    ImPlotAxis& yAxis = currentPlot.YAxis(0);
    if (!yAxis.Enabled || (!yAxis.FitThisFrame && !yAxis.IsAutoFitting())) {
        return false;
    }

    // 核心流程：ImPlot 内置 fit 会在 EndPlot 末尾按原始数据边界套用 1x/默认 padding。
    // 这里在 EndPlot 前接管 Y 轴 fit 请求，统一改成配置文件控制的倍率范围。
    if (view.lockVerticalRange) {
        yAxis.SetRange(view.manualVerticalMin, view.manualVerticalMax);
        yAxis.FitThisFrame = false;
        return true;
    }
    if (!bounds.valid) {
        return false;
    }

    const auto range = plot::makeVerticalAutoFitRange(bounds.minValue, bounds.maxValue, view.verticalAutoFitMultiplier);
    yAxis.SetRange(range.minValue, range.maxValue);
    yAxis.FitThisFrame = false;
    view.viewMinValue = range.minValue;
    view.viewMaxValue = range.maxValue;
    view.forceNextMainPlotLimits = true;
    return true;
}

bool excludesLegendHiddenChannels(const plot::WaveViewState& view) {
    return view.hiddenChannelPolicy == plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews;
}

bool channelHiddenByLegendState(const plot::WaveDockState& wave, const std::string& label) {
    return std::find(wave.hiddenChannelLabels.begin(), wave.hiddenChannelLabels.end(), label) != wave.hiddenChannelLabels.end();
}

std::vector<std::size_t> channelIndicesForDerivedViews(const plot::WaveDockState& wave,
                                                       const plot::WaveSnapshot& snapshot) {
    std::vector<std::size_t> indices;
    indices.reserve(snapshot.channels.size());
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto& label = snapshot.channels[channelIndex].label;
        if (excludesLegendHiddenChannels(wave.view) && channelHiddenByLegendState(wave, label)) {
            continue;
        }
        indices.push_back(channelIndex);
    }
    return indices;
}

plot::WaveDataBounds boundsForDerivedViews(const plot::WaveDockState& wave,
                                           const plot::WaveDisplayData& displayData,
                                           const std::vector<std::size_t>& channelIndices) {
    if (excludesLegendHiddenChannels(wave.view)) {
        return plot::computeDisplayBoundsForChannels(displayData, channelIndices, (std::max)(wave.view.minVisibleTimeSpan, 1e-6));
    }
    return wave.cachedDisplayBounds;
}

void applySavedLegendVisibility(const plot::WaveDockState& wave, const std::string& label) {
    const bool hidden = channelHiddenByLegendState(wave, label);
    if (hidden || wave.legendVisibilityRestorePending) {
        ImPlot::HideNextItem(hidden, wave.legendVisibilityRestorePending ? ImPlotCond_Always : ImPlotCond_Once);
    }
}

void syncLegendVisibilityState(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot) {
    std::vector<std::string> hiddenLabels;
    hiddenLabels.reserve(snapshot.channels.size());
    for (const auto& channel : snapshot.channels) {
        const ImPlotItem* item = ImPlot::GetItem(channel.label.c_str());
        if (item != nullptr && !item->Show) {
            hiddenLabels.push_back(channel.label);
        }
    }
    wave.hiddenChannelLabels = std::move(hiddenLabels);
    wave.legendVisibilityRestorePending = false;
}

std::vector<plot::EnvelopePoint> buildDisplayEnvelope(const std::vector<plot::WaveSample>& samples,
                                                      double visibleMinTime,
                                                      double visibleMaxTime,
                                                      std::size_t pointLimit,
                                                      std::size_t* sourceSampleCount) {
    std::vector<plot::EnvelopePoint> envelope;
    if (sourceSampleCount != nullptr) {
        *sourceSampleCount = 0;
    }
    if (samples.empty() || pointLimit == 0) {
        return envelope;
    }
    if (visibleMaxTime < visibleMinTime) {
        std::swap(visibleMinTime, visibleMaxTime);
    }
    auto begin = std::lower_bound(samples.begin(), samples.end(), visibleMinTime, [](const plot::WaveSample& sample, double value) {
        return sample.time < value;
    });
    auto end = std::upper_bound(samples.begin(), samples.end(), visibleMaxTime, [](double value, const plot::WaveSample& sample) {
        return value < sample.time;
    });
    const std::size_t visibleSampleCount = begin < end ? static_cast<std::size_t>(std::distance(begin, end)) : 0;
    if (sourceSampleCount != nullptr) {
        *sourceSampleCount = visibleSampleCount;
    }

    // 核心流程：高倍缩放可能落在两个采样点之间，此时可视区内没有点。
    // 额外纳入左右相邻保护点，让 ImPlot 仍能裁剪并绘制穿过视窗的线段。
    if (begin != samples.begin()) {
        --begin;
    }
    if (end != samples.end()) {
        ++end;
    }

    if (begin >= end) {
        return envelope;
    }

    const std::size_t visibleCount = static_cast<std::size_t>(std::distance(begin, end));
    const std::size_t bucketCount = (std::min)(pointLimit, visibleCount);
    envelope.reserve(bucketCount);
    for (std::size_t bucket = 0; bucket < bucketCount; ++bucket) {
        const std::size_t bucketBegin = bucket * visibleCount / bucketCount;
        const std::size_t bucketEnd = (bucket + 1) * visibleCount / bucketCount;
        double minValue = std::numeric_limits<double>::infinity();
        double maxValue = -std::numeric_limits<double>::infinity();
        double timeSum = 0.0;
        std::size_t count = 0;
        for (std::size_t offset = bucketBegin; offset < bucketEnd; ++offset) {
            const auto& sample = *(begin + static_cast<std::ptrdiff_t>(offset));
            minValue = (std::min)(minValue, sample.value);
            maxValue = (std::max)(maxValue, sample.value);
            timeSum += sample.time;
            ++count;
        }
        if (count > 0) {
            envelope.push_back({
                .time = timeSum / static_cast<double>(count),
                .minValue = minValue,
                .maxValue = maxValue,
                .sampleCount = count,
            });
        }
    }
    return envelope;
}

void clampActiveChannel(plot::WaveViewState& view, std::size_t channelCount) {
    if (channelCount == 0 || view.measurementChannelIndex >= channelCount) {
        view.measurementChannelIndex = 0;
    }
}

const char* snapScopeName(plot::WaveCursorSnapScope scope) {
    switch (scope) {
    case plot::WaveCursorSnapScope::AllChannels:
        return "全部波形";
    case plot::WaveCursorSnapScope::ActiveChannel:
        return "当前激活波形";
    }
    return "未知";
}

std::optional<plot::CursorReadout> findNearestDisplayByScope(const plot::WaveDisplayData& displayData,
                                                             const plot::WaveViewState& view,
                                                             double time,
                                                             double maxTimeDistance) {
    if (view.cursorSnapScope == plot::WaveCursorSnapScope::ActiveChannel) {
        return plot::findNearestDisplayByTime(displayData, view.measurementChannelIndex, time, maxTimeDistance);
    }
    return plot::findNearestDisplayByTimeAcrossChannels(displayData, time, maxTimeDistance);
}

bool currentPlotItemVisible(const std::string& label) {
    const ImPlotItem* item = ImPlot::GetItem(label.c_str());
    return item != nullptr && item->Show;
}

std::vector<std::size_t> visibleChannelIndicesForFit(const plot::WaveSnapshot& snapshot) {
    std::vector<std::size_t> indices;
    indices.reserve(snapshot.channels.size());
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto& label = snapshot.channels[channelIndex].label;
        const ImPlotItem* item = ImPlot::GetItem(label.c_str());
        if (item == nullptr || item->Show) {
            indices.push_back(channelIndex);
        }
    }
    return indices;
}

} // namespace protoscope::ui
