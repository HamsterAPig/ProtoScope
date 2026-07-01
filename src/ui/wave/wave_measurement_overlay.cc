#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace protoscope::ui {

bool cursorSmartSnapActive(const plot::WaveViewState& view, const ImGuiIO& io)
{
    return view.cursorSnapMode == plot::WaveCursorSnapMode::SmartSnap ||
           (view.cursorSnapMode == plot::WaveCursorSnapMode::ModifierSnap && (io.KeyShift || io.KeyCtrl));
}

double normalizedSnapScore(
    const plot::CursorReadout& readout, double time, double mouseValue, double maxTimeDistance, double maxValueDistance)
{
    const double timeScore = maxTimeDistance > 0.0 ? std::abs(readout.time - time) / maxTimeDistance : 0.0;
    const double valueScore =
        maxValueDistance > 0.0 ? std::abs(readout.displayValue - mouseValue) / maxValueDistance : 0.0;
    return timeScore * timeScore + valueScore * valueScore;
}

std::optional<plot::CursorReadout> findNearestBitTransition(const plot::WaveSnapshot& snapshot,
                                                            const BitLaneLayout& layout,
                                                            double time,
                                                            double plotY,
                                                            double maxTimeDistance,
                                                            double maxValueDistance)
{
    if (!std::isfinite(time) || !std::isfinite(plotY) || !std::isfinite(maxTimeDistance) ||
        !std::isfinite(maxValueDistance) || maxTimeDistance < 0.0 || maxValueDistance < 0.0) {
        return std::nullopt;
    }

    std::optional<plot::CursorReadout> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (const auto& lane : layout.lanes) {
        if (lane.parentChannelIndex >= snapshot.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[lane.parentChannelIndex];
        if (channel.samples == nullptr) {
            continue;
        }
        const std::size_t begin = (std::min)(channel.visibleBegin, channel.totalSamples);
        const std::size_t end = (std::min)(channel.visibleEnd, channel.totalSamples);
        if (end <= begin + 1U) {
            continue;
        }

        bool previousState = rawBitEnabled(channel.samples[begin].value, lane.bitIndex);
        for (std::size_t sampleIndex = begin + 1U; sampleIndex < end; ++sampleIndex) {
            const bool currentState = rawBitEnabled(channel.samples[sampleIndex].value, lane.bitIndex);
            if (currentState == previousState) {
                continue;
            }
            previousState = currentState;

            const auto& sample = channel.samples[sampleIndex];
            const double displayY = currentState ? lane.highY : lane.lowY;
            const double timeDistance = std::abs(sample.time - time);
            const double valueDistance = std::abs(displayY - plotY);
            if (timeDistance > maxTimeDistance || valueDistance > maxValueDistance) {
                continue;
            }
            const double score = timeDistance * timeDistance + valueDistance * valueDistance;
            if (best.has_value() && score >= bestScore) {
                continue;
            }
            bestScore = score;
            best = plot::CursorReadout{
                .valid = true,
                .channelIndex = lane.parentChannelIndex,
                .sampleIndex = sampleIndex,
                .time = sample.time,
                .value = currentState ? 1.0 : 0.0,
                .displayValue = displayY,
                .bit =
                    plot::BitLaneReadout{
                        .parentChannelIndex = lane.parentChannelIndex,
                        .bitIndex = lane.bitIndex,
                        .laneIndex = lane.laneIndex,
                        .value = currentState,
                        .y = displayY,
                    },
            };
        }
    }
    return best;
}

namespace {

    std::optional<plot::CursorReadout> findCurrentBitLaneReadout(const plot::WaveSnapshot& snapshot,
                                                                 const plot::WaveDisplayData& displayData,
                                                                 const BitLaneLayoutEntry& lane,
                                                                 double time,
                                                                 double maxTimeDistance)
    {
        if (!std::isfinite(time) || !std::isfinite(maxTimeDistance) || maxTimeDistance < 0.0 ||
            lane.parentChannelIndex >= snapshot.channels.size() ||
            lane.parentChannelIndex >= displayData.channels.size()) {
            return std::nullopt;
        }

        const auto& sourceChannel = snapshot.channels[lane.parentChannelIndex];
        const auto& displayChannel = displayData.channels[lane.parentChannelIndex];
        const auto& samples = displayChannel.samples;
        if (samples.empty()) {
            return std::nullopt;
        }
        if (time < samples.front().time - maxTimeDistance || time > samples.back().time + maxTimeDistance) {
            return std::nullopt;
        }

        const auto upper = std::upper_bound(
            samples.begin(), samples.end(), time, [](double value, const auto& sample) { return value < sample.time; });
        const auto sampleIt = upper == samples.begin() ? samples.begin() : std::prev(upper);
        const std::size_t displaySampleIndex = static_cast<std::size_t>(std::distance(samples.begin(), sampleIt));

        double rawValue = sampleIt->value;
        std::size_t sourceSampleIndex = displaySampleIndex;
        if (sourceChannel.samples != nullptr) {
            const std::size_t begin = (std::min)(sourceChannel.visibleBegin, sourceChannel.totalSamples);
            const std::size_t mappedSourceIndex = begin + displaySampleIndex;
            if (mappedSourceIndex < sourceChannel.totalSamples) {
                sourceSampleIndex = mappedSourceIndex;
                rawValue = sourceChannel.samples[mappedSourceIndex].value;
            }
        } else if (displaySampleIndex < displayChannel.actualValues.size()) {
            rawValue = displayChannel.actualValues[displaySampleIndex];
        }

        const bool value = rawBitEnabled(rawValue, lane.bitIndex);
        const double displayY = value ? lane.highY : lane.lowY;
        return plot::CursorReadout{
            .valid = true,
            .channelIndex = lane.parentChannelIndex,
            .sampleIndex = sourceSampleIndex,
            .time = time,
            .value = value ? 1.0 : 0.0,
            .displayValue = displayY,
            .bit =
                plot::BitLaneReadout{
                    .parentChannelIndex = lane.parentChannelIndex,
                    .bitIndex = lane.bitIndex,
                    .laneIndex = lane.laneIndex,
                    .value = value,
                    .y = displayY,
                },
        };
    }

    std::vector<std::size_t> waveformHoverChannels(const plot::WaveSnapshot& snapshot,
                                                   const plot::WaveDisplayData& displayData,
                                                   const std::vector<std::size_t>& visibleChannelIndices)
    {
        std::vector<std::size_t> channels;
        channels.reserve(visibleChannelIndices.size());
        for (const std::size_t channelIndex : visibleChannelIndices) {
            if (channelIndex >= snapshot.channels.size() || channelIndex >= displayData.channels.size()) {
                continue;
            }
            if (bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
                continue;
            }
            channels.push_back(channelIndex);
        }
        return channels;
    }

    bool hasVisibleBitLane(const BitLaneLayout& bitLayout, const std::vector<std::size_t>& visibleChannelIndices)
    {
        for (const auto& lane : bitLayout.lanes) {
            if (std::find(visibleChannelIndices.begin(), visibleChannelIndices.end(), lane.parentChannelIndex) !=
                visibleChannelIndices.end()) {
                return true;
            }
        }
        return false;
    }

    bool visibleChannelContains(const std::vector<std::size_t>& visibleChannelIndices, std::size_t channelIndex)
    {
        return std::find(visibleChannelIndices.begin(), visibleChannelIndices.end(), channelIndex) !=
               visibleChannelIndices.end();
    }

    bool bitReadoutCandidateAllowed(plot::WaveBitDisplayReadoutPolicy policy,
                                    const BitLaneLayout& bitLayout,
                                    double plotY,
                                    double maxValueDistance,
                                    bool activeBitLaneVisibleForReadout)
    {
        if (policy == plot::WaveBitDisplayReadoutPolicy::MixedNearest) {
            return true;
        }
        return activeBitLaneVisibleForReadout || findBitLaneAtPlotValue(bitLayout, plotY, maxValueDistance).has_value();
    }

} // namespace

std::optional<HoverReadout> findHoverReadout(const plot::WaveSnapshot& snapshot,
                                             const plot::WaveDisplayData& displayData,
                                             const std::vector<std::size_t>& visibleChannelIndices,
                                             const BitLaneLayout& bitLayout,
                                             double time,
                                             double plotY,
                                             double maxTimeDistance,
                                             double maxValueDistance,
                                             bool preferWaveformHoverReadout,
                                             plot::WaveBitDisplayReadoutPolicy bitDisplayReadoutPolicy,
                                             bool activeBitLaneVisibleForReadout)
{
    if (visibleChannelIndices.empty()) {
        return std::nullopt;
    }

    const auto findBitLaneReadout = [&]() -> std::optional<HoverReadout> {
        const auto bitLane = findBitLaneAtPlotValue(bitLayout, plotY, maxValueDistance);
        if (!bitLane.has_value()) {
            return std::nullopt;
        }
        if (!visibleChannelContains(visibleChannelIndices, bitLane->lane.parentChannelIndex)) {
            return std::nullopt;
        }
        if (const auto readout =
                findCurrentBitLaneReadout(snapshot, displayData, bitLane->lane, time, maxTimeDistance)) {
            return HoverReadout{.kind = HoverReadoutKind::BitLane, .readout = *readout};
        }
        return std::nullopt;
    };

    const auto findWaveformReadout = [&]() -> std::optional<HoverReadout> {
        const auto waveformChannels = waveformHoverChannels(snapshot, displayData, visibleChannelIndices);
        const auto readout = plot::findNearestDisplayPointInChannels(
            displayData, waveformChannels, time, plotY, maxTimeDistance, maxValueDistance);
        if (!readout.has_value()) {
            return std::nullopt;
        }
        return HoverReadout{.kind = HoverReadoutKind::Waveform, .readout = *readout};
    };

    const auto waveformReadout = findWaveformReadout();
    std::optional<HoverReadout> bitLaneReadout;
    if (hasVisibleBitLane(bitLayout, visibleChannelIndices) &&
        bitReadoutCandidateAllowed(
            bitDisplayReadoutPolicy, bitLayout, plotY, maxValueDistance, activeBitLaneVisibleForReadout)) {
        bitLaneReadout = findBitLaneReadout();
    }

    if (waveformReadout.has_value() && bitLaneReadout.has_value()) {
        const double waveformScore =
            normalizedSnapScore(waveformReadout->readout, time, plotY, maxTimeDistance, maxValueDistance);
        const double bitScore =
            normalizedSnapScore(bitLaneReadout->readout, time, plotY, maxTimeDistance, maxValueDistance);
        if (bitScore < waveformScore || (!preferWaveformHoverReadout && bitScore <= waveformScore)) {
            return bitLaneReadout;
        }
        return waveformReadout;
    }

    if (bitLaneReadout.has_value()) {
        return bitLaneReadout;
    }

    return waveformReadout;
}

std::vector<CursorIntersectionReadout> collectCursorIntersectionReadouts(
    const plot::WaveViewState& view,
    const plot::WaveSnapshot& snapshot,
    const plot::WaveDisplayData& displayData,
    const std::vector<std::size_t>& visibleChannelIndices,
    double maxTimeDistance)
{
    std::vector<CursorIntersectionReadout> readouts;
    if (!view.showCursorIntersectionReadouts || !view.showCursors || visibleChannelIndices.empty()) {
        return readouts;
    }

    for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
        const auto& cursor = view.cursors[cursorIndex];
        if (!cursor.enabled) {
            continue;
        }
        for (const std::size_t channelIndex : visibleChannelIndices) {
            if (channelIndex >= snapshot.channels.size() || channelIndex >= displayData.channels.size()) {
                continue;
            }
            if (bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
                continue;
            }
            // 核心流程：交点读数按游标时间找最近采样点，和现有 hover/游标读数保持一致，不做插值。
            const auto readout =
                plot::findNearestDisplayByTime(displayData, channelIndex, cursor.time, maxTimeDistance);
            if (!readout.has_value()) {
                continue;
            }
            readouts.push_back({
                .cursorIndex = cursorIndex,
                .cursorTime = cursor.time,
                .readout = *readout,
            });
        }
    }
    return readouts;
}

bool bitLaneMeasurementActive(const plot::WaveViewState& view)
{
    return view.activeBitLane.active;
}

bool activeBitLaneVisible(const plot::WaveViewState& view, const BitLaneLayout& layout)
{
    if (!view.activeBitLane.active) {
        return false;
    }
    for (const auto& lane : layout.lanes) {
        if (lane.parentChannelIndex == view.activeBitLane.parentChannelIndex &&
            lane.bitIndex == view.activeBitLane.bitIndex && lane.laneIndex == view.activeBitLane.laneIndex) {
            return true;
        }
    }
    return false;
}

bool cursorPairUsesBitLanes(const std::array<std::optional<plot::CursorReadout>, 2>& cursorReadouts)
{
    return cursorReadouts[0].has_value() && cursorReadouts[1].has_value() && cursorReadouts[0]->bit.has_value() &&
           cursorReadouts[1]->bit.has_value();
}

plot::MeasurementReadout makeBitIntervalMeasurement(const plot::CursorReadout& left, const plot::CursorReadout& right)
{
    if (!left.valid || !right.valid || !left.bit.has_value() || !right.bit.has_value()) {
        return {};
    }
    return plot::MeasurementReadout{
        .valid = true,
        .channelIndex = left.channelIndex,
        .sampleCount = 0,
        .duration = std::abs(right.time - left.time),
    };
}

std::optional<SmartCursorSnap> findNearestWaveformExtreme(const plot::WaveDisplayData& displayData,
                                                          std::size_t channelIndex,
                                                          double time,
                                                          double mouseValue,
                                                          double valueHeight,
                                                          double maxTimeDistance)
{
    if (!std::isfinite(mouseValue) || valueHeight <= 0.0) {
        return std::nullopt;
    }

    constexpr double kExtremeSnapValueRatio = 0.08;
    const double maxValueDistance = valueHeight * kExtremeSnapValueRatio;
    std::optional<SmartCursorSnap> best;
    double bestScore = std::numeric_limits<double>::infinity();
    const auto considerExtreme = [&](plot::WaveExtremeKind kind, std::string_view label) {
        const auto candidate = plot::findLocalExtremeNearTime(displayData, channelIndex, time, maxTimeDistance, kind);
        if (!candidate.has_value() || !std::isfinite(candidate->displayValue)) {
            return;
        }
        const double valueDistance = std::abs(candidate->displayValue - mouseValue);
        if (valueDistance > maxValueDistance) {
            return;
        }
        // 核心流程：默认策略按鼠标到峰/谷显示点的二维距离评分，避免远离波形时被极值抢吸附。
        const double score = normalizedSnapScore(*candidate, time, mouseValue, maxTimeDistance, maxValueDistance);
        if (!best.has_value() || score < bestScore) {
            bestScore = score;
            best = SmartCursorSnap{.readout = *candidate, .label = label};
        }
    };
    considerExtreme(plot::WaveExtremeKind::Maximum, "Peak");
    considerExtreme(plot::WaveExtremeKind::Minimum, "Trough");
    return best;
}

std::optional<SmartCursorSnap> findSmartCursorSnapForChannel(const plot::WaveDisplayData& displayData,
                                                             std::size_t channelIndex,
                                                             double time,
                                                             double mouseValue,
                                                             const ImPlotRect& limits,
                                                             double maxTimeDistance,
                                                             plot::WaveCursorExtremeSnapPolicy extremeSnapPolicy)
{
    if (channelIndex >= displayData.channels.size()) {
        return std::nullopt;
    }
    const double minValue = (std::min)(limits.Y.Min, limits.Y.Max);
    const double maxValue = (std::max)(limits.Y.Min, limits.Y.Max);
    const double valueHeight = maxValue - minValue;
    constexpr double kExtremeSnapZoneRatio = 0.15;
    if (std::isfinite(mouseValue) && valueHeight > 0.0) {
        if (extremeSnapPolicy == plot::WaveCursorExtremeSnapPolicy::NearestWaveform) {
            if (auto nearestExtreme = findNearestWaveformExtreme(
                    displayData, channelIndex, time, mouseValue, valueHeight, maxTimeDistance)) {
                return nearestExtreme;
            }
        } else if (mouseValue >= maxValue - valueHeight * kExtremeSnapZoneRatio) {
            // 兼容旧策略：鼠标靠近绘图区顶部/底部时，极值优先于边沿。
            auto peak = plot::findLocalExtremeNearTime(
                displayData, channelIndex, time, maxTimeDistance, plot::WaveExtremeKind::Maximum);
            if (peak.has_value()) {
                return SmartCursorSnap{.readout = *peak, .label = "Peak"};
            }
        } else if (mouseValue <= minValue + valueHeight * kExtremeSnapZoneRatio) {
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

std::optional<SmartCursorSnap> findSmartCursorSnapByWaveformScope(const plot::WaveSnapshot& snapshot,
                                                                  const plot::WaveDisplayData& displayData,
                                                                  const plot::WaveViewState& view,
                                                                  double time,
                                                                  double mouseValue,
                                                                  const ImPlotRect& limits,
                                                                  double maxTimeDistance,
                                                                  std::optional<std::size_t> forcedChannelIndex)
{
    if (forcedChannelIndex.has_value()) {
        const std::size_t channelIndex = *forcedChannelIndex;
        if (channelIndex >= snapshot.channels.size() || bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
            return std::nullopt;
        }
        return findSmartCursorSnapForChannel(
            displayData, channelIndex, time, mouseValue, limits, maxTimeDistance, view.cursorExtremeSnapPolicy);
    }

    if (view.cursorSnapScope == plot::WaveCursorSnapScope::ActiveChannel) {
        if (view.measurementChannelIndex >= snapshot.channels.size() ||
            bitDisplayEnabled(snapshot.channels[view.measurementChannelIndex].bitDisplay)) {
            return std::nullopt;
        }
        return findSmartCursorSnapForChannel(displayData,
                                             view.measurementChannelIndex,
                                             time,
                                             mouseValue,
                                             limits,
                                             maxTimeDistance,
                                             view.cursorExtremeSnapPolicy);
    }

    std::optional<SmartCursorSnap> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (std::size_t channelIndex = 0; channelIndex < displayData.channels.size(); ++channelIndex) {
        if (channelIndex >= snapshot.channels.size() || bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
            continue;
        }
        const auto candidate = findSmartCursorSnapForChannel(
            displayData, channelIndex, time, mouseValue, limits, maxTimeDistance, view.cursorExtremeSnapPolicy);
        if (!candidate.has_value()) {
            continue;
        }
        const double timeDistance = std::abs(candidate->readout.time - time);
        const double valueDistance =
            std::isfinite(mouseValue) ? std::abs(candidate->readout.displayValue - mouseValue) : 0.0;
        const double score = timeDistance * timeDistance + valueDistance * valueDistance;
        if (!best.has_value() || score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

std::optional<SmartCursorSnap> findSmartCursorSnapByScope(const plot::WaveDisplayData& displayData,
                                                          const plot::WaveViewState& view,
                                                          double time,
                                                          double mouseValue,
                                                          const ImPlotRect& limits,
                                                          double maxTimeDistance,
                                                          std::optional<std::size_t> forcedChannelIndex)
{
    if (forcedChannelIndex.has_value()) {
        return findSmartCursorSnapForChannel(
            displayData, *forcedChannelIndex, time, mouseValue, limits, maxTimeDistance, view.cursorExtremeSnapPolicy);
    }

    if (view.cursorSnapScope == plot::WaveCursorSnapScope::ActiveChannel) {
        return findSmartCursorSnapForChannel(displayData,
                                             view.measurementChannelIndex,
                                             time,
                                             mouseValue,
                                             limits,
                                             maxTimeDistance,
                                             view.cursorExtremeSnapPolicy);
    }

    std::optional<SmartCursorSnap> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (std::size_t channelIndex = 0; channelIndex < displayData.channels.size(); ++channelIndex) {
        const auto candidate = findSmartCursorSnapForChannel(
            displayData, channelIndex, time, mouseValue, limits, maxTimeDistance, view.cursorExtremeSnapPolicy);
        if (!candidate.has_value()) {
            continue;
        }
        const double timeDistance = std::abs(candidate->readout.time - time);
        const double valueDistance =
            std::isfinite(mouseValue) ? std::abs(candidate->readout.displayValue - mouseValue) : 0.0;
        const double score = timeDistance * timeDistance + valueDistance * valueDistance;
        if (!best.has_value() || score < bestScore) {
            bestScore = score;
            best = candidate;
        }
    }
    return best;
}

std::optional<SmartCursorSnap> findSmartCursorSnapByScope(const plot::WaveSnapshot& snapshot,
                                                          const plot::WaveDisplayData& displayData,
                                                          const plot::WaveViewState& view,
                                                          const BitLaneLayout& bitLayout,
                                                          double time,
                                                          double mouseValue,
                                                          const ImPlotRect& limits,
                                                          double maxTimeDistance,
                                                          std::optional<std::size_t> forcedChannelIndex)
{
    const double maxValueDistance = std::abs(limits.Y.Max - limits.Y.Min) / 30.0;
    const auto waveformSnap = findSmartCursorSnapByWaveformScope(
        snapshot, displayData, view, time, mouseValue, limits, maxTimeDistance, forcedChannelIndex);
    std::optional<SmartCursorSnap> bitSnap;
    if (bitReadoutCandidateAllowed(view.bitDisplayReadoutPolicy,
                                   bitLayout,
                                   mouseValue,
                                   maxValueDistance,
                                   activeBitLaneVisible(view, bitLayout))) {
        if (const auto transition =
                findNearestBitTransition(snapshot, bitLayout, time, mouseValue, maxTimeDistance, maxValueDistance)) {
            bitSnap = SmartCursorSnap{.readout = *transition, .label = "Bit Edge"};
        }
    }

    if (waveformSnap.has_value() && bitSnap.has_value()) {
        const double waveformScore =
            normalizedSnapScore(waveformSnap->readout, time, mouseValue, maxTimeDistance, maxValueDistance);
        const double bitScore =
            normalizedSnapScore(bitSnap->readout, time, mouseValue, maxTimeDistance, maxValueDistance);
        return bitScore < waveformScore ? bitSnap : waveformSnap;
    }

    return bitSnap.has_value() ? bitSnap : waveformSnap;
}

plot::MeasurementReadout measureDisplayWindow(const plot::WaveDisplayData& displayData,
                                              std::size_t channelIndex,
                                              double beginTime,
                                              double endTime,
                                              std::optional<std::size_t> referenceChannelIndex,
                                              std::optional<double> manualReferenceValue)
{
    plot::MeasurementReadout result{};
    if (channelIndex >= displayData.channels.size()) {
        return result;
    }
    if (endTime < beginTime) {
        std::swap(beginTime, endTime);
    }
    const auto& samples = displayData.channels[channelIndex].samples;
    const auto begin =
        std::lower_bound(samples.begin(), samples.end(), beginTime, [](const plot::WaveSample& sample, double value) {
            return sample.time < value;
        });
    const auto end =
        std::upper_bound(samples.begin(), samples.end(), endTime, [](double value, const plot::WaveSample& sample) {
            return value < sample.time;
        });
    if (begin >= end) {
        return result;
    }

    const auto beginIndex = static_cast<std::size_t>(std::distance(samples.begin(), begin));
    const auto endIndex = static_cast<std::size_t>(std::distance(samples.begin(), end));
    const auto& actualValues = displayData.channels[channelIndex].actualValues;
    std::vector<double> times;
    std::vector<double> values;
    std::vector<double> referenceValues;
    times.reserve(endIndex - beginIndex);
    values.reserve(endIndex - beginIndex);
    referenceValues.reserve(endIndex - beginIndex);
    for (std::size_t sampleIndex = beginIndex; sampleIndex < endIndex; ++sampleIndex) {
        times.push_back(samples[sampleIndex].time);
        values.push_back(sampleIndex < actualValues.size() ? actualValues[sampleIndex] : samples[sampleIndex].value);
    }

    if (manualReferenceValue.has_value()) {
        referenceValues.assign(values.size(), *manualReferenceValue);
    } else if (referenceChannelIndex.has_value() && *referenceChannelIndex < displayData.channels.size()) {
        const auto& referenceChannel = displayData.channels[*referenceChannelIndex];
        const auto& referenceSamples = referenceChannel.samples;
        const auto& referenceActualValues = referenceChannel.actualValues;
        for (const double time : times) {
            const auto match =
                std::lower_bound(referenceSamples.begin(),
                                 referenceSamples.end(),
                                 time,
                                 [](const plot::WaveSample& sample, double value) { return sample.time < value; });
            if (match == referenceSamples.end() || std::abs(match->time - time) > 1e-9) {
                referenceValues.clear();
                break;
            }
            const auto referenceIndex = static_cast<std::size_t>(std::distance(referenceSamples.begin(), match));
            referenceValues.push_back(
                referenceIndex < referenceActualValues.size() ? referenceActualValues[referenceIndex] : match->value);
        }
    }
    return plot::makeMeasurementReadout(
        channelIndex, times, values, referenceValues.size() == values.size() ? &referenceValues : nullptr);
}

void drawCursorAnnotation(std::size_t cursorIndex,
                          const plot::CursorReadout& readout,
                          const plot::ChannelView& channel,
                          std::string_view timeUnit,
                          std::string_view snapLabel)
{
    const std::string labelPrefix = snapLabel.empty() ? "" : std::string(snapLabel) + " ";
    ImPlot::Annotation(readout.time,
                       readout.displayValue,
                       ImVec4(1.0F, 1.0F, 1.0F, 0.92F),
                       ImVec2(10.0F, cursorIndex == 0 ? -18.0F : 18.0F),
                       true,
                       "%c %s%s\n%s %.6g",
                       cursorIndex == 0 ? 'A' : 'B',
                       labelPrefix.c_str(),
                       formatMetricText(readout.time, std::string(timeUnit).c_str()).c_str(),
                       channel.label.c_str(),
                       readout.value);
}

void drawCursorIntersectionReadouts(const std::vector<CursorIntersectionReadout>& readouts,
                                    const plot::WaveSnapshot& snapshot)
{
    for (std::size_t index = 0; index < readouts.size(); ++index) {
        const auto& entry = readouts[index];
        if (entry.readout.channelIndex >= snapshot.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[entry.readout.channelIndex];
        const float yOffset = entry.cursorIndex == 0 ? -18.0F : 18.0F;
        const float xOffset = 10.0F + static_cast<float>(index % 3U) * 4.0F;
        ImPlot::Annotation(entry.cursorTime,
                           entry.readout.displayValue,
                           ImVec4(1.0F, 1.0F, 1.0F, 0.86F),
                           ImVec2(xOffset, yOffset),
                           true,
                           "%c %s\n%.6g %s",
                           entry.cursorIndex == 0 ? 'A' : 'B',
                           channel.label.c_str(),
                           entry.readout.value,
                           channel.unit.c_str());
    }
}

void drawCursorIntervalHint(const plot::CursorReadout& left,
                            const plot::CursorReadout& right,
                            const plot::CursorIntervalText& intervalText,
                            const ImPlotRect& limits)
{
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
                                  ? ("dt " + formatMetricText(intervalText.delta, intervalText.deltaUnit.c_str()) +
                                     "\n" + formatMetricText(intervalText.frequencyHz, "Hz"))
                                  : ("dsample " + formatMetricText(intervalText.delta, intervalText.deltaUnit.c_str()));
    const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
    const ImVec2 center = ImVec2(0.5F * (start.x + end.x), start.y);
    const ImVec2 textMin = ImVec2(center.x - 0.5F * textSize.x - 5.0F, center.y - textSize.y - 7.0F);
    const ImVec2 textMax = ImVec2(center.x + 0.5F * textSize.x + 5.0F, center.y - 3.0F);
    drawList->AddRectFilled(textMin, textMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.06F, 0.06F, 0.04F, 0.72F)), 3.0F);
    drawList->AddText(ImVec2(textMin.x + 5.0F, textMin.y + 2.0F), lineColor, label.c_str());

    const auto drawTimeChip = [&](const char* prefix, const plot::CursorReadout& readout, ImVec4 color) {
        const std::string chip =
            std::string(prefix) + " " + formatMetricText(readout.time, intervalText.deltaUnit.c_str());
        const ImVec2 chipSize = ImGui::CalcTextSize(chip.c_str());
        const ImVec2 anchor = ImPlot::PlotToPixels(readout.time, limits.Y.Min);
        const ImVec2 chipMin(anchor.x - chipSize.x * 0.5F - 5.0F, anchor.y - chipSize.y - 8.0F);
        const ImVec2 chipMax(anchor.x + chipSize.x * 0.5F + 5.0F, anchor.y - 3.0F);
        const ImU32 chipColor =
            ImGui::ColorConvertFloat4ToU32(ImVec4(color.x * 0.20F, color.y * 0.20F, color.z * 0.20F, 0.82F));
        const ImU32 chipTextColor = ImGui::ColorConvertFloat4ToU32(color);
        drawList->AddRectFilled(chipMin, chipMax, chipColor, 3.0F);
        drawList->AddRect(chipMin, chipMax, chipTextColor, 3.0F);
        drawList->AddText(ImVec2(chipMin.x + 5.0F, chipMin.y + 2.0F), chipTextColor, chip.c_str());
    };
    drawTimeChip("A", left, ImVec4(1.0F, 0.761F, 0.278F, 1.0F));
    drawTimeChip("B", right, ImVec4(0.0F, 0.722F, 1.0F, 1.0F));
    ImPlot::PopPlotClipRect();
}

} // namespace protoscope::ui
