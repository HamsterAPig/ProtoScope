#pragma once

#include "protoscope/plot/oscilloscope.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::plot {

enum class WaveTimeAxisSource {
    ScriptTime,
    SampleFrequency,
    SampleIndex,
};

enum class WaveZoomMode {
    XOnly,
    YOnly,
    XY,
};

struct FrequencyParseResult {
    bool accepted{false};
    double valueHz{0.0};
    std::string error;
};

struct WaveDisplayChannel {
    std::vector<WaveSample> samples;
};

struct WaveDisplayData {
    WaveTimeAxisSource axisSource{WaveTimeAxisSource::SampleIndex};
    std::string timeUnit{"sample"};
    std::vector<WaveDisplayChannel> channels;
};

struct WaveViewport {
    double minTime{0.0};
    double maxTime{1.0};
    double minValue{-1.0};
    double maxValue{1.0};
};

struct WaveDataBounds {
    double minTime{0.0};
    double maxTime{1.0};
    double minValue{-1.0};
    double maxValue{1.0};
    double minStep{1.0};
    bool valid{false};
};

struct CursorIntervalText {
    bool valid{false};
    bool showFrequency{false};
    double delta{0.0};
    double frequencyHz{0.0};
    std::string deltaUnit{"sample"};
};

FrequencyParseResult parseSampleFrequencyText(std::string_view text);
bool scriptTimeUsable(const std::vector<WaveSample>& samples);
WaveDisplayData buildDisplayData(const WaveSnapshot& snapshot, double sampleFrequencyHz);
WaveDataBounds computeDisplayBounds(const WaveDisplayData& data, double fallbackStep);
WaveViewport normalizeOverviewViewport(const WaveViewport& viewport,
                                       const WaveDataBounds& bounds,
                                       double minTimeWidth);
WaveViewport zoomViewport(const WaveViewport& viewport,
                          WaveZoomMode mode,
                          double wheelDelta,
                          double centerTime,
                          double centerValue,
                          const WaveDataBounds& bounds,
                          double minTimeWidth,
                          bool clampTimeToBounds);
CursorIntervalText makeCursorIntervalText(const CursorReadout& left,
                                          const CursorReadout& right,
                                          WaveTimeAxisSource axisSource,
                                          std::string_view timeUnit);
void lockCursorInterval(double movedTime, double& pairedTime, double lockedInterval, bool movedLeftCursor);

} // namespace protoscope::plot
