#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/wave_math.hpp"
#include "protoscope/plot/wave_state.hpp"

#include <imgui.h>
#include <implot.h>
#include <implot_internal.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::ui {

struct RenderBudget {
    std::size_t pointsPerChannel{1};
    std::size_t estimatedVerticesPerPoint{4};
};

struct WaveFrameData {
    plot::WaveSnapshot snapshot;
    const plot::WaveSnapshot* fullSnapshot{nullptr};
    const plot::WaveDisplayData* displayData{nullptr};
    const plot::WaveDisplayData* overviewDisplayData{nullptr};
    const plot::WaveFftFrame* fftFrame{nullptr};
    plot::WaveDataBounds displayBounds{};
    RenderBudget renderBudget;
};

struct PlotRenderResult {
    bool plotRendered{false};
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

struct SmartCursorSnap {
    plot::CursorReadout readout;
    std::string_view label;
};

std::string formatMetricText(double value, const char* baseUnit);
bool plotInteractionActive(bool toolHeld);
bool drawRightPanelSplitter(const char* id,
                            float& rightWidth,
                            float minRightWidth,
                            float minLeftWidth,
                            float totalWidth,
                            float thickness);
bool drawHorizontalSplitter(const char* id,
                            float& topHeight,
                            float minTopHeight,
                            float minBottomHeight,
                            float totalHeight,
                            float thickness);
void recordMainPlotLimits(plot::WaveViewState& view, const ImPlotRect& limits);
bool syncAutoFitAxisLimits(plot::WaveViewState& view, const ImPlotRect& limits);
bool handleMainPlotAxisDoubleClick(plot::WaveViewState& view, const plot::WaveDataBounds& bounds);
bool applyFullViewport(plot::WaveViewState& view, double minTime, double maxTime, double minValue, double maxValue);
bool applyFitVisibleWaveforms(plot::WaveViewState& view,
                              const plot::WaveDisplayData& displayData,
                              const std::vector<std::size_t>& channelIndices);
ZoomSelectionResult handleMainPlotZoomSelection(plot::WaveViewState& view);
bool handleActiveWaveformDoubleClickOffsetReset(plot::WaveDockState& wave,
                                                const plot::WaveDisplayData& displayData,
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

RenderBudget makeRenderBudget(const plot::WaveViewState& view,
                              std::size_t channelCount,
                              std::size_t pixelWidth,
                              bool phosphorGlowEnabled);
ImPlotPoint envelopeLineMinGetter(int index, void* data);
ImPlotPoint envelopeLineMaxGetter(int index, void* data);
ImPlotPoint waveSampleGetter(int index, void* data);
std::vector<plot::EnvelopePoint> buildDisplayEnvelope(const std::vector<plot::WaveSample>& samples,
                                                      double minTime,
                                                      double maxTime,
                                                      std::size_t maxPoints,
                                                      std::size_t* sourceSampleCount = nullptr);
void renderEnvelopeAsBars(const std::vector<plot::EnvelopePoint>& points, const ImVec4& color);
void renderPhosphorEnvelope(const std::vector<plot::EnvelopePoint>& points,
                            const ImVec4& color,
                            double latestTime,
                            double persistenceWindow,
                            double glowIntensity);
ImVec4 withAlpha(ImVec4 color, float alphaScale);
ImVec4 fallbackChannelColor(std::size_t channelIndex);
ImVec4 channelColor(const plot::ChannelSpec& spec, std::size_t channelIndex);
ImVec4 channelColor(const plot::ChannelView& channel, std::size_t channelIndex);
void drawWaveChannel(const plot::ChannelView& channel,
                     const std::vector<plot::EnvelopePoint>& envelope,
                     const ImVec4& color,
                     bool phosphorGlowEnabled);

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
void drawChannelControls(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot);

bool updateActiveChannelScale(plot::WaveDockState& wave, double factor);
bool updateActiveChannelOffset(plot::WaveDockState& wave, double displayDelta);
bool handleOscilloscopeChannelInteractions(plot::WaveDockState& wave,
                                           const plot::WaveDisplayData& displayData,
                                           const ImPlotPoint& mousePos,
                                           double timeSnapDistance,
                                           double valueSnapDistance);
bool applyPendingVerticalAutoFitOverride(plot::WaveViewState& view, const plot::WaveDataBounds& bounds);
bool excludesLegendHiddenChannels(const plot::WaveViewState& view);
bool channelHiddenByLegendState(const plot::WaveDockState& wave, const std::string& label);
std::vector<std::size_t> channelIndicesForDerivedViews(const plot::WaveDockState& wave,
                                                       const plot::WaveSnapshot& snapshot);
plot::WaveDataBounds boundsForDerivedViews(const plot::WaveDockState& wave,
                                           const plot::WaveDisplayData& displayData,
                                           const std::vector<std::size_t>& channelIndices);
void applySavedLegendVisibility(const plot::WaveDockState& wave, const std::string& label);
void syncLegendVisibilityState(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot);
void clampActiveChannel(plot::WaveViewState& view, std::size_t channelCount);
bool currentPlotItemVisible(const std::string& label);
const char* snapScopeName(plot::WaveCursorSnapScope scope);
std::optional<plot::CursorReadout> findNearestDisplayByScope(const plot::WaveDisplayData& displayData,
                                                             const plot::WaveViewState& view,
                                                             double time,
                                                             double maxTimeDistance);
std::vector<std::size_t> visibleChannelIndicesForFit(const plot::WaveSnapshot& snapshot);
bool cursorSmartSnapActive(const plot::WaveViewState& view, const ImGuiIO& io);
std::optional<SmartCursorSnap> findSmartCursorSnapByScope(const plot::WaveDisplayData& displayData,
                                                          const plot::WaveViewState& view,
                                                          double time,
                                                          double mouseValue,
                                                          const ImPlotRect& limits,
                                                          double maxTimeDistance);
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
                        std::vector<std::size_t>& visibleChannelIndices);
PlotRenderResult drawOscilloscopePlot(plot::WaveDockState& wave, const WaveFrameData& frame);
void drawMeasurementOverlay(const plot::WaveViewState& view,
                            const plot::WaveSnapshot& snapshot,
                            const plot::WaveDisplayData& displayData,
                            const PlotRenderResult& result);

} // namespace protoscope::ui
