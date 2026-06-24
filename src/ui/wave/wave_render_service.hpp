#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/wave_math.hpp"
#include "protoscope/plot/wave_state.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <implot.h>
#include <implot_internal.h>

namespace protoscope::ui {

struct RenderBudget {
    std::size_t pointsPerChannel{1};
    std::size_t estimatedVerticesPerPoint{4};
};

struct WaveFrameData {
    plot::WaveSnapshot snapshot;
    const plot::WaveSnapshot* fullSnapshot{nullptr};
    const plot::WaveDisplayData* displayData{nullptr};
    const plot::WaveDisplayData* renderDisplayData{nullptr};
    const plot::WaveDisplayData* overviewDisplayData{nullptr};
    const plot::WaveFftFrame* fftFrame{nullptr};
    plot::WaveDataBounds displayBounds{};
    RenderBudget renderBudget;
};

struct PlotRenderResult {
    bool plotRendered{false};
    bool bitMeasurementActive{false};
    std::array<std::optional<plot::CursorReadout>, 2> cursorReadouts{};
    std::optional<plot::MeasurementReadout> measurement;
};

struct ZoomSelectionResult {
    bool consumed{false};
    bool viewportChanged{false};
};

struct ChannelLegendMetrics {
    float cardWidth{180.0F};
    float cardHeight{0.0F};
    float stripHeight{0.0F};
    float totalHeight{0.0F};
};

struct PlotGetterPayload {
    const plot::EnvelopePoint* points{nullptr};
};

struct WaveSampleGetterPayload {
    const plot::WaveSample* samples{nullptr};
};

struct WavePhosphorTrigger {
    double time{0.0};
    double value{0.0};
};

struct WavePhosphorTriggerWindow {
    double sourceMinTime{0.0};
    double sourceMaxTime{0.0};
    double targetMinTime{0.0};
    double targetMaxTime{0.0};
};

struct WavePhosphorStrokeStyle {
    ImVec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    float lineWidth{1.0F};
};

struct SmartCursorSnap {
    plot::CursorReadout readout;
    std::string_view label;
};

struct BitLaneLayoutEntry {
    std::size_t parentChannelIndex{0};
    std::size_t bitIndex{0};
    std::size_t laneIndex{0};
    std::size_t rowIndex{0};
    double lowY{0.0};
    double highY{0.0};
    double centerY{0.0};
    float lowPixelY{0.0F};
    float highPixelY{0.0F};
    float centerPixelY{0.0F};
    float lanePixelPitch{1.0F};
};

struct BitLaneLayout {
    std::vector<BitLaneLayoutEntry> lanes;
};

struct BitLaneHit {
    BitLaneLayoutEntry lane;
    double distance{0.0};
};

enum class HoverReadoutKind {
    Waveform,
    BitLane,
};

struct HoverReadout {
    HoverReadoutKind kind{HoverReadoutKind::Waveform};
    plot::CursorReadout readout;
};

struct WaveStatusOverlayItem {
    std::string_view label;
};

std::string formatMetricText(double value, const char* baseUnit);
bool plotInteractionActive(bool toolHeld);
std::vector<WaveStatusOverlayItem> buildWaveStatusOverlayItems(const plot::WaveViewState& view);
void drawWaveStatusOverlay(const plot::WaveViewState& view,
                           const plot::WaveDisplayData* displayData = nullptr,
                           const std::vector<std::size_t>* channelIndices = nullptr);
bool drawRightPanelSplitter(
    const char* id, float& rightWidth, float minRightWidth, float minLeftWidth, float totalWidth, float thickness);
bool drawHorizontalSplitter(
    const char* id, float& topHeight, float minTopHeight, float minBottomHeight, float totalHeight, float thickness);
void recordMainPlotLimits(plot::WaveViewState& view, const ImPlotRect& limits);
bool syncAutoFitAxisLimits(plot::WaveViewState& view, const ImPlotRect& limits);
bool handleMainPlotAxisDoubleClick(plot::WaveViewState& view,
                                   const plot::WaveSnapshot& snapshot,
                                   const plot::WaveDataBounds& visibleWindowBounds,
                                   const plot::WaveDataBounds& fullHistoryBounds,
                                   const plot::WaveDataBounds& yAutoFitBounds);
bool applyFullViewport(plot::WaveViewState& view, double minTime, double maxTime, double minValue, double maxValue);
bool applyFitVisibleWaveforms(plot::WaveViewState& view,
                              const plot::WaveSnapshot& snapshot,
                              const plot::WaveDisplayData& displayData,
                              const std::vector<std::size_t>& channelIndices);
ZoomSelectionResult handleMainPlotZoomSelection(plot::WaveViewState& view, bool suppressEscapeCancel = false);
bool handleActiveWaveformDoubleClickOffsetReset(plot::WaveDockState& wave,
                                                const plot::WaveSnapshot& snapshot,
                                                const BitLaneLayout& bitLayout,
                                                const plot::WaveDisplayData& displayData,
                                                const std::vector<std::size_t>& visibleChannelIndices,
                                                const ImPlotPoint& mousePos,
                                                double timeSnapDistance,
                                                double valueSnapDistance);
const char* axisSourceName(plot::WaveTimeAxisSource source);
plot::WaveViewport currentViewport(const plot::WaveViewState& view);
void applyViewport(plot::WaveViewState& view, const plot::WaveViewport& viewport);
plot::ChannelSpec channelDefaultSpec(const plot::WaveDockState& wave,
                                     std::size_t channelIndex,
                                     const plot::ChannelSpec& snapshotSpec);
void applyChannelTransformOverride(plot::WaveDockState& wave,
                                   std::size_t channelIndex,
                                   const plot::ChannelSpec& updated,
                                   const plot::ChannelSpec& defaultSpec);
void invalidateWaveDisplayCaches(plot::WaveDockState& wave);

RenderBudget makeRenderBudget(const plot::WaveViewState& view,
                              std::size_t channelCount,
                              std::size_t pixelWidth,
                              bool glowEnabled);
ImPlotPoint envelopeLineMinGetter(int index, const void* data);
ImPlotPoint envelopeLineMaxGetter(int index, const void* data);
ImPlotPoint waveSampleGetter(int index, const void* data);
std::vector<plot::EnvelopePoint> buildDisplayEnvelope(const std::vector<plot::WaveSample>& samples,
                                                      double minTime,
                                                      double maxTime,
                                                      std::size_t maxPoints,
                                                      std::size_t* sourceSampleCount = nullptr);
std::vector<plot::WaveSample> buildPeakDetectDownsample(const std::vector<plot::WaveSample>& samples,
                                                        double minTime,
                                                        double maxTime,
                                                        std::size_t maxPoints,
                                                        std::size_t* sourceSampleCount = nullptr);
void renderEnvelopeAsBars(const std::vector<plot::EnvelopePoint>& points, const ImVec4& color, float lineWidth);
void renderGlowEnvelope(const std::vector<plot::EnvelopePoint>& points,
                        const ImVec4& color,
                        double glowIntensity,
                        float lineWidth);
void renderGlowSamples(const plot::WaveSample* samples,
                       std::size_t sampleCount,
                       const ImVec4& color,
                       double glowIntensity,
                       float lineWidth);
std::vector<WavePhosphorTrigger> findWavePhosphorTriggers(const std::vector<plot::WaveSample>& samples,
                                                          double minTime,
                                                          double maxTime,
                                                          plot::WavePhosphorTriggerEdge edge,
                                                          double threshold);
WavePhosphorTriggerWindow makeWavePhosphorTriggerWindow(double triggerTime,
                                                        double visibleMinTime,
                                                        double visibleDuration,
                                                        double triggerPositionRatio);
double alignWavePhosphorSampleTime(const WavePhosphorTriggerWindow& window, double sourceTime);
bool wavePhosphorShouldAdvance(const plot::WaveViewState& view);
bool renderWavePhosphor(plot::WaveViewState& view,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const std::vector<std::size_t>& visibleChannelIndices,
                        const ImPlotRect& limits);
ImVec4 withAlpha(ImVec4 color, float alphaScale);
ImVec4 fallbackChannelColor(std::size_t channelIndex);
ImVec4 channelColor(const plot::ChannelSpec& spec, std::size_t channelIndex);
ImVec4 channelColor(const plot::ChannelView& channel, std::size_t channelIndex);
WavePhosphorStrokeStyle wavePhosphorStrokeStyle(const plot::ChannelView& channel, std::size_t channelIndex);
bool bitDisplayEnabled(const plot::BitDisplaySpec& spec);
std::uint64_t rawBitsFromSampleValue(double value);
bool rawBitEnabled(double value, std::size_t bitIndex);
std::string bitLaneDisplayLabel(std::size_t bitIndex);
std::vector<plot::WaveSample> buildBitRenderLanePoints(const std::vector<plot::WaveSample>& displaySamples,
                                                       const plot::WaveSample* sourceSamples,
                                                       std::size_t sourceSampleCount,
                                                       std::size_t bitIndex,
                                                       double lowY,
                                                       double highY,
                                                       double fallbackMaxTime,
                                                       std::size_t maxPoints);
double bitDisplayLanePitch();
double bitDisplayLaneHeight();
std::vector<std::size_t> bitDisplayRowsForChannels(const plot::WaveSnapshot& snapshot,
                                                   const std::vector<std::size_t>& channelIndices);
double bitDisplayGroupBase(const plot::WaveSnapshot& snapshot, std::size_t channelIndex);
plot::WaveValueRange bitDisplayValueRange(const plot::WaveSnapshot& snapshot,
                                          std::size_t channelIndex,
                                          const plot::BitDisplaySpec& spec);
BitLaneLayout buildBitLaneLayout(const plot::WaveSnapshot& snapshot,
                                 const std::vector<std::size_t>& visibleChannelIndices,
                                 const ImPlotRect& limits,
                                 const ImVec2& plotPos,
                                 const ImVec2& plotSize);
std::optional<BitLaneHit> findBitLaneAtPlotValue(const BitLaneLayout& layout, double plotY, double maxDistance);
std::optional<plot::CursorReadout> findNearestBitTransition(const plot::WaveSnapshot& snapshot,
                                                            const BitLaneLayout& layout,
                                                            double time,
                                                            double plotY,
                                                            double maxTimeDistance,
                                                            double maxValueDistance);
std::optional<HoverReadout> findHoverReadout(
    const plot::WaveSnapshot& snapshot,
    const plot::WaveDisplayData& displayData,
    const std::vector<std::size_t>& visibleChannelIndices,
    const BitLaneLayout& bitLayout,
    double time,
    double plotY,
    double maxTimeDistance,
    double maxValueDistance,
    bool preferWaveformHoverReadout = true,
    plot::WaveBitDisplayReadoutPolicy bitDisplayReadoutPolicy = plot::WaveBitDisplayReadoutPolicy::MixedNearest,
    bool activeBitLaneVisibleForReadout = false);
bool bitLaneMeasurementActive(const plot::WaveViewState& view);
bool activeBitLaneVisible(const plot::WaveViewState& view, const BitLaneLayout& layout);
bool cursorPairUsesBitLanes(const std::array<std::optional<plot::CursorReadout>, 2>& cursorReadouts);
plot::MeasurementReadout makeBitIntervalMeasurement(const plot::CursorReadout& left, const plot::CursorReadout& right);
std::optional<std::size_t> findBitDisplayChannelAtValue(const plot::WaveDockState& wave,
                                                        const plot::WaveSnapshot& snapshot,
                                                        double value,
                                                        double maxDistance);
void drawWaveChannel(const plot::ChannelView& channel,
                     const std::vector<plot::EnvelopePoint>& envelope,
                     const ImVec4& color,
                     bool glowEnabled);

void drawChannelCardText(const ImVec2& min, const ImVec2& max, const std::string& text, ImU32 color);
void drawChannelCardTooltip(const plot::ChannelSpec& spec, bool active);
void drawChannelLegendPopup(plot::WaveDockState& wave,
                            std::size_t channelIndex,
                            const plot::ChannelSpec& spec,
                            bool active);
ChannelLegendMetrics measureChannelLegendMetrics(float availableWidth, const plot::WaveViewState& view);
double offsetParameterDeltaFromDisplayDelta(const plot::ChannelSpec& spec,
                                            plot::WaveDisplayFormula formula,
                                            double displayDelta);
float measureChannelLegendHeight(const plot::WaveSnapshot& snapshot, const plot::WaveDockState& wave);
void drawChannelLegendBar(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot);
void drawChannelLegendOverlay(plot::WaveDockState& wave,
                              const plot::WaveSnapshot& snapshot,
                              const ImVec2& plotPos,
                              const ImVec2& plotSize,
                              ImGuiViewport* hostViewport);
void drawChannelControls(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot);

bool updateActiveChannelScale(plot::WaveDockState& wave, double factor);
bool updateActiveChannelOffset(plot::WaveDockState& wave, double displayDelta);
bool allowsMouseYOffsetDrag(plot::WaveMouseYOffsetDragMode mode, bool shiftDown);
bool handleOscilloscopeChannelInteractions(plot::WaveDockState& wave,
                                           const plot::WaveSnapshot& snapshot,
                                           const plot::WaveDisplayData& displayData,
                                           const std::vector<std::size_t>& visibleChannelIndices,
                                           const ImPlotRect& limits,
                                           const ImPlotPoint& mousePos,
                                           double timeSnapDistance,
                                           double valueSnapDistance);
bool applyPendingVerticalAutoFitOverride(plot::WaveViewState& view, const plot::WaveDataBounds& bounds);
bool resetChannelBitYOffsetToZero(plot::WaveDockState& wave, std::size_t channelIndex);
bool excludesLegendHiddenChannels(const plot::WaveViewState& view);
std::string waveChannelItemLabel(std::string_view label, std::size_t channelIndex);
bool channelHiddenByLegendState(const plot::WaveDockState& wave, std::size_t channelIndex);
std::vector<std::size_t> channelIndicesForDerivedViews(const plot::WaveDockState& wave,
                                                       const plot::WaveSnapshot& snapshot);
plot::WaveDataBounds boundsForDerivedViews(const plot::WaveDockState& wave,
                                           const plot::WaveSnapshot& snapshot,
                                           const plot::WaveDisplayData& displayData,
                                           const std::vector<std::size_t>& channelIndices);
plot::WaveDataBounds boundsForVisibleWaveforms(const plot::WaveViewState& view,
                                               const plot::WaveSnapshot& snapshot,
                                               const plot::WaveDisplayData& displayData,
                                               const std::vector<std::size_t>& channelIndices);
plot::WaveDataBounds boundsForYAxisAutoFit(const plot::WaveDockState& wave,
                                           const plot::WaveSnapshot& snapshot,
                                           const plot::WaveDisplayData& displayData,
                                           const std::vector<std::size_t>& channelIndices);
void applySavedLegendVisibility(const plot::WaveDockState& wave, std::size_t channelIndex);
void syncLegendVisibilityState(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot);
void clampActiveChannel(plot::WaveViewState& view, std::size_t channelCount);
bool currentPlotItemVisible(const std::string& label, std::size_t channelIndex);
const char* snapScopeName(plot::WaveCursorSnapScope scope);
std::optional<plot::CursorReadout> findNearestDisplayByScope(const plot::WaveDisplayData& displayData,
                                                             const plot::WaveViewState& view,
                                                             double time,
                                                             double maxTimeDistance);
int splitCursorDragId(std::size_t channelIndex, std::size_t cursorIndex);
std::optional<plot::CursorReadout> findNearestCursorByScope(const plot::WaveSnapshot& snapshot,
                                                             const plot::WaveDisplayData& displayData,
                                                             const plot::WaveViewState& view,
                                                             const BitLaneLayout& bitLayout,
                                                             double time,
                                                             double plotY,
                                                             double maxTimeDistance,
                                                             double maxValueDistance,
                                                             bool allowActiveChannelTimeFallback = false,
                                                             std::optional<std::size_t> forcedChannelIndex =
                                                                 std::nullopt);
std::vector<std::size_t> visibleChannelIndicesForFit(const plot::WaveSnapshot& snapshot);
bool cursorSmartSnapActive(const plot::WaveViewState& view, const ImGuiIO& io);
std::optional<SmartCursorSnap> findSmartCursorSnapByScope(const plot::WaveDisplayData& displayData,
                                                          const plot::WaveViewState& view,
                                                          double time,
                                                          double mouseValue,
                                                          const ImPlotRect& limits,
                                                          double maxTimeDistance,
                                                          std::optional<std::size_t> forcedChannelIndex = std::nullopt);
std::optional<SmartCursorSnap> findSmartCursorSnapByScope(const plot::WaveSnapshot& snapshot,
                                                          const plot::WaveDisplayData& displayData,
                                                          const plot::WaveViewState& view,
                                                          const BitLaneLayout& bitLayout,
                                                          double time,
                                                          double mouseValue,
                                                          const ImPlotRect& limits,
                                                          double maxTimeDistance,
                                                          std::optional<std::size_t> forcedChannelIndex = std::nullopt);
plot::MeasurementReadout measureDisplayWindow(const plot::WaveDisplayData& displayData,
                                              std::size_t channelIndex,
                                              double beginTime,
                                              double endTime,
                                              std::optional<std::size_t> referenceChannelIndex = std::nullopt,
                                              std::optional<double> manualReferenceValue = std::nullopt);
void drawCursorIntervalHint(const plot::CursorReadout& left,
                            const plot::CursorReadout& right,
                            const plot::CursorIntervalText& intervalText,
                            const ImPlotRect& limits);
void drawCursorAnnotation(std::size_t cursorIndex,
                          const plot::CursorReadout& readout,
                          const plot::ChannelView& channel,
                          std::string_view timeUnit,
                          std::string_view snapLabel);

void drawOverviewWindow(plot::WaveViewState& view,
                        const plot::ViewConfig& config,
                        const plot::WaveSnapshot& fullSnapshot,
                        const plot::WaveDisplayData& displayData,
                        const plot::WaveDataBounds& displayBounds,
                        const std::vector<std::size_t>& channelIndices,
                        const RenderBudget& renderBudget);

void initializeWaveViewIfNeeded(plot::WaveViewState& view);
WaveFrameData prepareWaveFrame(plot::WaveDockState& wave, float availableWidth);
const char* waveRenderModeLabel(const plot::WaveRenderStats& stats);
void drawCursorToolbar(plot::WaveViewState& view,
                       const plot::ViewConfig& config,
                       const plot::WaveDisplayData& displayData);
void placeCursorInViewport(plot::WaveViewState& view,
                           const plot::ViewConfig& config,
                           const plot::WaveDisplayData& displayData,
                           std::size_t cursorIndex,
                           double ratio);
void placeCursorPairInViewport(plot::WaveViewState& view,
                               const plot::ViewConfig& config,
                               const plot::WaveDisplayData& displayData);
bool handleMainPlotZoom(plot::WaveViewState& view, const ImPlotPoint& mousePos);
void applyMainPlotAxesAndLimits(plot::WaveViewState& view,
                                const plot::WaveSnapshot& snapshot,
                                const plot::WaveDisplayData& displayData);

void renderWaveChannels(plot::WaveDockState& wave,
                        const plot::WaveSnapshot& snapshot,
                        const plot::WaveDisplayData& displayData,
                        const RenderBudget& renderBudget,
                        const ImPlotRect& limits,
                        std::vector<std::size_t>& visibleChannelIndices,
                        BitLaneLayout& outBitLayout);
PlotRenderResult drawOscilloscopePlot(plot::WaveDockState& wave, const WaveFrameData& frame);
PlotRenderResult drawWaveFftPlot(plot::WaveDockState& wave,
                                 const WaveFrameData& frame,
                                 bool includePhase,
                                 bool enableCursorInteraction = true);
void drawMeasurementOverlay(const plot::WaveViewState& view,
                            const plot::WaveSnapshot& snapshot,
                            const plot::WaveDisplayData& displayData,
                            const PlotRenderResult& result,
                            const ImVec2& plotPos,
                            const ImVec2& plotSize,
                            ImDrawList* drawList);

} // namespace protoscope::ui
