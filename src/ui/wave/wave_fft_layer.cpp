#include "wave_render_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace protoscope::ui {
namespace {

struct FftGetterPayload {
    const plot::WaveFftBin* bins{nullptr};
    bool phase{false};
};

enum class FftZoomSelectionAxisMode {
    XOnly,
    YOnly,
    XY,
};

ImPlotPoint fftBinGetter(int index, void* userData) {
    const auto* payload = static_cast<const FftGetterPayload*>(userData);
    const auto& bin = payload->bins[index];
    return {bin.frequencyHz, payload->phase ? bin.phaseDegrees : bin.displayMagnitude};
}

void drawCenteredHint(const char* message) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 textSize = ImGui::CalcTextSize(message);
    ImGui::SetCursorPosX((std::max)(0.0F, (available.x - textSize.x) * 0.5F));
    ImGui::SetCursorPosY((std::max)(0.0F, (available.y - textSize.y) * 0.5F));
    ImGui::TextUnformatted(message);
}

void cancelFftZoomSelection(plot::WaveViewState& view) {
    view.zoomSelectionActive = false;
    view.zoomSelectionDragging = false;
}

FftZoomSelectionAxisMode resolveFftZoomSelectionAxisMode(float pixelWidth, float pixelHeight) {
    constexpr float kDirectionalBias = 2.0F;
    if (pixelWidth >= pixelHeight * kDirectionalBias) {
        return FftZoomSelectionAxisMode::XOnly;
    }
    if (pixelHeight >= pixelWidth * kDirectionalBias) {
        return FftZoomSelectionAxisMode::YOnly;
    }
    return FftZoomSelectionAxisMode::XY;
}

void drawFftZoomSelectionPreview(ImDrawList& drawList,
                                 FftZoomSelectionAxisMode mode,
                                 const ImVec2& rectMin,
                                 const ImVec2& rectMax,
                                 const ImVec2& selectionStart,
                                 const ImVec2& selectionCurrent) {
    const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.2F, 0.55F, 1.0F, 0.16F));
    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.45F, 0.75F, 1.0F, 0.95F));
    switch (mode) {
    case FftZoomSelectionAxisMode::XOnly: {
        const float lineY = 0.5F * (selectionStart.y + selectionCurrent.y);
        drawList.AddLine(ImVec2(rectMin.x, lineY), ImVec2(rectMax.x, lineY), lineColor, 2.0F);
        break;
    }
    case FftZoomSelectionAxisMode::YOnly: {
        const float lineX = 0.5F * (selectionStart.x + selectionCurrent.x);
        drawList.AddLine(ImVec2(lineX, rectMin.y), ImVec2(lineX, rectMax.y), lineColor, 2.0F);
        break;
    }
    case FftZoomSelectionAxisMode::XY:
        drawList.AddRectFilled(rectMin, rectMax, fillColor);
        drawList.AddRect(rectMin, rectMax, lineColor, 0.0F, 0, 2.0F);
        break;
    }
}

bool applyFftZoomSelection(plot::WaveViewState& view,
                           FftZoomSelectionAxisMode mode,
                           bool phasePlot,
                           const ImPlotPoint& plotStart,
                           const ImPlotPoint& plotEnd,
                           double minFrequencyWidth) {
    const double minFrequency = (std::min)(plotStart.x, plotEnd.x);
    const double maxFrequency = (std::max)(plotStart.x, plotEnd.x);
    const double minValue = (std::min)(plotStart.y, plotEnd.y);
    const double maxValue = (std::max)(plotStart.y, plotEnd.y);

    auto applyX = [&]() {
        if (!std::isfinite(minFrequency) || !std::isfinite(maxFrequency) || maxFrequency - minFrequency < minFrequencyWidth) {
            return false;
        }
        view.fftFrequencyMin = minFrequency;
        view.fftFrequencyMax = maxFrequency;
        return true;
    };
    auto applyY = [&]() {
        if (!std::isfinite(minValue) || !std::isfinite(maxValue) || maxValue <= minValue) {
            return false;
        }
        if (phasePlot) {
            view.fftPhaseMin = minValue;
            view.fftPhaseMax = maxValue;
        } else {
            view.fftMagnitudeMin = minValue;
            view.fftMagnitudeMax = maxValue;
        }
        return true;
    };

    switch (mode) {
    case FftZoomSelectionAxisMode::XOnly:
        return applyX();
    case FftZoomSelectionAxisMode::YOnly:
        return applyY();
    case FftZoomSelectionAxisMode::XY: {
        const bool xChanged = applyX();
        const bool yChanged = applyY();
        return xChanged || yChanged;
    }
    }
    return false;
}

bool handleFftZoomSelection(plot::WaveViewState& view, bool phasePlot, double minFrequencyWidth) {
    if (!view.zoomSelectionActive && !view.zoomSelectionDragging) {
        return false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)
        || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        cancelFftZoomSelection(view);
        return true;
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    if (view.zoomSelectionActive && !view.zoomSelectionDragging && ImPlot::IsPlotHovered()
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        view.zoomSelectionDragging = true;
        view.zoomSelectionStartX = mouse.x;
        view.zoomSelectionStartY = mouse.y;
        view.zoomSelectionCurrentX = mouse.x;
        view.zoomSelectionCurrentY = mouse.y;
    }
    if (!view.zoomSelectionDragging) {
        return true;
    }

    view.zoomSelectionCurrentX = mouse.x;
    view.zoomSelectionCurrentY = mouse.y;
    const ImVec2 rectMin(static_cast<float>((std::min)(view.zoomSelectionStartX, view.zoomSelectionCurrentX)),
                         static_cast<float>((std::min)(view.zoomSelectionStartY, view.zoomSelectionCurrentY)));
    const ImVec2 rectMax(static_cast<float>((std::max)(view.zoomSelectionStartX, view.zoomSelectionCurrentX)),
                         static_cast<float>((std::max)(view.zoomSelectionStartY, view.zoomSelectionCurrentY)));
    const float pixelWidth = rectMax.x - rectMin.x;
    const float pixelHeight = rectMax.y - rectMin.y;
    const auto mode = resolveFftZoomSelectionAxisMode(pixelWidth, pixelHeight);
    if (auto* drawList = ImPlot::GetPlotDrawList()) {
        drawFftZoomSelectionPreview(*drawList,
                                    mode,
                                    rectMin,
                                    rectMax,
                                    ImVec2(static_cast<float>(view.zoomSelectionStartX), static_cast<float>(view.zoomSelectionStartY)),
                                    ImVec2(static_cast<float>(view.zoomSelectionCurrentX), static_cast<float>(view.zoomSelectionCurrentY)));
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        return true;
    }

    constexpr float kSelectionThreshold = 4.0F;
    const bool validSelection =
        (mode == FftZoomSelectionAxisMode::XOnly && pixelWidth >= kSelectionThreshold)
        || (mode == FftZoomSelectionAxisMode::YOnly && pixelHeight >= kSelectionThreshold)
        || (mode == FftZoomSelectionAxisMode::XY && pixelWidth >= kSelectionThreshold && pixelHeight >= kSelectionThreshold);
    if (validSelection) {
        const ImPlotPoint plotStart =
            ImPlot::PixelsToPlot(ImVec2(static_cast<float>(view.zoomSelectionStartX), static_cast<float>(view.zoomSelectionStartY)));
        const ImPlotPoint plotEnd =
            ImPlot::PixelsToPlot(ImVec2(static_cast<float>(view.zoomSelectionCurrentX), static_cast<float>(view.zoomSelectionCurrentY)));
        applyFftZoomSelection(view, mode, phasePlot, plotStart, plotEnd, minFrequencyWidth);
    }
    cancelFftZoomSelection(view);
    return true;
}

double paddedMin(double minValue, double maxValue) {
    return minValue - (std::max)(1e-9, (maxValue - minValue) * 0.08);
}

double paddedMax(double minValue, double maxValue) {
    return maxValue + (std::max)(1e-9, (maxValue - minValue) * 0.08);
}

void ensureFftViewport(plot::WaveViewState& view, const plot::WaveFftFrame& frame) {
    if (!frame.valid) {
        return;
    }
    if (!view.fftViewportInitialized || view.fftFitAllRequested) {
        view.fftFrequencyMin = 0.0;
        view.fftFrequencyMax = frame.maxFrequencyHz;
        view.fftMagnitudeMin = paddedMin(frame.minDisplayMagnitude, frame.maxDisplayMagnitude);
        view.fftMagnitudeMax = paddedMax(frame.minDisplayMagnitude, frame.maxDisplayMagnitude);
        view.fftPhaseMin = -180.0;
        view.fftPhaseMax = 180.0;
        view.fftViewportInitialized = true;
        view.fftFitAllRequested = false;
    }
    if (view.fftFrequencyMax <= view.fftFrequencyMin) {
        view.fftFrequencyMin = 0.0;
        view.fftFrequencyMax = (std::max)(1.0, frame.maxFrequencyHz);
    }
    if (view.fftMagnitudeMax <= view.fftMagnitudeMin) {
        view.fftMagnitudeMax = view.fftMagnitudeMin + 1.0;
    }
    if (view.fftPhaseMax <= view.fftPhaseMin) {
        view.fftPhaseMin = -180.0;
        view.fftPhaseMax = 180.0;
    }
}

std::optional<plot::WaveFftReadout> nearestActiveReadout(const plot::WaveFftFrame& frame,
                                                         const plot::WaveViewState& view,
                                                         double frequencyHz) {
    if (const auto readout = plot::findNearestFftBin(frame, view.measurementChannelIndex, frequencyHz)) {
        return readout;
    }
    for (const auto& channel : frame.channels) {
        if (channel.enabled && channel.valid) {
            return plot::findNearestFftBin(frame, channel.channelIndex, frequencyHz);
        }
    }
    return std::nullopt;
}

void drawCursorAnnotation(const plot::WaveFftReadout& readout, bool phasePlot) {
    const double y = phasePlot ? readout.phaseDegrees : readout.displayMagnitude;
    const ImVec4 color(1.0F, 1.0F, 0.2F, 1.0F);
    if (phasePlot) {
        ImPlot::Annotation(readout.frequencyHz,
                           y,
                           color,
                           ImVec2(10.0F, -10.0F),
                           true,
                           "%.4g Hz\n%.3g°",
                           readout.frequencyHz,
                           readout.phaseDegrees);
    } else {
        ImPlot::Annotation(readout.frequencyHz,
                           y,
                           color,
                           ImVec2(10.0F, -10.0F),
                           true,
                           "%.4g Hz\n%.6g",
                           readout.frequencyHz,
                           readout.displayMagnitude);
    }
}

bool drawFftCursors(plot::WaveViewState& view,
                    const plot::WaveFftFrame& frame,
                    bool phasePlot,
                    std::array<std::optional<plot::WaveFftReadout>, 2>& cursorReadouts) {
    if (!view.showCursors) {
        return false;
    }

    bool heldAny = false;
    const ImVec4 cursorColors[2] = {
        ImVec4(1.0F, 0.85F, 0.15F, 1.0F),
        ImVec4(0.2F, 0.85F, 1.0F, 1.0F),
    };
    for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
        auto& cursor = view.cursors[cursorIndex];
        if (!cursor.enabled) {
            continue;
        }
        double dragFrequency = cursor.time;
        bool held = false;
        const int dragId = static_cast<int>((phasePlot ? 3000 : 2000) + cursorIndex);
        ImPlot::DragLineX(dragId, &dragFrequency, cursorColors[cursorIndex], 1.2F, 0, nullptr, nullptr, &held);
        heldAny = heldAny || held;
        const auto readout = nearestActiveReadout(frame, view, dragFrequency);
        if (readout.has_value()) {
            cursor.time = readout->frequencyHz;
            cursor.value = phasePlot ? readout->phaseDegrees : readout->displayMagnitude;
            cursor.channelIndex = readout->channelIndex;
            cursorReadouts[cursorIndex] = readout;
            drawCursorAnnotation(*readout, phasePlot);
        }
    }
    return heldAny;
}

std::optional<plot::WaveFftReadout> drawHoverReadout(plot::WaveViewState& view,
                                                     const plot::WaveFftFrame& frame,
                                                     bool phasePlot,
                                                     const ImPlotRect& limits) {
    if (!view.showHoverReadout || !ImPlot::IsPlotHovered()) {
        return std::nullopt;
    }
    const auto mouse = ImPlot::GetPlotMousePos();
    std::optional<plot::WaveFftReadout> hovered;
    if (phasePlot) {
        hovered = nearestActiveReadout(frame, view, mouse.x);
    } else {
        hovered = plot::findNearestFftBinAcrossChannels(frame,
                                                        mouse.x,
                                                        mouse.y,
                                                        std::abs(limits.X.Max - limits.X.Min) / 80.0,
                                                        std::abs(limits.Y.Max - limits.Y.Min) / 30.0);
    }
    if (hovered.has_value()) {
        drawCursorAnnotation(*hovered, phasePlot);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            view.measurementChannelIndex = hovered->channelIndex;
        }
    }
    return hovered;
}

void drawFftChannelLines(const plot::WaveFftFrame& frame,
                         const plot::WaveSnapshot& snapshot,
                         bool phasePlot) {
    for (const auto& channel : frame.channels) {
        if (!channel.enabled || !channel.valid || channel.bins.empty() || channel.channelIndex >= snapshot.channels.size()) {
            continue;
        }
        const auto color = channelColor(snapshot.channels[channel.channelIndex], channel.channelIndex);
        FftGetterPayload payload{.bins = channel.bins.data(), .phase = phasePlot};
        const std::string label = channel.label + (phasePlot ? " 相位" : " 幅值");
        ImPlotSpec spec{};
        spec.LineColor = color;
        spec.LineWeight = 1.5F;
        ImPlot::PlotLineG(label.c_str(), &fftBinGetter, &payload, static_cast<int>(channel.bins.size()), spec);
        if (!phasePlot && channel.fundamental.has_value() && std::isfinite(channel.fundamental->frequencyHz)) {
            ImPlot::TagX(channel.fundamental->frequencyHz, color, "基波 %.4g Hz", channel.fundamental->frequencyHz);
        }
    }
}

void recordFftPlotLimits(plot::WaveViewState& view, bool phasePlot) {
    const auto limits = ImPlot::GetPlotLimits();
    if (std::isfinite(limits.X.Min) && std::isfinite(limits.X.Max) && limits.X.Max > limits.X.Min) {
        view.fftFrequencyMin = limits.X.Min;
        view.fftFrequencyMax = limits.X.Max;
    }
    if (std::isfinite(limits.Y.Min) && std::isfinite(limits.Y.Max) && limits.Y.Max > limits.Y.Min) {
        if (phasePlot) {
            view.fftPhaseMin = limits.Y.Min;
            view.fftPhaseMax = limits.Y.Max;
        } else {
            view.fftMagnitudeMin = limits.Y.Min;
            view.fftMagnitudeMax = limits.Y.Max;
        }
    }
}

std::string readoutText(const char* label, const std::optional<plot::WaveFftReadout>& readout) {
    if (!readout.has_value()) {
        return std::string(label) + ": --";
    }
    char buffer[192]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%s: f=%.6g Hz  mag=%.6g  phase=%.3g°",
                  label,
                  readout->frequencyHz,
                  readout->displayMagnitude,
                  readout->phaseDegrees);
    return buffer;
}

double wrapDeltaPhase(double value) {
    while (value > 180.0) {
        value -= 360.0;
    }
    while (value <= -180.0) {
        value += 360.0;
    }
    return value;
}

void drawCursorSummary(const std::array<std::optional<plot::WaveFftReadout>, 2>& cursorReadouts) {
    ImGui::TextUnformatted(readoutText("C1", cursorReadouts[0]).c_str());
    ImGui::TextUnformatted(readoutText("C2", cursorReadouts[1]).c_str());
    if (!cursorReadouts[0].has_value() || !cursorReadouts[1].has_value()) {
        return;
    }
    const auto& left = *cursorReadouts[0];
    const auto& right = *cursorReadouts[1];
    const double deltaFrequency = right.frequencyHz - left.frequencyHz;
    const double deltaMagnitude = right.displayMagnitude - left.displayMagnitude;
    const double deltaPhase = wrapDeltaPhase(right.phaseDegrees - left.phaseDegrees);
    if (std::abs(deltaFrequency) > 1e-12) {
        ImGui::Text("Δf=%.6g Hz  T=%.6g s  Δmag=%.6g  Δphase=%.3g°",
                    deltaFrequency,
                    1.0 / std::abs(deltaFrequency),
                    deltaMagnitude,
                    deltaPhase);
    } else {
        ImGui::Text("Δf=0 Hz  Δmag=%.6g  Δphase=%.3g°", deltaMagnitude, deltaPhase);
    }
}

} // namespace

PlotRenderResult drawWaveFftPlot(plot::WaveDockState& wave, const WaveFrameData& frame) {
    PlotRenderResult result{};
    const auto* fftFrame = frame.fftFrame;
    if (fftFrame == nullptr || !fftFrame->enabled) {
        drawCenteredHint("FFT 未启用");
        return result;
    }
    if (!fftFrame->valid) {
        drawCenteredHint(fftFrame->message.empty() ? "当前视图无法计算 FFT" : fftFrame->message.c_str());
        return result;
    }

    auto& view = wave.view;
    ensureFftViewport(view, *fftFrame);
    const char* yLabel = view.fft.magnitudeMode == plot::WaveFftMagnitudeMode::Decibel ? "幅值 (dB)" : "幅值";
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const float summaryHeight = view.showCursors ? 58.0F : 8.0F;
    const float plotGap = ImGui::GetStyle().ItemSpacing.y;
    const float plotAreaHeight = (std::max)(120.0F, available.y - summaryHeight - plotGap);
    const float magnitudeHeight = (std::max)(90.0F, plotAreaHeight * 0.58F);
    const float phaseHeight = (std::max)(80.0F, plotAreaHeight - magnitudeHeight - plotGap);
    std::array<std::optional<plot::WaveFftReadout>, 2> cursorReadouts{};

    if (ImPlot::BeginPlot("##wave_fft_magnitude", ImVec2(-1.0F, magnitudeHeight), ImPlotFlags_NoMenus)) {
        result.plotRendered = true;
        ImPlot::SetupAxes("频率 (Hz)", yLabel);
        ImPlot::SetupAxisLimits(ImAxis_X1, view.fftFrequencyMin, view.fftFrequencyMax, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, view.fftMagnitudeMin, view.fftMagnitudeMax, ImGuiCond_Always);
        drawFftChannelLines(*fftFrame, *frame.fullSnapshot, false);
        const bool zoomConsumed = handleFftZoomSelection(view, false, (std::max)(fftFrame->frequencyResolutionHz, 1e-9));
        const auto limits = ImPlot::GetPlotLimits();
        if (!zoomConsumed) {
            drawHoverReadout(view, *fftFrame, false, limits);
            drawFftCursors(view, *fftFrame, false, cursorReadouts);
            recordFftPlotLimits(view, false);
        }
        ImPlot::EndPlot();
    }

    if (ImPlot::BeginPlot("##wave_fft_phase", ImVec2(-1.0F, phaseHeight), ImPlotFlags_NoMenus)) {
        result.plotRendered = true;
        ImPlot::SetupAxes("频率 (Hz)", "相位 (deg)");
        ImPlot::SetupAxisLimits(ImAxis_X1, view.fftFrequencyMin, view.fftFrequencyMax, ImGuiCond_Always);
        ImPlot::SetupAxisLimits(ImAxis_Y1, view.fftPhaseMin, view.fftPhaseMax, ImGuiCond_Always);
        drawFftChannelLines(*fftFrame, *frame.fullSnapshot, true);
        const bool zoomConsumed = handleFftZoomSelection(view, true, (std::max)(fftFrame->frequencyResolutionHz, 1e-9));
        const auto limits = ImPlot::GetPlotLimits();
        if (!zoomConsumed) {
            drawHoverReadout(view, *fftFrame, true, limits);
            drawFftCursors(view, *fftFrame, true, cursorReadouts);
            recordFftPlotLimits(view, true);
        }
        ImPlot::EndPlot();
    }

    if (view.showCursors) {
        drawCursorSummary(cursorReadouts);
    }
    return result;
}

} // namespace protoscope::ui
