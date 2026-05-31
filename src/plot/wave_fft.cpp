#include "protoscope/plot/wave_fft.hpp"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

namespace protoscope::plot {
namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr std::size_t kMinFftPointCount = 16;
constexpr double kMagnitudeFloor = 1e-12;

std::size_t largestPowerOfTwoAtMost(std::size_t value) {
    std::size_t result = 1;
    while (result <= value / 2) {
        result *= 2;
    }
    return result;
}

double windowWeight(WaveFftWindow window, std::size_t index, std::size_t count) {
    if (count <= 1) {
        return 1.0;
    }
    const double phase = 2.0 * kPi * static_cast<double>(index) / static_cast<double>(count - 1);
    switch (window) {
    case WaveFftWindow::Rectangular:
        return 1.0;
    case WaveFftWindow::Hann:
        return 0.5 * (1.0 - std::cos(phase));
    case WaveFftWindow::Hamming:
        return 0.54 - 0.46 * std::cos(phase);
    case WaveFftWindow::BlackmanHarris:
        return 0.35875 - 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase) - 0.01168 * std::cos(3.0 * phase);
    }
    return 1.0;
}

void fftInPlace(std::vector<std::complex<double>>& data) {
    const std::size_t count = data.size();
    for (std::size_t index = 1, reversed = 0; index < count; ++index) {
        std::size_t bit = count >> 1;
        for (; (reversed & bit) != 0; bit >>= 1) {
            reversed ^= bit;
        }
        reversed ^= bit;
        if (index < reversed) {
            std::swap(data[index], data[reversed]);
        }
    }

    for (std::size_t length = 2; length <= count; length <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(length);
        const std::complex<double> lengthStep(std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < count; offset += length) {
            std::complex<double> factor(1.0, 0.0);
            for (std::size_t inner = 0; inner < length / 2; ++inner) {
                const auto even = data[offset + inner];
                const auto odd = factor * data[offset + inner + length / 2];
                data[offset + inner] = even + odd;
                data[offset + inner + length / 2] = even - odd;
                factor *= lengthStep;
            }
        }
    }
}

std::size_t resolvePointCount(WaveFftPointCount pointCount, std::size_t visibleSampleCount, std::size_t autoMaxPointCount) {
    if (pointCount != WaveFftPointCount::Auto) {
        return fftPointCountValue(pointCount);
    }
    if (visibleSampleCount < kMinFftPointCount) {
        return 0;
    }
    const std::size_t maxAuto = (std::max)(kMinFftPointCount, largestPowerOfTwoAtMost(autoMaxPointCount));
    const std::size_t usable = (std::min)(visibleSampleCount, maxAuto);
    return largestPowerOfTwoAtMost(usable);
}

double displayMagnitude(double magnitude, WaveFftMagnitudeMode mode) {
    if (mode == WaveFftMagnitudeMode::Decibel) {
        return 20.0 * std::log10((std::max)(magnitude, kMagnitudeFloor));
    }
    return magnitude;
}

std::optional<WaveFftPeak> findFundamentalPeak(const std::vector<WaveFftBin>& bins) {
    if (bins.size() <= 1) {
        return std::nullopt;
    }
    std::size_t bestIndex = 1;
    double bestMagnitude = bins[1].magnitude;
    for (std::size_t index = 2; index < bins.size(); ++index) {
        if (bins[index].magnitude > bestMagnitude) {
            bestIndex = index;
            bestMagnitude = bins[index].magnitude;
        }
    }
    if (bestMagnitude <= 0.0 || !std::isfinite(bestMagnitude)) {
        return std::nullopt;
    }
    return WaveFftPeak{.frequencyHz = bins[bestIndex].frequencyHz, .magnitude = bestMagnitude, .binIndex = bestIndex};
}

} // namespace

bool operator==(const WaveFftConfig& lhs, const WaveFftConfig& rhs) {
    return lhs.enabled == rhs.enabled
        && lhs.pointCount == rhs.pointCount
        && lhs.window == rhs.window
        && lhs.magnitudeMode == rhs.magnitudeMode
        && lhs.fundamentalMode == rhs.fundamentalMode
        && lhs.manualFundamentalHz == rhs.manualFundamentalHz
        && lhs.autoMaxPointCount == rhs.autoMaxPointCount;
}

bool operator==(const WaveFftCacheKey& lhs, const WaveFftCacheKey& rhs) {
    return lhs.dataRevision == rhs.dataRevision
        && lhs.viewMinTime == rhs.viewMinTime
        && lhs.viewMaxTime == rhs.viewMaxTime
        && lhs.sampleFrequencyHz == rhs.sampleFrequencyHz
        && lhs.config == rhs.config
        && lhs.channelEnabled == rhs.channelEnabled;
}

std::size_t fftPointCountValue(WaveFftPointCount pointCount) {
    switch (pointCount) {
    case WaveFftPointCount::Auto:
        return 0;
    case WaveFftPointCount::N256:
        return 256;
    case WaveFftPointCount::N512:
        return 512;
    case WaveFftPointCount::N1024:
        return 1024;
    case WaveFftPointCount::N2048:
        return 2048;
    case WaveFftPointCount::N4096:
        return 4096;
    case WaveFftPointCount::N8192:
        return 8192;
    case WaveFftPointCount::N16384:
        return 16384;
    }
    return 0;
}

const char* fftPointCountName(WaveFftPointCount pointCount) {
    switch (pointCount) {
    case WaveFftPointCount::Auto:
        return "Auto";
    case WaveFftPointCount::N256:
        return "256";
    case WaveFftPointCount::N512:
        return "512";
    case WaveFftPointCount::N1024:
        return "1024";
    case WaveFftPointCount::N2048:
        return "2048";
    case WaveFftPointCount::N4096:
        return "4096";
    case WaveFftPointCount::N8192:
        return "8192";
    case WaveFftPointCount::N16384:
        return "16384";
    }
    return "Auto";
}

const char* fftWindowName(WaveFftWindow window) {
    switch (window) {
    case WaveFftWindow::Rectangular:
        return "Rectangular";
    case WaveFftWindow::Hann:
        return "Hann";
    case WaveFftWindow::Hamming:
        return "Hamming";
    case WaveFftWindow::BlackmanHarris:
        return "Blackman-Harris";
    }
    return "Hann";
}

const char* fftMagnitudeModeName(WaveFftMagnitudeMode mode) {
    switch (mode) {
    case WaveFftMagnitudeMode::Linear:
        return "Linear";
    case WaveFftMagnitudeMode::Decibel:
        return "dB";
    }
    return "Linear";
}

const char* fftFundamentalModeName(WaveFftFundamentalMode mode) {
    switch (mode) {
    case WaveFftFundamentalMode::Auto:
        return "Auto";
    case WaveFftFundamentalMode::Manual:
        return "Manual";
    }
    return "Auto";
}

WaveFftFrame buildWaveFftFrame(const WaveSnapshot& snapshot,
                               const WaveDisplayData& displayData,
                               const WaveFftConfig& config,
                               const std::vector<std::uint8_t>& channelEnabled,
                               double viewMinTime,
                               double viewMaxTime,
                               double sampleFrequencyHz) {
    WaveFftFrame frame{};
    frame.enabled = config.enabled;
    frame.sampleFrequencyHz = sampleFrequencyHz;
    if (!config.enabled) {
        frame.message = "FFT 未启用";
        return frame;
    }
    if (sampleFrequencyHz <= 0.0 || !std::isfinite(sampleFrequencyHz)) {
        frame.message = "需要先设置有效采样频率";
        return frame;
    }
    if (viewMaxTime < viewMinTime) {
        std::swap(viewMinTime, viewMaxTime);
    }
    if (viewMaxTime <= viewMinTime) {
        frame.message = "当前可视区时间范围无效";
        return frame;
    }

    const std::size_t channelCount = (std::min)(snapshot.channels.size(), displayData.channels.size());
    frame.channels.reserve(channelCount);
    for (std::size_t channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
        const bool enabled = channelIndex < channelEnabled.size() && channelEnabled[channelIndex] != 0;
        WaveFftChannelResult result{};
        result.channelIndex = channelIndex;
        result.label = snapshot.channels[channelIndex].label;
        result.unit = snapshot.channels[channelIndex].unit;
        result.enabled = enabled;
        if (!enabled) {
            frame.channels.push_back(std::move(result));
            continue;
        }

        const auto& displayChannel = displayData.channels[channelIndex];
        const std::size_t sampleCount = (std::min)(displayChannel.samples.size(), displayChannel.actualValues.size());
        std::vector<double> values;
        values.reserve(sampleCount);
        for (std::size_t sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex) {
            const double time = displayChannel.samples[sampleIndex].time;
            if (time < viewMinTime || time > viewMaxTime) {
                continue;
            }
            const double value = displayChannel.actualValues[sampleIndex];
            if (std::isfinite(value)) {
                values.push_back(value);
            }
        }
        result.visibleSampleCount = values.size();
        frame.visibleSampleCount = (std::max)(frame.visibleSampleCount, result.visibleSampleCount);

        const std::size_t pointCount = resolvePointCount(config.pointCount, values.size(), config.autoMaxPointCount);
        if (pointCount < kMinFftPointCount || values.size() < pointCount) {
            result.message = "当前可视区样本不足";
            frame.channels.push_back(std::move(result));
            continue;
        }

        values.erase(values.begin(), values.end() - static_cast<std::ptrdiff_t>(pointCount));
        std::vector<std::complex<double>> fftData(pointCount);
        double coherentGain = 0.0;
        for (std::size_t index = 0; index < pointCount; ++index) {
            const double weight = windowWeight(config.window, index, pointCount);
            coherentGain += weight;
            fftData[index] = std::complex<double>(values[index] * weight, 0.0);
        }
        coherentGain /= static_cast<double>(pointCount);
        if (coherentGain <= 0.0 || !std::isfinite(coherentGain)) {
            coherentGain = 1.0;
        }

        fftInPlace(fftData);
        result.usedSampleCount = pointCount;
        frame.usedSampleCount = (std::max)(frame.usedSampleCount, result.usedSampleCount);
        frame.pointCount = (std::max)(frame.pointCount, pointCount);
        frame.frequencyResolutionHz = sampleFrequencyHz / static_cast<double>(pointCount);
        frame.maxFrequencyHz = sampleFrequencyHz * 0.5;

        const std::size_t binCount = pointCount / 2 + 1;
        result.bins.reserve(binCount);
        for (std::size_t binIndex = 0; binIndex < binCount; ++binIndex) {
            double magnitude = std::abs(fftData[binIndex]) / (static_cast<double>(pointCount) * coherentGain);
            if (binIndex > 0 && binIndex + 1 < binCount) {
                magnitude *= 2.0;
            }
            const double shownMagnitude = displayMagnitude(magnitude, config.magnitudeMode);
            result.bins.push_back({
                .frequencyHz = static_cast<double>(binIndex) * frame.frequencyResolutionHz,
                .magnitude = magnitude,
                .displayMagnitude = shownMagnitude,
            });
            if (result.bins.size() == 1 && frame.channels.empty()) {
                frame.minDisplayMagnitude = shownMagnitude;
                frame.maxDisplayMagnitude = shownMagnitude;
            } else {
                frame.minDisplayMagnitude = (std::min)(frame.minDisplayMagnitude, shownMagnitude);
                frame.maxDisplayMagnitude = (std::max)(frame.maxDisplayMagnitude, shownMagnitude);
            }
        }

        result.valid = !result.bins.empty();
        result.fundamental = config.fundamentalMode == WaveFftFundamentalMode::Manual && config.manualFundamentalHz > 0.0
            ? std::optional<WaveFftPeak>(WaveFftPeak{.frequencyHz = config.manualFundamentalHz, .magnitude = 0.0, .binIndex = 0})
            : findFundamentalPeak(result.bins);
        if (!frame.fundamentalHz.has_value() && result.fundamental.has_value()) {
            frame.fundamentalHz = result.fundamental->frequencyHz;
        }
        frame.valid = true;
        frame.channels.push_back(std::move(result));
    }

    if (!frame.valid && frame.message.empty()) {
        frame.message = "没有可计算 FFT 的通道";
    }
    if (frame.maxDisplayMagnitude <= frame.minDisplayMagnitude) {
        frame.maxDisplayMagnitude = frame.minDisplayMagnitude + 1.0;
    }
    return frame;
}

} // namespace protoscope::plot
