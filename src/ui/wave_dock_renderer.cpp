#include "protoscope/ui/wave_dock_renderer.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::ui {

namespace {

std::string formatMetricText(double value, const char* baseUnit) {
    const char* unit = baseUnit != nullptr ? baseUnit : "";
    const double absValue = std::abs(value);
    double scaled = value;
    const char* prefix = "";
    if (absValue >= 1e9) {
        scaled = value / 1e9;
        prefix = "G";
    } else if (absValue >= 1e6) {
        scaled = value / 1e6;
        prefix = "M";
    } else if (absValue >= 1e3) {
        scaled = value / 1e3;
        prefix = "k";
    } else if (absValue > 0.0 && absValue < 1e-9) {
        scaled = value * 1e12;
        prefix = "p";
    } else if (absValue > 0.0 && absValue < 1e-6) {
        scaled = value * 1e9;
        prefix = "n";
    } else if (absValue > 0.0 && absValue < 1e-3) {
        scaled = value * 1e6;
        prefix = "u";
    } else if (absValue > 0.0 && absValue < 1.0) {
        scaled = value * 1e3;
        prefix = "m";
    }

    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%.4g %s%s", scaled, prefix, unit);
    return buffer;
}

struct PlotGetterPayload {
    const plot::EnvelopePoint* points{nullptr};
};

struct RenderBudget {
    std::size_t pointsPerChannel{1};
    std::size_t estimatedVerticesPerPoint{4};
};

ImPlotPoint envelopeLineMinGetter(int index, void* data) {
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].minValue};
}

ImPlotPoint envelopeLineMaxGetter(int index, void* data) {
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].maxValue};
}

ImPlotPoint envelopeLineValueGetter(int index, void* data) {
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].minValue};
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

ImVec4 channelColor(std::size_t channelIndex) {
    return ImVec4(0.15F + 0.25F * static_cast<float>(channelIndex % 3),
                  0.75F,
                  0.35F + 0.2F * static_cast<float>((channelIndex + 1) % 3),
                  1.0F);
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

bool plotInteractionActive(bool toolHeld) {
    const auto& io = ImGui::GetIO();
    const bool mouseAction = ImGui::IsMouseDragging(ImGuiMouseButton_Left)
        || ImGui::IsMouseDragging(ImGuiMouseButton_Right)
        || ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        || ImPlot::IsPlotSelected()
        || io.MouseWheel != 0.0F;
    const bool interactionAreaHovered = ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1) || ImPlot::IsAxisHovered(ImAxis_Y1);
    return toolHeld || (mouseAction && interactionAreaHovered);
}

void recordMainPlotLimits(plot::WaveViewState& view, const ImPlotRect& limits, const plot::ViewConfig& config) {
    view.viewMinTime = limits.X.Min;
    view.viewMaxTime = limits.X.Max;
    view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, config.timeScale);
    view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    if (!view.lockVerticalRange) {
        view.viewMinValue = limits.Y.Min;
        view.viewMaxValue = limits.Y.Max;
    }
}

const char* axisSourceName(plot::WaveTimeAxisSource source) {
    switch (source) {
    case plot::WaveTimeAxisSource::SampleFrequency:
        return "控件频率";
    case plot::WaveTimeAxisSource::ScriptTime:
        return "脚本时间";
    case plot::WaveTimeAxisSource::SampleIndex:
        return "点数轴";
    }
    return "未知";
}

void applyFrequencyInput(plot::WaveViewState& view) {
    const auto parsed = plot::parseSampleFrequencyText(view.sampleFrequencyInput);
    if (parsed.accepted) {
        view.sampleFrequencyHz = parsed.valueHz;
        view.sampleFrequencyError.clear();
    } else {
        view.sampleFrequencyError = parsed.error;
    }
}

void applyViewport(plot::WaveViewState& view, const plot::WaveViewport& viewport, const plot::ViewConfig& config) {
    view.viewMinTime = viewport.minTime;
    view.viewMaxTime = viewport.maxTime;
    view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, config.timeScale);
    view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    if (!view.lockVerticalRange) {
        view.viewMinValue = viewport.minValue;
        view.viewMaxValue = viewport.maxValue;
    }
    view.autoFollowLatest = false;
    view.forceNextMainPlotLimits = true;
}

plot::WaveViewport currentViewport(const plot::WaveViewState& view) {
    return {
        .minTime = view.viewMinTime,
        .maxTime = view.viewMaxTime,
        .minValue = view.lockVerticalRange ? view.manualVerticalMin : view.viewMinValue,
        .maxValue = view.lockVerticalRange ? view.manualVerticalMax : view.viewMaxValue,
    };
}

std::vector<plot::EnvelopePoint> buildDisplayEnvelope(const std::vector<plot::WaveSample>& samples,
                                                      double visibleMinTime,
                                                      double visibleMaxTime,
                                                      std::size_t pointLimit,
                                                      std::size_t* sourceSampleCount = nullptr) {
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
    if (begin >= end) {
        return envelope;
    }

    const std::size_t visibleCount = static_cast<std::size_t>(std::distance(begin, end));
    if (sourceSampleCount != nullptr) {
        *sourceSampleCount = visibleCount;
    }
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

bool cursorSmartSnapActive(const plot::WaveViewState& view, const ImGuiIO& io) {
    return view.cursorSnapMode == plot::WaveCursorSnapMode::SmartSnap
        || (view.cursorSnapMode == plot::WaveCursorSnapMode::ModifierSnap && (io.KeyShift || io.KeyCtrl));
}

struct SmartCursorSnap {
    plot::CursorReadout readout;
    std::string_view label;
};

std::optional<SmartCursorSnap> findSmartCursorSnapForChannel(const plot::WaveDisplayData& displayData,
                                                             std::size_t channelIndex,
                                                             double time,
                                                             double mouseValue,
                                                             const ImPlotRect& limits,
                                                             double maxTimeDistance) {
    if (channelIndex >= displayData.channels.size()) {
        return std::nullopt;
    }
    const auto& samples = displayData.channels[channelIndex].samples;
    const double minValue = (std::min)(limits.Y.Min, limits.Y.Max);
    const double maxValue = (std::max)(limits.Y.Min, limits.Y.Max);
    const double valueHeight = maxValue - minValue;
    constexpr double kExtremeSnapZoneRatio = 0.15;
    if (std::isfinite(mouseValue) && valueHeight > 0.0) {
        // 鼠标靠近绘图区顶部/底部时，极值优先于边沿，方便直接锁峰值或谷值。
        if (mouseValue >= maxValue - valueHeight * kExtremeSnapZoneRatio) {
            auto peak = plot::findLocalExtremeNearTime(
                samples, channelIndex, time, maxTimeDistance, plot::WaveExtremeKind::Maximum);
            if (peak.has_value()) {
                return SmartCursorSnap{.readout = *peak, .label = "Peak"};
            }
        }
        if (mouseValue <= minValue + valueHeight * kExtremeSnapZoneRatio) {
            auto trough = plot::findLocalExtremeNearTime(
                samples, channelIndex, time, maxTimeDistance, plot::WaveExtremeKind::Minimum);
            if (trough.has_value()) {
                return SmartCursorSnap{.readout = *trough, .label = "Trough"};
            }
        }
    }
    // 常规智能吸附优先找最大跳变；找不到再交给调用方使用按时间最近点兜底。
    auto edge = plot::findStrongestEdgeNearTime(samples, channelIndex, time, maxTimeDistance);
    if (edge.has_value()) {
        return SmartCursorSnap{.readout = *edge, .label = "Edge"};
    }
    return std::nullopt;
}

std::optional<SmartCursorSnap> findSmartCursorSnapByScope(const plot::WaveDisplayData& displayData,
                                                          const plot::WaveViewState& view,
                                                          double time,
                                                          double mouseValue,
                                                          const ImPlotRect& limits,
                                                          double maxTimeDistance) {
    if (view.cursorSnapScope == plot::WaveCursorSnapScope::ActiveChannel) {
        return findSmartCursorSnapForChannel(displayData, view.measurementChannelIndex, time, mouseValue, limits, maxTimeDistance);
    }

    std::optional<SmartCursorSnap> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (std::size_t channelIndex = 0; channelIndex < displayData.channels.size(); ++channelIndex) {
        const auto candidate = findSmartCursorSnapForChannel(displayData, channelIndex, time, mouseValue, limits, maxTimeDistance);
        if (!candidate.has_value()) {
            continue;
        }
        const double timeDistance = std::abs(candidate->readout.time - time);
        const double valueDistance = std::isfinite(mouseValue) ? std::abs(candidate->readout.value - mouseValue) : 0.0;
        const double score = timeDistance * timeDistance + valueDistance * valueDistance;
        if (!best.has_value() || score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

plot::MeasurementReadout measureDisplayWindow(const plot::WaveDisplayData& displayData,
                                              std::size_t channelIndex,
                                              double beginTime,
                                              double endTime) {
    plot::MeasurementReadout result{};
    if (channelIndex >= displayData.channels.size()) {
        return result;
    }
    if (endTime < beginTime) {
        std::swap(beginTime, endTime);
    }
    const auto& samples = displayData.channels[channelIndex].samples;
    const auto begin = std::lower_bound(samples.begin(), samples.end(), beginTime, [](const plot::WaveSample& sample, double value) {
        return sample.time < value;
    });
    const auto end = std::upper_bound(samples.begin(), samples.end(), endTime, [](double value, const plot::WaveSample& sample) {
        return value < sample.time;
    });
    if (begin >= end) {
        return result;
    }

    result.valid = true;
    result.channelIndex = channelIndex;
    result.sampleCount = static_cast<std::size_t>(std::distance(begin, end));
    result.minValue = std::numeric_limits<double>::infinity();
    result.maxValue = -std::numeric_limits<double>::infinity();
    double sum = 0.0;
    double squareSum = 0.0;
    for (auto iterator = begin; iterator != end; ++iterator) {
        result.minValue = (std::min)(result.minValue, iterator->value);
        result.maxValue = (std::max)(result.maxValue, iterator->value);
        sum += iterator->value;
        squareSum += iterator->value * iterator->value;
    }
    result.duration = std::prev(end)->time - begin->time;
    result.peakToPeak = result.maxValue - result.minValue;
    result.meanValue = sum / static_cast<double>(result.sampleCount);
    result.rmsValue = std::sqrt(squareSum / static_cast<double>(result.sampleCount));
    return result;
}

void drawCursorAnnotation(std::size_t cursorIndex,
                          const plot::CursorReadout& readout,
                          const plot::ChannelView& channel,
                          std::string_view timeUnit,
                          std::string_view snapLabel) {
    const std::string labelPrefix = snapLabel.empty() ? "" : std::string(snapLabel) + " ";
    ImPlot::Annotation(readout.time,
                       readout.value,
                       ImVec4(1.0F, 1.0F, 1.0F, 0.92F),
                       ImVec2(10.0F, cursorIndex == 0 ? -18.0F : 18.0F),
                       true,
                       "C%zu %s%s\n%s %.6g",
                       cursorIndex + 1,
                       labelPrefix.c_str(),
                       formatMetricText(readout.time, std::string(timeUnit).c_str()).c_str(),
                       channel.label.c_str(),
                        readout.value);
}

void drawCursorIntervalHint(const plot::CursorReadout& left,
                            const plot::CursorReadout& right,
                            const plot::CursorIntervalText& intervalText,
                            const ImPlotRect& limits) {
    if (!intervalText.valid) {
        return;
    }
    const double beginTime = (std::max)((std::min)(left.time, right.time), limits.X.Min);
    const double endTime = (std::min)((std::max)(left.time, right.time), limits.X.Max);
    if (!std::isfinite(beginTime) || !std::isfinite(endTime) || endTime <= beginTime) {
        return;
    }

    const double centerValue = 0.5 * (limits.Y.Min + limits.Y.Max);
    const ImVec2 start = ImPlot::PlotToPixels(beginTime, centerValue);
    const ImVec2 end = ImPlot::PlotToPixels(endTime, centerValue);
    auto* drawList = ImPlot::GetPlotDrawList();
    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.0F, 0.92F, 0.25F, 0.9F));
    ImPlot::PushPlotClipRect();
    constexpr float kDashLength = 8.0F;
    constexpr float kGapLength = 5.0F;
    for (float x = start.x; x < end.x; x += kDashLength + kGapLength) {
        drawList->AddLine(ImVec2(x, start.y), ImVec2((std::min)(x + kDashLength, end.x), end.y), lineColor, 1.4F);
    }

    const std::string label = intervalText.showFrequency
        ? ("Δt " + formatMetricText(intervalText.delta, intervalText.deltaUnit.c_str()) + " / f "
            + formatMetricText(intervalText.frequencyHz, "Hz"))
        : ("Δsample " + formatMetricText(intervalText.delta, intervalText.deltaUnit.c_str()));
    const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    const ImVec2 center = ImVec2(0.5F * (start.x + end.x), start.y);
    const ImVec2 textMin = ImVec2(center.x - 0.5F * textSize.x - 5.0F, center.y - textSize.y - 7.0F);
    const ImVec2 textMax = ImVec2(center.x + 0.5F * textSize.x + 5.0F, center.y - 3.0F);
    drawList->AddRectFilled(textMin, textMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.06F, 0.06F, 0.04F, 0.72F)), 3.0F);
    drawList->AddText(ImVec2(textMin.x + 5.0F, textMin.y + 2.0F), lineColor, label.c_str());
    ImPlot::PopPlotClipRect();
}

void drawOverviewWindow(plot::WaveViewState& view,
                        const plot::ViewConfig& config,
                        const plot::WaveSnapshot& fullSnapshot,
                        const plot::WaveDisplayData& displayData,
                        const plot::WaveDataBounds& displayBounds,
                        const RenderBudget& renderBudget) {
    if (fullSnapshot.channels.empty()) {
        return;
    }

    double overviewMinTime = displayBounds.valid ? displayBounds.minTime : std::numeric_limits<double>::infinity();
    double overviewMaxTime = displayBounds.valid ? displayBounds.maxTime : -std::numeric_limits<double>::infinity();
    double overviewMinValue = displayBounds.valid ? displayBounds.minValue : std::numeric_limits<double>::infinity();
    double overviewMaxValue = displayBounds.valid ? displayBounds.maxValue : -std::numeric_limits<double>::infinity();
    if (!displayBounds.valid) {
        for (const auto& channel : fullSnapshot.channels) {
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

    ImGui::Separator();
    ImGui::TextUnformatted("概览");
    ImGui::SetNextItemWidth(180.0F);
    int maxSamplesInput = static_cast<int>((std::min)(view.overviewMaxSamples, static_cast<std::size_t>(1000000)));
    if (ImGui::InputInt("概览最大样本/通道", &maxSamplesInput, 1000, 10000)) {
        view.overviewMaxSamples = static_cast<std::size_t>((std::max)(0, maxSamplesInput));
    }

    const ImPlotFlags plotFlags = ImPlotFlags_NoTitle | ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText | ImPlotFlags_NoMenus;
    constexpr float kOverviewHeight = 110.0F;
    if (ImPlot::BeginPlot("##wave_overview", ImVec2(-1.0F, kOverviewHeight), plotFlags)) {
        constexpr ImPlotAxisFlags axisFlags = ImPlotAxisFlags_NoHighlight | ImPlotAxisFlags_NoMenus;
        ImPlot::SetupAxis(ImAxis_X1, nullptr, axisFlags);
        ImPlot::SetupAxis(ImAxis_Y1, nullptr, axisFlags | ImPlotAxisFlags_NoTickLabels);
        ImPlot::SetupAxisLimits(ImAxis_X1, overviewMinTime, overviewMaxTime, ImPlotCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, overviewMinValue, overviewMaxValue, ImPlotCond_Always);

        const float contentWidth = ImGui::GetContentRegionAvail().x;
        const std::size_t pixelWidth = static_cast<std::size_t>((std::max)(contentWidth, 64.0F));
        const std::size_t overviewPointLimit = view.overviewMaxSamples > 0
            ? (std::min)({pixelWidth, renderBudget.pointsPerChannel, view.overviewMaxSamples})
            : (std::min)(pixelWidth, renderBudget.pointsPerChannel);
        for (std::size_t channelIndex = 0; channelIndex < fullSnapshot.channels.size(); ++channelIndex) {
            auto overview =
                buildDisplayEnvelope(displayData.channels[channelIndex].samples, overviewMinTime, overviewMaxTime, overviewPointLimit);
            if (overview.empty()) {
                continue;
            }
            PlotGetterPayload payload{.points = overview.data()};
            const auto color = withAlpha(channelColor(channelIndex), 0.65F);
            ImPlotSpec spec{};
            spec.LineColor = color;
            spec.LineWeight = 1.0F;
            ImPlot::PlotLineG((fullSnapshot.channels[channelIndex].label + " overview min").c_str(), &envelopeLineMinGetter, &payload, static_cast<int>(overview.size()), spec);
            ImPlot::PlotLineG((fullSnapshot.channels[channelIndex].label + " overview max").c_str(), &envelopeLineMaxGetter, &payload, static_cast<int>(overview.size()), spec);
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
            .minStep = (std::max)(config.timeScale, 1e-6),
            .valid = true,
        };
        if (ImPlot::DragRect(300, &rectMinTime, &rectMinValue, &rectMaxTime, &rectMaxValue,
                ImVec4(1.0F, 0.85F, 0.2F, 0.35F),
                ImPlotDragToolFlags_NoFit,
                nullptr,
                 &rectHovered,
                 &rectHeld)) {
            const auto normalized = plot::normalizeOverviewViewport(
                {.minTime = rectMinTime, .maxTime = rectMaxTime, .minValue = view.viewMinValue, .maxValue = view.viewMaxValue},
                overviewBounds,
                (std::max)(config.timeScale, 1e-6));
            view.viewMinTime = normalized.minTime;
            view.viewMaxTime = normalized.maxTime;
            view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, config.timeScale);
            view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
            view.autoFollowLatest = false;
            view.forceNextMainPlotLimits = true;
        }
        const auto mousePlotPos = ImPlot::GetPlotMousePos();
        const ImVec2 rectMinPixel = ImPlot::PlotToPixels((std::min)(rectMinTime, rectMaxTime), overviewMaxValue);
        const ImVec2 rectMaxPixel = ImPlot::PlotToPixels((std::max)(rectMinTime, rectMaxTime), overviewMinValue);
        const ImVec2 mousePixel = ImGui::GetMousePos();
        constexpr float kDragEdgePadding = 8.0F;
        const bool mouseInsideWindowBody = ImPlot::IsPlotHovered()
            && mousePixel.x > rectMinPixel.x + kDragEdgePadding
            && mousePixel.x < rectMaxPixel.x - kDragEdgePadding
            && mousePixel.y > rectMinPixel.y + kDragEdgePadding
            && mousePixel.y < rectMaxPixel.y - kDragEdgePadding;
        if (mouseInsideWindowBody && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.overviewWindowDragging = true;
            view.overviewDragLastTime = mousePlotPos.x;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            view.overviewWindowDragging = false;
        }
        if (view.overviewWindowDragging && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const double deltaTime = mousePlotPos.x - view.overviewDragLastTime;
            const auto moved = plot::moveViewportByDelta(
                currentViewport(view), deltaTime, overviewBounds, (std::max)(config.timeScale, 1e-6));
            applyViewport(view, moved, config);
            view.overviewDragLastTime = mousePlotPos.x;
        }
        if ((rectHovered || rectHeld) && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            view.autoFollowLatest = false;
        }
        const auto& io = ImGui::GetIO();
        if (ImPlot::IsPlotHovered() && io.MouseWheel != 0.0F) {
            const auto mousePos = ImPlot::GetPlotMousePos();
            const double centerTime = std::isfinite(mousePos.x) ? mousePos.x : 0.5 * (view.viewMinTime + view.viewMaxTime);
            const auto zoomed = plot::zoomViewport(currentViewport(view),
                                                   plot::WaveZoomMode::XOnly,
                                                   io.MouseWheel,
                                                   centerTime,
                                                   0.0,
                                                   overviewBounds,
                                                   (std::max)(config.timeScale, 1e-6),
                                                   true);
            applyViewport(view, zoomed, config);
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
}

struct WaveFrameData {
    plot::WaveSnapshot snapshot;
    plot::WaveSnapshot fullSnapshot;
    plot::WaveDisplayData displayData;
    plot::WaveDataBounds displayBounds;
    RenderBudget renderBudget;
};

struct PlotRenderResult {
    bool plotRendered{false};
    std::array<std::optional<plot::CursorReadout>, 2> cursorReadouts{};
    std::optional<plot::MeasurementReadout> measurement;
};

void drawWaveToolbar(app::Application& application,
                     plot::WaveViewState& view,
                     const plot::ViewConfig& config) {
    ImGui::Checkbox("自动跟随最新数据", &view.autoFollowLatest);
    ImGui::SameLine();
    ImGui::Checkbox("交互后暂停跟随", &view.pauseAutoFollowOnInteraction);
    ImGui::SameLine();
    ImGui::Checkbox("锁定纵轴", &view.lockVerticalRange);
    ImGui::SameLine();
    if (ImGui::Button("清空历史")) {
        application.resetWaveHistory();
    }

    ImGui::SetNextItemWidth(180.0F);
    ImGui::InputDouble("可视时长", &view.visibleDuration, config.timeScale, config.timeScale * 10.0, "%.6f");
    if (view.visibleDuration <= 0.0) {
        view.visibleDuration = (std::max)(config.timeScale, 1e-6);
    }
    ImGui::SameLine();
    ImGui::Checkbox("显示游标", &view.showCursors);
    ImGui::SameLine();
    const bool smartSnapMode = view.cursorSnapMode == plot::WaveCursorSnapMode::SmartSnap;
    if (ImGui::Button(smartSnapMode ? "智能吸附" : "按键吸附")) {
        view.cursorSnapMode = smartSnapMode ? plot::WaveCursorSnapMode::ModifierSnap : plot::WaveCursorSnapMode::SmartSnap;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        ImGui::TextUnformatted(smartSnapMode ? "拖动游标时自动吸附边沿/极值" : "按住 Shift 或 Ctrl 拖动时启用智能吸附");
        ImGui::EndTooltip();
    }
    ImGui::SameLine();
    ImGui::Checkbox("磷光辉光", &view.phosphorGlowEnabled);
    ImGui::SameLine();
    ImGui::Text("渲染点: %zu / 源样本: %zu", view.lastRenderPointCount, view.lastRenderSourceSampleCount);

    char frequencyBuffer[64]{};
    std::strncpy(frequencyBuffer, view.sampleFrequencyInput.c_str(), sizeof(frequencyBuffer) - 1);
    ImGui::SetNextItemWidth(160.0F);
    if (ImGui::InputText("发送频率 Hz", frequencyBuffer, sizeof(frequencyBuffer))) {
        view.sampleFrequencyInput = frequencyBuffer;
        applyFrequencyInput(view);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox(view.cursorIntervalLocked ? "锁定游标间隔" : "解锁游标间隔", &view.cursorIntervalLocked)) {
        view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
    }
    if (!view.sampleFrequencyError.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.25F, 1.0F), "%s", view.sampleFrequencyError.c_str());
    }

    ImGui::SetNextItemWidth(180.0F);
    ImGui::InputDouble("余辉时间窗", &view.persistenceWindow, config.timeScale, config.timeScale * 10.0, "%.6f");
    if (view.persistenceWindow <= 0.0) {
        view.persistenceWindow = (std::max)(config.timeScale, 1e-6);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0F);
    const double glowMin = 0.2;
    const double glowMax = 2.5;
    ImGui::SliderScalar("辉光强度", ImGuiDataType_Double, &view.glowIntensity, &glowMin, &glowMax, "%.2f");

    if (view.lockVerticalRange) {
        ImGui::SetNextItemWidth(140.0F);
        ImGui::InputDouble("纵轴最小", &view.manualVerticalMin, 0.1, 1.0, "%.6f");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0F);
        ImGui::InputDouble("纵轴最大", &view.manualVerticalMax, 0.1, 1.0, "%.6f");
    }
}

void initializeWaveViewIfNeeded(plot::WaveViewState& view) {
    if (view.initialized) {
        return;
    }
    const double halfDuration = view.visibleDuration * 0.5;
    view.viewMinTime = view.centerTime - halfDuration;
    view.viewMaxTime = view.centerTime + halfDuration;
    view.viewMinValue = view.manualVerticalMin;
    view.viewMaxValue = view.manualVerticalMax;
    view.initialized = true;
}

WaveFrameData prepareWaveFrame(plot::WaveDockState& wave, float availableWidth) {
    auto& view = wave.view;
    const auto& config = wave.buffer.viewConfig();

    WaveFrameData frame;
    frame.snapshot = wave.buffer.snapshot(view.viewMinTime, view.viewMaxTime);
    frame.fullSnapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    frame.displayData = plot::buildDisplayData(frame.fullSnapshot, view.sampleFrequencyHz);

    view.timeAxisSource = frame.displayData.axisSource;
    frame.displayBounds = plot::computeDisplayBounds(frame.displayData, (std::max)(config.timeScale, 1e-6));
    frame.renderBudget = makeRenderBudget(view,
                                          frame.displayData.channels.size(),
                                          static_cast<std::size_t>((std::max)(availableWidth, 64.0F)),
                                          view.phosphorGlowEnabled);
    view.lastRenderPointCount = 0;
    view.lastRenderSourceSampleCount = 0;
    if (frame.displayBounds.valid && view.autoFollowLatest) {
        view.viewMaxTime = frame.displayBounds.maxTime;
        view.viewMinTime = view.viewMaxTime - view.visibleDuration;
        if (view.viewMinTime < frame.displayBounds.minTime) {
            view.viewMinTime = frame.displayBounds.minTime;
            view.viewMaxTime = (std::min)(frame.displayBounds.maxTime, view.viewMinTime + view.visibleDuration);
        }
        view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    }

    frame.snapshot = wave.buffer.snapshot(view.viewMinTime, view.viewMaxTime);
    return frame;
}

void drawCursorToolbar(plot::WaveViewState& view,
                       const plot::ViewConfig& config,
                       const plot::WaveDisplayData& displayData) {
    ImGui::Text("时间轴: %s (%s)", axisSourceName(view.timeAxisSource), displayData.timeUnit.c_str());
    if (!view.showCursors || displayData.channels.empty()) {
        return;
    }
    clampActiveChannel(view, displayData.channels.size());
    const double cursorSnapDistance = (std::max)(view.viewMaxTime - view.viewMinTime, config.timeScale) / 80.0;
    auto placeCursorInViewport = [&](std::size_t cursorIndex, double ratio) {
        auto& cursor = view.cursors[cursorIndex];
        cursor.enabled = true;
        cursor.time = plot::cursorTimeInViewport(currentViewport(view), ratio);
        cursor.channelIndex = view.measurementChannelIndex;
        const auto best = findNearestDisplayByScope(displayData, view, cursor.time, cursorSnapDistance);
        if (best.has_value()) {
            cursor.time = best->time;
            cursor.value = best->value;
            cursor.channelIndex = best->channelIndex;
        }
    };
    if (ImGui::Button("C1 到视窗")) {
        // 快捷定位只移动游标本身，不改变当前视窗，便于快速重取测量区间。
        placeCursorInViewport(0, 0.5);
    }
    ImGui::SameLine();
    if (ImGui::Button("C2 到视窗")) {
        placeCursorInViewport(1, 0.5);
    }
    ImGui::SameLine();
    if (ImGui::Button("C1+C2 到视窗")) {
        placeCursorInViewport(0, 0.4);
        placeCursorInViewport(1, 0.6);
        view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
    }
}

void applyMainPlotAxesAndLimits(plot::WaveViewState& view,
                                const plot::WaveSnapshot& snapshot,
                                const plot::WaveDisplayData& displayData) {
    constexpr ImPlotAxisFlags axisFlags = ImPlotAxisFlags_NoHighlight;
    ImPlot::SetupAxis(ImAxis_X1, displayData.timeUnit == "sample" ? "Sample" : "Time", axisFlags);
    ImPlot::SetupAxis(ImAxis_Y1, snapshot.config.verticalUnit.c_str(), axisFlags);
    const bool forceMainPlotLimits = view.forceNextMainPlotLimits;
    ImPlot::SetupAxisLimits(ImAxis_X1,
                            view.viewMinTime,
                            view.viewMaxTime,
                            (view.autoFollowLatest || forceMainPlotLimits) ? ImPlotCond_Always : ImPlotCond_Once);
    view.forceNextMainPlotLimits = false;
    if (view.lockVerticalRange) {
        ImPlot::SetupAxisLimits(ImAxis_Y1, view.manualVerticalMin, view.manualVerticalMax, ImPlotCond_Always);
    } else {
        ImPlot::SetupAxisLimits(ImAxis_Y1, view.viewMinValue, view.viewMaxValue, ImPlotCond_Once);
    }
}

bool handleMainPlotZoom(plot::WaveViewState& view,
                        const plot::ViewConfig& config,
                        const ImPlotPoint& mousePos) {
    const auto& io = ImGui::GetIO();
    if (io.MouseWheel == 0.0F
        || (!ImPlot::IsPlotHovered() && !ImPlot::IsAxisHovered(ImAxis_X1) && !ImPlot::IsAxisHovered(ImAxis_Y1))) {
        return false;
    }

    plot::WaveZoomMode zoomMode = plot::WaveZoomMode::XY;
    if (ImPlot::IsAxisHovered(ImAxis_X1)) {
        zoomMode = plot::WaveZoomMode::XOnly;
    } else if (ImPlot::IsAxisHovered(ImAxis_Y1)) {
        zoomMode = plot::WaveZoomMode::YOnly;
    }
    const plot::WaveDataBounds bounds{
        .minTime = -std::numeric_limits<double>::infinity(),
        .maxTime = std::numeric_limits<double>::infinity(),
        .minValue = -std::numeric_limits<double>::infinity(),
        .maxValue = std::numeric_limits<double>::infinity(),
        .minStep = (std::max)(config.timeScale, 1e-6),
        .valid = false,
    };
    const auto zoomed = plot::zoomViewport(currentViewport(view),
                                           zoomMode,
                                           io.MouseWheel,
                                           mousePos.x,
                                           mousePos.y,
                                           bounds,
                                           (std::max)(config.timeScale, 1e-6),
                                           false);
    applyViewport(view, zoomed, config);
    return true;
}

void renderWaveChannels(plot::WaveViewState& view,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const RenderBudget& renderBudget,
                        const ImPlotRect& limits) {
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto& channel = snapshot.channels[channelIndex];
        std::size_t sourceSampleCount = 0;
        const auto envelope = buildDisplayEnvelope(displayData.channels[channelIndex].samples,
                                                   limits.X.Min,
                                                   limits.X.Max,
                                                   renderBudget.pointsPerChannel,
                                                   &sourceSampleCount);
        view.lastRenderSourceSampleCount += sourceSampleCount;
        if (envelope.empty()) {
            continue;
        }
        view.lastRenderPointCount += envelope.size();

        PlotGetterPayload payload{.points = envelope.data()};
        const ImVec4 color = channelColor(channelIndex);
        if (sourceSampleCount <= renderBudget.pointsPerChannel * 2) {
            // 低密度数据保持单条普通波形，避免 min/max 辅助边界被误认为额外通道。
            ImPlotSpec spec{};
            spec.LineColor = color;
            spec.LineWeight = 1.5F;
            ImPlot::PlotLineG(channel.label.c_str(), &envelopeLineValueGetter, &payload, static_cast<int>(envelope.size()), spec);
        } else if (view.phosphorGlowEnabled) {
            renderPhosphorEnvelope(envelope, color, limits.X.Max, view.persistenceWindow, view.glowIntensity);
        } else {
            renderEnvelopeAsBars(envelope, color);
        }
    }
}

void handleHoverReadout(plot::WaveViewState& view,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const ImPlotPoint& mousePos,
                        double timeSnapDistance,
                        double valueSnapDistance) {
    if (!view.showCursors || !ImPlot::IsPlotHovered() || !view.showHoverReadout) {
        return;
    }
    auto hovered = plot::findNearestDisplayPoint(displayData, mousePos.x, mousePos.y, timeSnapDistance, valueSnapDistance);
    if (!hovered.has_value() || hovered->channelIndex >= snapshot.channels.size()) {
        return;
    }
    const auto& hoveredChannel = snapshot.channels[hovered->channelIndex];
    ImPlot::Annotation(hovered->time, hovered->value, ImVec4(1.0F, 1.0F, 0.2F, 1.0F), ImVec2(12.0F, -12.0F), true,
        "%s t=%s y=%.6g %s",
        hoveredChannel.label.c_str(),
        formatMetricText(hovered->time, displayData.timeUnit.c_str()).c_str(),
        hovered->value,
        hoveredChannel.unit.c_str());
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
                       std::array<std::optional<plot::CursorReadout>, 2>& cursorReadouts) {
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
        ImPlot::DragLineX(static_cast<int>(100 + cursorIndex), &dragTime,
            ImVec4(cursorIndex == 0 ? 0.2F : 1.0F, 0.9F, 0.3F, 1.0F),
            1.5F,
            dragFlags,
            &clicked,
            &hovered,
            &held);
        anyCursorHeld = anyCursorHeld || held;
        if (held && smartSnapActive) {
            // 核心流程：先用 DragLineX 写入的鼠标时间查吸附，再回写游标时间，配合 Delayed 让绘制使用受约束位置。
            auto smartSnapTarget = findSmartCursorSnapByScope(displayData, view, dragTime, mousePos.y, limits, smartSnapDistance);
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
            drawCursorAnnotation(cursorIndex, *best, snapshot.channels[best->channelIndex], displayData.timeUnit, snapLabel);
        }
    }
    return anyCursorHeld;
}

PlotRenderResult drawOscilloscopePlot(plot::WaveViewState& view,
                                      const plot::ViewConfig& config,
                                      const WaveFrameData& frame) {
    PlotRenderResult result;
    if (frame.fullSnapshot.channels.empty()) {
        ImGui::TextUnformatted("Lua 尚未通过 proto.plot.setup / proto.plot.push 提供波形数据。");
        return result;
    }

    if (!ImPlot::BeginPlot("##oscilloscope",
            ImVec2(-1.0F, (std::max)(220.0F, ImGui::GetContentRegionAvail().y - 150.0F)),
            ImPlotFlags_NoLegend)) {
        return result;
    }

    result.plotRendered = true;
    applyMainPlotAxesAndLimits(view, frame.snapshot, frame.displayData);

    const ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
    const ImPlotRect limits = ImPlot::GetPlotLimits();
    const double visibleTimeWidth = std::abs(limits.X.Max - limits.X.Min);
    const double timeSnapDistance = visibleTimeWidth / 80.0;
    double smartSnapDistance = (std::max)(timeSnapDistance, visibleTimeWidth * 0.02);
    if (frame.displayBounds.valid) {
        smartSnapDistance = (std::max)(smartSnapDistance, frame.displayBounds.minStep * 2.0);
    }
    const double valueSnapDistance = (limits.Y.Max - limits.Y.Min) / 30.0;

    const bool viewportChangedThisFrame = handleMainPlotZoom(view, config, mousePos);
    renderWaveChannels(view, frame.snapshot, frame.displayData, frame.renderBudget, limits);
    handleHoverReadout(view, frame.snapshot, frame.displayData, mousePos, timeSnapDistance, valueSnapDistance);

    const bool anyCursorHeld = handlePlotCursors(view,
                                                 frame.snapshot,
                                                 frame.displayData,
                                                 mousePos,
                                                 limits,
                                                 timeSnapDistance,
                                                 smartSnapDistance,
                                                 result.cursorReadouts);
    const bool userInteracting = plotInteractionActive(anyCursorHeld);
    if (userInteracting && !viewportChangedThisFrame) {
        recordMainPlotLimits(view, limits, config);
    }
    if (userInteracting && view.pauseAutoFollowOnInteraction) {
        view.autoFollowLatest = false;
    }

    if (view.showCursors && result.cursorReadouts[0].has_value() && result.cursorReadouts[1].has_value()) {
        const auto intervalText = plot::makeCursorIntervalText(
            *result.cursorReadouts[0], *result.cursorReadouts[1], frame.displayData.axisSource, frame.displayData.timeUnit);
        drawCursorIntervalHint(*result.cursorReadouts[0], *result.cursorReadouts[1], intervalText, limits);
    }

    ImPlot::EndPlot();

    if (view.showCursors && result.cursorReadouts[0].has_value() && result.cursorReadouts[1].has_value()) {
        result.measurement = measureDisplayWindow(
            frame.displayData, view.measurementChannelIndex, result.cursorReadouts[0]->time, result.cursorReadouts[1]->time);
    }
    return result;
}

void drawWaveReadouts(const plot::WaveViewState& view,
                      const plot::WaveSnapshot& snapshot,
                      const plot::WaveDisplayData& displayData,
                      const PlotRenderResult& result) {
    if (view.showCursors && result.cursorReadouts[0].has_value()) {
        const auto& c0 = *result.cursorReadouts[0];
        ImGui::Text("Cursor A: %s  t=%s  y=%.6g",
            snapshot.channels[c0.channelIndex].label.c_str(),
            formatMetricText(c0.time, displayData.timeUnit.c_str()).c_str(),
            c0.value);
    }
    if (view.showCursors && result.cursorReadouts[1].has_value()) {
        const auto& c1 = *result.cursorReadouts[1];
        ImGui::Text("Cursor B: %s  t=%s  y=%.6g",
            snapshot.channels[c1.channelIndex].label.c_str(),
            formatMetricText(c1.time, displayData.timeUnit.c_str()).c_str(),
            c1.value);
    }
    if (view.showCursors && result.cursorReadouts[0].has_value() && result.cursorReadouts[1].has_value()) {
        const auto delta = plot::OscilloscopeBuffer::makeDelta(*result.cursorReadouts[0], *result.cursorReadouts[1]);
        const auto intervalText = plot::makeCursorIntervalText(
            *result.cursorReadouts[0], *result.cursorReadouts[1], displayData.axisSource, displayData.timeUnit);
        if (intervalText.valid && intervalText.showFrequency) {
            ImGui::Text("Δt=%s  Δy=%.6g  f=%s",
                formatMetricText(delta.deltaTime, displayData.timeUnit.c_str()).c_str(),
                delta.deltaValue,
                formatMetricText(intervalText.frequencyHz, "Hz").c_str());
        } else if (intervalText.valid) {
            ImGui::Text("Δsample=%s  Δy=%.6g",
                formatMetricText(intervalText.delta, intervalText.deltaUnit.c_str()).c_str(),
                delta.deltaValue);
        }
    }
    if (view.showCursors && result.measurement.has_value() && result.measurement->valid) {
        const auto& m = *result.measurement;
        const auto& channel = snapshot.channels[m.channelIndex];
        ImGui::Text("Measure %s: N=%zu  span=%s  Vpp=%.6g %s",
            channel.label.c_str(),
            m.sampleCount,
            formatMetricText(m.duration, displayData.timeUnit.c_str()).c_str(),
            m.peakToPeak,
            channel.unit.c_str());
        ImGui::Text("min=%.6g  max=%.6g  mean=%.6g  rms=%.6g",
            m.minValue,
            m.maxValue,
            m.meanValue,
            m.rmsValue);
    }
}

void drawChannelLegendBar(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot) {
    if (snapshot.channels.empty()) {
        return;
    }
    auto& view = wave.view;
    clampActiveChannel(view, snapshot.channels.size());

    ImGui::Separator();
    ImGui::TextUnformatted("波形图例");
    ImGui::SameLine();

    int scopeIndex = view.cursorSnapScope == plot::WaveCursorSnapScope::AllChannels ? 0 : 1;
    const char* scopeItems[] = {"全部波形", "当前激活波形"};
    ImGui::SetNextItemWidth(150.0F);
    if (ImGui::Combo("吸附范围", &scopeIndex, scopeItems, IM_ARRAYSIZE(scopeItems))) {
        view.cursorSnapScope = scopeIndex == 0 ? plot::WaveCursorSnapScope::AllChannels : plot::WaveCursorSnapScope::ActiveChannel;
    }

    const auto& activeChannel = snapshot.channels[view.measurementChannelIndex];
    ImGui::SameLine();
    ImGui::Text("当前激活: %s", activeChannel.label.c_str());

    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto spec = wave.buffer.channelSpec(channelIndex);
        if (!spec.has_value()) {
            continue;
        }
        ImGui::PushID(static_cast<int>(channelIndex));
        if (channelIndex > 0) {
            ImGui::SameLine();
        }
        ImGui::ColorButton("##legend_color", channelColor(channelIndex), ImGuiColorEditFlags_NoTooltip, ImVec2(12.0F, 12.0F));
        ImGui::SameLine(0.0F, 4.0F);
        const bool active = channelIndex == view.measurementChannelIndex;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22F, 0.36F, 0.24F, 0.9F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28F, 0.44F, 0.30F, 1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20F, 0.32F, 0.22F, 1.0F));
        }
        if (ImGui::Button(spec->label.c_str())) {
            view.measurementChannelIndex = channelIndex;
        }
        if (active) {
            ImGui::PopStyleColor(3);
        }
        ImGui::PopID();
    }
}

void drawChannelControls(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot) {
    if (snapshot.channels.empty()) {
        return;
    }
    auto& view = wave.view;
    clampActiveChannel(view, snapshot.channels.size());

    ImGui::Separator();
    ImGui::Text("通道控制（吸附范围：%s）", snapScopeName(view.cursorSnapScope));
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto current = wave.buffer.channelSpec(channelIndex);
        if (!current.has_value()) {
            continue;
        }
        const plot::ChannelSpec fallbackDefault{
            .label = current->label,
            .unit = current->unit,
            .scale = 1.0,
            .offset = 0.0,
        };
        const plot::ChannelSpec defaultSpec = channelIndex < wave.defaultChannelSpecs.size()
            ? wave.defaultChannelSpecs[channelIndex]
            : fallbackDefault;

        ImGui::PushID(static_cast<int>(channelIndex));
        ImGui::ColorButton("##channel_color", channelColor(channelIndex), ImGuiColorEditFlags_NoTooltip, ImVec2(14.0F, 14.0F));
        ImGui::SameLine();

        const bool active = channelIndex == view.measurementChannelIndex;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.22F, 0.36F, 0.24F, 0.9F));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.28F, 0.44F, 0.30F, 1.0F));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.20F, 0.32F, 0.22F, 1.0F));
        }
        if (ImGui::Button(active ? "激活中" : "设为激活")) {
            view.measurementChannelIndex = channelIndex;
        }
        if (active) {
            ImGui::PopStyleColor(3);
        }
        ImGui::SameLine();
        ImGui::Text("%s  samples=%zu", snapshot.channels[channelIndex].label.c_str(), snapshot.channels[channelIndex].totalSamples);
        if (!current->unit.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("[%s]", current->unit.c_str());
        }

        auto updated = *current;
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::InputDouble("Scale", &updated.scale, 0.1, 1.0, "%.6g")) {
            wave.buffer.setChannelSpec(channelIndex, updated);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0F);
        if (ImGui::InputDouble("Offset", &updated.offset, 0.1, 1.0, "%.6g")) {
            wave.buffer.setChannelSpec(channelIndex, updated);
        }
        ImGui::SameLine();
        if (ImGui::Button("恢复默认")) {
            wave.buffer.setChannelSpec(channelIndex, defaultSpec);
        }
        ImGui::PopID();
    }
}

} // namespace

WaveDockRenderer::WaveDockRenderer(app::Application& application)
    : application_(application) {}

std::string WaveDockRenderer::formatMetric(double value, const char* baseUnit) {
    return formatMetricText(value, baseUnit);
}

void WaveDockRenderer::draw(bool& showWaveDock) {
    if (!showWaveDock) {
        return;
    }

    if (ImGui::Begin("波形", &showWaveDock)) {
        auto& wave = application_.docks().waveState();
        auto& view = wave.view;
        const auto& config = wave.buffer.viewConfig();

        drawWaveToolbar(application_, view, config);
        syncWaveViewToLatest();
        initializeWaveViewIfNeeded(view);

        auto frame = prepareWaveFrame(wave, ImGui::GetContentRegionAvail().x);
        drawCursorToolbar(view, config, frame.displayData);
        drawChannelLegendBar(wave, frame.fullSnapshot);
        const auto plotResult = drawOscilloscopePlot(view, config, frame);
        if (plotResult.plotRendered) {
            drawOverviewWindow(view, config, frame.fullSnapshot, frame.displayData, frame.displayBounds, frame.renderBudget);
            drawWaveReadouts(view, frame.snapshot, frame.displayData, plotResult);
        }
        drawChannelControls(wave, frame.fullSnapshot);
    }
    ImGui::End();
}

void WaveDockRenderer::syncWaveViewToLatest() {
    auto& wave = application_.docks().waveState();
    if (!wave.view.autoFollowLatest) {
        return;
    }

    const auto snapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    double latestTime = 0.0;
    bool found = false;
    for (const auto& channel : snapshot.channels) {
        if (channel.totalSamples == 0 || channel.samples == nullptr) {
            continue;
        }
        latestTime = (std::max)(latestTime, channel.samples[channel.totalSamples - 1].time);
        found = true;
    }
    if (found) {
        wave.view.viewMaxTime = latestTime;
        wave.view.viewMinTime = latestTime - wave.view.visibleDuration;
        wave.view.centerTime = 0.5 * (wave.view.viewMinTime + wave.view.viewMaxTime);
        if (!wave.view.lockVerticalRange && !wave.view.initialized) {
            wave.view.viewMinValue = wave.view.manualVerticalMin;
            wave.view.viewMaxValue = wave.view.manualVerticalMax;
        }
    }
}

} // namespace protoscope::ui
