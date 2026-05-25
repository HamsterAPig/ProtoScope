#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/wave_math.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace protoscope::plot {

struct WaveCursorState {
    bool enabled{true};
    bool pinned{false};
    std::size_t channelIndex{0};
    double time{0.0};
    double value{0.0};
};

struct WaveViewState {
    bool autoFollowLatest{true};
    bool pauseAutoFollowOnInteraction{true};
    bool lockVerticalRange{false};
    bool showPointsWhenSparse{true};
    bool showHoverReadout{true};
    bool showCursors{true};
    bool phosphorGlowEnabled{true};
    bool initialized{false};
    bool cursorIntervalLocked{false};
    bool overviewWindowDragging{false};
    bool forceNextMainPlotLimits{false};
    std::size_t overviewMaxSamples{20000};
    std::size_t measurementChannelIndex{0};
    double visibleDuration{1.0};
    double persistenceWindow{0.25};
    double glowIntensity{1.0};
    double sampleFrequencyHz{0.0};
    double lockedCursorInterval{0.0};
    double overviewDragLastTime{0.0};
    double centerTime{0.0};
    double viewMinTime{0.0};
    double viewMaxTime{1.0};
    double manualVerticalMin{-1.0};
    double manualVerticalMax{1.0};
    double viewMinValue{-1.0};
    double viewMaxValue{1.0};
    std::string sampleFrequencyInput;
    std::string sampleFrequencyError;
    WaveTimeAxisSource timeAxisSource{WaveTimeAxisSource::SampleIndex};
    std::array<WaveCursorState, 2> cursors{};
};

struct WaveDockState {
    OscilloscopeBuffer buffer{};
    WaveViewState view{};
    std::string statusMessage;
    std::vector<std::string> channelSummaries;
};

} // namespace protoscope::plot
