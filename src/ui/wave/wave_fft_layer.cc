#include "wave_fft_component.hpp"
#include "wave_render_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace protoscope::ui {
namespace {

    struct FftXAxisScale {
        plot::WaveFftXAxisMode mode{plot::WaveFftXAxisMode::FrequencyHz};
        double fundamentalHz{0.0};
        bool valid{true};
    };

    struct FftGetterPayload {
        const plot::WaveFftBin* bins{nullptr};
        const FftXAxisScale* xAxis{nullptr};
        bool phase{false};
        double magnitudeOffset{0.0};
    };

    enum class FftZoomSelectionAxisMode {
        XOnly,
        YOnly,
        XY,
    };

    struct FftValueRange {
        double min{0.0};
        double max{1.0};
    };

    constexpr double kFftMinValueHeight = 1e-6;
    constexpr double kFftWheelZoomBase = 0.85;

    std::span<const double> fftMagnitudeOffsets(const plot::WaveDockState& wave)
    {
        return {wave.fftMagnitudeChannelOffsets.data(), wave.fftMagnitudeChannelOffsets.size()};
    }

    double fftMagnitudeOffsetForChannel(std::span<const double> offsets, std::size_t channelIndex)
    {
        return channelIndex < offsets.size() ? offsets[channelIndex] : 0.0;
    }

    plot::WaveFftFitOptions makeFftFitOptions(const plot::WaveDockState& wave, bool ignoreFundamental)
    {
        return {
            .ignoreFundamentalBinsForMagnitude = ignoreFundamental,
            .magnitudeChannelOffsets = fftMagnitudeOffsets(wave),
        };
    }

    FftXAxisScale makeFftXAxisScale(const plot::WaveViewState& view, const plot::WaveFftFrame& frame)
    {
        FftXAxisScale xAxis{.mode = view.fftXAxisMode};
        if (frame.fundamentalHz.has_value() && std::isfinite(*frame.fundamentalHz) && *frame.fundamentalHz > 0.0) {
            xAxis.fundamentalHz = *frame.fundamentalHz;
        }
        xAxis.valid = xAxis.mode != plot::WaveFftXAxisMode::Order || xAxis.fundamentalHz > 0.0;
        return xAxis;
    }

    const char* fftXAxisLabel(const FftXAxisScale& xAxis)
    {
        switch (xAxis.mode) {
            case plot::WaveFftXAxisMode::FrequencyHz:
                return "频率 (Hz)";
            case plot::WaveFftXAxisMode::Order:
                return "次数 (x)";
            case plot::WaveFftXAxisMode::Log10Hz:
                return "log10(Hz)";
        }
        return "频率 (Hz)";
    }

    const char* invalidFftXAxisMessage(const FftXAxisScale& xAxis)
    {
        if (xAxis.mode == plot::WaveFftXAxisMode::Order) {
            return "次数横轴需要有效基波";
        }
        return "当前 FFT 横轴无法显示";
    }

    const char* fftMagnitudeAxisLabel(plot::WaveFftMagnitudeMode mode)
    {
        switch (mode) {
            case plot::WaveFftMagnitudeMode::Linear:
                return "幅值";
            case plot::WaveFftMagnitudeMode::Decibel:
                return "幅值 (dB)";
            case plot::WaveFftMagnitudeMode::FundamentalPercent:
                return "幅值 (% 基波)";
        }
        return "幅值";
    }

    const char* fftMagnitudeReadoutUnit(plot::WaveFftMagnitudeMode mode)
    {
        switch (mode) {
            case plot::WaveFftMagnitudeMode::Linear:
                return "幅值";
            case plot::WaveFftMagnitudeMode::Decibel:
                return "dB";
            case plot::WaveFftMagnitudeMode::FundamentalPercent:
                return "% 基波";
        }
        return "幅值";
    }

    std::optional<double> fftDisplayX(const FftXAxisScale& xAxis, double frequencyHz)
    {
        return plot::fftXAxisValue(xAxis.mode, frequencyHz, xAxis.fundamentalHz);
    }

    std::optional<double> fftFrequencyHzFromDisplayX(const FftXAxisScale& xAxis, double axisValue)
    {
        return plot::frequencyHzFromFftXAxisValue(xAxis.mode, axisValue, xAxis.fundamentalHz);
    }

    std::string formatXAxisReadout(const FftXAxisScale& xAxis, double frequencyHz)
    {
        const auto axisValue = fftDisplayX(xAxis, frequencyHz);
        if (!axisValue.has_value()) {
            return "x=--";
        }
        char buffer[96]{};
        switch (xAxis.mode) {
            case plot::WaveFftXAxisMode::FrequencyHz:
                std::snprintf(buffer, sizeof(buffer), "f=%.6g Hz", frequencyHz);
                break;
            case plot::WaveFftXAxisMode::Order:
                std::snprintf(buffer, sizeof(buffer), "x=%.6g  f=%.6g Hz", *axisValue, frequencyHz);
                break;
            case plot::WaveFftXAxisMode::Log10Hz:
                std::snprintf(buffer, sizeof(buffer), "log10=%.6g  f=%.6g Hz", *axisValue, frequencyHz);
                break;
        }
        return buffer;
    }

    FftValueRange normalizeFftValueRange(double minValue, double maxValue, double fallbackMin, double fallbackMax)
    {
        if (maxValue < minValue) {
            std::swap(minValue, maxValue);
        }
        if (!std::isfinite(minValue)) {
            minValue = fallbackMin;
        }
        if (!std::isfinite(maxValue)) {
            maxValue = fallbackMax;
        }
        if (maxValue < minValue) {
            std::swap(minValue, maxValue);
        }
        const double height = maxValue - minValue;
        if (!std::isfinite(height) || height < kFftMinValueHeight) {
            const double center =
                std::isfinite(0.5 * (minValue + maxValue)) ? 0.5 * (minValue + maxValue) : fallbackMin;
            minValue = center - 0.5 * kFftMinValueHeight;
            maxValue = minValue + kFftMinValueHeight;
        }
        return {.min = minValue, .max = maxValue};
    }

    FftValueRange panFftValueRangeUnbounded(const FftValueRange& current, double deltaValue)
    {
        if (!std::isfinite(deltaValue)) {
            return current;
        }
        // 核心流程：频域 Y 轴平移只保护数值有效性，不再按当前数据幅值/相位范围夹紧。
        return normalizeFftValueRange(current.min + deltaValue, current.max + deltaValue, current.min, current.max);
    }

    FftValueRange zoomFftValueRangeUnbounded(const FftValueRange& current, double wheelDelta, double centerValue)
    {
        if (std::abs(wheelDelta) <= 0.0) {
            return current;
        }
        const double height = (std::max)(current.max - current.min, kFftMinValueHeight);
        if (!std::isfinite(centerValue)) {
            centerValue = 0.5 * (current.min + current.max);
        }
        const double nextHeight = (std::max)(height * std::pow(kFftWheelZoomBase, wheelDelta), kFftMinValueHeight);
        const double ratio = (std::clamp)((centerValue - current.min) / height, 0.0, 1.0);
        // 核心流程：滚轮缩放允许 Y 轴越过 fit 范围，双击/显示全部再回到自动 fit。
        const double nextMin = centerValue - ratio * nextHeight;
        return normalizeFftValueRange(nextMin, nextMin + nextHeight, current.min, current.max);
    }

    double firstPositiveFftFrequencyHz(const plot::WaveFftFrame& frame)
    {
        double best = std::numeric_limits<double>::infinity();
        for (const auto& channel : frame.channels) {
            if (!channel.enabled || !channel.valid) {
                continue;
            }
            for (const auto& bin : channel.bins) {
                if (std::isfinite(bin.frequencyHz) && bin.frequencyHz > 0.0) {
                    best = (std::min)(best, bin.frequencyHz);
                    break;
                }
            }
        }
        if (std::isfinite(best)) {
            return best;
        }
        if (std::isfinite(frame.frequencyResolutionHz) && frame.frequencyResolutionHz > 0.0) {
            return frame.frequencyResolutionHz;
        }
        return 1.0;
    }

    std::optional<FftValueRange> displayFftFrequencyRange(const plot::WaveViewState& view,
                                                          const plot::WaveFftFrame& frame,
                                                          const FftXAxisScale& xAxis)
    {
        double minFrequencyHz = view.fftFrequencyMin;
        double maxFrequencyHz = view.fftFrequencyMax;
        if (!std::isfinite(minFrequencyHz) || !std::isfinite(maxFrequencyHz) || maxFrequencyHz <= minFrequencyHz) {
            minFrequencyHz = 0.0;
            maxFrequencyHz = (std::max)(1.0, frame.maxFrequencyHz);
        }
        if (xAxis.mode == plot::WaveFftXAxisMode::Log10Hz) {
            minFrequencyHz = (std::max)(minFrequencyHz, firstPositiveFftFrequencyHz(frame));
            if (maxFrequencyHz <= minFrequencyHz) {
                maxFrequencyHz = (std::max)(minFrequencyHz * 10.0, frame.maxFrequencyHz);
            }
        }
        const auto minX = fftDisplayX(xAxis, minFrequencyHz);
        const auto maxX = fftDisplayX(xAxis, maxFrequencyHz);
        if (!minX.has_value() || !maxX.has_value() || *maxX <= *minX) {
            return std::nullopt;
        }
        return FftValueRange{.min = *minX, .max = *maxX};
    }

    ImPlotPoint fftBinGetter(int index, void* userData)
    {
        const auto* payload = static_cast<const FftGetterPayload*>(userData);
        const auto& bin = payload->bins[index];
        const auto x = payload->xAxis == nullptr ? std::optional<double>{bin.frequencyHz}
                                                 : fftDisplayX(*payload->xAxis, bin.frequencyHz);
        return {x.value_or(std::numeric_limits<double>::quiet_NaN()),
                payload->phase ? bin.phaseDegrees : bin.displayMagnitude + payload->magnitudeOffset};
    }

    void drawCenteredHint(const char* message)
    {
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const ImVec2 textSize = ImGui::CalcTextSize(message);
        ImGui::SetCursorPosX((std::max)(0.0F, (available.x - textSize.x) * 0.5F));
        ImGui::SetCursorPosY((std::max)(0.0F, (available.y - textSize.y) * 0.5F));
        ImGui::TextUnformatted(message);
    }

    void cancelFftZoomSelection(plot::WaveViewState& view)
    {
        view.zoomSelectionActive = false;
        view.zoomSelectionDragging = false;
    }

    FftZoomSelectionAxisMode resolveFftZoomSelectionAxisMode(float pixelWidth, float pixelHeight)
    {
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
                                     const ImVec2& selectionCurrent)
    {
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
                               const FftXAxisScale& xAxis,
                               FftZoomSelectionAxisMode mode,
                               bool phasePlot,
                               const ImPlotPoint& plotStart,
                               const ImPlotPoint& plotEnd,
                               double minFrequencyWidth)
    {
        const auto startFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, plotStart.x);
        const auto endFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, plotEnd.x);
        const double minValue = (std::min)(plotStart.y, plotEnd.y);
        const double maxValue = (std::max)(plotStart.y, plotEnd.y);

        auto applyX = [&]() {
            if (!startFrequencyHz.has_value() || !endFrequencyHz.has_value()) {
                return false;
            }
            const double minFrequency = (std::min)(*startFrequencyHz, *endFrequencyHz);
            const double maxFrequency = (std::max)(*startFrequencyHz, *endFrequencyHz);
            if (!std::isfinite(minFrequency) || !std::isfinite(maxFrequency) ||
                maxFrequency - minFrequency < minFrequencyWidth) {
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

    bool handleFftZoomSelection(plot::WaveViewState& view,
                                const FftXAxisScale& xAxis,
                                bool phasePlot,
                                double minFrequencyWidth,
                                bool suppressEscapeCancel)
    {
        if (!view.zoomSelectionActive && !view.zoomSelectionDragging) {
            return false;
        }
        const bool cancelByEscape = ImGui::IsKeyPressed(ImGuiKey_Escape) && !suppressEscapeCancel;
        if (cancelByEscape || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            cancelFftZoomSelection(view);
            return true;
        }

        const ImVec2 mouse = ImGui::GetMousePos();
        if (view.zoomSelectionActive && !view.zoomSelectionDragging && ImPlot::IsPlotHovered() &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
            drawFftZoomSelectionPreview(
                *drawList,
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
        const bool validSelection = (mode == FftZoomSelectionAxisMode::XOnly && pixelWidth >= kSelectionThreshold) ||
                                    (mode == FftZoomSelectionAxisMode::YOnly && pixelHeight >= kSelectionThreshold) ||
                                    (mode == FftZoomSelectionAxisMode::XY && pixelWidth >= kSelectionThreshold &&
                                     pixelHeight >= kSelectionThreshold);
        if (validSelection) {
            const ImPlotPoint plotStart = ImPlot::PixelsToPlot(
                ImVec2(static_cast<float>(view.zoomSelectionStartX), static_cast<float>(view.zoomSelectionStartY)));
            const ImPlotPoint plotEnd = ImPlot::PixelsToPlot(
                ImVec2(static_cast<float>(view.zoomSelectionCurrentX), static_cast<float>(view.zoomSelectionCurrentY)));
            applyFftZoomSelection(view, xAxis, mode, phasePlot, plotStart, plotEnd, minFrequencyWidth);
        }
        cancelFftZoomSelection(view);
        return true;
    }

    void ensureFftViewport(plot::WaveDockState& wave, const plot::WaveFftFrame& frame)
    {
        auto& view = wave.view;
        if (!frame.valid) {
            return;
        }
        if (!view.fftViewportInitialized || view.fftFitAllRequested) {
            const auto fitViewport = plot::makeFftFitViewport(frame, makeFftFitOptions(wave, false));
            view.fftFrequencyMin = fitViewport.frequencyMin;
            view.fftFrequencyMax = fitViewport.frequencyMax;
            view.fftMagnitudeMin = fitViewport.magnitudeMin;
            view.fftMagnitudeMax = fitViewport.magnitudeMax;
            view.fftPhaseMin = fitViewport.phaseMin;
            view.fftPhaseMax = fitViewport.phaseMax;
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

    void applyFftFitFrequency(plot::WaveViewState& view, const plot::WaveFftFrame& frame)
    {
        const auto fitViewport = plot::makeFftFitViewport(frame);
        view.fftFrequencyMin = fitViewport.frequencyMin;
        view.fftFrequencyMax = fitViewport.frequencyMax;
    }

    void applyFftFitValue(plot::WaveDockState& wave, const plot::WaveFftFrame& frame, bool phasePlot)
    {
        auto& view = wave.view;
        const auto fitViewport = plot::makeFftFitViewport(
            frame, makeFftFitOptions(wave, !phasePlot && view.fftMagnitudeAutoFitIgnoreFundamental));
        if (phasePlot) {
            view.fftPhaseMin = fitViewport.phaseMin;
            view.fftPhaseMax = fitViewport.phaseMax;
        } else {
            view.fftMagnitudeMin = fitViewport.magnitudeMin;
            view.fftMagnitudeMax = fitViewport.magnitudeMax;
        }
    }

    bool handleFftAxisDoubleClick(plot::WaveDockState& wave, const plot::WaveFftFrame& frame, bool phasePlot)
    {
        auto& view = wave.view;
        if (!frame.valid || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            return false;
        }

        bool changed = false;
        if (ImPlot::IsAxisHovered(ImAxis_X1)) {
            applyFftFitFrequency(view, frame);
            changed = true;
        }
        if (ImPlot::IsAxisHovered(ImAxis_Y1)) {
            applyFftFitValue(wave, frame, phasePlot);
            changed = true;
        }
        return changed;
    }

    bool handleFftPan(plot::WaveViewState& view,
                      const plot::WaveDockState& wave,
                      const plot::WaveFftFrame& frame,
                      const FftXAxisScale& xAxis,
                      bool phasePlot,
                      double minFrequencyWidth,
                      bool cursorHeld)
    {
        if (cursorHeld || !ImPlot::IsPlotHovered() || !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            return false;
        }
        const auto& io = ImGui::GetIO();
        if (std::abs(io.MouseDelta.x) <= 0.0F && std::abs(io.MouseDelta.y) <= 0.0F) {
            return false;
        }

        const ImVec2 mousePixel = ImGui::GetMousePos();
        const ImVec2 previousPixel{mousePixel.x - io.MouseDelta.x, mousePixel.y - io.MouseDelta.y};
        const ImPlotPoint currentPlot = ImPlot::PixelsToPlot(mousePixel);
        const ImPlotPoint previousPlot = ImPlot::PixelsToPlot(previousPixel);
        const auto currentFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, currentPlot.x);
        const auto previousFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, previousPlot.x);
        if (!currentFrequencyHz.has_value() || !previousFrequencyHz.has_value()) {
            return false;
        }
        const auto fitViewport = plot::makeFftFitViewport(frame, makeFftFitOptions(wave, false));
        const auto currentValueRange =
            normalizeFftValueRange(phasePlot ? view.fftPhaseMin : view.fftMagnitudeMin,
                                   phasePlot ? view.fftPhaseMax : view.fftMagnitudeMax,
                                   phasePlot ? fitViewport.phaseMin : fitViewport.magnitudeMin,
                                   phasePlot ? fitViewport.phaseMax : fitViewport.magnitudeMax);
        plot::WaveViewport viewport{
            .minTime = view.fftFrequencyMin,
            .maxTime = view.fftFrequencyMax,
            .minValue = currentValueRange.min,
            .maxValue = currentValueRange.max,
        };
        const plot::WaveDataBounds bounds{
            .minTime = fitViewport.frequencyMin,
            .maxTime = fitViewport.frequencyMax,
            .minValue = currentValueRange.min,
            .maxValue = currentValueRange.max,
            .minStep = minFrequencyWidth,
            .valid = true,
        };

        viewport.minTime += *previousFrequencyHz - *currentFrequencyHz;
        viewport.maxTime += *previousFrequencyHz - *currentFrequencyHz;
        const auto normalized = plot::normalizeOverviewViewport(viewport, bounds, minFrequencyWidth);
        const auto pannedValueRange = panFftValueRangeUnbounded(currentValueRange, previousPlot.y - currentPlot.y);

        // 核心流程：频域左键拖动只移动独立频域视口，不改变 FFT 输入窗口和时域主视图。
        view.fftFrequencyMin = normalized.minTime;
        view.fftFrequencyMax = normalized.maxTime;
        if (phasePlot) {
            view.fftPhaseMin = pannedValueRange.min;
            view.fftPhaseMax = pannedValueRange.max;
        } else {
            view.fftMagnitudeMin = pannedValueRange.min;
            view.fftMagnitudeMax = pannedValueRange.max;
        }
        return true;
    }

    bool handleFftWheelZoom(plot::WaveViewState& view,
                            const plot::WaveDockState& wave,
                            const plot::WaveFftFrame& frame,
                            const FftXAxisScale& xAxis,
                            bool phasePlot,
                            double minFrequencyWidth)
    {
        const auto& io = ImGui::GetIO();
        if (io.MouseWheel == 0.0F ||
            (!ImPlot::IsPlotHovered() && !ImPlot::IsAxisHovered(ImAxis_X1) && !ImPlot::IsAxisHovered(ImAxis_Y1))) {
            return false;
        }

        plot::WaveZoomMode zoomMode = plot::WaveZoomMode::XOnly;
        if (ImPlot::IsAxisHovered(ImAxis_Y1)) {
            zoomMode = plot::WaveZoomMode::YOnly;
        }
        const ImPlotPoint mouse = ImPlot::GetPlotMousePos();
        const auto mouseFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, mouse.x);
        const plot::WaveViewport current{
            .minTime = view.fftFrequencyMin,
            .maxTime = view.fftFrequencyMax,
            .minValue = phasePlot ? view.fftPhaseMin : view.fftMagnitudeMin,
            .maxValue = phasePlot ? view.fftPhaseMax : view.fftMagnitudeMax,
        };
        const auto fitViewport = plot::makeFftFitViewport(frame, makeFftFitOptions(wave, false));
        const plot::WaveDataBounds bounds{
            .minTime = fitViewport.frequencyMin,
            .maxTime = fitViewport.frequencyMax,
            .minValue = phasePlot ? fitViewport.phaseMin : fitViewport.magnitudeMin,
            .maxValue = phasePlot ? fitViewport.phaseMax : fitViewport.magnitudeMax,
            .minStep = minFrequencyWidth,
            .valid = true,
        };
        if (zoomMode == plot::WaveZoomMode::YOnly) {
            const auto currentValueRange =
                normalizeFftValueRange(current.minValue, current.maxValue, bounds.minValue, bounds.maxValue);
            const auto zoomedValueRange = zoomFftValueRangeUnbounded(currentValueRange, io.MouseWheel, mouse.y);
            if (phasePlot) {
                view.fftPhaseMin = zoomedValueRange.min;
                view.fftPhaseMax = zoomedValueRange.max;
            } else {
                view.fftMagnitudeMin = zoomedValueRange.min;
                view.fftMagnitudeMax = zoomedValueRange.max;
            }
            return true;
        }
        // 核心流程：FFT 频域使用独立视口，滚轮只更新频率/幅值/相位范围，绝不回写时域输入窗口。
        if (!mouseFrequencyHz.has_value()) {
            return false;
        }
        const auto zoomed = plot::zoomViewport(
            current, zoomMode, io.MouseWheel, *mouseFrequencyHz, mouse.y, bounds, minFrequencyWidth, true);
        view.fftFrequencyMin = zoomed.minTime;
        view.fftFrequencyMax = zoomed.maxTime;
        if (phasePlot) {
            view.fftPhaseMin = zoomed.minValue;
            view.fftPhaseMax = zoomed.maxValue;
        } else {
            view.fftMagnitudeMin = zoomed.minValue;
            view.fftMagnitudeMax = zoomed.maxValue;
        }
        return true;
    }

    std::optional<plot::WaveFftReadout> nearestActiveReadout(const plot::WaveFftFrame& frame,
                                                             const plot::WaveViewState& view,
                                                             double frequencyHz)
    {
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

    std::optional<plot::WaveFftReadout> nearestReadoutNearPoint(const plot::WaveFftFrame& frame,
                                                                double frequencyHz,
                                                                double value,
                                                                double maxFrequencyDistance,
                                                                double maxValueDistance,
                                                                bool phasePlot,
                                                                std::span<const double> magnitudeOffsets)
    {
        std::optional<plot::WaveFftReadout> best;
        double bestScore = std::numeric_limits<double>::infinity();
        for (const auto& channel : frame.channels) {
            if (!channel.enabled || !channel.valid) {
                continue;
            }
            const auto candidate = plot::findNearestFftBin(frame, channel.channelIndex, frequencyHz);
            if (!candidate.has_value()) {
                continue;
            }
            const double frequencyDistance = std::abs(candidate->frequencyHz - frequencyHz);
            const double candidateValue =
                phasePlot ? candidate->phaseDegrees
                          : candidate->displayMagnitude +
                                fftMagnitudeOffsetForChannel(magnitudeOffsets, candidate->channelIndex);
            const double valueDistance = std::abs(candidateValue - value);
            if (frequencyDistance > maxFrequencyDistance || valueDistance > maxValueDistance) {
                continue;
            }
            const double score = frequencyDistance / (std::max)(maxFrequencyDistance, 1e-12) +
                                 valueDistance / (std::max)(maxValueDistance, 1e-12);
            if (score < bestScore) {
                best = candidate;
                bestScore = score;
            }
        }
        return best;
    }

    void drawCursorAnnotation(const plot::WaveFftReadout& readout,
                              const FftXAxisScale& xAxis,
                              plot::WaveFftMagnitudeMode magnitudeMode,
                              bool phasePlot,
                              std::span<const double> magnitudeOffsets)
    {
        const auto x = fftDisplayX(xAxis, readout.frequencyHz);
        if (!x.has_value()) {
            return;
        }
        const double y =
            phasePlot ? readout.phaseDegrees
                      : readout.displayMagnitude + fftMagnitudeOffsetForChannel(magnitudeOffsets, readout.channelIndex);
        const ImVec4 color(1.0F, 1.0F, 0.2F, 1.0F);
        const std::string axisText = formatXAxisReadout(xAxis, readout.frequencyHz);
        const char* magnitudeUnit = fftMagnitudeReadoutUnit(magnitudeMode);
        ImPlot::Annotation(*x,
                           y,
                           color,
                           ImVec2(10.0F, -10.0F),
                           true,
                           phasePlot ? "%s\n%.3g°" : "%s\n%.6g %s",
                           axisText.c_str(),
                           phasePlot ? readout.phaseDegrees : readout.displayMagnitude,
                           magnitudeUnit);
    }

    bool drawFftCursors(plot::WaveViewState& view,
                        const plot::WaveFftFrame& frame,
                        const FftXAxisScale& xAxis,
                        bool phasePlot,
                        bool enableInteraction,
                        std::span<const double> magnitudeOffsets,
                        std::array<std::optional<plot::WaveFftReadout>, 2>& cursorReadouts)
    {
        if (!view.showCursors || !enableInteraction) {
            // 分屏 FFT 下方面板只做读数，不能把 Hz 写回复用的时域游标。
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
            auto dragX = fftDisplayX(xAxis, cursor.time);
            if (!dragX.has_value()) {
                continue;
            }
            bool held = false;
            bool hovered = false;
            const int dragId = static_cast<int>((phasePlot ? 3000 : 2000) + cursorIndex);
            ImPlot::DragLineX(dragId, &(*dragX), cursorColors[cursorIndex], 1.2F, 0, nullptr, &hovered, &held);
            heldAny = heldAny || held;
            const auto dragFrequency = fftFrequencyHzFromDisplayX(xAxis, *dragX);
            if (!dragFrequency.has_value()) {
                continue;
            }
            const auto readout = nearestActiveReadout(frame, view, *dragFrequency);
            if (readout.has_value()) {
                cursor.time = readout->frequencyHz;
                cursor.value = phasePlot ? readout->phaseDegrees : readout->displayMagnitude;
                cursor.channelIndex = readout->channelIndex;
                cursorReadouts[cursorIndex] = readout;
                if (held || hovered || cursor.pinned) {
                    drawCursorAnnotation(*readout, xAxis, view.fft.magnitudeMode, phasePlot, magnitudeOffsets);
                }
            }
        }
        return heldAny;
    }

    std::optional<plot::WaveFftReadout> drawHoverReadout(plot::WaveViewState& view,
                                                         const plot::WaveFftFrame& frame,
                                                         const FftXAxisScale& xAxis,
                                                         bool phasePlot,
                                                         const ImPlotRect& limits,
                                                         std::span<const double> magnitudeOffsets)
    {
        if (!view.showHoverReadout || !ImPlot::IsPlotHovered()) {
            return std::nullopt;
        }
        const auto mouse = ImPlot::GetPlotMousePos();
        const auto mouseFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, mouse.x);
        if (!mouseFrequencyHz.has_value()) {
            return std::nullopt;
        }
        const auto hovered = nearestReadoutNearPoint(frame,
                                                     *mouseFrequencyHz,
                                                     mouse.y,
                                                     std::abs(view.fftFrequencyMax - view.fftFrequencyMin) / 80.0,
                                                     std::abs(limits.Y.Max - limits.Y.Min) / 30.0,
                                                     phasePlot,
                                                     magnitudeOffsets);
        if (hovered.has_value()) {
            drawCursorAnnotation(*hovered, xAxis, view.fft.magnitudeMode, phasePlot, magnitudeOffsets);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                view.measurementChannelIndex = hovered->channelIndex;
            }
        }
        return hovered;
    }

    bool handleFftMagnitudeOffsetDoubleClick(plot::WaveDockState& wave,
                                             const plot::WaveFftFrame& frame,
                                             const FftXAxisScale& xAxis,
                                             const ImPlotRect& limits,
                                             std::span<const double> magnitudeOffsets)
    {
        if (!ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1) || ImPlot::IsAxisHovered(ImAxis_Y1) ||
            !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            return false;
        }
        auto& view = wave.view;
        const auto mouse = ImPlot::GetPlotMousePos();
        const auto mouseFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, mouse.x);
        if (!mouseFrequencyHz.has_value()) {
            return false;
        }
        const auto hit = nearestReadoutNearPoint(frame,
                                                 *mouseFrequencyHz,
                                                 mouse.y,
                                                 std::abs(view.fftFrequencyMax - view.fftFrequencyMin) / 80.0,
                                                 std::abs(limits.Y.Max - limits.Y.Min) / 30.0,
                                                 false,
                                                 magnitudeOffsets);
        if (!hit.has_value()) {
            return false;
        }

        if (hit->channelIndex >= wave.fftMagnitudeChannelOffsets.size()) {
            wave.fftMagnitudeChannelOffsets.resize(hit->channelIndex + 1, 0.0);
        }
        wave.fftMagnitudeChannelOffsets[hit->channelIndex] = 0.0;
        view.measurementChannelIndex = hit->channelIndex;
        view.activeFftMagnitudeOffsetDrag = false;
        return true;
    }

    bool handleFftMagnitudeOffsetDrag(plot::WaveDockState& wave,
                                      const plot::WaveFftFrame& frame,
                                      const FftXAxisScale& xAxis,
                                      const ImPlotRect& limits,
                                      double minFrequencyWidth,
                                      bool cursorHeld,
                                      std::span<const double> magnitudeOffsets)
    {
        auto& view = wave.view;
        const auto& io = ImGui::GetIO();
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            view.activeFftMagnitudeOffsetDrag = false;
        }
        if (cursorHeld || !ImPlot::IsPlotHovered()) {
            return false;
        }

        bool changed = false;
        const bool canDragYOffset = allowsMouseYOffsetDrag(view.mouseYOffsetDragMode, io.KeyShift);
        const auto mouse = ImPlot::GetPlotMousePos();
        const auto mouseFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, mouse.x);
        if (canDragYOffset && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && mouseFrequencyHz.has_value()) {
            const auto hit = nearestReadoutNearPoint(frame,
                                                     *mouseFrequencyHz,
                                                     mouse.y,
                                                     std::abs(view.fftFrequencyMax - view.fftFrequencyMin) / 80.0,
                                                     std::abs(limits.Y.Max - limits.Y.Min) / 30.0,
                                                     false,
                                                     magnitudeOffsets);
            if (hit.has_value()) {
                view.activeFftMagnitudeOffsetDrag = true;
                view.activeFftMagnitudeOffsetDragChannelIndex = hit->channelIndex;
                changed = view.measurementChannelIndex != hit->channelIndex || changed;
                view.measurementChannelIndex = hit->channelIndex;
            }
        }

        if (!view.activeFftMagnitudeOffsetDrag || !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            return changed;
        }
        if (std::abs(io.MouseDelta.x) <= 0.0F && std::abs(io.MouseDelta.y) <= 0.0F) {
            return changed;
        }

        const ImVec2 mousePixel = ImGui::GetMousePos();
        const ImVec2 previousPixel{mousePixel.x - io.MouseDelta.x, mousePixel.y - io.MouseDelta.y};
        const ImPlotPoint currentPlot = ImPlot::PixelsToPlot(mousePixel);
        const ImPlotPoint previousPlot = ImPlot::PixelsToPlot(previousPixel);
        const auto currentFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, currentPlot.x);
        const auto previousFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, previousPlot.x);
        if (currentFrequencyHz.has_value() && previousFrequencyHz.has_value()) {
            const auto fitViewport = plot::makeFftFitViewport(frame, makeFftFitOptions(wave, false));
            plot::WaveViewport viewport{
                .minTime = view.fftFrequencyMin,
                .maxTime = view.fftFrequencyMax,
                .minValue = view.fftMagnitudeMin,
                .maxValue = view.fftMagnitudeMax,
            };
            const plot::WaveDataBounds bounds{
                .minTime = fitViewport.frequencyMin,
                .maxTime = fitViewport.frequencyMax,
                .minValue = view.fftMagnitudeMin,
                .maxValue = view.fftMagnitudeMax,
                .minStep = minFrequencyWidth,
                .valid = true,
            };
            viewport.minTime += *previousFrequencyHz - *currentFrequencyHz;
            viewport.maxTime += *previousFrequencyHz - *currentFrequencyHz;
            const auto normalized = plot::normalizeOverviewViewport(viewport, bounds, minFrequencyWidth);
            view.fftFrequencyMin = normalized.minTime;
            view.fftFrequencyMax = normalized.maxTime;
            changed = true;
        }

        if (canDragYOffset) {
            const auto channelIndex = view.activeFftMagnitudeOffsetDragChannelIndex;
            if (channelIndex >= wave.fftMagnitudeChannelOffsets.size()) {
                wave.fftMagnitudeChannelOffsets.resize(channelIndex + 1, 0.0);
            }
            // 核心流程：FFT 幅值 offset 只改变视觉 Y 坐标，不回写通道配置或 FFT 计算输入。
            wave.fftMagnitudeChannelOffsets[channelIndex] += currentPlot.y - previousPlot.y;
            changed = true;
        }
        return changed;
    }

    void drawFftChannelLines(const plot::WaveFftFrame& frame,
                             const plot::WaveSnapshot& snapshot,
                             const FftXAxisScale& xAxis,
                             bool phasePlot,
                             std::span<const double> magnitudeOffsets)
    {
        for (const auto& channel : frame.channels) {
            if (!channel.enabled || !channel.valid || channel.bins.empty() ||
                channel.channelIndex >= snapshot.channels.size()) {
                continue;
            }
            std::size_t firstDrawableBin = 0;
            while (firstDrawableBin < channel.bins.size() &&
                   !fftDisplayX(xAxis, channel.bins[firstDrawableBin].frequencyHz).has_value()) {
                ++firstDrawableBin;
            }
            if (firstDrawableBin >= channel.bins.size()) {
                continue;
            }
            const auto color = channelColor(snapshot.channels[channel.channelIndex], channel.channelIndex);
            FftGetterPayload payload{
                .bins = channel.bins.data() + firstDrawableBin,
                .xAxis = &xAxis,
                .phase = phasePlot,
                .magnitudeOffset = fftMagnitudeOffsetForChannel(magnitudeOffsets, channel.channelIndex)};
            const std::string label = channel.label + (phasePlot ? " 相位" : " 幅值");
            const std::string itemLabel = label + "##wave_fft_channel_" + std::to_string(channel.channelIndex) +
                                          (phasePlot ? "_phase" : "_magnitude");
            ImPlotSpec spec{};
            spec.LineColor = color;
            spec.LineWeight = 1.5F;
            ImPlot::PlotLineG(itemLabel.c_str(),
                              &fftBinGetter,
                              &payload,
                              static_cast<int>(channel.bins.size() - firstDrawableBin),
                              spec);
            if (!phasePlot && channel.fundamental.has_value() && std::isfinite(channel.fundamental->frequencyHz)) {
                if (const auto fundamentalX = fftDisplayX(xAxis, channel.fundamental->frequencyHz)) {
                    ImPlot::TagX(*fundamentalX, color, "基波 %.4g Hz", channel.fundamental->frequencyHz);
                }
            }
        }
    }

    void recordFftPlotLimits(plot::WaveViewState& view, const FftXAxisScale& xAxis, bool phasePlot)
    {
        const auto limits = ImPlot::GetPlotLimits();
        if (xAxis.mode != plot::WaveFftXAxisMode::Log10Hz && std::isfinite(limits.X.Min) &&
            std::isfinite(limits.X.Max) && limits.X.Max > limits.X.Min) {
            const auto minFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, limits.X.Min);
            const auto maxFrequencyHz = fftFrequencyHzFromDisplayX(xAxis, limits.X.Max);
            if (minFrequencyHz.has_value() && maxFrequencyHz.has_value() && *maxFrequencyHz > *minFrequencyHz) {
                view.fftFrequencyMin = *minFrequencyHz;
                view.fftFrequencyMax = *maxFrequencyHz;
            }
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

    std::string readoutText(const char* label,
                            const std::optional<plot::WaveFftReadout>& readout,
                            const FftXAxisScale& xAxis,
                            plot::WaveFftMagnitudeMode magnitudeMode)
    {
        if (!readout.has_value()) {
            return std::string(label) + ": --";
        }
        char buffer[192]{};
        const std::string axisText = formatXAxisReadout(xAxis, readout->frequencyHz);
        const char* magnitudeUnit = fftMagnitudeReadoutUnit(magnitudeMode);
        std::snprintf(buffer,
                      sizeof(buffer),
                      "%s: %s  mag=%.6g %s  phase=%.3g°",
                      label,
                      axisText.c_str(),
                      readout->displayMagnitude,
                      magnitudeUnit,
                      readout->phaseDegrees);
        return buffer;
    }

    double wrapDeltaPhase(double value)
    {
        while (value > 180.0) {
            value -= 360.0;
        }
        while (value <= -180.0) {
            value += 360.0;
        }
        return value;
    }

    void drawCursorSummary(const std::array<std::optional<plot::WaveFftReadout>, 2>& cursorReadouts,
                           const FftXAxisScale& xAxis,
                           plot::WaveFftMagnitudeMode magnitudeMode)
    {
        ImGui::TextUnformatted(readoutText("C1", cursorReadouts[0], xAxis, magnitudeMode).c_str());
        ImGui::TextUnformatted(readoutText("C2", cursorReadouts[1], xAxis, magnitudeMode).c_str());
        if (!cursorReadouts[0].has_value() || !cursorReadouts[1].has_value()) {
            return;
        }
        const auto& left = *cursorReadouts[0];
        const auto& right = *cursorReadouts[1];
        const double deltaFrequency = right.frequencyHz - left.frequencyHz;
        const double deltaMagnitude = right.displayMagnitude - left.displayMagnitude;
        const double deltaPhase = wrapDeltaPhase(right.phaseDegrees - left.phaseDegrees);
        const char* magnitudeUnit = fftMagnitudeReadoutUnit(magnitudeMode);
        if (std::abs(deltaFrequency) > 1e-12) {
            ImGui::Text("Δf=%.6g Hz  T=%.6g s  Δmag=%.6g %s  Δphase=%.3g°",
                        deltaFrequency,
                        1.0 / std::abs(deltaFrequency),
                        deltaMagnitude,
                        magnitudeUnit,
                        deltaPhase);
        } else {
            ImGui::Text("Δf=0 Hz  Δmag=%.6g %s  Δphase=%.3g°", deltaMagnitude, magnitudeUnit, deltaPhase);
        }
    }

    void drawCursorOverlay(const std::array<std::optional<plot::WaveFftReadout>, 2>& cursorReadouts,
                           const FftXAxisScale& xAxis,
                           plot::WaveFftMagnitudeMode magnitudeMode)
    {
        std::vector<std::string> lines;
        if (cursorReadouts[0].has_value()) {
            lines.push_back(readoutText("C1", cursorReadouts[0], xAxis, magnitudeMode));
        }
        if (cursorReadouts[1].has_value()) {
            lines.push_back(readoutText("C2", cursorReadouts[1], xAxis, magnitudeMode));
        }
        if (cursorReadouts[0].has_value() && cursorReadouts[1].has_value()) {
            const auto& left = *cursorReadouts[0];
            const auto& right = *cursorReadouts[1];
            const double deltaFrequency = right.frequencyHz - left.frequencyHz;
            const double deltaMagnitude = right.displayMagnitude - left.displayMagnitude;
            const double deltaPhase = wrapDeltaPhase(right.phaseDegrees - left.phaseDegrees);
            const char* magnitudeUnit = fftMagnitudeReadoutUnit(magnitudeMode);
            char buffer[192]{};
            if (std::abs(deltaFrequency) > 1e-12) {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "Δf=%.6g Hz  T=%.6g s  Δmag=%.6g %s  Δphase=%.3g°",
                              deltaFrequency,
                              1.0 / std::abs(deltaFrequency),
                              deltaMagnitude,
                              magnitudeUnit,
                              deltaPhase);
            } else {
                std::snprintf(buffer,
                              sizeof(buffer),
                              "Δf=0 Hz  Δmag=%.6g %s  Δphase=%.3g°",
                              deltaMagnitude,
                              magnitudeUnit,
                              deltaPhase);
            }
            lines.emplace_back(buffer);
        }
        if (lines.empty()) {
            return;
        }

        const ImVec2 plotPos = ImPlot::GetPlotPos();
        const ImVec2 plotSize = ImPlot::GetPlotSize();
        const float padding = 7.0F;
        const float lineSpacing = ImGui::GetTextLineHeightWithSpacing();
        ImVec2 textSize(0.0F, 0.0F);
        for (const auto& line : lines) {
            const ImVec2 size = ImGui::CalcTextSize(line.c_str());
            textSize.x = (std::max)(textSize.x, size.x);
            textSize.y += lineSpacing;
        }
        textSize.y = (std::max)(0.0F, textSize.y - ImGui::GetStyle().ItemSpacing.y);

        const ImVec2 overlayMax(plotPos.x + plotSize.x - padding, plotPos.y + padding + textSize.y + padding * 2.0F);
        const ImVec2 overlayMin(overlayMax.x - textSize.x - padding * 2.0F, plotPos.y + padding);
        auto* drawList = ImPlot::GetPlotDrawList();
        drawList->AddRectFilled(
            overlayMin, overlayMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.04F, 0.045F, 0.05F, 0.68F)), 5.0F);
        drawList->AddRect(
            overlayMin, overlayMax, ImGui::ColorConvertFloat4ToU32(ImVec4(1.0F, 1.0F, 1.0F, 0.18F)), 5.0F);
        ImVec2 textPos(overlayMin.x + padding, overlayMin.y + padding);
        const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.92F, 0.94F, 0.98F, 0.95F));
        for (const auto& line : lines) {
            drawList->AddText(textPos, textColor, line.c_str());
            textPos.y += lineSpacing;
        }
    }

} // namespace

namespace {

    PlotRenderResult drawWaveFftPlotContent(plot::WaveDockState& wave,
                                            const WaveFrameData& frame,
                                            bool includePhase,
                                            bool enableCursorInteraction)
    {
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
        ensureFftViewport(wave, *fftFrame);
        const auto magnitudeOffsets = fftMagnitudeOffsets(wave);
        const FftXAxisScale xAxis = makeFftXAxisScale(view, *fftFrame);
        if (!xAxis.valid) {
            drawCenteredHint(invalidFftXAxisMessage(xAxis));
            return result;
        }
        if (!displayFftFrequencyRange(view, *fftFrame, xAxis).has_value()) {
            drawCenteredHint("当前 FFT 横轴范围无效");
            return result;
        }
        const char* yLabel = fftMagnitudeAxisLabel(view.fft.magnitudeMode);
        const ImVec2 available = ImGui::GetContentRegionAvail();
        const bool showFrequencyCursors = enableCursorInteraction && view.showCursors;
        const float summaryHeight = showFrequencyCursors && !view.showMeasurementOverlay ? 58.0F : 8.0F;
        const float plotGap = ImGui::GetStyle().ItemSpacing.y;
        const float plotAreaHeight = includePhase ? (std::max)(120.0F, available.y - summaryHeight - plotGap)
                                                  : (std::max)(1.0F, available.y - summaryHeight - plotGap);
        const float magnitudeHeight =
            includePhase ? (std::max)(90.0F, plotAreaHeight * 0.58F) : (std::max)(1.0F, plotAreaHeight);
        const float phaseHeight = includePhase ? (std::max)(80.0F, plotAreaHeight - magnitudeHeight - plotGap) : 0.0F;
        std::array<std::optional<plot::WaveFftReadout>, 2> cursorReadouts{};

        auto& inputMap = ImPlot::GetInputMap();
        const auto savedInputMap = inputMap;
        inputMap.PanMod = ImGuiMod_Ctrl;

        ImPlotFlags plotFlags = ImPlotFlags_NoMenus;
        if (!view.showFftLegend) {
            plotFlags |= ImPlotFlags_NoLegend;
        }

        if (ImPlot::BeginPlot("##wave_fft_magnitude", ImVec2(-1.0F, magnitudeHeight), plotFlags)) {
            const auto frequencyRange = displayFftFrequencyRange(view, *fftFrame, xAxis);
            result.plotRendered = true;
            ImPlot::SetupAxes(fftXAxisLabel(xAxis), yLabel);
            ImPlot::SetupAxisLimits(ImAxis_X1, frequencyRange->min, frequencyRange->max, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, view.fftMagnitudeMin, view.fftMagnitudeMax, ImGuiCond_Always);
            drawFftChannelLines(*fftFrame, *frame.fullSnapshot, xAxis, false, magnitudeOffsets);
            const double minFrequencyWidth = (std::max)(fftFrame->frequencyResolutionHz, 1e-9);
            const bool zoomSelectionConsumed = handleFftZoomSelection(
                view, xAxis, false, minFrequencyWidth, wave.suppressZoomSelectionEscapeThisFrame);
            const auto limits = ImPlot::GetPlotLimits();
            if (!zoomSelectionConsumed) {
                drawHoverReadout(view, *fftFrame, xAxis, false, limits, magnitudeOffsets);
                const bool cursorHeld = drawFftCursors(
                    view, *fftFrame, xAxis, false, enableCursorInteraction, magnitudeOffsets, cursorReadouts);
                if (showFrequencyCursors && view.showMeasurementOverlay) {
                    drawCursorOverlay(cursorReadouts, xAxis, view.fft.magnitudeMode);
                }
                drawWaveStatusOverlay(view);
                const bool axisResetConsumed = handleFftAxisDoubleClick(wave, *fftFrame, false);
                const bool offsetResetConsumed =
                    !axisResetConsumed &&
                    handleFftMagnitudeOffsetDoubleClick(wave, *fftFrame, xAxis, limits, magnitudeOffsets);
                const bool offsetDragConsumed =
                    !axisResetConsumed && !offsetResetConsumed &&
                    handleFftMagnitudeOffsetDrag(
                        wave, *fftFrame, xAxis, limits, minFrequencyWidth, cursorHeld, magnitudeOffsets);
                const bool panConsumed =
                    !axisResetConsumed && !offsetResetConsumed && !offsetDragConsumed &&
                    handleFftPan(view, wave, *fftFrame, xAxis, false, minFrequencyWidth, cursorHeld);
                const bool wheelConsumed = !axisResetConsumed && !offsetResetConsumed && !offsetDragConsumed &&
                                           !panConsumed &&
                                           handleFftWheelZoom(view, wave, *fftFrame, xAxis, false, minFrequencyWidth);
                if (!axisResetConsumed && !offsetResetConsumed && !offsetDragConsumed && !panConsumed &&
                    !wheelConsumed) {
                    recordFftPlotLimits(view, xAxis, false);
                }
            }
            ImPlot::EndPlot();
        }

        if (includePhase && ImPlot::BeginPlot("##wave_fft_phase", ImVec2(-1.0F, phaseHeight), plotFlags)) {
            const auto frequencyRange = displayFftFrequencyRange(view, *fftFrame, xAxis);
            result.plotRendered = true;
            ImPlot::SetupAxes(fftXAxisLabel(xAxis), "相位 (deg)");
            ImPlot::SetupAxisLimits(ImAxis_X1, frequencyRange->min, frequencyRange->max, ImGuiCond_Always);
            ImPlot::SetupAxisLimits(ImAxis_Y1, view.fftPhaseMin, view.fftPhaseMax, ImGuiCond_Always);
            drawFftChannelLines(*fftFrame, *frame.fullSnapshot, xAxis, true, magnitudeOffsets);
            const double minFrequencyWidth = (std::max)(fftFrame->frequencyResolutionHz, 1e-9);
            const bool zoomSelectionConsumed =
                handleFftZoomSelection(view, xAxis, true, minFrequencyWidth, wave.suppressZoomSelectionEscapeThisFrame);
            const auto limits = ImPlot::GetPlotLimits();
            if (!zoomSelectionConsumed) {
                drawHoverReadout(view, *fftFrame, xAxis, true, limits, magnitudeOffsets);
                const bool cursorHeld = drawFftCursors(
                    view, *fftFrame, xAxis, true, enableCursorInteraction, magnitudeOffsets, cursorReadouts);
                const bool axisResetConsumed = handleFftAxisDoubleClick(wave, *fftFrame, true);
                const bool panConsumed =
                    !axisResetConsumed &&
                    handleFftPan(view, wave, *fftFrame, xAxis, true, minFrequencyWidth, cursorHeld);
                const bool wheelConsumed = !axisResetConsumed && !panConsumed &&
                                           handleFftWheelZoom(view, wave, *fftFrame, xAxis, true, minFrequencyWidth);
                if (!axisResetConsumed && !panConsumed && !wheelConsumed) {
                    recordFftPlotLimits(view, xAxis, true);
                }
            }
            ImPlot::EndPlot();
        }

        inputMap = savedInputMap;

        if (showFrequencyCursors && !view.showMeasurementOverlay) {
            drawCursorSummary(cursorReadouts, xAxis, view.fft.magnitudeMode);
        }
        return result;
    }

} // namespace

static_assert(std::is_base_of_v<IWaveComponent, WaveFftComponent>, "WaveFftComponent 必须通过波形组件基类接入");

void WaveFftComponent::draw(WaveContext& context)
{
    ImGui::BeginChild(
        "##wave_main_panel", ImVec2(0.0F, context.layout->mainHeight), false, ImGuiWindowFlags_NoScrollbar);
    drawWaveFftPlot(context.wave, *context.renderFrame, true);
    ImGui::EndChild();
}

PlotRenderResult drawWaveFftPlot(plot::WaveDockState& wave,
                                 const WaveFrameData& frame,
                                 bool includePhase,
                                 bool enableCursorInteraction)
{
    return drawWaveFftPlotContent(wave, frame, includePhase, enableCursorInteraction);
}

} // namespace protoscope::ui
