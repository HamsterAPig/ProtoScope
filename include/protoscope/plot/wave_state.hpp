#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/wave_math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace protoscope::plot {

enum class WaveCursorSnapMode {
    SmartSnap,
    ModifierSnap,
};

enum class WaveCursorSnapScope {
    AllChannels,
    ActiveChannel,
};

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
    bool showMeasurementOverlay{true};
    bool phosphorGlowEnabled{true};
    bool initialized{false};
    bool cursorIntervalLocked{false};
    bool overviewWindowDragging{false};
    bool forceNextMainPlotLimits{false};
    std::size_t maxRenderPointsPerChannel{1200};
    std::size_t maxRenderVertices{60000};
    std::size_t overviewMaxSamples{20000};
    std::size_t lastRenderPointCount{0};
    std::size_t lastRenderSourceSampleCount{0};
    std::size_t measurementChannelIndex{0};
    double visibleDuration{1.0};
    double minVisibleTimeSpan{0.001};
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
    WaveCursorSnapMode cursorSnapMode{WaveCursorSnapMode::SmartSnap};
    WaveCursorSnapScope cursorSnapScope{WaveCursorSnapScope::AllChannels};
    std::array<WaveCursorState, 2> cursors{};
};

struct WaveDockState {
    struct ChannelTransformOverride {
        bool scaleOverridden{false};
        bool offsetOverridden{false};
        double scale{1.0};
        double offset{0.0};
    };

    OscilloscopeBuffer buffer{};
    WaveViewState view{};
    std::string statusMessage;
    std::vector<std::string> channelSummaries;
    std::vector<ChannelSpec> defaultChannelSpecs;
    std::vector<ChannelTransformOverride> channelOverrides;
    bool toolsCollapsed{false};
    float toolsExpandedWidth{280.0F};
    float toolsCollapsedWidth{34.0F};
    float overviewPanelHeight{120.0F};
    float contentToolsSplitterWidth{6.0F};
    float overviewMainSplitterHeight{6.0F};
    float minOverviewPanelHeight{72.0F};
    float minMainPanelHeight{160.0F};
    float minToolsExpandedWidth{220.0F};
    float maxToolsExpandedWidth{520.0F};
    std::uint64_t displayDataRevision{0};
    double displayDataSampleFrequencyHz{0.0};
    WaveSnapshot cachedFullSnapshot{};
    WaveDisplayData cachedDisplayData{};
    WaveDataBounds cachedDisplayBounds{};
};

} // namespace protoscope::plot
