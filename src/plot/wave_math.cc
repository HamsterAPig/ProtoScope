#include "protoscope/plot/wave_math.hpp"

#include "protoscope/plot/wave_state.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <utility>

namespace protoscope::plot {

namespace {

    constexpr double kEpsilon = 1e-12;

    std::string trimCopy(std::string_view text)
    {
        const auto begin = text.find_first_not_of(" \t\r\n");
        if (begin == std::string_view::npos) {
            return {};
        }
        const auto end = text.find_last_not_of(" \t\r\n");
        return std::string{text.substr(begin, end - begin + 1)};
    }

    double clampPositiveWidth(double width, double minWidth)
    {
        return (std::max)(width, (std::max)(minWidth, kEpsilon));
    }

    double adjacentEngineeringValue(double value, int direction)
    {
        if (!std::isfinite(value) || value <= 0.0 || direction == 0) {
            return value;
        }

        const double exponent = std::floor(std::log10(value));
        double candidate = direction > 0 ? std::numeric_limits<double>::infinity() : 0.0;
        constexpr double kMantissas[]{1.0, 2.0, 5.0};
        for (int exponentOffset = -2; exponentOffset <= 2; ++exponentOffset) {
            const double power = std::pow(10.0, exponent + static_cast<double>(exponentOffset));
            for (const double mantissa : kMantissas) {
                const double engineeringValue = mantissa * power;
                const double tolerance = (std::max)(std::abs(value), engineeringValue) * 1e-12;
                if (direction > 0 && engineeringValue > value + tolerance) {
                    candidate = (std::min)(candidate, engineeringValue);
                } else if (direction < 0 && engineeringValue < value - tolerance) {
                    candidate = (std::max)(candidate, engineeringValue);
                }
            }
        }
        if (direction > 0 && std::isfinite(candidate)) {
            return candidate;
        }
        if (direction < 0 && candidate > 0.0) {
            return candidate;
        }
        return value;
    }

    std::size_t acceleratedEngineeringStepCount(WaveChannelScaleWheelAcceleration acceleration,
                                                std::size_t continuousStepCount)
    {
        switch (acceleration) {
            case WaveChannelScaleWheelAcceleration::None:
                return 1U;
            case WaveChannelScaleWheelAcceleration::Linear:
                return (std::min)(std::size_t{4}, std::size_t{1} + (continuousStepCount - 1U) / 2U);
            case WaveChannelScaleWheelAcceleration::Log:
                if (continuousStepCount >= 8U) {
                    return 3U;
                }
                return continuousStepCount >= 4U ? 2U : 1U;
        }
        return 1U;
    }

    double resolveActualValue(const WaveDisplayChannel& channel, std::size_t sampleIndex, double fallbackValue)
    {
        if (sampleIndex < channel.actualValues.size()) {
            return channel.actualValues[sampleIndex];
        }
        return fallbackValue;
    }

    CursorReadout makeCursorReadout(const WaveDisplayChannel& channel,
                                    std::size_t channelIndex,
                                    std::size_t sampleIndex)
    {
        const auto& sample = channel.samples[sampleIndex];
        return CursorReadout{
            .valid = true,
            .channelIndex = channelIndex,
            .sampleIndex = sampleIndex,
            .time = sample.time,
            .value = resolveActualValue(channel, sampleIndex, sample.value),
            .displayValue = sample.value,
        };
    }

    bool extremeValueBetter(WaveExtremeKind kind, double candidateValue, double bestValue)
    {
        return kind == WaveExtremeKind::Maximum ? candidateValue > bestValue : candidateValue < bestValue;
    }

    void clampTimeRange(double& minTime, double& maxTime, const WaveDataBounds& bounds)
    {
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

    void clampValueRange(double& minValue, double& maxValue, const WaveDataBounds& bounds)
    {
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

FrequencyParseResult parseSampleFrequencyText(std::string_view text)
{
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
                                float fixedContentHeight)
{
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
        const float overviewRatio =
            safeMinOverview + safeMinMain > 0.0F ? safeMinOverview / (safeMinOverview + safeMinMain) : 0.35F;
        result.overviewHeight = std::floor(availableHeight * overviewRatio);
        result.mainHeight = availableHeight - result.overviewHeight;
        return result;
    }

    result.overviewHeight = (std::clamp)(requestedOverviewHeight, safeMinOverview, availableHeight - safeMinMain);
    result.mainHeight = availableHeight - result.overviewHeight;
    return result;
}

float solveSplitWavePlotHeight(std::size_t visibleChannelCount,
                               float availableHeight,
                               float rowSpacingY,
                               float preferredMinPlotHeight,
                               std::size_t maxRowsWithoutScroll)
{
    if (visibleChannelCount == 0 || maxRowsWithoutScroll == 0) {
        return 0.0F;
    }

    const std::size_t fittedRows = (std::min)(visibleChannelCount, maxRowsWithoutScroll);
    const float safeAvailableHeight = (std::max)(availableHeight, 0.0F);
    const float safeSpacing = (std::max)(rowSpacingY, 0.0F);
    const float totalSpacing = safeSpacing * static_cast<float>(fittedRows - 1U);
    const float fittedHeight = (std::max)(0.0F, safeAvailableHeight - totalSpacing) / static_cast<float>(fittedRows);

    // 分屏 4 行以内优先完整塞进当前 child，超过 4 行才保留首屏高度并交给滚动条处理。
    if (visibleChannelCount <= maxRowsWithoutScroll) {
        return fittedHeight;
    }
    return (std::max)(fittedHeight, (std::max)(preferredMinPlotHeight, 0.0F));
}

bool scriptTimeUsable(const std::vector<WaveSample>& samples)
{
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

std::pair<std::size_t, std::size_t> displaySampleRange(const ChannelView& channel)
{
    if (channel.samples == nullptr || channel.totalSamples == 0) {
        return {0, 0};
    }
    const std::size_t begin = (std::min)(channel.visibleBegin, channel.totalSamples);
    std::size_t end = (std::min)(channel.visibleEnd, channel.totalSamples);
    if (end < begin) {
        end = begin;
    }
    return {begin, end};
}

bool scriptTimeUsable(const ChannelView& channel, std::size_t begin, std::size_t end)
{
    if (channel.samples == nullptr || begin >= end) {
        return false;
    }
    double previous = channel.samples[begin].time;
    if (!std::isfinite(previous)) {
        return false;
    }
    for (std::size_t index = begin + 1; index < end; ++index) {
        const double current = channel.samples[index].time;
        if (!std::isfinite(current) || current <= previous) {
            return false;
        }
        previous = current;
    }
    return true;
}

namespace {

    void resetDisplayDataChannels(WaveDisplayData& data, std::size_t channelCount)
    {
        data.channels.resize(channelCount);
        for (auto& channel : data.channels) {
            channel.samples.clear();
            channel.actualValues.clear();
        }
    }

    bool hasUsableScriptTime(const WaveSnapshot& snapshot)
    {
        for (const auto& channel : snapshot.channels) {
            const auto [begin, end] = displaySampleRange(channel);
            if (scriptTimeUsable(channel, begin, end)) {
                return true;
            }
        }
        return false;
    }

    void resolveDisplayTimeAxis(const WaveSnapshot& snapshot, double sampleFrequencyHz, WaveDisplayData& data)
    {
        if (sampleFrequencyHz > 0.0 && std::isfinite(sampleFrequencyHz)) {
            data.axisSource = WaveTimeAxisSource::SampleFrequency;
            data.timeUnit = "s";
        } else if (hasUsableScriptTime(snapshot)) {
            data.axisSource = WaveTimeAxisSource::ScriptTime;
            data.timeUnit = snapshot.config.timeUnit.empty() ? "s" : snapshot.config.timeUnit;
        } else {
            data.axisSource = WaveTimeAxisSource::SampleIndex;
            data.timeUnit = "sample";
        }
    }

    double resolveDisplaySampleTime(const WaveSample& source,
                                    std::size_t sampleIndexOffset,
                                    std::size_t sampleIndex,
                                    double sampleFrequencyHz,
                                    WaveTimeAxisSource axisSource)
    {
        const std::size_t globalSampleIndex = sampleIndexOffset + sampleIndex;
        if (axisSource == WaveTimeAxisSource::SampleFrequency) {
            return static_cast<double>(globalSampleIndex) / sampleFrequencyHz;
        }
        if (axisSource == WaveTimeAxisSource::ScriptTime) {
            return source.time;
        }
        return static_cast<double>(globalSampleIndex);
    }

    void fillDisplayChannel(const ChannelView& channel,
                            const ViewConfig& config,
                            double sampleFrequencyHz,
                            WaveTimeAxisSource axisSource,
                            WaveDisplayChannel& displayChannel)
    {
        const auto [begin, end] = displaySampleRange(channel);
        if (begin >= end) {
            return;
        }

        const std::size_t visibleSamples = end - begin;
        auto& display = displayChannel.samples;
        display.resize(visibleSamples);
        displayChannel.actualValues.resize(visibleSamples);
        const bool offsetThenScale = config.displayFormula == WaveDisplayFormula::OffsetThenScale;
        for (std::size_t sampleIndex = begin; sampleIndex < end; ++sampleIndex) {
            const auto& source = channel.samples[sampleIndex];
            const double actualValue = source.value * channel.ratio;
            const double displayValue = offsetThenScale ? (actualValue + channel.offset) * channel.scale
                                                        : actualValue * channel.scale + channel.offset;
            const auto outputIndex = sampleIndex - begin;
            display[outputIndex] = {
                .time = resolveDisplaySampleTime(
                    source, channel.sampleIndexOffset, sampleIndex, sampleFrequencyHz, axisSource),
                .value = displayValue,
            };
            displayChannel.actualValues[outputIndex] = actualValue;
        }
    }

} // namespace

void buildDisplayDataInto(const WaveSnapshot& snapshot, double sampleFrequencyHz, WaveDisplayData& data)
{
    resetDisplayDataChannels(data, snapshot.channels.size());
    resolveDisplayTimeAxis(snapshot, sampleFrequencyHz, data);

    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        fillDisplayChannel(snapshot.channels[channelIndex],
                           snapshot.config,
                           sampleFrequencyHz,
                           data.axisSource,
                           data.channels[channelIndex]);
    }
}

WaveDisplayData buildDisplayData(const WaveSnapshot& snapshot, double sampleFrequencyHz)
{
    WaveDisplayData data{};
    buildDisplayDataInto(snapshot, sampleFrequencyHz, data);
    return data;
}

void applySampleFrequencyVisibleRange(WaveSnapshot& snapshot, double minTime, double maxTime, double sampleFrequencyHz)
{
    if (sampleFrequencyHz <= 0.0 || !std::isfinite(sampleFrequencyHz)) {
        return;
    }
    if (maxTime < minTime) {
        std::swap(minTime, maxTime);
    }
    const double clampedMinTime = (std::max)(0.0, minTime);
    const double clampedMaxTime = (std::max)(clampedMinTime, maxTime);
    const auto beginIndex = static_cast<std::size_t>((std::ceil)(clampedMinTime * sampleFrequencyHz));
    const auto endIndex = static_cast<std::size_t>((std::floor)(clampedMaxTime * sampleFrequencyHz)) + 1U;
    for (auto& channel : snapshot.channels) {
        const std::size_t begin = beginIndex <= channel.sampleIndexOffset ? 0 : beginIndex - channel.sampleIndexOffset;
        const std::size_t end = endIndex <= channel.sampleIndexOffset ? 0 : endIndex - channel.sampleIndexOffset;
        channel.visibleBegin = (std::min)(begin, channel.totalSamples);
        channel.visibleEnd = (std::min)(end, channel.totalSamples);
        if (channel.visibleEnd < channel.visibleBegin) {
            channel.visibleEnd = channel.visibleBegin;
        }
        // 核心流程：采样频率时间轴也保留左右邻接样本，避免单点视口里折线被上游裁成孤点。
        if (channel.visibleBegin > 0) {
            --channel.visibleBegin;
        }
        if (channel.visibleEnd < channel.totalSamples) {
            ++channel.visibleEnd;
        }
    }
}

std::optional<CursorReadout> findNearestDisplayByTime(const WaveDisplayData& displayData,
                                                      std::size_t channelIndex,
                                                      double time,
                                                      double maxTimeDistance)
{
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
                                                                    double maxTimeDistance)
{
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

std::optional<CursorReadout> findNearestDisplayPoint(
    const WaveDisplayData& displayData, double time, double value, double maxTimeDistance, double maxValueDistance)
{
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

std::optional<CursorReadout> findNearestDisplayPointInChannels(const WaveDisplayData& displayData,
                                                               const std::vector<std::size_t>& channelIndices,
                                                               double time,
                                                               double value,
                                                               double maxTimeDistance,
                                                               double maxValueDistance)
{
    std::optional<CursorReadout> best;
    double bestScore = std::numeric_limits<double>::infinity();
    for (const std::size_t channelIndex : channelIndices) {
        if (channelIndex >= displayData.channels.size()) {
            continue;
        }
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

void includeSampleInBounds(WaveDataBounds& bounds, const std::vector<WaveSample>& samples, std::size_t index)
{
    const auto& sample = samples[index];
    if (!std::isfinite(sample.time) || !std::isfinite(sample.value)) {
        return;
    }
    bounds.minTime = (std::min)(bounds.minTime, sample.time);
    bounds.maxTime = (std::max)(bounds.maxTime, sample.time);
    bounds.minValue = (std::min)(bounds.minValue, sample.value);
    bounds.maxValue = (std::max)(bounds.maxValue, sample.value);
    if (index > 0) {
        const double step = sample.time - samples[index - 1].time;
        if (step > kEpsilon) {
            bounds.minStep = (std::min)(bounds.minStep, step);
        }
    }
    bounds.valid = true;
}

void finalizeDisplayBounds(WaveDataBounds& bounds)
{
    if (!bounds.valid) {
        bounds.minTime = 0.0;
        bounds.maxTime = 1.0;
        bounds.minValue = -1.0;
        bounds.maxValue = 1.0;
        return;
    }
    if (std::abs(bounds.maxTime - bounds.minTime) <= kEpsilon) {
        bounds.maxTime = bounds.minTime + bounds.minStep;
    }
    if (std::abs(bounds.maxValue - bounds.minValue) <= kEpsilon) {
        bounds.minValue -= 1.0;
        bounds.maxValue += 1.0;
    }
}

WaveDataBounds computeDisplayBounds(const WaveDisplayData& data, double fallbackStep)
{
    WaveDataBounds bounds{};
    bounds.minTime = std::numeric_limits<double>::infinity();
    bounds.maxTime = -std::numeric_limits<double>::infinity();
    bounds.minValue = std::numeric_limits<double>::infinity();
    bounds.maxValue = -std::numeric_limits<double>::infinity();
    bounds.minStep = (std::max)(fallbackStep, kEpsilon);

    for (const auto& channel : data.channels) {
        for (std::size_t index = 0; index < channel.samples.size(); ++index) {
            includeSampleInBounds(bounds, channel.samples, index);
        }
    }

    finalizeDisplayBounds(bounds);
    return bounds;
}

WaveDataBounds computeDisplayBoundsForChannels(const WaveDisplayData& data,
                                               const std::vector<std::size_t>& channelIndices,
                                               double fallbackStep)
{
    WaveDataBounds bounds{};
    bounds.minTime = std::numeric_limits<double>::infinity();
    bounds.maxTime = -std::numeric_limits<double>::infinity();
    bounds.minValue = std::numeric_limits<double>::infinity();
    bounds.maxValue = -std::numeric_limits<double>::infinity();
    bounds.minStep = (std::max)(fallbackStep, kEpsilon);

    for (const std::size_t channelIndex : channelIndices) {
        if (channelIndex >= data.channels.size()) {
            continue;
        }
        const auto& channel = data.channels[channelIndex];
        for (std::size_t sampleIndex = 0; sampleIndex < channel.samples.size(); ++sampleIndex) {
            includeSampleInBounds(bounds, channel.samples, sampleIndex);
        }
    }

    finalizeDisplayBounds(bounds);
    return bounds;
}

const WaveDataBounds& selectXAxisDoubleClickBounds(const WaveXAxisDoubleClickAction action,
                                                   const WaveDataBounds& visibleWindowBounds,
                                                   const WaveDataBounds& fullHistoryBounds)
{
    // 核心流程：X 轴双击默认回到当前内存保留的全历史；全历史无效时保留旧的当前窗口 fallback。
    if (action == WaveXAxisDoubleClickAction::FitFullHistory && fullHistoryBounds.valid) {
        return fullHistoryBounds;
    }
    return visibleWindowBounds;
}

double waveDisplayValuePerDivision(double minValue, double maxValue)
{
    return std::abs(maxValue - minValue) / static_cast<double>(kWaveGridMajorYDivisions);
}

std::optional<double> waveActualValuePerDivision(double minValue, double maxValue, double scale)
{
    if (!std::isfinite(scale) || std::abs(scale) <= kEpsilon) {
        return std::nullopt;
    }
    const double displayValuePerDivision = waveDisplayValuePerDivision(minValue, maxValue);
    if (!std::isfinite(displayValuePerDivision)) {
        return std::nullopt;
    }
    return displayValuePerDivision / std::abs(scale);
}

std::optional<double> waveScaleForActualValuePerDivision(double minValue,
                                                         double maxValue,
                                                         double targetValuePerDivision,
                                                         double currentScale)
{
    if (!std::isfinite(targetValuePerDivision) || targetValuePerDivision <= 0.0) {
        return std::nullopt;
    }
    const double displayValuePerDivision = waveDisplayValuePerDivision(minValue, maxValue);
    if (!std::isfinite(displayValuePerDivision) || displayValuePerDivision <= kEpsilon) {
        return std::nullopt;
    }
    const double sign = std::signbit(currentScale) ? -1.0 : 1.0;
    const double magnitude = displayValuePerDivision / targetValuePerDivision;
    if (!std::isfinite(magnitude) || magnitude <= kEpsilon) {
        return std::nullopt;
    }
    return sign * magnitude;
}

std::optional<double> parseWaveScaleFromActualValuePerDivision(double minValue,
                                                               double maxValue,
                                                               std::string_view text,
                                                               double currentScale)
{
    const std::string trimmed = trimCopy(text);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    char* parseEnd = nullptr;
    errno = 0;
    const double targetValuePerDivision = std::strtod(trimmed.c_str(), &parseEnd);
    if (parseEnd == trimmed.c_str() || *parseEnd != '\0' || errno == ERANGE) {
        return std::nullopt;
    }
    return waveScaleForActualValuePerDivision(
        minValue, maxValue, targetValuePerDivision, currentScale);
}

std::optional<double> waveChannelValuePerDivision(double displayValuePerDivision,
                                                  const ChannelSpec& spec,
                                                  WaveDisplayFormula formula,
                                                  WaveGridDivisionReadoutMode mode)
{
    // 核心逻辑：每格读数是差值换算，offset 在两种显示公式中都会抵消。
    static_cast<void>(formula);
    switch (mode) {
        case WaveGridDivisionReadoutMode::DisplayValue:
            return displayValuePerDivision;
        case WaveGridDivisionReadoutMode::ActualValue:
            if (std::abs(spec.scale) <= kEpsilon) {
                return std::nullopt;
            }
            return displayValuePerDivision / std::abs(spec.scale);
        case WaveGridDivisionReadoutMode::RawValue: {
            const double displayPerRaw = spec.scale * spec.ratio;
            if (std::abs(displayPerRaw) <= kEpsilon) {
                return std::nullopt;
            }
            return displayValuePerDivision / std::abs(displayPerRaw);
        }
    }
    return std::nullopt;
}

WaveChannelScaleWheelResult stepWaveChannelValuePerDivision(double currentValuePerDivision,
                                                            double wheelDelta,
                                                            std::size_t channelIndex,
                                                            double eventTimeSec,
                                                            WaveChannelScaleWheelAcceleration acceleration,
                                                            WaveChannelScaleWheelState& state)
{
    WaveChannelScaleWheelResult result{.valuePerDivision = currentValuePerDivision};
    if (!std::isfinite(currentValuePerDivision) || currentValuePerDivision <= 0.0 || !std::isfinite(wheelDelta) ||
        wheelDelta == 0.0) {
        return result;
    }

    const int direction = wheelDelta > 0.0 ? 1 : -1;
    constexpr double kContinuationTimeoutSec = 0.250;
    const bool timedOut = !std::isfinite(eventTimeSec) || eventTimeSec < state.lastEventTimeSec ||
                          eventTimeSec - state.lastEventTimeSec > kContinuationTimeoutSec;
    if (state.channelIndex != channelIndex || state.direction != direction || timedOut) {
        state.channelIndex = channelIndex;
        state.direction = direction;
        state.continuousStepCount = 0;
        state.fractionalDelta = 0.0;
    }
    state.lastEventTimeSec = std::isfinite(eventTimeSec) ? eventTimeSec : 0.0;
    state.fractionalDelta += wheelDelta;

    // 核心逻辑：触控板小数滚轮先累计到完整刻度，再按 1-2-5 工程刻度推进。
    while (std::abs(state.fractionalDelta) + kEpsilon >= 1.0) {
        ++state.continuousStepCount;
        const std::size_t engineeringSteps =
            acceleratedEngineeringStepCount(acceleration, state.continuousStepCount);
        const int valueDirection = direction > 0 ? -1 : 1;
        for (std::size_t step = 0; step < engineeringSteps; ++step) {
            result.valuePerDivision = adjacentEngineeringValue(result.valuePerDivision, valueDirection);
        }
        state.fractionalDelta -= static_cast<double>(direction);
        ++result.appliedNotches;
    }
    return result;
}

WaveViewport normalizeOverviewViewport(const WaveViewport& viewport, const WaveDataBounds& bounds, double minTimeWidth)
{
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
    const double center = std::isfinite(0.5 * (next.minTime + next.maxTime)) ? 0.5 * (next.minTime + next.maxTime)
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
                          bool clampTimeToBounds,
                          bool fineAdjustmentEnabled)
{
    if (std::abs(wheelDelta) <= kEpsilon) {
        return viewport;
    }

    const double zoomFactor =
        fineAdjustmentEnabled ? std::pow(1.01, -wheelDelta) : std::pow(0.85, wheelDelta);
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
                                          std::string_view timeUnit)
{
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
                                                       double maxTimeDistance)
{
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
        if (!std::isfinite(left.time) || !std::isfinite(left.value) || !std::isfinite(right.time) ||
            !std::isfinite(right.value)) {
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
                                                      WaveExtremeKind kind)
{
    if (channelIndex >= displayData.channels.size()) {
        return std::nullopt;
    }
    const auto& channel = displayData.channels[channelIndex];
    const auto& samples = channel.samples;
    if (samples.empty() || !std::isfinite(centerTime) || !std::isfinite(maxTimeDistance) || maxTimeDistance <= 0.0) {
        return std::nullopt;
    }

    std::optional<CursorReadout> bestLocalExtreme;
    std::optional<CursorReadout> bestWindowExtreme;
    double bestLocalDistance = std::numeric_limits<double>::infinity();
    double bestWindowDistance = std::numeric_limits<double>::infinity();
    double windowMinValue = std::numeric_limits<double>::infinity();
    double windowMaxValue = -std::numeric_limits<double>::infinity();
    auto acceptCandidate = [&](std::optional<CursorReadout>& best, double& bestDistance, std::size_t index) {
        const auto& current = samples[index];
        const double distance = std::abs(current.time - centerTime);
        if (!best.has_value() || extremeValueBetter(kind, current.value, best->displayValue) ||
            (std::abs(current.value - best->displayValue) <= kEpsilon && distance < bestDistance)) {
            bestDistance = distance;
            best = makeCursorReadout(channel, channelIndex, index);
        }
    };

    for (std::size_t index = 0; index < samples.size(); ++index) {
        const auto& current = samples[index];
        if (!std::isfinite(current.time) || !std::isfinite(current.value)) {
            continue;
        }
        const double distance = std::abs(current.time - centerTime);
        if (distance > maxTimeDistance) {
            continue;
        }
        windowMinValue = (std::min)(windowMinValue, current.value);
        windowMaxValue = (std::max)(windowMaxValue, current.value);
        // 核心流程：先记录窗口内显示值最强的样本，正弦峰值被采样成平顶或平台时仍能稳定吸附到视觉峰/谷。
        acceptCandidate(bestWindowExtreme, bestWindowDistance, index);
        if (index == 0 || index + 1 >= samples.size()) {
            continue;
        }
        const auto& previous = samples[index - 1];
        const auto& next = samples[index + 1];
        if (!std::isfinite(previous.value) || !std::isfinite(next.value)) {
            continue;
        }
        // 严格局部峰/谷仍优先，避免单调斜坡在搜索窗口里过早抢占真正的离散极值。
        const bool isMaximum = current.value > previous.value && current.value > next.value;
        const bool isMinimum = current.value < previous.value && current.value < next.value;
        if ((kind == WaveExtremeKind::Maximum && isMaximum) || (kind == WaveExtremeKind::Minimum && isMinimum)) {
            acceptCandidate(bestLocalExtreme, bestLocalDistance, index);
        }
    }
    if (bestLocalExtreme.has_value()) {
        return bestLocalExtreme;
    }
    return windowMaxValue - windowMinValue > kEpsilon ? bestWindowExtreme : std::nullopt;
}

double applyCursorDragSnap(double dragTime, const std::optional<CursorReadout>& smartSnap)
{
    if (smartSnap.has_value() && std::isfinite(smartSnap->time)) {
        // 拖动时智能吸附结果必须覆盖鼠标时间，否则 UI 游标线会继续跟随鼠标移动。
        return smartSnap->time;
    }
    return dragTime;
}

void lockCursorInterval(double movedTime, double& pairedTime, double lockedInterval, bool movedLeftCursor)
{
    if (!std::isfinite(lockedInterval) || lockedInterval <= 0.0) {
        return;
    }
    pairedTime = movedLeftCursor ? movedTime + lockedInterval : movedTime - lockedInterval;
}

WaveViewport moveViewportByDelta(const WaveViewport& viewport,
                                 double deltaTime,
                                 const WaveDataBounds& bounds,
                                 double minTimeWidth)
{
    WaveViewport moved = viewport;
    if (!std::isfinite(deltaTime)) {
        deltaTime = 0.0;
    }
    moved.minTime += deltaTime;
    moved.maxTime += deltaTime;
    return normalizeOverviewViewport(moved, bounds, minTimeWidth);
}

double cursorTimeInViewport(const WaveViewport& viewport, double ratio)
{
    const double minTime = (std::min)(viewport.minTime, viewport.maxTime);
    const double maxTime = (std::max)(viewport.minTime, viewport.maxTime);
    if (!std::isfinite(minTime) || !std::isfinite(maxTime)) {
        return 0.0;
    }
    // 快捷定位只在当前可视窗口内布点，ratio 先夹紧，避免异常输入把游标推到窗外。
    const double clampedRatio = (std::clamp)(std::isfinite(ratio) ? ratio : 0.5, 0.0, 1.0);
    return minTime + (maxTime - minTime) * clampedRatio;
}

bool shiftMeasurementCursorsForViewportScroll(WaveViewState& view,
                                              const WaveViewport& oldViewport,
                                              const WaveViewport& newViewport)
{
    if (!view.followMeasurementCursorsOnScroll) {
        return false;
    }

    const double oldWidth = oldViewport.maxTime - oldViewport.minTime;
    const double newWidth = newViewport.maxTime - newViewport.minTime;
    const double deltaTime = newViewport.minTime - oldViewport.minTime;
    const double widthScale = (std::max)({std::abs(oldWidth), std::abs(newWidth), 1.0});
    if (!std::isfinite(oldWidth) || !std::isfinite(newWidth) || !std::isfinite(deltaTime) ||
        std::abs(oldWidth - newWidth) > widthScale * 1e-9 || std::abs(deltaTime) <= kEpsilon) {
        return false;
    }

    bool shifted = false;
    for (auto& cursor : view.cursors) {
        if (!cursor.enabled) {
            continue;
        }
        cursor.time += deltaTime;
        shifted = true;
    }
    if (shifted) {
        // 核心流程：跟随滚动只平移时间；下一帧主图按时间重绑定读数，避免旧 Y 锚点失效。
        view.measurementCursorReadoutRefreshPending = true;
    }
    return shifted;
}

double resolveChannelCardWidth(WaveChannelCardWidthMode mode,
                               double fixedWidth,
                               double adaptiveRatio,
                               double availableWidth)
{
    if (mode == WaveChannelCardWidthMode::Fixed) {
        return fixedWidth > 0.0 ? fixedWidth : 128.0;
    }

    // 核心流程：自适应模式沿用原有比例布局，同时保留安全夹取，避免极窄或极宽卡片影响可读性。
    const double safeRatio = adaptiveRatio > 0.0 ? adaptiveRatio : 0.22;
    return (std::clamp)(availableWidth * safeRatio, 160.0, 220.0);
}

WaveValueRange makeVerticalAutoFitRange(double minValue, double maxValue, double multiplier)
{
    const double safeMultiplier = multiplier > 0.0 ? multiplier : 1.25;
    const double center = (minValue + maxValue) * 0.5;
    double span = maxValue - minValue;
    if (std::abs(span) <= kEpsilon || !std::isfinite(span)) {
        span = 2.0;
    }
    const double viewSpan = span * safeMultiplier;
    const double halfRange = viewSpan * 0.5;
    return {
        .minValue = center - halfRange,
        .maxValue = center + halfRange,
    };
}

bool resetChannelConfigToDefault(WaveDockState& wave,
                                 std::size_t channelIndex,
                                 const ChannelSpec& defaultSpec,
                                 WaveChannelDoubleClickAction action)
{
    const auto currentSpec = wave.buffer.channelSpec(channelIndex);
    if (!currentSpec.has_value()) {
        return false;
    }

    auto updated = *currentSpec;
    // 核心流程：按配置只回退指定字段，避免双击误触清掉用户想保留的通道覆盖。
    switch (action) {
        case WaveChannelDoubleClickAction::ResetAll:
            updated = defaultSpec;
            break;
        case WaveChannelDoubleClickAction::ResetScaleOffset:
            updated.scale = defaultSpec.scale;
            updated.offset = defaultSpec.offset;
            break;
        case WaveChannelDoubleClickAction::ResetScale:
            updated.scale = defaultSpec.scale;
            break;
        case WaveChannelDoubleClickAction::ResetOffset:
            updated.offset = defaultSpec.offset;
            break;
    }

    if (channelIndex >= wave.channelOverrides.size()) {
        wave.channelOverrides.resize(channelIndex + 1);
    }
    auto& overrideState = wave.channelOverrides[channelIndex];
    overrideState.labelOverridden = updated.label != defaultSpec.label;
    overrideState.ratioOverridden = std::abs(updated.ratio - defaultSpec.ratio) > kEpsilon;
    overrideState.scaleOverridden = std::abs(updated.scale - defaultSpec.scale) > kEpsilon;
    overrideState.offsetOverridden = std::abs(updated.offset - defaultSpec.offset) > kEpsilon;
    overrideState.colorOverridden = updated.color != defaultSpec.color;
    overrideState.bitYOffsetOverridden =
        std::abs(updated.bitDisplay.yOffset - defaultSpec.bitDisplay.yOffset) > kEpsilon;
    overrideState.label = updated.label;
    overrideState.ratio = updated.ratio;
    overrideState.scale = updated.scale;
    overrideState.offset = updated.offset;
    overrideState.color = updated.color;
    overrideState.bitYOffset = updated.bitDisplay.yOffset;
    wave.buffer.setChannelSpec(channelIndex, std::move(updated));
    return true;
}

bool resetChannelConfigToDefault(WaveDockState& wave, std::size_t channelIndex, WaveChannelDoubleClickAction action)
{
    if (channelIndex >= wave.defaultChannelSpecs.size()) {
        return false;
    }
    return resetChannelConfigToDefault(wave, channelIndex, wave.defaultChannelSpecs[channelIndex], action);
}

bool resetChannelOffsetToDefault(WaveDockState& wave, std::size_t channelIndex)
{
    return resetChannelConfigToDefault(wave, channelIndex, WaveChannelDoubleClickAction::ResetOffset);
}

bool resetOneChannelViewSettings(WaveDockState& wave, std::size_t channelIndex)
{
    if (channelIndex >= wave.defaultChannelSpecs.size()) {
        return false;
    }
    bool changed = false;
    const auto hiddenEnd =
        std::remove(wave.hiddenChannelIndices.begin(), wave.hiddenChannelIndices.end(), channelIndex);
    const bool hiddenStateChanged = hiddenEnd != wave.hiddenChannelIndices.end();
    wave.hiddenChannelIndices.erase(hiddenEnd, wave.hiddenChannelIndices.end());
    wave.hiddenChannelLabels.clear();
    if (hiddenStateChanged) {
        wave.legendVisibilityRestorePending = true;
        changed = true;
    }
    if (!resetChannelConfigToDefault(
            wave, channelIndex, wave.defaultChannelSpecs[channelIndex], WaveChannelDoubleClickAction::ResetAll)) {
        return false;
    }
    changed = true;
    if (channelIndex < wave.channelOverrides.size()) {
        wave.channelOverrides[channelIndex] = {};
    }
    if (channelIndex < wave.fftMagnitudeChannelOffsets.size() &&
        std::abs(wave.fftMagnitudeChannelOffsets[channelIndex]) > kEpsilon) {
        wave.fftMagnitudeChannelOffsets[channelIndex] = 0.0;
        changed = true;
    }
    return changed;
}

bool resetAllChannelViewSettings(WaveDockState& wave)
{
    bool changed = false;
    const std::size_t channelCount = wave.buffer.channelCount();
    for (std::size_t channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        changed = resetOneChannelViewSettings(wave, channelIndex) || changed;
    }

    wave.hiddenChannelIndices.clear();
    wave.hiddenChannelLabels.clear();
    wave.fftMagnitudeChannelOffsets.assign(channelCount, 0.0);
    wave.channelOverrides.clear();
    wave.legendOverlay.expanded = false;
    wave.legendOverlay.hoverFloating = false;
    wave.legendOverlay.hoverInteractionLocked = false;
    wave.legendOverlay.hoverCloseRemainingSec = 0.0F;
    wave.legendOverlay.offsetX = 8.0F;
    wave.legendOverlay.offsetY = 8.0F;
    wave.legendVisibilityRestorePending = true;
    return changed || channelCount > 0;
}

} // namespace protoscope::plot
