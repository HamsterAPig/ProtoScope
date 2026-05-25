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
        auto& display = data.channels[channelIndex].samples;
        if (channel.samples == nullptr || channel.totalSamples == 0) {
            continue;
        }
        display.reserve(channel.totalSamples);
        for (std::size_t sampleIndex = 0; sampleIndex < channel.totalSamples; ++sampleIndex) {
            const auto& source = channel.samples[sampleIndex];
            double time = static_cast<double>(sampleIndex);
            if (data.axisSource == WaveTimeAxisSource::SampleFrequency) {
                time = static_cast<double>(sampleIndex) / sampleFrequencyHz;
            } else if (data.axisSource == WaveTimeAxisSource::ScriptTime) {
                time = source.time;
            }
            display.push_back({.time = time, .value = source.value});
        }
    }
    return data;
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

void lockCursorInterval(double movedTime, double& pairedTime, double lockedInterval, bool movedLeftCursor) {
    if (!std::isfinite(lockedInterval) || lockedInterval <= 0.0) {
        return;
    }
    pairedTime = movedLeftCursor ? movedTime + lockedInterval : movedTime - lockedInterval;
}

} // namespace protoscope::plot
