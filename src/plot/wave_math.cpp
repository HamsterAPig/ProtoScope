#include "protoscope/plot/wave_math.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace protoscope::plot {

namespace {

constexpr double kEpsilon = 1e-12;

std::string trimCopy(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string_view::npos) {
        return {};
    }
    const auto end = text.find_last_not_of(" \t\r\n");
    return std::string{text.substr(begin, end - begin + 1)};
}

double clampPositiveWidth(double width, double minWidth) {
    return (std::max)(width, (std::max)(minWidth, kEpsilon));
}

double resolveActualValue(const WaveDisplayChannel& channel, std::size_t sampleIndex, double fallbackValue) {
    if (sampleIndex < channel.actualValues.size()) {
        return channel.actualValues[sampleIndex];
    }
    return fallbackValue;
}

void clampTimeRange(double& minTime, double& maxTime, const WaveDataBounds& bounds) {
    if (!bounds.valid) {
        return;
    }
    const double width = maxTime - minTime;
    const double dataWidth = (std::max)(bounds.maxTime - bounds.minTime, kEpsilon);
    if (width >= dataWidth) {
        minTime = bounds.minTime;
        maxTime = bounds.maxTime;
        return;
    }
    if (minTime < bounds.minTime) {
        minTime = bounds.minTime;
        maxTime = minTime + width;
    }
    if (maxTime > bounds.maxTime) {
        maxTime = bounds.maxTime;
        minTime = maxTime - width;
    }
}

void clampValueRange(double& minValue, double& maxValue, const WaveDataBounds& bounds) {
    if (!bounds.valid) {
        return;
    }
    const double height = maxValue - minValue;
    const double dataHeight = (std::max)(bounds.maxValue - bounds.minValue, kEpsilon);
    if (height >= dataHeight) {
        minValue = bounds.minValue;
        maxValue = bounds.maxValue;
        return;
    }
    if (minValue < bounds.minValue) {
        minValue = bounds.minValue;
        maxValue = minValue + height;
    }
    if (maxValue > bounds.maxValue) {
        maxValue = bounds.maxValue;
        minValue = maxValue - height;
    }
}

} // namespace

FrequencyParseResult parseSampleFrequencyText(std::string_view text) {
    const std::string trimmed = trimCopy(text);
    if (trimmed.empty()) {
        return {.accepted = true, .valueHz = 0.0, .error = {}};
    }

    char* parseEnd = nullptr;
    errno = 0;
    const double parsed = std::strtod(trimmed.c_str(), &parseEnd);
    if (parseEnd == trimmed.c_str() || *parseEnd != '\0' || errno == ERANGE || !std::isfinite(parsed) || parsed < 0.0) {
        return {.accepted = false, .valueHz = 0.0, .error = "频率格式无效"};
    }
    return {.accepted = true, .valueHz = parsed, .error = {}};
}

WaveLayoutSizes solveWaveLayout(float contentWidth,
                                float contentHeight,
                                float requestedOverviewHeight,
                                float requestedToolsWidth,
                                float toolsCollapsedWidth,
                                bool toolsCollapsed,
                                float contentToolsSplitterWidth,
                                float overviewMainSplitterHeight,
                                float minOverviewHeight,
                                float minMainHeight,
                                float minToolsWidth,
                                 float maxToolsWidth,
                                 float fixedContentHeight) {
    WaveLayoutSizes result{};
    const float safeContentWidth = (std::max)(contentWidth, 0.0F);
    const float safeContentHeight = (std::max)(contentHeight, 0.0F);
    const float safeContentToolsSplitterWidth = (std::max)(contentToolsSplitterWidth, 0.0F);
    const float safeSplitterHeight = (std::max)(overviewMainSplitterHeight, 0.0F);
    // fixedContentHeight 覆盖图例栏、内容区 spacing 与主图横轴安全空间，避免主图高度虚高。
    const float safeFixedHeight = (std::max)(fixedContentHeight, 0.0F);
    const float safeMinOverview = (std::max)(minOverviewHeight, 0.0F);
    const float safeMinMain = (std::max)(minMainHeight, 0.0F);
    const float availableToolsWidth = (std::max)(0.0F, safeContentWidth - safeContentToolsSplitterWidth);

    if (toolsCollapsed) {
        result.toolsWidth = (std::clamp)(toolsCollapsedWidth, 0.0F, availableToolsWidth);
    } else {
        const float safeMinToolsWidth = (std::max)(minToolsWidth, 0.0F);
        const float safeMaxToolsWidth = (std::max)(maxToolsWidth, safeMinToolsWidth);
        result.toolsWidth = (std::clamp)(requestedToolsWidth, safeMinToolsWidth, safeMaxToolsWidth);
        result.toolsWidth = (std::min)(result.toolsWidth, availableToolsWidth);
    }

    const float availableHeight = (std::max)(0.0F, safeContentHeight - safeFixedHeight - safeSplitterHeight);
    if (availableHeight <= 0.0F) {
        return result;
    }

    if (availableHeight < safeMinOverview + safeMinMain) {
        const float overviewRatio = safeMinOverview + safeMinMain > 0.0F
            ? safeMinOverview / (safeMinOverview + safeMinMain)
            : 0.35F;
        result.overviewHeight = std::floor(availableHeight * overviewRatio);
        result.mainHeight = availableHeight - result.overviewHeight;
        return result;
    }

    result.overviewHeight = (std::clamp)(requestedOverviewHeight, safeMinOverview, availableHeight - safeMinMain);
    result.mainHeight = availableHeight - result.overviewHeight;
    return result;
}

bool scriptTimeUsable(const std::vector<WaveSample>& samples) {
    if (samples.empty()) {
        return false;
    }
    double previous = samples.front().time;
    if (!std::isfinite(previous)) {
        return false;
    }
    for (std::size_t index = 1; index < samples.size(); ++index) {
        const double current = samples[index].time;
        if (!std::isfinite(current) || current <= previous) {
            return false;
        }
        previous = current;
    }
    return true;
}

WaveDisplayData buildDisplayData(const WaveSnapshot& snapshot, double sampleFrequencyHz) {
    WaveDisplayData data{};
    data.channels.resize(snapshot.channels.size());

    bool hasScriptTime = false;
    for (const auto& channel : snapshot.channels) {
        if (channel.samples == nullptr || channel.totalSamples == 0) {
            continue;
        }
        const std::vector<WaveSample> samples(channel.samples, channel.samples + channel.totalSamples);
        if (scriptTimeUsable(samples)) {
            hasScriptTime = true;
            break;
        }
    }

    if (sampleFrequencyHz > 0.0 && std::isfinite(sampleFrequencyHz)) {
        data.axisSource = WaveTimeAxisSource::SampleFrequency;
        data.timeUnit = "s";
    } else if (hasScriptTime) {
        data.axisSource = WaveTimeAxisSource::ScriptTime;
        data.timeUnit = snapshot.config.timeUnit.empty() ? "s" : snapshot.config.timeUnit;
    } else {
        data.axisSource = WaveTimeAxisSource::SampleIndex;
        data.timeUnit = "sample";
    }

    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        const auto& channel = snapshot.channels[channelIndex];
        auto& displayChannel = data.channels[channelIndex];
        auto& display = displayChannel.samples;
        if (channel.samples == nullptr || channel.totalSamples == 0) {
            continue;
        }
        display.reserve(channel.totalSamples);
        displayChannel.actualValues.reserve(channel.totalSamples);
        for (std::size_t sampleIndex = 0; sampleIndex < channel.totalSamples; ++sampleIndex) {
            const auto& source = channel.samples[sampleIndex];
            double time = static_cast<double>(sampleIndex);
            if (data.axisSource == WaveTimeAxisSource::SampleFrequency) {
                time = static_cast<double>(sampleIndex) / sampleFrequencyHz;
            } else if (data.axisSource == WaveTimeAxisSource::ScriptTime) {
                time = source.time;
            }
            const ChannelSpec spec{
                .label = channel.label,
                .unit = channel.unit,
                .ratio = channel.ratio,
                .scale = channel.scale,
                .offset = channel.offset,
                .color = channel.color,
            };
            display.push_back({
                .time = time,
                .value = applyChannelDisplayTransform(source.value, spec, snapshot.config.displayFormula),
            });
            displayChannel.actualValues.push_back(applyChannelActualValue(source.value, spec));
        }
    }
    return data;
}

std::optional<CursorReadout> findNearestDisplayByTime(const WaveDisplayData& displayData,
                                                      std::size_t channelIndex,
                                                      double time,
                                                      double maxTimeDistance) {
    if (channelIndex >= displayData.channels.size()) {
        return std::nullopt;
    }
    const auto& channel = displayData.channels[channelIndex];
    const auto& samples = channel.samples;
    if (samples.empty() || !std::isfinite(time) || !std::isfinite(maxTimeDistance) || maxTimeDistance < 0.0) {
        return std::nullopt;
    }

    auto lower = std::lower_bound(samples.begin(), samples.end(), time, [](const WaveSample& sample, double value) {
        return sample.time < value;
    });
    std::optional<CursorReadout> best;
    auto consider = [&](std::vector<WaveSample>::const_iterator iterator) {
        if (iterator == samples.end()) {
            return;
        }
        const double distance = std::abs(iterator->time - time);
        if (distance > maxTimeDistance) {
            return;
        }
        if (!best.has_value() || distance < std::abs(best->time - time)) {
            const std::size_t sampleIndex = static_cast<std::size_t>(std::distance(samples.begin(), iterator));
            best = CursorReadout{
                .valid = true,
                .channelIndex = channelIndex,
                .sampleIndex = sampleIndex,
                .time = iterator->time,
                .value = resolveActualValue(channel, sampleIndex, iterator->value),
                .displayValue = iterator->value,
            };
        }
    };
    consider(lower);
    if (lower != samples.begin()) {
        consider(std::prev(lower));
    }
    return best;
}

std::optional<CursorReadout> findNearestDisplayByTimeAcrossChannels(const WaveDisplayData& displayData,
                                                                    double time,
                                                                    double maxTimeDistance) {
    std::optional<CursorReadout> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t channelIndex = 0; channelIndex < displayData.channels.size(); ++channelIndex) {
        const auto candidate = findNearestDisplayByTime(displayData, channelIndex, time, maxTimeDistance);
        if (!candidate.has_value()) {
            continue;
        }
        const double distance = std::abs(candidate->time - time);
        if (!best.has_value() || distance < bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return best;
}

std::optional<CursorReadout> findNearestDisplayPoint(const WaveDisplayData& displayData,
                                                     double time,
                                                     double value,
                                                     double maxTimeDistance,
                                                     double maxValueDistance) {
    std::optional<CursorReadout> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (std::size_t channelIndex = 0; channelIndex < displayData.channels.size(); ++channelIndex) {
        const auto nearest = findNearestDisplayByTime(displayData, channelIndex, time, maxTimeDistance);
        if (!nearest.has_value()) {
            continue;
        }
        const double valueDistance = std::abs(nearest->displayValue - value);
        if (valueDistance > maxValueDistance) {
            continue;
        }
        const double timeDistance = std::abs(nearest->time - time);
        const double score = timeDistance * timeDistance + valueDistance * valueDistance;
        if (score < bestScore) {
            bestScore = score;
            best = nearest;
        }
    }
    return best;
}

WaveDataBounds computeDisplayBounds(const WaveDisplayData& data, double fallbackStep) {
    WaveDataBounds bounds{};
    bounds.minTime = std::numeric_limits<double>::infinity();
    bounds.maxTime = -std::numeric_limits<double>::infinity();
    bounds.minValue = std::numeric_limits<double>::infinity();
    bounds.maxValue = -std::numeric_limits<double>::infinity();
    bounds.minStep = (std::max)(fallbackStep, kEpsilon);

    for (const auto& channel : data.channels) {
        for (std::size_t index = 0; index < channel.samples.size(); ++index) {
            const auto& sample = channel.samples[index];
            if (!std::isfinite(sample.time) || !std::isfinite(sample.value)) {
                continue;
            }
            bounds.minTime = (std::min)(bounds.minTime, sample.time);
            bounds.maxTime = (std::max)(bounds.maxTime, sample.time);
            bounds.minValue = (std::min)(bounds.minValue, sample.value);
            bounds.maxValue = (std::max)(bounds.maxValue, sample.value);
            if (index > 0) {
                const double step = sample.time - channel.samples[index - 1].time;
                if (step > kEpsilon) {
                    bounds.minStep = (std::min)(bounds.minStep, step);
                }
            }
            bounds.valid = true;
        }
    }

    if (!bounds.valid) {
        bounds.minTime = 0.0;
        bounds.maxTime = 1.0;
        bounds.minValue = -1.0;
        bounds.maxValue = 1.0;
        return bounds;
    }
    if (std::abs(bounds.maxTime - bounds.minTime) <= kEpsilon) {
        bounds.maxTime = bounds.minTime + bounds.minStep;
    }
    if (std::abs(bounds.maxValue - bounds.minValue) <= kEpsilon) {
        bounds.minValue -= 1.0;
        bounds.maxValue += 1.0;
    }
    return bounds;
}

WaveViewport normalizeOverviewViewport(const WaveViewport& viewport,
                                       const WaveDataBounds& bounds,
                                       double minTimeWidth) {
    WaveViewport next = viewport;
    if (next.maxTime < next.minTime) {
        std::swap(next.minTime, next.maxTime);
    }

    const double fallbackMin = bounds.valid ? bounds.minTime : 0.0;
    const double fallbackMax = bounds.valid ? bounds.maxTime : fallbackMin + clampPositiveWidth(minTimeWidth, 1e-6);
    if (!std::isfinite(next.minTime)) {
        next.minTime = fallbackMin;
    }
    if (!std::isfinite(next.maxTime)) {
        next.maxTime = fallbackMax;
    }

    const double minWidth = clampPositiveWidth(minTimeWidth, bounds.minStep);
    double width = clampPositiveWidth(next.maxTime - next.minTime, minWidth);
    const double center = std::isfinite(0.5 * (next.minTime + next.maxTime))
        ? 0.5 * (next.minTime + next.maxTime)
        : 0.5 * (fallbackMin + fallbackMax);
    next.minTime = center - 0.5 * width;
    next.maxTime = next.minTime + width;

    // 概览框只约束时间窗：反向拖动、越界和过窄窗口都在这里统一收口。
    clampTimeRange(next.minTime, next.maxTime, bounds);
    width = next.maxTime - next.minTime;
    if (width < minWidth && bounds.valid) {
        const double dataWidth = (std::max)(bounds.maxTime - bounds.minTime, kEpsilon);
        width = (std::min)(minWidth, dataWidth);
        next.minTime = (std::clamp)(center - 0.5 * width, bounds.minTime, bounds.maxTime - width);
        next.maxTime = next.minTime + width;
    }
    return next;
}

WaveViewport zoomViewport(const WaveViewport& viewport,
                          WaveZoomMode mode,
                          double wheelDelta,
                          double centerTime,
                          double centerValue,
                          const WaveDataBounds& bounds,
                          double minTimeWidth,
                          bool clampTimeToBounds) {
    if (std::abs(wheelDelta) <= kEpsilon) {
        return viewport;
    }

    const double zoomFactor = std::pow(0.85, wheelDelta);
    WaveViewport next = viewport;
    const double timeWidth = clampPositiveWidth(viewport.maxTime - viewport.minTime, minTimeWidth);
    const double valueHeight = clampPositiveWidth(viewport.maxValue - viewport.minValue, 1e-6);

    if (!std::isfinite(centerTime)) {
        centerTime = 0.5 * (viewport.minTime + viewport.maxTime);
    }
    if (!std::isfinite(centerValue)) {
        centerValue = 0.5 * (viewport.minValue + viewport.maxValue);
    }

    if (mode == WaveZoomMode::XOnly || mode == WaveZoomMode::XY) {
        const double newWidth = clampPositiveWidth(timeWidth * zoomFactor, minTimeWidth);
        const double ratio = (std::clamp)((centerTime - viewport.minTime) / timeWidth, 0.0, 1.0);
        next.minTime = centerTime - ratio * newWidth;
        next.maxTime = next.minTime + newWidth;
    }
    if (mode == WaveZoomMode::YOnly || mode == WaveZoomMode::XY) {
        const double newHeight = clampPositiveWidth(valueHeight * zoomFactor, 1e-6);
        const double ratio = (std::clamp)((centerValue - viewport.minValue) / valueHeight, 0.0, 1.0);
        next.minValue = centerValue - ratio * newHeight;
        next.maxValue = next.minValue + newHeight;
    }

    if (clampTimeToBounds) {
        clampTimeRange(next.minTime, next.maxTime, bounds);
    }
    if (mode == WaveZoomMode::YOnly || mode == WaveZoomMode::XY) {
        clampValueRange(next.minValue, next.maxValue, bounds);
    }
    return next;
}

CursorIntervalText makeCursorIntervalText(const CursorReadout& left,
                                          const CursorReadout& right,
                                          WaveTimeAxisSource axisSource,
                                          std::string_view timeUnit) {
    if (!left.valid || !right.valid) {
        return {};
    }
    const double delta = std::abs(right.time - left.time);
    if (!std::isfinite(delta)) {
        return {};
    }
    CursorIntervalText text{
        .valid = true,
        .showFrequency = axisSource != WaveTimeAxisSource::SampleIndex && delta > kEpsilon,
        .delta = delta,
        .frequencyHz = delta > kEpsilon ? 1.0 / delta : 0.0,
        .deltaUnit = std::string(timeUnit.empty() ? "sample" : timeUnit),
    };
    if (axisSource == WaveTimeAxisSource::SampleIndex) {
        text.deltaUnit = "sample";
    }
    return text;
}

std::optional<CursorReadout> findStrongestEdgeNearTime(const WaveDisplayData& displayData,
                                                       std::size_t channelIndex,
                                                       double centerTime,
                                                       double maxTimeDistance) {
    if (channelIndex >= displayData.channels.size()) {
        return std::nullopt;
    }
    const auto& channel = displayData.channels[channelIndex];
    const auto& samples = channel.samples;
    if (samples.size() < 2 || !std::isfinite(centerTime) || !std::isfinite(maxTimeDistance) || maxTimeDistance <= 0.0) {
        return std::nullopt;
    }

    std::optional<CursorReadout> best;
    double bestScore = 0.0;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 1; index < samples.size(); ++index) {
        const auto& left = samples[index - 1];
        const auto& right = samples[index];
        if (!std::isfinite(left.time) || !std::isfinite(left.value) || !std::isfinite(right.time) || !std::isfinite(right.value)) {
            continue;
        }
        const double edgeTime = 0.5 * (left.time + right.time);
        const double distance = std::abs(edgeTime - centerTime);
        if (distance > maxTimeDistance) {
            continue;
        }
        // 边沿评分只关注相邻样本的幅值突变，平坦片段不参与吸附，避免游标无意义跳动。
        const double score = std::abs(right.value - left.value);
        if (score <= kEpsilon) {
            continue;
        }
        if (score > bestScore || (std::abs(score - bestScore) <= kEpsilon && distance < bestDistance)) {
            bestScore = score;
            bestDistance = distance;
            const double displayValue = 0.5 * (left.value + right.value);
            // 边沿时间位于两个样本之间：锚点按显示几何放在中点，读数文本取跳变右侧样本真实值。
            best = CursorReadout{
                .valid = true,
                .channelIndex = channelIndex,
                .sampleIndex = index,
                .time = edgeTime,
                .value = resolveActualValue(channel, index, right.value),
                .displayValue = displayValue,
            };
        }
    }
    return best;
}

std::optional<CursorReadout> findLocalExtremeNearTime(const WaveDisplayData& displayData,
                                                      std::size_t channelIndex,
                                                      double centerTime,
                                                      double maxTimeDistance,
                                                      WaveExtremeKind kind) {
    if (channelIndex >= displayData.channels.size()) {
        return std::nullopt;
    }
    const auto& channel = displayData.channels[channelIndex];
    const auto& samples = channel.samples;
    if (samples.size() < 3 || !std::isfinite(centerTime) || !std::isfinite(maxTimeDistance) || maxTimeDistance <= 0.0) {
        return std::nullopt;
    }

    std::optional<CursorReadout> best;
    double bestDistance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 1; index + 1 < samples.size(); ++index) {
        const auto& previous = samples[index - 1];
        const auto& current = samples[index];
        const auto& next = samples[index + 1];
        if (!std::isfinite(previous.value) || !std::isfinite(current.time) || !std::isfinite(current.value)
            || !std::isfinite(next.value)) {
            continue;
        }
        const double distance = std::abs(current.time - centerTime);
        if (distance > maxTimeDistance) {
            continue;
        }
        // 极值吸附只接受局部峰/谷，防止普通斜坡点在顶部/底部区域误触发。
        const bool isMaximum = current.value > previous.value && current.value > next.value;
        const bool isMinimum = current.value < previous.value && current.value < next.value;
        if ((kind == WaveExtremeKind::Maximum && !isMaximum) || (kind == WaveExtremeKind::Minimum && !isMinimum)) {
            continue;
        }
        const bool betterValue = !best.has_value()
            || (kind == WaveExtremeKind::Maximum ? current.value > best->displayValue : current.value < best->displayValue);
        if (betterValue || (best.has_value() && std::abs(current.value - best->displayValue) <= kEpsilon && distance < bestDistance)) {
            bestDistance = distance;
            best = CursorReadout{
                .valid = true,
                .channelIndex = channelIndex,
                .sampleIndex = index,
                .time = current.time,
                .value = resolveActualValue(channel, index, current.value),
                .displayValue = current.value,
            };
        }
    }
    return best;
}

double applyCursorDragSnap(double dragTime, const std::optional<CursorReadout>& smartSnap) {
    if (smartSnap.has_value() && std::isfinite(smartSnap->time)) {
        // 拖动时智能吸附结果必须覆盖鼠标时间，否则 UI 游标线会继续跟随鼠标移动。
        return smartSnap->time;
    }
    return dragTime;
}

void lockCursorInterval(double movedTime, double& pairedTime, double lockedInterval, bool movedLeftCursor) {
    if (!std::isfinite(lockedInterval) || lockedInterval <= 0.0) {
        return;
    }
    pairedTime = movedLeftCursor ? movedTime + lockedInterval : movedTime - lockedInterval;
}

WaveViewport moveViewportByDelta(const WaveViewport& viewport,
                                 double deltaTime,
                                 const WaveDataBounds& bounds,
                                 double minTimeWidth) {
    WaveViewport moved = viewport;
    if (!std::isfinite(deltaTime)) {
        deltaTime = 0.0;
    }
    moved.minTime += deltaTime;
    moved.maxTime += deltaTime;
    return normalizeOverviewViewport(moved, bounds, minTimeWidth);
}

double cursorTimeInViewport(const WaveViewport& viewport, double ratio) {
    const double minTime = (std::min)(viewport.minTime, viewport.maxTime);
    const double maxTime = (std::max)(viewport.minTime, viewport.maxTime);
    if (!std::isfinite(minTime) || !std::isfinite(maxTime)) {
        return 0.0;
    }
    // 快捷定位只在当前可视窗口内布点，ratio 先夹紧，避免异常输入把游标推到窗外。
    const double clampedRatio = (std::clamp)(std::isfinite(ratio) ? ratio : 0.5, 0.0, 1.0);
    return minTime + (maxTime - minTime) * clampedRatio;
}

} // namespace protoscope::plot
