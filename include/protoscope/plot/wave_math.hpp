#pragma once

#include "protoscope/plot/oscilloscope.hpp"

#include <cstddef>
#include <optional>
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

enum class WaveChannelCardWidthMode {
    Fixed,
    Adaptive,
};

enum class WaveChannelDoubleClickAction {
    ResetAll,
    ResetScaleOffset,
    ResetScale,
    ResetOffset,
};

enum class WaveXAxisDoubleClickAction {
    FitFullHistory,
    FitVisibleWindow,
};

enum class WaveHiddenChannelPolicy {
    IncludeInDerivedViews,
    ExcludeFromDerivedViews,
};

enum class WaveGridDivisionReadoutMode {
    DisplayValue,
    ActualValue,
    RawValue,
};

enum class WaveExtremeKind {
    Maximum,
    Minimum,
};

inline constexpr int kWaveGridMajorXDivisions{10};
inline constexpr int kWaveGridMajorYDivisions{8};
inline constexpr int kWaveGridMinorDivisionsPerMajor{5};

struct FrequencyParseResult {
    bool accepted{false};
    double valueHz{0.0};
    std::string error;
};

struct WaveDisplayChannel {
    std::vector<WaveSample> samples;
    std::vector<double> actualValues;
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

struct WaveValueRange {
    double minValue{-1.0};
    double maxValue{1.0};
};

struct CursorIntervalText {
    bool valid{false};
    bool showFrequency{false};
    double delta{0.0};
    double frequencyHz{0.0};
    std::string deltaUnit{"sample"};
};

struct WaveLayoutSizes {
    float overviewHeight{0.0F};
    float mainHeight{0.0F};
    float toolsWidth{0.0F};
};

FrequencyParseResult parseSampleFrequencyText(std::string_view text);
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
                                float fixedContentHeight);
float solveSplitWavePlotHeight(std::size_t visibleChannelCount,
                               float availableHeight,
                               float rowSpacingY,
                               float preferredMinPlotHeight,
                               std::size_t maxRowsWithoutScroll);
bool scriptTimeUsable(const std::vector<WaveSample>& samples);
void buildDisplayDataInto(const WaveSnapshot& snapshot, double sampleFrequencyHz, WaveDisplayData& data);
WaveDisplayData buildDisplayData(const WaveSnapshot& snapshot, double sampleFrequencyHz);
void applySampleFrequencyVisibleRange(WaveSnapshot& snapshot, double minTime, double maxTime, double sampleFrequencyHz);
WaveDataBounds computeDisplayBounds(const WaveDisplayData& data, double fallbackStep);
WaveDataBounds computeDisplayBoundsForChannels(const WaveDisplayData& data,
                                               const std::vector<std::size_t>& channelIndices,
                                               double fallbackStep);
const WaveDataBounds& selectXAxisDoubleClickBounds(WaveXAxisDoubleClickAction action,
                                                   const WaveDataBounds& visibleWindowBounds,
                                                   const WaveDataBounds& fullHistoryBounds);
double waveDisplayValuePerDivision(double minValue, double maxValue);
std::optional<double> waveChannelValuePerDivision(double displayValuePerDivision,
                                                  const ChannelSpec& spec,
                                                  WaveDisplayFormula formula,
                                                  WaveGridDivisionReadoutMode mode);
std::optional<CursorReadout> findNearestDisplayByTime(const WaveDisplayData& displayData,
                                                      std::size_t channelIndex,
                                                      double time,
                                                      double maxTimeDistance);
std::optional<CursorReadout> findNearestDisplayByTimeAcrossChannels(const WaveDisplayData& displayData,
                                                                    double time,
                                                                    double maxTimeDistance);
std::optional<CursorReadout> findNearestDisplayPoint(
    const WaveDisplayData& displayData, double time, double value, double maxTimeDistance, double maxValueDistance);
std::optional<CursorReadout> findNearestDisplayPointInChannels(const WaveDisplayData& displayData,
                                                               const std::vector<std::size_t>& channelIndices,
                                                               double time,
                                                               double value,
                                                               double maxTimeDistance,
                                                               double maxValueDistance);
WaveViewport normalizeOverviewViewport(const WaveViewport& viewport, const WaveDataBounds& bounds, double minTimeWidth);
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
std::optional<CursorReadout> findStrongestEdgeNearTime(const WaveDisplayData& displayData,
                                                       std::size_t channelIndex,
                                                       double centerTime,
                                                       double maxTimeDistance);
std::optional<CursorReadout> findLocalExtremeNearTime(const WaveDisplayData& displayData,
                                                      std::size_t channelIndex,
                                                      double centerTime,
                                                      double maxTimeDistance,
                                                      WaveExtremeKind kind);
double applyCursorDragSnap(double dragTime, const std::optional<CursorReadout>& smartSnap);
void lockCursorInterval(double movedTime, double& pairedTime, double lockedInterval, bool movedLeftCursor);
WaveViewport moveViewportByDelta(const WaveViewport& viewport,
                                 double deltaTime,
                                 const WaveDataBounds& bounds,
                                 double minTimeWidth);
double cursorTimeInViewport(const WaveViewport& viewport, double ratio);
double resolveChannelCardWidth(WaveChannelCardWidthMode mode,
                               double fixedWidth,
                               double adaptiveRatio,
                               double availableWidth);
WaveValueRange makeVerticalAutoFitRange(double minValue, double maxValue, double multiplier);

} // namespace protoscope::plot
