// 本文件由 wave_dock_renderer.cpp 按原顺序包含，承接对应 Wave 业务组件实现。

#if !defined(PROTOSCOPE_WAVE_RENDERER_COMPONENT_INCLUDE)
#error "This wave component implementation is included by wave_dock_renderer.cpp"
#endif


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
    const double minValue = (std::min)(limits.Y.Min, limits.Y.Max);
    const double maxValue = (std::max)(limits.Y.Min, limits.Y.Max);
    const double valueHeight = maxValue - minValue;
    constexpr double kExtremeSnapZoneRatio = 0.15;
    if (std::isfinite(mouseValue) && valueHeight > 0.0) {
        // 鼠标靠近绘图区顶部/底部时，极值优先于边沿，方便直接锁峰值或谷值。
        if (mouseValue >= maxValue - valueHeight * kExtremeSnapZoneRatio) {
            auto peak = plot::findLocalExtremeNearTime(
                displayData, channelIndex, time, maxTimeDistance, plot::WaveExtremeKind::Maximum);
            if (peak.has_value()) {
                return SmartCursorSnap{.readout = *peak, .label = "Peak"};
            }
        }
        if (mouseValue <= minValue + valueHeight * kExtremeSnapZoneRatio) {
            auto trough = plot::findLocalExtremeNearTime(
                displayData, channelIndex, time, maxTimeDistance, plot::WaveExtremeKind::Minimum);
            if (trough.has_value()) {
                return SmartCursorSnap{.readout = *trough, .label = "Trough"};
            }
        }
    }
    // 常规智能吸附优先找最大跳变；找不到再交给调用方使用按时间最近点兜底。
    auto edge = plot::findStrongestEdgeNearTime(displayData, channelIndex, time, maxTimeDistance);
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
        const double valueDistance = std::isfinite(mouseValue) ? std::abs(candidate->readout.displayValue - mouseValue) : 0.0;
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
    const auto beginIndex = static_cast<std::size_t>(std::distance(samples.begin(), begin));
    const auto endIndex = static_cast<std::size_t>(std::distance(samples.begin(), end));
    const auto& actualValues = displayData.channels[channelIndex].actualValues;
    for (std::size_t sampleIndex = beginIndex; sampleIndex < endIndex; ++sampleIndex) {
        const double value = sampleIndex < actualValues.size() ? actualValues[sampleIndex] : samples[sampleIndex].value;
        result.minValue = (std::min)(result.minValue, value);
        result.maxValue = (std::max)(result.maxValue, value);
        sum += value;
        squareSum += value * value;
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
                       readout.displayValue,
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
