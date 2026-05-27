#include "protoscope/plot/oscilloscope.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace protoscope::plot {

namespace {

constexpr double kEpsilon = 1e-12;

double safeFrequency(double deltaTime) {
    if (std::abs(deltaTime) <= kEpsilon) {
        return 0.0;
    }
    return 1.0 / std::abs(deltaTime);
}

double sanitizeScale(double scale) {
    return std::isfinite(scale) ? scale : 1.0;
}

double sanitizeOffset(double offset) {
    return std::isfinite(offset) ? offset : 0.0;
}

} // namespace

double applyChannelDisplayTransform(double rawValue, const ChannelSpec& spec) {
    return rawValue * sanitizeScale(spec.scale) + sanitizeOffset(spec.offset);
}

void OscilloscopeBuffer::clear() {
    source_.clear();
    channels_.clear();
    config_ = ViewConfig{};
}

void OscilloscopeBuffer::configureChannels(std::size_t channelCount) {
    channels_.resize(channelCount);
    for (std::size_t index = 0; index < channels_.size(); ++index) {
        if (channels_[index].spec.label.empty()) {
            channels_[index].spec.label = "CH" + std::to_string(index + 1);
        }
    }
}

void OscilloscopeBuffer::setChannelSpec(std::size_t channelIndex, ChannelSpec spec) {
    if (channelIndex >= channels_.size()) {
        configureChannels(channelIndex + 1);
    }
    if (spec.label.empty()) {
        spec.label = "CH" + std::to_string(channelIndex + 1);
    }
    spec.scale = sanitizeScale(spec.scale);
    spec.offset = sanitizeOffset(spec.offset);
    channels_[channelIndex].spec = std::move(spec);
}

void OscilloscopeBuffer::setViewConfig(const ViewConfig& config) {
    config_ = config;
    if (config_.historyLimit == 0) {
        config_.historyLimit = 1;
    }
    if (std::abs(config_.timeScale) <= kEpsilon) {
        config_.timeScale = 1.0;
    }
}

std::size_t OscilloscopeBuffer::channelCount() const {
    return channels_.size();
}

std::optional<ChannelSpec> OscilloscopeBuffer::channelSpec(std::size_t channelIndex) const {
    if (channelIndex >= channels_.size()) {
        return std::nullopt;
    }
    return channels_[channelIndex].spec;
}

const ViewConfig& OscilloscopeBuffer::viewConfig() const {
    return config_;
}

bool OscilloscopeBuffer::append(std::size_t channelIndex, WaveAppendRequest request) {
    if (request.samples.empty()) {
        return false;
    }
    if (channelIndex >= channels_.size()) {
        configureChannels(channelIndex + 1);
    }
    auto& channel = channels_[channelIndex];
    if (!request.source.empty()) {
        source_ = std::move(request.source);
    }

    std::sort(request.samples.begin(), request.samples.end(), [](const WaveSample& left, const WaveSample& right) {
        return left.time < right.time;
    });

    if (!channel.samples.empty()) {
        const double lastTime = channel.samples.back().time;
        request.samples.erase(
            std::remove_if(request.samples.begin(), request.samples.end(), [lastTime](const WaveSample& sample) {
                return sample.time <= lastTime;
            }),
            request.samples.end());
    }

    if (request.samples.empty()) {
        return false;
    }

    channel.samples.insert(channel.samples.end(), request.samples.begin(), request.samples.end());
    trimHistory(channel);
    return true;
}

WaveSnapshot OscilloscopeBuffer::snapshot(double visibleMinTime, double visibleMaxTime) const {
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
        view.scale = channel.spec.scale;
        view.offset = channel.spec.offset;
        view.totalSamples = channel.samples.size();
        view.samples = channel.samples.data();
        view.visibleBegin = lowerBoundByTime(channel.samples, visibleMinTime);
        view.visibleEnd = upperBoundByTime(channel.samples, visibleMaxTime);
        view.stats = makeStats(channel.samples, view.visibleBegin, view.visibleEnd, channel.spec);
        snapshot.channels.push_back(view);
    }
    return snapshot;
}

EnvelopeView OscilloscopeBuffer::buildEnvelope(std::size_t channelIndex,
                                               double visibleMinTime,
                                               double visibleMaxTime,
                                               std::size_t pixelWidth) const {
    return buildLimitedEnvelope(channelIndex, visibleMinTime, visibleMaxTime, pixelWidth, 0);
}

EnvelopeView OscilloscopeBuffer::buildLimitedEnvelope(std::size_t channelIndex,
                                                      double visibleMinTime,
                                                      double visibleMaxTime,
                                                      std::size_t pixelWidth,
                                                      std::size_t maxSamples) const {
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
            const double displayValue = applyChannelDisplayTransform(samples[index].value, spec);
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
        const double bucketStart = visibleMinTime + span * static_cast<double>(bucketIndex) / static_cast<double>(bucketCount);
        const double bucketEnd = visibleMinTime + span * static_cast<double>(bucketIndex + 1) / static_cast<double>(bucketCount);

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
                const double displayValue = applyChannelDisplayTransform(sample.value, spec);
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
            const double displayValue = applyChannelDisplayTransform(samples[index].value, spec);
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

std::optional<CursorReadout> OscilloscopeBuffer::findNearest(std::size_t channelIndex,
                                                             double time,
                                                             double value,
                                                             double maxTimeDistance,
                                                             double maxValueDistance) const {
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
        const double displayValue = applyChannelDisplayTransform(sample.value, spec);
        const double dt = std::abs(sample.time - time);
        const double dv = std::abs(displayValue - value);
        if (dt > maxTimeDistance || dv > maxValueDistance) {
            continue;
        }
        const double score = dt * dt + dv * dv;
        if (score < bestScore) {
            bestScore = score;
            best = CursorReadout{
                .valid = true,
                .channelIndex = channelIndex,
                .sampleIndex = index,
                .time = sample.time,
                .value = displayValue,
            };
        }
    }
    return best;
}

std::optional<CursorReadout> OscilloscopeBuffer::findNearestByTime(std::size_t channelIndex,
                                                                   double time,
                                                                   double maxTimeDistance) const {
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
        best = CursorReadout{
            .valid = true,
            .channelIndex = channelIndex,
            .sampleIndex = index,
            .time = samples[index].time,
            .value = applyChannelDisplayTransform(samples[index].value, spec),
        };
    }
    return best;
}

MeasurementReadout OscilloscopeBuffer::measureWindow(std::size_t channelIndex,
                                                     double beginTime,
                                                     double endTime) const {
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

    result.valid = true;
    result.channelIndex = channelIndex;
    result.sampleCount = end - begin;
    result.minValue = std::numeric_limits<double>::infinity();
    result.maxValue = -std::numeric_limits<double>::infinity();

    double sum = 0.0;
    double squareSum = 0.0;
    for (std::size_t index = begin; index < end; ++index) {
        const double value = applyChannelDisplayTransform(samples[index].value, spec);
        result.minValue = (std::min)(result.minValue, value);
        result.maxValue = (std::max)(result.maxValue, value);
        sum += value;
        squareSum += value * value;
    }

    result.duration = samples[end - 1].time - samples[begin].time;
    result.peakToPeak = result.maxValue - result.minValue;
    result.meanValue = sum / static_cast<double>(result.sampleCount);
    result.rmsValue = std::sqrt(squareSum / static_cast<double>(result.sampleCount));
    return result;
}

DeltaReadout OscilloscopeBuffer::makeDelta(const CursorReadout& left, const CursorReadout& right) {
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

std::size_t OscilloscopeBuffer::lowerBoundByTime(const std::vector<WaveSample>& samples, double time) const {
    return static_cast<std::size_t>(std::distance(
        samples.begin(),
        std::lower_bound(samples.begin(), samples.end(), time, [](const WaveSample& sample, double pivot) {
            return sample.time < pivot;
        })));
}

std::size_t OscilloscopeBuffer::upperBoundByTime(const std::vector<WaveSample>& samples, double time) const {
    return static_cast<std::size_t>(std::distance(
        samples.begin(),
        std::upper_bound(samples.begin(), samples.end(), time, [](double pivot, const WaveSample& sample) {
            return pivot < sample.time;
        })));
}

void OscilloscopeBuffer::trimHistory(ChannelBuffer& channel) {
    if (channel.samples.size() <= config_.historyLimit) {
        return;
    }
    const std::size_t removeCount = channel.samples.size() - config_.historyLimit;
    channel.samples.erase(channel.samples.begin(), channel.samples.begin() + static_cast<std::ptrdiff_t>(removeCount));
}

WaveStats OscilloscopeBuffer::makeStats(const std::vector<WaveSample>& samples,
                                        std::size_t begin,
                                        std::size_t end,
                                        const ChannelSpec& spec) const {
    WaveStats stats{};
    stats.totalSamples = samples.size();
    if (begin >= end || begin >= samples.size() || end > samples.size()) {
        return stats;
    }

    stats.visibleSamples = end - begin;
    stats.minValue = std::numeric_limits<double>::infinity();
    stats.maxValue = -std::numeric_limits<double>::infinity();
    for (std::size_t index = begin; index < end; ++index) {
        const double displayValue = applyChannelDisplayTransform(samples[index].value, spec);
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
