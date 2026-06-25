#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/wave_math.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::plot {

enum class WaveFftPointCount {
    VisibleSamples,
    Auto,
    Manual,
    N256,
    N512,
    N1024,
    N2048,
    N4096,
    N8192,
    N16384,
};

enum class WaveFftWindow {
    Rectangular,
    Hann,
    Hamming,
    BlackmanHarris,
};

enum class WaveFftMagnitudeMode {
    Linear,
    Decibel,
};

enum class WaveFftFundamentalMode {
    Auto,
    Manual,
};

enum class WaveFftDisplayMode {
    FullSpectrum,
    CursorSplit,
};

struct WaveFftConfig {
    bool enabled{false};
    WaveFftDisplayMode displayMode{WaveFftDisplayMode::FullSpectrum};
    WaveFftPointCount pointCount{WaveFftPointCount::VisibleSamples};
    WaveFftWindow window{WaveFftWindow::Hann};
    WaveFftMagnitudeMode magnitudeMode{WaveFftMagnitudeMode::Linear};
    WaveFftFundamentalMode fundamentalMode{WaveFftFundamentalMode::Auto};
    double manualFundamentalHz{0.0};
    std::size_t manualPointCount{1000};
    std::size_t autoMaxPointCount{4096};
};

struct WaveFftBin {
    double frequencyHz{0.0};
    double magnitude{0.0};
    double displayMagnitude{0.0};
    double phaseRadians{0.0};
    double phaseDegrees{0.0};
};

struct WaveFftPeak {
    double frequencyHz{0.0};
    double magnitude{0.0};
    std::size_t binIndex{0};
};

struct WaveFftChannelResult {
    std::size_t channelIndex{0};
    std::string label;
    std::string unit;
    std::vector<WaveFftBin> bins;
    std::optional<WaveFftPeak> fundamental;
    std::size_t visibleSampleCount{0};
    std::size_t usedSampleCount{0};
    bool enabled{false};
    bool valid{false};
    std::string message;
};

struct WaveFftFrame {
    bool enabled{false};
    bool valid{false};
    double sampleFrequencyHz{0.0};
    double frequencyResolutionHz{0.0};
    double maxFrequencyHz{0.0};
    std::size_t pointCount{0};
    std::size_t visibleSampleCount{0};
    std::size_t usedSampleCount{0};
    double minDisplayMagnitude{0.0};
    double maxDisplayMagnitude{1.0};
    double minPhaseDegrees{-180.0};
    double maxPhaseDegrees{180.0};
    std::optional<double> fundamentalHz;
    std::vector<WaveFftChannelResult> channels;
    std::string message;
};

struct WaveFftCursorWindow {
    double minTime{0.0};
    double maxTime{0.0};
    std::size_t pointCount{0};
    double durationSeconds{0.0};
};

struct WaveFftReadout {
    bool valid{false};
    std::size_t channelIndex{0};
    std::size_t binIndex{0};
    double frequencyHz{0.0};
    double magnitude{0.0};
    double displayMagnitude{0.0};
    double phaseDegrees{0.0};
};

struct WaveFftViewport {
    double frequencyMin{0.0};
    double frequencyMax{1.0};
    double magnitudeMin{0.0};
    double magnitudeMax{1.0};
    double phaseMin{-180.0};
    double phaseMax{180.0};
};

struct WaveFftCacheKey {
    std::uint64_t dataRevision{0};
    double viewMinTime{0.0};
    double viewMaxTime{0.0};
    double sampleFrequencyHz{0.0};
    WaveFftConfig config{};
    std::vector<std::uint8_t> channelEnabled;
};

bool operator==(const WaveFftConfig& lhs, const WaveFftConfig& rhs);
bool operator==(const WaveFftCacheKey& lhs, const WaveFftCacheKey& rhs);

std::size_t fftPointCountValue(WaveFftPointCount pointCount);
const char* fftPointCountName(WaveFftPointCount pointCount);
const char* fftWindowName(WaveFftWindow window);
const char* fftMagnitudeModeName(WaveFftMagnitudeMode mode);
const char* fftFundamentalModeName(WaveFftFundamentalMode mode);
const char* fftDisplayModeName(WaveFftDisplayMode mode);
std::size_t resolveWaveFftPointCount(const WaveFftConfig& config, std::size_t visibleSampleCount);
std::optional<WaveFftCursorWindow> resolveWaveFftCursorWindow(const WaveFftConfig& config,
                                                              std::size_t visibleSampleCount,
                                                              double sampleFrequencyHz,
                                                              double rightCursorTime);

WaveFftFrame buildWaveFftFrame(const WaveSnapshot& snapshot,
                               const WaveDisplayData& displayData,
                               const WaveFftConfig& config,
                               const std::vector<std::uint8_t>& channelEnabled,
                               double viewMinTime,
                               double viewMaxTime,
                               double sampleFrequencyHz);
WaveFftViewport makeFftFitViewport(const WaveFftFrame& frame);
std::optional<WaveFftReadout> findNearestFftBin(const WaveFftFrame& frame,
                                                std::size_t channelIndex,
                                                double frequencyHz);
std::optional<WaveFftReadout> findNearestFftBinAcrossChannels(const WaveFftFrame& frame,
                                                              double frequencyHz,
                                                              double displayMagnitude,
                                                              double maxFrequencyDistance,
                                                              double maxMagnitudeDistance);

} // namespace protoscope::plot
