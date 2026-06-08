#include "protoscope/plot/oscilloscope.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace protoscope::plot {

namespace {

    constexpr double kEpsilon = 1e-12;

    double safeFrequency(double deltaTime)
    {
        if (std::abs(deltaTime) <= kEpsilon) {
            return 0.0;
        }
        return 1.0 / std::abs(deltaTime);
    }

    double sanitizeScale(double scale)
    {
        return std::isfinite(scale) ? scale : 1.0;
    }

    double sanitizeRatio(double ratio)
    {
        return std::isfinite(ratio) ? ratio : 1.0;
    }

    double sanitizeOffset(double offset)
    {
        return std::isfinite(offset) ? offset : 0.0;
    }

    double estimateTimeStep(const std::vector<WaveSample>& samples)
    {
        for (std::size_t index = 1; index < samples.size(); ++index) {
            const double previous = samples[index - 1].time;
            const double current = samples[index].time;
            if (std::isfinite(previous) && std::isfinite(current) && current > previous) {
                return current - previous;
            }
        }
        return kEpsilon;
    }

    void sortSamplesByTime(std::vector<WaveSample>& samples)
    {
        // 核心流程：实时链路通常按时间递增推送大批量采样，已递增时跳过排序以降低 append 成本。
        const bool monotonic =
            std::is_sorted(samples.begin(), samples.end(), [](const WaveSample& left, const WaveSample& right) {
                return left.time < right.time;
            });
        if (!monotonic) {
            std::sort(samples.begin(), samples.end(), [](const WaveSample& left, const WaveSample& right) {
                return left.time < right.time;
            });
        }
    }

    bool startsAtHistoryOrigin(const std::vector<WaveSample>& samples, double lastTime)
    {
        if (samples.empty()) {
            return false;
        }
        const double firstTime = samples.front().time;
        return firstTime <= lastTime && firstTime <= kEpsilon;
    }

    void shiftSamplesAfterLastTime(std::vector<WaveSample>& samples, double lastTime)
    {
        if (samples.empty()) {
            return;
        }
        const double offset = lastTime + estimateTimeStep(samples) - samples.front().time;
        for (auto& sample : samples) {
            sample.time += offset;
        }
    }

    void dropSamplesNotAfterLastTime(std::vector<WaveSample>& samples, double lastTime)
    {
        samples.erase(std::remove_if(samples.begin(),
                                     samples.end(),
                                     [lastTime](const WaveSample& sample) { return sample.time <= lastTime; }),
                      samples.end());
    }

    double nearestRankPercentile(const std::vector<double>& sortedValues, double percentile)
    {
        if (sortedValues.empty()) {
            return 0.0;
        }
        const double rawRank = (std::ceil)(percentile * static_cast<double>(sortedValues.size()));
        const auto rank = static_cast<std::size_t>(rawRank <= 1.0 ? 0.0 : rawRank - 1.0);
        return sortedValues[(std::min)(rank, sortedValues.size() - 1)];
    }

    std::optional<double> firstCrossingDuration(const std::vector<double>& times,
                                                const std::vector<double>& values,
                                                double startLevel,
                                                double endLevel,
                                                bool rising)
    {
        std::optional<double> startTime;
        for (std::size_t index = 1; index < values.size(); ++index) {
            const double previous = values[index - 1];
            const double current = values[index];
            const double previousTime = times[index - 1];
            const double currentTime = times[index];
            const double width = currentTime - previousTime;
            if (width < 0.0 || std::abs(current - previous) <= kEpsilon) {
                continue;
            }
            const auto crosses = [&](double level) {
                return rising ? previous < level && current >= level : previous > level && current <= level;
            };
            const auto crossingTime = [&](double level) {
                return previousTime + (level - previous) * width / (current - previous);
            };
            if (!startTime.has_value() && crosses(startLevel)) {
                startTime = crossingTime(startLevel);
            }
            if (startTime.has_value() && crosses(endLevel)) {
                return crossingTime(endLevel) - *startTime;
            }
        }
        return std::nullopt;
    }

} // namespace

MeasurementReadout makeMeasurementReadout(std::size_t channelIndex,
                                          const std::vector<double>& times,
                                          const std::vector<double>& values,
                                          const std::vector<double>* referenceValues)
{
    MeasurementReadout result{};
    if (values.empty() || times.size() < values.size()) {
        return result;
    }

    result.valid = true;
    result.channelIndex = channelIndex;
    result.sampleCount = values.size();
    result.minValue = *std::min_element(values.begin(), values.end());
    result.maxValue = *std::max_element(values.begin(), values.end());
    result.peakToPeak = result.maxValue - result.minValue;
    result.duration = times[values.size() - 1] - times.front();

    const double count = static_cast<double>(values.size());
    const double sum = std::accumulate(values.begin(), values.end(), 0.0);
    const double squareSum = std::accumulate(
        values.begin(), values.end(), 0.0, [](double total, double value) { return total + value * value; });
    result.meanValue = sum / count;
    result.rmsValue = std::sqrt(squareSum / count);

    std::vector<double> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    result.medianValue = nearestRankPercentile(sorted, 0.50);
    result.p95Value = nearestRankPercentile(sorted, 0.95);
    result.p99Value = nearestRankPercentile(sorted, 0.99);
    result.iqr = nearestRankPercentile(sorted, 0.75) - nearestRankPercentile(sorted, 0.25);
    result.p95Spread = nearestRankPercentile(sorted, 0.95) - nearestRankPercentile(sorted, 0.05);

    double varianceSum = 0.0;
    double absDeviationSum = 0.0;
    std::vector<double> medianDeviations;
    medianDeviations.reserve(values.size());
    for (const double value : values) {
        const double deviation = value - result.meanValue;
        varianceSum += deviation * deviation;
        absDeviationSum += std::abs(deviation);
        medianDeviations.push_back(std::abs(value - result.medianValue));
    }
    result.variance = varianceSum / count;
    result.stddev = std::sqrt(result.variance);
    if (std::abs(result.meanValue) > kEpsilon) {
        result.cv = result.stddev / std::abs(result.meanValue);
    }
    result.mad = absDeviationSum / count;
    std::sort(medianDeviations.begin(), medianDeviations.end());
    result.medianAbsDev = nearestRankPercentile(medianDeviations, 0.50);

    if (values.size() >= 2 && result.peakToPeak > kEpsilon) {
        const double threshold = (result.minValue + result.maxValue) * 0.5;
        const double riseStart = result.minValue + result.peakToPeak * 0.1;
        const double riseEnd = result.minValue + result.peakToPeak * 0.9;
        const double fallStart = riseEnd;
        const double fallEnd = riseStart;
        result.riseTime = firstCrossingDuration(times, values, riseStart, riseEnd, true);
        result.fallTime = firstCrossingDuration(times, values, fallStart, fallEnd, false);
        for (std::size_t index = 1; index < values.size(); ++index) {
            const double previous = values[index - 1];
            const double current = values[index];
            const double width = times[index] - times[index - 1];
            if ((previous < threshold && current >= threshold) || (previous > threshold && current <= threshold)) {
                ++result.edgeCount;
            }
            if (width <= 0.0) {
                continue;
            }
            if (previous >= threshold && current >= threshold) {
                result.highWidth += width;
            } else if (previous < threshold && current < threshold) {
                result.lowWidth += width;
            }
        }
        if (result.duration > kEpsilon) {
            result.dutyCycle = result.highWidth / result.duration * 100.0;
        }
    }

    if (referenceValues != nullptr && referenceValues->size() == values.size()) {
        double errorSum = 0.0;
        double errorSquareSum = 0.0;
        double absErrorSum = 0.0;
        double maxAbsError = 0.0;
        for (std::size_t index = 0; index < values.size(); ++index) {
            const double error = values[index] - (*referenceValues)[index];
            const double absError = std::abs(error);
            errorSum += error;
            errorSquareSum += error * error;
            absErrorSum += absError;
            maxAbsError = (std::max)(maxAbsError, absError);
        }
        result.absoluteError = values.back() - referenceValues->back();
        if (std::abs(referenceValues->back()) > kEpsilon) {
            result.relativeErrorPercent = *result.absoluteError / std::abs(referenceValues->back()) * 100.0;
        }
        result.meanError = errorSum / count;
        result.mse = errorSquareSum / count;
        result.rmse = std::sqrt(*result.mse);
        result.mae = absErrorSum / count;
        result.maxAbsError = maxAbsError;
        result.bias = *result.meanError;
    }
    return result;
}

double applyChannelActualValue(double rawValue, const ChannelSpec& spec)
{
    return rawValue * sanitizeRatio(spec.ratio);
}

double applyChannelDisplayTransform(double rawValue, const ChannelSpec& spec, WaveDisplayFormula formula)
{
    const double actualValue = applyChannelActualValue(rawValue, spec);
    const double scale = sanitizeScale(spec.scale);
    const double offset = sanitizeOffset(spec.offset);
    if (formula == WaveDisplayFormula::ScaleThenOffset) {
        return actualValue * scale + offset;
    }
    return (actualValue + offset) * scale;
}

void OscilloscopeBuffer::clear()
{
    source_.clear();
    channels_.clear();
    config_ = ViewConfig{};
    preservedHistoryLimit_ = 0;
    ++dataRevision_;
}

void OscilloscopeBuffer::configureChannels(std::size_t channelCount)
{
    const auto previousCount = channels_.size();
    channels_.resize(channelCount);
    for (std::size_t index = 0; index < channels_.size(); ++index) {
        if (channels_[index].spec.label.empty()) {
            channels_[index].spec.label = "CH" + std::to_string(index + 1);
        }
    }
    if (channels_.size() != previousCount) {
        ++dataRevision_;
    }
}

void OscilloscopeBuffer::setChannelSpec(std::size_t channelIndex, ChannelSpec spec)
{
    if (channelIndex >= channels_.size()) {
        configureChannels(channelIndex + 1);
    }
    if (spec.label.empty()) {
        spec.label = "CH" + std::to_string(channelIndex + 1);
    }
    spec.ratio = sanitizeRatio(spec.ratio);
    spec.scale = sanitizeScale(spec.scale);
    spec.offset = sanitizeOffset(spec.offset);
    auto& channelSpec = channels_[channelIndex].spec;
    if (channelSpec.label != spec.label || channelSpec.unit != spec.unit || channelSpec.ratio != spec.ratio ||
        channelSpec.scale != spec.scale || channelSpec.offset != spec.offset) {
        channelSpec = std::move(spec);
        ++dataRevision_;
    }
}

void OscilloscopeBuffer::setViewConfig(const ViewConfig& config)
{
    config_ = config;
    if (std::abs(config_.timeScale) <= kEpsilon) {
        config_.timeScale = 1.0;
    }
    ++dataRevision_;
}

void OscilloscopeBuffer::setHistoryTrimSuspended(bool suspended)
{
    historyTrimSuspended_ = suspended;
}

void OscilloscopeBuffer::setMaxTotalSamples(std::size_t maxTotalSamples)
{
    maxTotalSamples_ = maxTotalSamples;
    // 核心流程：若上限减小，立即对所有通道执行裁剪
    if (maxTotalSamples > 0) {
        for (auto& channel : channels_) {
            if (channel.samples.size() > maxTotalSamples) {
                trimHistory(channel);
            }
        }
    }
}

void OscilloscopeBuffer::setResetHistoryOnTimeReset(bool enabled)
{
    resetHistoryOnTimeReset_ = enabled;
}

void OscilloscopeBuffer::preserveHistoryLimitAtLeast(std::size_t sampleCount)
{
    preservedHistoryLimit_ = (std::max)(preservedHistoryLimit_, sampleCount);
}

std::size_t OscilloscopeBuffer::channelCount() const
{
    return channels_.size();
}

std::optional<ChannelSpec> OscilloscopeBuffer::channelSpec(std::size_t channelIndex) const
{
    if (channelIndex >= channels_.size()) {
        return std::nullopt;
    }
    return channels_[channelIndex].spec;
}

const ViewConfig& OscilloscopeBuffer::viewConfig() const
{
    return config_;
}

std::uint64_t OscilloscopeBuffer::dataRevision() const
{
    return dataRevision_;
}

std::optional<double> OscilloscopeBuffer::latestTime() const
{
    std::optional<double> latest;
    for (const auto& channel : channels_) {
        if (channel.samples.empty()) {
            continue;
        }
        const double candidate = channel.samples.back().time;
        if (!latest.has_value() || candidate > *latest) {
            latest = candidate;
        }
    }
    return latest;
}

bool OscilloscopeBuffer::append(std::size_t channelIndex, WaveAppendRequest request)
{
    if (request.samples.empty()) {
        return false;
    }
    auto& channel = ensureChannel(channelIndex);
    applyAppendSource(request.source);

    sortSamplesByTime(request.samples);

    if (!channel.samples.empty()) {
        const double lastTime = channel.samples.back().time;
        if (startsAtHistoryOrigin(request.samples, lastTime)) {
            if (resetHistoryOnTimeReset_) {
                return appendAfterHistoryReset(channelIndex, request);
            }
            shiftSamplesAfterLastTime(request.samples, lastTime);
        }
        dropSamplesNotAfterLastTime(request.samples, lastTime);
    }

    return appendPreparedSamples(channel, request.samples);
}

OscilloscopeBuffer::ChannelBuffer& OscilloscopeBuffer::ensureChannel(std::size_t channelIndex)
{
    if (channelIndex >= channels_.size()) {
        configureChannels(channelIndex + 1);
    }
    return channels_[channelIndex];
}

void OscilloscopeBuffer::applyAppendSource(const std::string& source)
{
    if (!source.empty()) {
        source_ = source;
    }
}

bool OscilloscopeBuffer::appendAfterHistoryReset(std::size_t channelIndex, const WaveAppendRequest& request)
{
    // 核心流程：脚本二次运行常从 t=0 重新输出，默认把它视为新一轮采集，避免旧历史挡住新样本。
    for (auto& existingChannel : channels_) {
        existingChannel.samples.clear();
    }
    preservedHistoryLimit_ = 0;
    auto& resetChannel = ensureChannel(channelIndex);
    applyAppendSource(request.source);
    return appendPreparedSamples(resetChannel, request.samples);
}

bool OscilloscopeBuffer::appendPreparedSamples(ChannelBuffer& channel, const std::vector<WaveSample>& samples)
{
    if (samples.empty()) {
        return false;
    }
    channel.samples.insert(channel.samples.end(), samples.begin(), samples.end());
    trimHistory(channel);
    ++dataRevision_;
    return true;
}

WaveSnapshot OscilloscopeBuffer::snapshot(double visibleMinTime, double visibleMaxTime, bool computeStats) const
{
    WaveSnapshot snapshot{};
    snapshot.source = source_;
    snapshot.config = config_;
    if (visibleMaxTime < visibleMinTime) {
        std::swap(visibleMinTime, visibleMaxTime);
    }

    snapshot.channels.reserve(channels_.size());
    for (const auto& channel : channels_) {
        ChannelView view{};
        view.label = channel.spec.label;
        view.unit = channel.spec.unit;
        view.ratio = channel.spec.ratio;
        view.scale = channel.spec.scale;
        view.offset = channel.spec.offset;
        view.color = channel.spec.color;
        view.totalSamples = channel.samples.size();
        view.samples = channel.samples.data();
        view.visibleBegin = lowerBoundByTime(channel.samples, visibleMinTime);
        view.visibleEnd = upperBoundByTime(channel.samples, visibleMaxTime);
        if (computeStats) {
            view.stats = makeStats(channel.samples, view.visibleBegin, view.visibleEnd, channel.spec);
        } else {
            // 核心流程：主视图缓存全历史快照只需要范围和样本指针，跳过 min/max 全量扫描。
            view.stats.totalSamples = view.totalSamples;
            view.stats.visibleSamples = view.visibleEnd > view.visibleBegin ? view.visibleEnd - view.visibleBegin : 0;
            if (view.stats.visibleSamples > 1) {
                const double span = channel.samples[view.visibleEnd - 1].time - channel.samples[view.visibleBegin].time;
                if (span > kEpsilon) {
                    view.stats.sampleRateHz = static_cast<double>(view.stats.visibleSamples - 1) / span;
                }
            }
        }
        snapshot.channels.push_back(view);
    }
    return snapshot;
}

EnvelopeView OscilloscopeBuffer::buildEnvelope(std::size_t channelIndex,
                                               double visibleMinTime,
                                               double visibleMaxTime,
                                               std::size_t pixelWidth) const
{
    return buildLimitedEnvelope(channelIndex, visibleMinTime, visibleMaxTime, pixelWidth, 0);
}

EnvelopeView OscilloscopeBuffer::buildLimitedEnvelope(std::size_t channelIndex,
                                                      double visibleMinTime,
                                                      double visibleMaxTime,
                                                      std::size_t pixelWidth,
                                                      std::size_t maxSamples) const
{
    EnvelopeView view{};
    if (channelIndex >= channels_.size() || pixelWidth == 0) {
        return view;
    }

    const auto& samples = channels_[channelIndex].samples;
    const auto& spec = channels_[channelIndex].spec;
    if (samples.empty()) {
        return view;
    }

    if (visibleMaxTime < visibleMinTime) {
        std::swap(visibleMinTime, visibleMaxTime);
    }

    std::size_t begin = lowerBoundByTime(samples, visibleMinTime);
    const std::size_t end = upperBoundByTime(samples, visibleMaxTime);
    if (begin >= end) {
        return view;
    }

    view.sourceSampleCount = end - begin;
    if (maxSamples > 0 && view.sourceSampleCount > maxSamples) {
        // 核心流程：概览只读取最近的有限样本，避免历史数据过大时拖慢 UI。
        begin = end - maxSamples;
    }

    const std::size_t sampledCount = end - begin;
    if (sampledCount <= pixelWidth * 2) {
        view.points.reserve(sampledCount);
        for (std::size_t index = begin; index < end; ++index) {
            const double displayValue =
                applyChannelDisplayTransform(samples[index].value, spec, config_.displayFormula);
            view.points.push_back(EnvelopePoint{
                .time = samples[index].time,
                .minValue = displayValue,
                .maxValue = displayValue,
                .sampleCount = 1,
            });
        }
        return view;
    }

    const std::size_t bucketCount = (std::max)(std::size_t{1}, pixelWidth);
    const double span = (std::max)(visibleMaxTime - visibleMinTime, kEpsilon);
    view.points.reserve(bucketCount);

    std::size_t cursor = begin;
    for (std::size_t bucketIndex = 0; bucketIndex < bucketCount && cursor < end; ++bucketIndex) {
        const double bucketStart =
            visibleMinTime + span * static_cast<double>(bucketIndex) / static_cast<double>(bucketCount);
        const double bucketEnd =
            visibleMinTime + span * static_cast<double>(bucketIndex + 1) / static_cast<double>(bucketCount);

        double minValue = std::numeric_limits<double>::infinity();
        double maxValue = -std::numeric_limits<double>::infinity();
        double timeAccumulator = 0.0;
        std::size_t count = 0;

        while (cursor < end) {
            const auto& sample = samples[cursor];
            if (sample.time > bucketEnd && count > 0) {
                break;
            }
            if (sample.time >= bucketStart && sample.time <= bucketEnd) {
                const double displayValue = applyChannelDisplayTransform(sample.value, spec, config_.displayFormula);
                minValue = (std::min)(minValue, displayValue);
                maxValue = (std::max)(maxValue, displayValue);
                timeAccumulator += sample.time;
                ++count;
            }
            if (sample.time > bucketEnd) {
                break;
            }
            ++cursor;
        }

        if (count > 0) {
            view.points.push_back(EnvelopePoint{
                .time = timeAccumulator / static_cast<double>(count),
                .minValue = minValue,
                .maxValue = maxValue,
                .sampleCount = count,
            });
        }
    }

    if (view.points.empty()) {
        for (std::size_t index = begin; index < end; ++index) {
            const double displayValue =
                applyChannelDisplayTransform(samples[index].value, spec, config_.displayFormula);
            view.points.push_back(EnvelopePoint{
                .time = samples[index].time,
                .minValue = displayValue,
                .maxValue = displayValue,
                .sampleCount = 1,
            });
        }
    }

    return view;
}

std::optional<CursorReadout> OscilloscopeBuffer::findNearest(
    std::size_t channelIndex, double time, double value, double maxTimeDistance, double maxValueDistance) const
{
    if (channelIndex >= channels_.size()) {
        return std::nullopt;
    }
    const auto& samples = channels_[channelIndex].samples;
    const auto& spec = channels_[channelIndex].spec;
    if (samples.empty()) {
        return std::nullopt;
    }

    const std::size_t pivot = lowerBoundByTime(samples, time);
    const std::size_t begin = pivot > 32 ? pivot - 32 : 0;
    const std::size_t end = (std::min)(samples.size(), pivot + 32);

    double bestScore = std::numeric_limits<double>::infinity();
    std::optional<CursorReadout> best;
    for (std::size_t index = begin; index < end; ++index) {
        const auto& sample = samples[index];
        const double displayValue = applyChannelDisplayTransform(sample.value, spec, config_.displayFormula);
        const double dt = std::abs(sample.time - time);
        const double dv = std::abs(displayValue - value);
        if (dt > maxTimeDistance || dv > maxValueDistance) {
            continue;
        }
        const double score = dt * dt + dv * dv;
        if (score < bestScore) {
            bestScore = score;
            const double actualValue = applyChannelActualValue(sample.value, spec);
            best = CursorReadout{
                .valid = true,
                .channelIndex = channelIndex,
                .sampleIndex = index,
                .time = sample.time,
                .value = actualValue,
                .displayValue = displayValue,
            };
        }
    }
    return best;
}

std::optional<CursorReadout> OscilloscopeBuffer::findNearestByTime(std::size_t channelIndex,
                                                                   double time,
                                                                   double maxTimeDistance) const
{
    if (channelIndex >= channels_.size()) {
        return std::nullopt;
    }
    const auto& samples = channels_[channelIndex].samples;
    const auto& spec = channels_[channelIndex].spec;
    if (samples.empty()) {
        return std::nullopt;
    }

    const std::size_t pivot = lowerBoundByTime(samples, time);
    const std::size_t begin = pivot > 16 ? pivot - 16 : 0;
    const std::size_t end = (std::min)(samples.size(), pivot + 16);
    std::optional<CursorReadout> best;
    double bestDt = std::numeric_limits<double>::infinity();
    for (std::size_t index = begin; index < end; ++index) {
        const double dt = std::abs(samples[index].time - time);
        if (dt > maxTimeDistance || dt >= bestDt) {
            continue;
        }
        bestDt = dt;
        const double actualValue = applyChannelActualValue(samples[index].value, spec);
        const double displayValue = applyChannelDisplayTransform(samples[index].value, spec, config_.displayFormula);
        best = CursorReadout{
            .valid = true,
            .channelIndex = channelIndex,
            .sampleIndex = index,
            .time = samples[index].time,
            .value = actualValue,
            .displayValue = displayValue,
        };
    }
    return best;
}

MeasurementReadout OscilloscopeBuffer::measureWindow(std::size_t channelIndex, double beginTime, double endTime) const
{
    MeasurementReadout result{};
    if (channelIndex >= channels_.size()) {
        return result;
    }
    if (endTime < beginTime) {
        std::swap(beginTime, endTime);
    }

    const auto& samples = channels_[channelIndex].samples;
    const auto& spec = channels_[channelIndex].spec;
    if (samples.empty()) {
        return result;
    }

    const std::size_t begin = lowerBoundByTime(samples, beginTime);
    const std::size_t end = upperBoundByTime(samples, endTime);
    if (begin >= end) {
        return result;
    }

    std::vector<double> times;
    std::vector<double> values;
    times.reserve(end - begin);
    values.reserve(end - begin);
    for (std::size_t index = begin; index < end; ++index) {
        times.push_back(samples[index].time);
        values.push_back(applyChannelActualValue(samples[index].value, spec));
    }
    return makeMeasurementReadout(channelIndex, times, values);
}

DeltaReadout OscilloscopeBuffer::makeDelta(const CursorReadout& left, const CursorReadout& right)
{
    if (!left.valid || !right.valid) {
        return DeltaReadout{};
    }
    const double deltaTime = right.time - left.time;
    return DeltaReadout{
        .valid = true,
        .deltaTime = deltaTime,
        .deltaValue = right.value - left.value,
        .frequencyHz = safeFrequency(deltaTime),
    };
}

std::size_t OscilloscopeBuffer::lowerBoundByTime(const std::vector<WaveSample>& samples, double time) const
{
    return static_cast<std::size_t>(std::distance(
        samples.begin(),
        std::lower_bound(samples.begin(), samples.end(), time, [](const WaveSample& sample, double pivot) {
            return sample.time < pivot;
        })));
}

std::size_t OscilloscopeBuffer::upperBoundByTime(const std::vector<WaveSample>& samples, double time) const
{
    return static_cast<std::size_t>(std::distance(
        samples.begin(),
        std::upper_bound(samples.begin(), samples.end(), time, [](double pivot, const WaveSample& sample) {
            return pivot < sample.time;
        })));
}

void OscilloscopeBuffer::trimHistory(ChannelBuffer& channel)
{
    if (historyTrimSuspended_) {
        return;
    }
    const std::size_t historyLimit = effectiveHistoryLimit();
    const bool hasHistoryLimit = historyLimit > 0;
    const bool hasMaxTotalSamples = maxTotalSamples_ > 0;
    if (!hasHistoryLimit && !hasMaxTotalSamples) {
        return;
    }
    const std::size_t effectiveLimit = hasHistoryLimit && hasMaxTotalSamples
                                           ? (std::min)(historyLimit, maxTotalSamples_)
                                           : (hasHistoryLimit ? historyLimit : maxTotalSamples_);
    if (channel.samples.size() <= effectiveLimit) {
        return;
    }
    if (effectiveLimit == 0) {
        channel.samples.clear();
        return;
    }
    const std::size_t keepOffset = channel.samples.size() - effectiveLimit;
    // 核心流程：大历史裁剪只把保留窗口搬到前部一次，避免 vector::erase 对尾部做额外析构搬移。
    std::move(channel.samples.begin() + static_cast<std::ptrdiff_t>(keepOffset),
              channel.samples.end(),
              channel.samples.begin());
    channel.samples.resize(effectiveLimit);
}

std::size_t OscilloscopeBuffer::effectiveHistoryLimit() const
{
    // 核心流程：导入回放后的完整历史是用户显式载入的数据，不能被较小的 Lua history_limit 立即裁掉。
    if (config_.historyLimit == 0U) {
        return preservedHistoryLimit_;
    }
    return (std::max)(config_.historyLimit, preservedHistoryLimit_);
}

WaveStats OscilloscopeBuffer::makeStats(const std::vector<WaveSample>& samples,
                                        std::size_t begin,
                                        std::size_t end,
                                        const ChannelSpec& spec) const
{
    WaveStats stats{};
    stats.totalSamples = samples.size();
    if (begin >= end || begin >= samples.size() || end > samples.size()) {
        return stats;
    }

    stats.visibleSamples = end - begin;
    stats.minValue = std::numeric_limits<double>::infinity();
    stats.maxValue = -std::numeric_limits<double>::infinity();
    for (std::size_t index = begin; index < end; ++index) {
        const double displayValue = applyChannelDisplayTransform(samples[index].value, spec, config_.displayFormula);
        stats.minValue = (std::min)(stats.minValue, displayValue);
        stats.maxValue = (std::max)(stats.maxValue, displayValue);
    }

    if (stats.visibleSamples > 1) {
        const double span = samples[end - 1].time - samples[begin].time;
        if (span > kEpsilon) {
            stats.sampleRateHz = static_cast<double>(stats.visibleSamples - 1) / span;
        }
    }
    return stats;
}

} // namespace protoscope::plot
