#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/plot/wave_fft.hpp"
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

enum class WaveMeasurementReferenceMode {
    Channel,
    ManualValue,
};

struct WaveCursorState {
    bool enabled{true};
    bool pinned{false};
    std::size_t channelIndex{0};
    double time{0.0};
    double value{0.0};
};

struct WaveMeasurementSelection {
    bool cursorA{true};
    bool cursorB{true};
    bool deltaTime{true};
    bool deltaValue{true};
    bool frequency{true};
    bool period{true};
    bool sampleCount{true};
    bool span{true};
    bool min{true};
    bool max{true};
    bool peakToPeak{true};
    bool mean{true};
    bool rms{true};
    bool median{false};
    bool p95{false};
    bool p99{false};
    bool variance{false};
    bool stddev{true};
    bool cv{false};
    bool mad{false};
    bool medianAbsDev{false};
    bool iqr{false};
    bool p95Spread{false};
    bool highWidth{false};
    bool lowWidth{false};
    bool dutyCycle{false};
    bool riseTime{false};
    bool fallTime{false};
    bool edgeCount{false};
    bool absoluteError{false};
    bool relativeErrorPercent{false};
    bool meanError{false};
    bool mse{false};
    bool rmse{false};
    bool mae{false};
    bool maxAbsError{false};
    bool bias{false};
};

struct WaveViewState {
    bool autoFollowLatest{true};
    bool pauseAutoFollowOnInteraction{true};
    bool lockVerticalRange{false};
    bool showPointsWhenSparse{true};
    bool showAxisLabels{false};
    bool showChannelLegend{true};
    bool showFftLegend{true};
    bool showHoverReadout{true};
    bool showCursors{true};
    bool showMeasurementOverlay{true};
    bool phosphorGlowEnabled{true};
    bool initialized{false};
    bool cursorIntervalLocked{false};
    bool overviewWindowDragging{false};
    bool forceNextMainPlotLimits{false};
    bool activeChannelOffsetDrag{false};
    bool activeChannelScaleDrag{false};
    bool zoomSelectionActive{false};
    bool zoomSelectionDragging{false};
    bool zoomSelectionAutoExit{false};
    bool fitVisibleWaveformsRequested{false};
    std::size_t maxRenderPointsPerChannel{1200};
    std::size_t maxRenderVertices{60000};
    std::size_t overviewMaxSamples{20000};
    std::size_t lastRenderPointCount{0};
    std::size_t lastRenderSourceSampleCount{0};
    std::size_t measurementChannelIndex{0};
    std::size_t referenceChannelIndex{0};
    WaveMeasurementReferenceMode referenceMode{WaveMeasurementReferenceMode::Channel};
    WaveMeasurementSelection measurement{};
    WaveControlMode controlMode{WaveControlMode::Oscilloscope};
    WaveDisplayFormula displayFormula{WaveDisplayFormula::OffsetThenScale};
    WaveChannelCardWidthMode channelCardWidthMode{WaveChannelCardWidthMode::Fixed};
    WaveChannelDoubleClickAction channelDoubleClickAction{WaveChannelDoubleClickAction::ResetScaleOffset};
    WaveHiddenChannelPolicy hiddenChannelPolicy{WaveHiddenChannelPolicy::ExcludeFromDerivedViews};
    WaveFftConfig fft{};
    bool fftSourceWindowValid{false};
    bool fftViewportInitialized{false};
    bool fftFitAllRequested{false};
    double visibleDuration{1.0};
    double minVisibleTimeSpan{0.001};
    double downsampleStartMultiplier{2.0};
    double channelCardFixedWidth{128.0};
    double channelCardAdaptiveRatio{0.22};
    double verticalAutoFitMultiplier{1.2};
    double persistenceWindow{0.25};
    double glowIntensity{1.0};
    double sampleFrequencyHz{0.0};
    double manualReferenceValue{0.0};
    double lockedCursorInterval{0.0};
    double overviewDragLastTime{0.0};
    double centerTime{0.0};
    double viewMinTime{0.0};
    double viewMaxTime{1.0};
    double fftSourceMinTime{0.0};
    double fftSourceMaxTime{1.0};
    double fftFrequencyMin{0.0};
    double fftFrequencyMax{1.0};
    double fftMagnitudeMin{0.0};
    double fftMagnitudeMax{1.0};
    double fftPhaseMin{-180.0};
    double fftPhaseMax{180.0};
    double manualVerticalMin{-1.0};
    double manualVerticalMax{1.0};
    double viewMinValue{-1.0};
    double viewMaxValue{1.0};
    double zoomSelectionStartX{0.0};
    double zoomSelectionStartY{0.0};
    double zoomSelectionCurrentX{0.0};
    double zoomSelectionCurrentY{0.0};
    std::string sampleFrequencyInput;
    std::string sampleFrequencyError;
    WaveTimeAxisSource timeAxisSource{WaveTimeAxisSource::SampleIndex};
    WaveCursorSnapMode cursorSnapMode{WaveCursorSnapMode::SmartSnap};
    WaveCursorSnapScope cursorSnapScope{WaveCursorSnapScope::AllChannels};
    std::array<WaveCursorState, 2> cursors{};
};

struct WaveDockState {
    struct ChannelTransformOverride {
        bool labelOverridden{false};
        bool ratioOverridden{false};
        bool scaleOverridden{false};
        bool offsetOverridden{false};
        std::string label;
        double ratio{1.0};
        double scale{1.0};
        double offset{0.0};
    };

    OscilloscopeBuffer buffer{};
    RawCaptureFileData rawCapture{};
    WaveViewState view{};
    std::string statusMessage;
    std::vector<std::string> channelSummaries;
    std::vector<ChannelSpec> defaultChannelSpecs;
    std::vector<ChannelTransformOverride> channelOverrides;
    std::vector<std::string> hiddenChannelLabels;
    std::vector<std::uint8_t> fftChannelEnabled;
    bool toolsCollapsed{false};
    bool overviewCollapsed{false};
    bool legendCollapsed{false};
    bool legendVisibilityRestorePending{false};
    float toolsExpandedWidth{280.0F};
    float toolsCollapsedWidth{34.0F};
    float overviewPanelHeight{120.0F};
    float overviewCollapsedHeight{30.0F};
    float contentToolsSplitterWidth{6.0F};
    float overviewMainSplitterHeight{6.0F};
    float minOverviewPanelHeight{32.0F};
    float minMainPanelHeight{160.0F};
    float minToolsExpandedWidth{220.0F};
    float maxToolsExpandedWidth{520.0F};
    std::uint64_t displayDataRevision{0};
    double displayDataSampleFrequencyHz{0.0};
    std::size_t lastLegendMeasurementChannelIndex{static_cast<std::size_t>(-1)};
    WaveSnapshot cachedFullSnapshot{};
    WaveDisplayData cachedDisplayData{};
    WaveDisplayData cachedOverviewDisplayData{};
    WaveDataBounds cachedDisplayBounds{};
    bool cachedFftKeyValid{false};
    WaveFftCacheKey cachedFftKey{};
    WaveFftFrame cachedFftFrame{};
};

bool resetChannelConfigToDefault(WaveDockState& wave,
                                 std::size_t channelIndex,
                                 const ChannelSpec& defaultSpec,
                                 WaveChannelDoubleClickAction action);
bool resetChannelConfigToDefault(WaveDockState& wave, std::size_t channelIndex, WaveChannelDoubleClickAction action);
bool resetChannelOffsetToDefault(WaveDockState& wave, std::size_t channelIndex);

} // namespace protoscope::plot
