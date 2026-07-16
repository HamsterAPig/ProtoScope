#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/plot/wave_fft.hpp"
#include "protoscope/plot/wave_math.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
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

enum class WaveCursorExtremeSnapPolicy {
    NearestWaveform,
    ViewportZone,
};

enum class WaveBitDisplayReadoutPolicy {
    MixedNearest,
    ExplicitActivation,
};

enum class WaveMeasurementReferenceMode {
    Channel,
    ManualValue,
};

enum class WaveViewMode {
    Overlay,
    Stacked,
    Split,
};

enum class WavePhosphorBackend {
    Auto,
    GpuFbo,
    CpuTexture,
};

enum class WavePhosphorMode {
    FreeRun,
    Triggered,
};

enum class WavePhosphorTriggerEdge {
    Rising,
    Falling,
};

enum class WaveLegendOverlayOpenMode {
    Hover,
    DoubleClick,
    Disabled,
};

struct WaveLegendOverlayState {
    bool expanded{false};
    bool hoverFloating{false};
    bool hoverInteractionLocked{false};
    WaveLegendOverlayOpenMode openMode{WaveLegendOverlayOpenMode::Hover};
    bool doubleClickAutoCollapse{true};
    float hoverCloseDelaySec{0.30F};
    float hoverCloseRemainingSec{0.0F};
    float offsetX{8.0F};
    float offsetY{8.0F};
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

struct ActiveBitLaneState {
    bool active{false};
    std::size_t parentChannelIndex{0};
    std::size_t bitIndex{0};
    std::size_t laneIndex{0};
};

enum class WaveMouseYOffsetDragMode {
    Direct,
    Shift,
    Disabled,
};

enum class WaveResetViewportScaleMode {
    Preserve,
    ProtocolDefault,
};

enum class WaveResetViewportAnchor {
    WaveStart,
    Latest,
};

enum class WaveResetViewportAutoFollowMode {
    Existing,
    Enable,
    Disable,
};

struct WaveRenderStats {
    std::size_t rawChannelCount{0};
    std::size_t peakDownsampleChannelCount{0};
    std::size_t envelopeDownsampleChannelCount{0};
    std::size_t phosphorChannelCount{0};
    std::size_t bitLaneChannelCount{0};
    std::size_t lastRenderPointBudget{0};
    std::size_t lastDownsampleThreshold{0};
    std::string phosphorBackendStatus{"关闭"};
};

struct WaveViewportAnimationState {
    bool active{false};
    WaveViewport start{};
    WaveViewport target{};
    double elapsedSec{0.0};
    double durationSec{0.16};
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
    bool preferWaveformHoverReadout{true};
    bool showCursorIntersectionReadouts{false};
    WaveBitDisplayReadoutPolicy bitDisplayReadoutPolicy{WaveBitDisplayReadoutPolicy::MixedNearest};
    bool showCursors{true};
    bool followMeasurementCursorsOnScroll{false};
    bool measurementCursorReadoutRefreshPending{false};
    bool showMeasurementOverlay{true};
    bool glowEnabled{true};
    bool interactionAnimationEnabled{true};
    bool effectiveInteractionAnimationEnabled{true};
    bool phosphorEnabled{false};
    bool initialized{false};
    bool cursorIntervalLocked{false};
    bool overviewWindowDragging{false};
    bool forceNextMainPlotLimits{false};
    bool activeChannelOffsetDrag{false};
    bool activeChannelScaleDrag{false};
    bool activeBitYOffsetDrag{false};
    bool activeFftMagnitudeOffsetDrag{false};
    bool zoomSelectionActive{false};
    bool zoomSelectionDragging{false};
    bool zoomSelectionAutoExit{false};
    bool fftMagnitudeAutoFitIgnoreFundamental{false};
    bool peakDetectDownsample{true};
    bool fitVisibleWaveformsRequested{false};
    bool defaultViewportPending{true};
    bool defaultViewportLegacyBehavior{false};
    bool resetViewportApplyOnPlotSetupReset{true};
    bool resetViewportApplyOnManualClear{true};
    bool resetViewportApplyOnRawImport{true};
    WaveResetViewportScaleMode defaultViewportXScale{WaveResetViewportScaleMode::Preserve};
    WaveResetViewportScaleMode defaultViewportYScale{WaveResetViewportScaleMode::Preserve};
    WaveResetViewportAnchor defaultViewportXAnchor{WaveResetViewportAnchor::WaveStart};
    WaveResetViewportAutoFollowMode defaultViewportAutoFollow{WaveResetViewportAutoFollowMode::Existing};
    std::size_t maxRenderPointsPerChannel{1200};
    std::size_t maxRenderVertices{60000};
    std::size_t overviewMaxSamples{20000};
    // 自适应性能只覆盖本帧预算，保存配置时仍使用上面的用户配置。
    std::optional<std::size_t> adaptiveMaxRenderPointsPerChannel{};
    std::optional<std::size_t> adaptiveMaxRenderVertices{};
    std::optional<std::size_t> adaptiveOverviewMaxSamples{};
    std::size_t lastRenderPointCount{0};
    std::size_t lastRenderSourceSampleCount{0};
    std::size_t measurementChannelIndex{0};
    std::size_t activeFftMagnitudeOffsetDragChannelIndex{0};
    std::size_t lastCursorFftAnchorIndex{1};
    ActiveBitLaneState activeBitLane{};
    std::size_t referenceChannelIndex{0};
    std::size_t triggerChannelIndex{0};
    WaveMeasurementReferenceMode referenceMode{WaveMeasurementReferenceMode::Channel};
    WaveMeasurementSelection measurement{};
    WaveMouseYOffsetDragMode mouseYOffsetDragMode{WaveMouseYOffsetDragMode::Direct};
    WaveControlMode controlMode{WaveControlMode::Oscilloscope};
    WaveDisplayFormula displayFormula{WaveDisplayFormula::OffsetThenScale};
    WaveGridDivisionReadoutMode gridDivisionReadoutMode{WaveGridDivisionReadoutMode::DisplayValue};
    WaveChannelCardWidthMode channelCardWidthMode{WaveChannelCardWidthMode::Fixed};
    WaveChannelDoubleClickAction channelDoubleClickAction{WaveChannelDoubleClickAction::ResetScaleOffset};
    WaveXAxisDoubleClickAction xAxisDoubleClickAction{WaveXAxisDoubleClickAction::FitFullHistory};
    WaveYAxisDoubleClickAction yAxisDoubleClickAction{WaveYAxisDoubleClickAction::FitVisibleChannels};
    bool yAxisDoubleClickAdjustOffset{true};
    WaveHiddenChannelPolicy hiddenChannelPolicy{WaveHiddenChannelPolicy::ExcludeFromDerivedViews};
    WavePhosphorBackend phosphorBackend{WavePhosphorBackend::Auto};
    WavePhosphorMode phosphorMode{WavePhosphorMode::FreeRun};
    WavePhosphorTriggerEdge triggerEdge{WavePhosphorTriggerEdge::Rising};
    WaveFftConfig fft{};
    WaveFftXAxisMode fftXAxisMode{WaveFftXAxisMode::FrequencyHz};
    WaveViewMode viewMode{WaveViewMode::Overlay};
    bool fftSourceWindowValid{false};
    bool fftViewportInitialized{false};
    bool fftFitAllRequested{false};
    double visibleDuration{1.0};
    double minVisibleTimeSpan{0.001};
    double downsampleStartMultiplier{2.0};
    double channelCardFixedWidth{128.0};
    double channelCardAdaptiveRatio{0.22};
    double legendChannelNameMaxWidth{0.0};
    double verticalAutoFitMultiplier{1.25};
    double persistenceWindow{0.25};
    double glowIntensity{1.0};
    double triggerThreshold{0.0};
    double triggerPositionRatio{0.2};
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
    std::array<float, 4> cursorFftHighlightRgba{0.20F, 0.55F, 1.00F, 0.16F};
    WaveViewportAnimationState viewportAnimation{};
    std::string sampleFrequencyInput;
    std::string sampleFrequencyError;
    WaveTimeAxisSource timeAxisSource{WaveTimeAxisSource::SampleIndex};
    WaveCursorSnapMode cursorSnapMode{WaveCursorSnapMode::SmartSnap};
    WaveCursorSnapScope cursorSnapScope{WaveCursorSnapScope::AllChannels};
    WaveCursorExtremeSnapPolicy cursorExtremeSnapPolicy{WaveCursorExtremeSnapPolicy::NearestWaveform};
    WaveRenderStats lastRenderStats{};
    std::array<WaveCursorState, 2> cursors{};
};

struct WaveAnalysisMarker {
    std::uint64_t id{0};
    std::string label;
    std::string note;
    double startTime{0.0};
    double endTime{0.0};
    std::size_t channelIndex{0};
};

enum class WaveToolsDrawer {
    Main,
    FFT,
    Renderer,
    Cursor,
    Measure,
    View,
};

struct WaveDockState {
    struct ChannelTransformOverride {
        bool labelOverridden{false};
        bool ratioOverridden{false};
        bool scaleOverridden{false};
        bool offsetOverridden{false};
        bool colorOverridden{false};
        bool bitYOffsetOverridden{false};
        std::string label;
        double ratio{1.0};
        double scale{1.0};
        double offset{0.0};
        std::optional<std::array<float, 4>> color{};
        double bitYOffset{0.0};
    };

    OscilloscopeBuffer buffer{};
    RawCaptureFileData rawCapture{};
    WaveViewState view{};
    std::vector<WaveAnalysisMarker> analysisMarkers;
    std::string statusMessage;
    std::vector<std::string> channelSummaries;
    std::vector<ChannelSpec> defaultChannelSpecs;
    std::vector<ChannelTransformOverride> channelOverrides;
    std::vector<std::size_t> hiddenChannelIndices;
    // 仅用于兼容旧协议 UI 状态中的 hidden_channel_labels，新的隐藏状态以通道下标为准。
    std::vector<std::string> hiddenChannelLabels;
    std::vector<std::uint8_t> fftChannelEnabled;
    std::vector<double> fftMagnitudeChannelOffsets;
    WaveLegendOverlayState legendOverlay{};
    bool oscilloscopeRunning{false};
    bool toolsCollapsed{true};
    WaveToolsDrawer activeToolsDrawer{WaveToolsDrawer::Main};
    bool overviewCollapsed{false};
    bool legendCollapsed{false};
    bool legendVisibilityRestorePending{false};
    float toolsDrawerProgress{0.0F};
    float overviewPanelProgress{1.0F};
    float legendOverlayProgress{0.0F};
    float toolsExpandedWidth{280.0F};
    float toolsCollapsedWidth{38.0F};
    float overviewPanelHeight{120.0F};
    float overviewCollapsedHeight{30.0F};
    float contentToolsSplitterWidth{6.0F};
    float overviewMainSplitterHeight{6.0F};
    float minOverviewPanelHeight{32.0F};
    float minMainPanelHeight{160.0F};
    float minToolsExpandedWidth{220.0F};
    float maxToolsExpandedWidth{520.0F};
    float mainToolbarContentWidth{0.0F};
    bool mainToolbarNeedsHorizontalScroll{true};
    std::uint64_t displayDataRevision{0};
    double displayDataSampleFrequencyHz{0.0};
    std::size_t lastLegendMeasurementChannelIndex{static_cast<std::size_t>(-1)};

    struct DisplayDataCacheKey {
        std::uint64_t dataRevision{0};
        double sampleFrequencyHz{0.0};
        double viewMinTime{0.0};
        double viewMaxTime{0.0};
        std::size_t channelCount{0};
        WaveDisplayFormula displayFormula{WaveDisplayFormula::OffsetThenScale};
        std::size_t rangeHash{0};

        bool operator==(const DisplayDataCacheKey& other) const
        {
            return dataRevision == other.dataRevision && sampleFrequencyHz == other.sampleFrequencyHz &&
                   viewMinTime == other.viewMinTime && viewMaxTime == other.viewMaxTime &&
                   channelCount == other.channelCount && displayFormula == other.displayFormula &&
                   rangeHash == other.rangeHash;
        }
    };

    struct OverviewDisplayDataCacheKey {
        std::uint64_t dataRevision{0};
        double sampleFrequencyHz{0.0};
        std::size_t channelCount{0};
        WaveDisplayFormula displayFormula{WaveDisplayFormula::OffsetThenScale};
        std::size_t rangeHash{0};
        std::size_t pointLimit{0};

        bool operator==(const OverviewDisplayDataCacheKey& other) const
        {
            return dataRevision == other.dataRevision && sampleFrequencyHz == other.sampleFrequencyHz &&
                   channelCount == other.channelCount && displayFormula == other.displayFormula &&
                   rangeHash == other.rangeHash && pointLimit == other.pointLimit;
        }
    };

    struct RenderEnvelopeCacheKey {
        std::uint64_t dataRevision{0};
        double sampleFrequencyHz{0.0};
        double visibleMinTime{0.0};
        double visibleMaxTime{0.0};
        std::size_t channelIndex{0};
        std::size_t pointLimit{0};
        std::size_t sampleCount{0};
        bool peakDetectDownsample{false};
        WaveDisplayFormula displayFormula{WaveDisplayFormula::OffsetThenScale};
        double ratio{1.0};
        double scale{1.0};
        double offset{0.0};

        bool operator==(const RenderEnvelopeCacheKey& other) const
        {
            return dataRevision == other.dataRevision && sampleFrequencyHz == other.sampleFrequencyHz &&
                   visibleMinTime == other.visibleMinTime && visibleMaxTime == other.visibleMaxTime &&
                   channelIndex == other.channelIndex && pointLimit == other.pointLimit &&
                   sampleCount == other.sampleCount && peakDetectDownsample == other.peakDetectDownsample &&
                   displayFormula == other.displayFormula && ratio == other.ratio && scale == other.scale &&
                   offset == other.offset;
        }
    };

    struct RenderEnvelopeCacheEntry {
        bool valid{false};
        RenderEnvelopeCacheKey key{};
        std::vector<EnvelopePoint> envelope;
        std::vector<WaveSample> peakDetectTrace;
        std::size_t sourceSampleCount{0};
    };

    struct BitRenderCacheKey {
        std::uint64_t dataRevision{0};
        std::size_t channelIndex{0};
        double visibleMinTime{0.0};
        double visibleMaxTime{0.0};
        double visibleMinValue{0.0};
        double visibleMaxValue{0.0};
        double sampleFrequencyHz{0.0};
        std::size_t firstBit{0};
        std::size_t bitCount{0};
        double yOffset{0.0};
        std::size_t plotPixelWidth{0};
        std::size_t plotPixelHeight{0};
        std::size_t layoutFingerprint{0};
        std::size_t vertexBudget{0};

        bool operator==(const BitRenderCacheKey&) const = default;
    };

    struct BitRenderCacheEntry {
        bool valid{false};
        BitRenderCacheKey key{};
        std::vector<std::vector<WaveSample>> lanes;
        std::size_t sourceSampleCount{0};
    };

    bool cachedDisplayKeyValid{false};
    DisplayDataCacheKey cachedDisplayKey{};
    bool cachedOverviewKeyValid{false};
    OverviewDisplayDataCacheKey cachedOverviewKey{};
    WaveSnapshot cachedFullSnapshot{};
    WaveDisplayData cachedDisplayData{};
    WaveDisplayData cachedOverviewDisplayData{};
    WaveDataBounds cachedDisplayBounds{};
    std::vector<RenderEnvelopeCacheEntry> renderEnvelopeCache;
    std::vector<BitRenderCacheEntry> bitRenderCache;
    bool cachedFftKeyValid{false};
    WaveFftCacheKey cachedFftKey{};
    WaveFftFrame cachedFftFrame{};
    bool suppressZoomSelectionEscapeThisFrame{false};
};

bool resetChannelConfigToDefault(WaveDockState& wave,
                                 std::size_t channelIndex,
                                 const ChannelSpec& defaultSpec,
                                 WaveChannelDoubleClickAction action);
bool resetChannelConfigToDefault(WaveDockState& wave, std::size_t channelIndex, WaveChannelDoubleClickAction action);
bool resetChannelOffsetToDefault(WaveDockState& wave, std::size_t channelIndex);
bool resetOneChannelViewSettings(WaveDockState& wave, std::size_t channelIndex);
bool resetAllChannelViewSettings(WaveDockState& wave);

} // namespace protoscope::plot
