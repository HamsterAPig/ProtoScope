#include "protoscope/ui/ui_theme.hpp"

#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace protoscope::ui {
namespace {

    using MetricChip = WaveMetricChip;
    using MetricChips = std::vector<MetricChip>;

    void addChip(MetricChips& chips, std::string label, std::string value)
    {
        if (!value.empty()) {
            chips.push_back({std::move(label), std::move(value)});
        }
    }

    void addMetricChip(MetricChips& chips, const char* label, double value, const char* unit = nullptr)
    {
        addChip(chips, label, formatMetricText(value, unit));
    }

    void addOptionalMetricChip(MetricChips& chips,
                               const char* label,
                               const std::optional<double>& value,
                               const char* unit = nullptr)
    {
        addChip(chips, label, value.has_value() ? formatMetricText(*value, unit) : "N/A");
    }

    const char* safeUnit(const std::string& unit)
    {
        return unit.empty() ? nullptr : unit.c_str();
    }

    ImVec2 calcChipSize(const MetricChip& chip, float padX, float padY)
    {
        constexpr float labelValueGap = 6.0F;

        const ImVec2 labelSize = ImGui::CalcTextSize(chip.label.c_str());
        const ImVec2 valueSize = ImGui::CalcTextSize(chip.value.c_str());

        return {padX * 2.0F + labelSize.x + labelValueGap + valueSize.x, padY * 2.0F + ImGui::GetTextLineHeight()};
    }

    ImVec2 calcChipGridSize(const MetricChips& chips, float maxWidth, float gapX, float gapY, float padX, float padY)
    {
        ImVec2 size(0.0F, 0.0F);

        float rowWidth = 0.0F;
        float rowHeight = 0.0F;

        for (const auto& chip : chips) {
            const ImVec2 chipSize = calcChipSize(chip, padX, padY);

            if (rowWidth > 0.0F && rowWidth + gapX + chipSize.x > maxWidth) {
                size.x = (std::max)(size.x, rowWidth);
                size.y += rowHeight + gapY;

                rowWidth = 0.0F;
                rowHeight = 0.0F;
            }

            if (rowWidth > 0.0F) {
                rowWidth += gapX;
            }

            rowWidth += chipSize.x;
            rowHeight = (std::max)(rowHeight, chipSize.y);
        }

        size.x = (std::max)(size.x, rowWidth);
        size.y += rowHeight;

        return size;
    }

    void drawMetricChip(ImDrawList* drawList,
                        ImVec2 pos,
                        const MetricChip& chip,
                        ImU32 bgColor,
                        ImU32 borderColor,
                        ImU32 labelColor,
                        ImU32 valueColor,
                        float padX,
                        float padY,
                        float rounding)
    {
        constexpr float labelValueGap = 6.0F;

        const ImVec2 chipSize = calcChipSize(chip, padX, padY);
        const ImVec2 chipMax(pos.x + chipSize.x, pos.y + chipSize.y);

        const ImVec2 labelSize = ImGui::CalcTextSize(chip.label.c_str());

        drawList->AddRectFilled(pos, chipMax, bgColor, rounding);
        drawList->AddRect(pos, chipMax, borderColor, rounding);

        const float textY = pos.y + padY;
        const float labelX = pos.x + padX;
        const float valueX = labelX + labelSize.x + labelValueGap;

        drawList->AddText(ImVec2(labelX, textY), labelColor, chip.label.c_str());
        drawList->AddText(ImVec2(valueX, textY), valueColor, chip.value.c_str());
    }

    void drawChipGrid(ImDrawList* drawList,
                      ImVec2 pos,
                      const MetricChips& chips,
                      float maxWidth,
                      ImU32 chipBgColor,
                      ImU32 chipBorderColor,
                      ImU32 labelColor,
                      ImU32 valueColor)
    {
        constexpr float gapX = 6.0F;
        constexpr float gapY = 6.0F;
        constexpr float padX = 8.0F;
        constexpr float padY = 4.0F;
        constexpr float rounding = 6.0F;

        float x = pos.x;
        float y = pos.y;
        float rowHeight = 0.0F;

        for (const auto& chip : chips) {
            const ImVec2 chipSize = calcChipSize(chip, padX, padY);

            if (x > pos.x && x + chipSize.x > pos.x + maxWidth) {
                x = pos.x;
                y += rowHeight + gapY;
                rowHeight = 0.0F;
            }

            drawMetricChip(drawList,
                           ImVec2(x, y),
                           chip,
                           chipBgColor,
                           chipBorderColor,
                           labelColor,
                           valueColor,
                           padX,
                           padY,
                           rounding);

            x += chipSize.x + gapX;
            rowHeight = (std::max)(rowHeight, chipSize.y);
        }
    }

    void appendCursorChips(MetricChips& chips,
                           const plot::WaveViewState& view,
                           const plot::WaveSnapshot& snapshot,
                           const plot::WaveDisplayData& displayData,
                           const PlotRenderResult& result)
    {
        const auto& selection = view.measurement;

        if (!view.showCursors) {
            return;
        }

        const auto cursorTime = [&](std::size_t cursorIndex) -> std::optional<double> {
            const auto& readout = result.cursorReadouts[cursorIndex];
            if (readout.has_value() && std::isfinite(readout->time)) {
                return readout->time;
            }
            const auto& cursor = view.cursors[cursorIndex];
            return cursor.enabled && std::isfinite(cursor.time) ? std::optional<double>(cursor.time) : std::nullopt;
        };

        const auto appendCursorValue = [&](std::size_t cursorIndex, std::string_view timeLabel) {
            const auto& readout = result.cursorReadouts[cursorIndex];
            if (!readout.has_value() || readout->channelIndex >= snapshot.channels.size()) {
                return;
            }
            const auto& channel = snapshot.channels[readout->channelIndex];
            if (readout->bit.has_value()) {
                addChip(chips, cursorIndex == 0U ? "A·val" : "B·val", readout->bit->value ? "1" : "0");
            } else {
                addChip(chips, std::string(timeLabel), formatMetricText(readout->value, safeUnit(channel.unit)));
            }
        };

        if (selection.cursorA && view.cursors[0].enabled) {
            if (const auto time = cursorTime(0)) {
                addChip(chips, "A·t", formatMetricText(*time, displayData.timeUnit.c_str()));
            }
            appendCursorValue(0, "A·y");
        }

        if (selection.cursorB && view.cursors[1].enabled) {
            if (const auto time = cursorTime(1)) {
                addChip(chips, "B·t", formatMetricText(*time, displayData.timeUnit.c_str()));
            }
            appendCursorValue(1, "B·y");
        }

        const auto leftTime = cursorTime(0);
        const auto rightTime = cursorTime(1);
        if (!view.cursors[0].enabled || !view.cursors[1].enabled || !leftTime.has_value() || !rightTime.has_value()) {
            return;
        }
        const auto intervalText =
            plot::makeCursorIntervalText(*leftTime, *rightTime, displayData.axisSource, displayData.timeUnit);
        if (!intervalText.valid) {
            return;
        }

        if (selection.deltaTime) {
            addChip(chips,
                    intervalText.showFrequency ? "Δt" : "ΔS",
                    formatMetricText(
                        intervalText.delta,
                        intervalText.showFrequency ? displayData.timeUnit.c_str() : intervalText.deltaUnit.c_str()));
        }

        if (selection.deltaValue && result.cursorReadouts[0].has_value() && result.cursorReadouts[1].has_value() &&
            !result.cursorReadouts[0]->bit.has_value() && !result.cursorReadouts[1]->bit.has_value()) {
            const auto delta =
                plot::OscilloscopeBuffer::makeDelta(*result.cursorReadouts[0], *result.cursorReadouts[1]);
            if (delta.valid) {
                addChip(chips, "Δy", formatMetricText(delta.deltaValue, nullptr));
            }
        }

        if (selection.frequency && intervalText.showFrequency) {
            addChip(chips, "Freq", formatMetricText(intervalText.frequencyHz, "Hz"));
        }

        if (selection.period && intervalText.showFrequency) {
            addChip(chips, "T", formatMetricText(intervalText.delta, displayData.timeUnit.c_str()));
        }
    }

    std::string appendMeasurementChips(MetricChips& chips,
                                       const plot::WaveViewState& view,
                                       const plot::WaveSnapshot& snapshot,
                                       const plot::WaveDisplayData& displayData,
                                       const PlotRenderResult& result)
    {
        const auto& selection = view.measurement;

        if (!view.showCursors || !result.measurement.has_value() || !result.measurement->valid) {
            return {};
        }

        const auto& m = *result.measurement;
        const auto& channel = snapshot.channels[m.channelIndex];
        const char* unit = safeUnit(channel.unit);

        if (selection.sampleCount) {
            addChip(chips, "N", std::to_string(m.sampleCount));
        }
        if (selection.span) {
            addMetricChip(chips, "Span", m.duration, displayData.timeUnit.c_str());
        }

        if (selection.min)
            addMetricChip(chips, "Min", m.minValue, unit);
        if (selection.max)
            addMetricChip(chips, "Max", m.maxValue, unit);
        if (selection.peakToPeak)
            addMetricChip(chips, "Vpp", m.peakToPeak, unit);
        if (selection.mean)
            addMetricChip(chips, "Mean", m.meanValue, unit);
        if (selection.rms)
            addMetricChip(chips, "RMS", m.rmsValue, unit);
        if (selection.median)
            addMetricChip(chips, "Med", m.medianValue, unit);
        if (selection.p95)
            addMetricChip(chips, "P95", m.p95Value, unit);
        if (selection.p99)
            addMetricChip(chips, "P99", m.p99Value, unit);

        if (selection.variance)
            addMetricChip(chips, "Var", m.variance);
        if (selection.stddev)
            addMetricChip(chips, "Std", m.stddev, unit);
        if (selection.cv)
            addOptionalMetricChip(chips, "CV", m.cv);
        if (selection.mad)
            addMetricChip(chips, "MAD", m.mad, unit);
        if (selection.medianAbsDev)
            addMetricChip(chips, "MedAD", m.medianAbsDev, unit);
        if (selection.iqr)
            addMetricChip(chips, "IQR", m.iqr, unit);
        if (selection.p95Spread)
            addMetricChip(chips, "P95Δ", m.p95Spread, unit);

        if (selection.highWidth)
            addMetricChip(chips, "High", m.highWidth, displayData.timeUnit.c_str());
        if (selection.lowWidth)
            addMetricChip(chips, "Low", m.lowWidth, displayData.timeUnit.c_str());
        if (selection.dutyCycle)
            addOptionalMetricChip(chips, "Duty", m.dutyCycle, "%");
        if (selection.riseTime)
            addOptionalMetricChip(chips, "Rise", m.riseTime, displayData.timeUnit.c_str());
        if (selection.fallTime)
            addOptionalMetricChip(chips, "Fall", m.fallTime, displayData.timeUnit.c_str());
        if (selection.edgeCount)
            addChip(chips, "Edges", std::to_string(m.edgeCount));

        if (selection.absoluteError)
            addOptionalMetricChip(chips, "AbsErr", m.absoluteError, unit);
        if (selection.relativeErrorPercent)
            addOptionalMetricChip(chips, "RelErr", m.relativeErrorPercent, "%");
        if (selection.meanError)
            addOptionalMetricChip(chips, "MeanErr", m.meanError, unit);
        if (selection.mse)
            addOptionalMetricChip(chips, "MSE", m.mse);
        if (selection.rmse)
            addOptionalMetricChip(chips, "RMSE", m.rmse, unit);
        if (selection.mae)
            addOptionalMetricChip(chips, "MAE", m.mae, unit);
        if (selection.maxAbsError)
            addOptionalMetricChip(chips, "MaxErr", m.maxAbsError, unit);
        if (selection.bias)
            addOptionalMetricChip(chips, "Bias", m.bias, unit);

        return channel.label;
    }

} // namespace

std::vector<WaveMetricChip> buildCursorMetricChips(const plot::WaveViewState& view,
                                                   const plot::WaveSnapshot& snapshot,
                                                   const plot::WaveDisplayData& displayData,
                                                   const PlotRenderResult& result)
{
    MetricChips chips;
    appendCursorChips(chips, view, snapshot, displayData, result);
    return chips;
}

std::vector<WaveMetricChip> buildMeasurementMetricChips(const plot::WaveViewState& view,
                                                        const plot::WaveSnapshot& snapshot,
                                                        const plot::WaveDisplayData& displayData,
                                                        const PlotRenderResult& result)
{
    MetricChips chips;
    static_cast<void>(appendMeasurementChips(chips, view, snapshot, displayData, result));
    return chips;
}

float resolveMeasurementSafeRightX(float contentLeftX,
                                   float contentWidth,
                                   bool toolsCollapsed,
                                   float requestedDrawerWidth,
                                   float minDrawerWidth,
                                   float maxDrawerWidth,
                                   float splitterWidth)
{
    const float safeContentWidth = (std::max)(contentWidth, 0.0F);
    const float contentRight = contentLeftX + safeContentWidth;
    if (toolsCollapsed) {
        return contentRight;
    }

    const float safeMinDrawerWidth = (std::max)(minDrawerWidth, 0.0F);
    const float safeMaxDrawerWidth = (std::max)(maxDrawerWidth, safeMinDrawerWidth);
    const float drawerWidth =
        (std::min)((std::clamp)(requestedDrawerWidth, safeMinDrawerWidth, safeMaxDrawerWidth), safeContentWidth);
    const float reservedSplitterWidth = (std::max)(splitterWidth, 0.0F);
    return (std::max)(contentLeftX, contentRight - drawerWidth - reservedSplitterWidth);
}

MeasurementOverlayPlacementSize resolveMeasurementOverlayPlacementSize(const ImVec2& plotPos,
                                                                       const ImVec2& plotSize,
                                                                       float measurementSafeRightX)
{
    constexpr float kMinMeasurementSafeWidth = 64.0F;
    MeasurementOverlayPlacementSize placement{.plotSize = plotSize, .visible = plotSize.x >= kMinMeasurementSafeWidth};
    if (!std::isfinite(measurementSafeRightX)) {
        return placement;
    }

    const float safeWidth = measurementSafeRightX - plotPos.x;
    if (safeWidth < kMinMeasurementSafeWidth) {
        placement.plotSize.x = (std::max)(safeWidth, 0.0F);
        placement.visible = false;
        return placement;
    }

    placement.plotSize.x = (std::min)(plotSize.x, safeWidth);
    placement.visible = placement.plotSize.x >= kMinMeasurementSafeWidth;
    return placement;
}

void drawMeasurementOverlay(const plot::WaveViewState& view,
                            const plot::WaveSnapshot& snapshot,
                            const plot::WaveDisplayData& displayData,
                            const PlotRenderResult& result,
                            const ImVec2& plotPos,
                            const ImVec2& plotSize,
                            ImDrawList* drawList)
{
    if (!view.showMeasurementOverlay || drawList == nullptr) {
        return;
    }

    MetricChips chips;
    appendCursorChips(chips, view, snapshot, displayData, result);

    std::string measuredChannel;
    if (!result.bitMeasurementActive) {
        measuredChannel = appendMeasurementChips(chips, view, snapshot, displayData, result);
    }

    if (chips.empty()) {
        return;
    }

    constexpr float margin = 8.0F;
    constexpr float padding = 9.0F;
    constexpr float headerGap = 7.0F;
    constexpr float overlayRounding = 8.0F;
    constexpr float maxOverlayWidth = 360.0F;

    constexpr float chipGapX = 6.0F;
    constexpr float chipGapY = 6.0F;
    constexpr float chipPadX = 8.0F;
    constexpr float chipPadY = 4.0F;

    const float gridMaxWidth = (std::min)(maxOverlayWidth, plotSize.x * 0.52F) - padding * 2.0F;

    const ImVec2 gridSize = calcChipGridSize(chips, gridMaxWidth, chipGapX, chipGapY, chipPadX, chipPadY);

    std::string title = "测量";
    if (result.bitMeasurementActive) {
        title = "周期测量";
    }
    if (!measuredChannel.empty()) {
        title += " · " + measuredChannel;
    }

    const float titleWidth = ImGui::CalcTextSize(title.c_str()).x;
    const float contentWidth = (std::max)(gridSize.x, titleWidth);

    const float overlayWidth = contentWidth + padding * 2.0F;
    const float overlayHeight = padding * 2.0F + ImGui::GetTextLineHeight() + headerGap + gridSize.y;

    const ImVec2 overlayMax(plotPos.x + plotSize.x - margin, plotPos.y + margin + overlayHeight);

    const ImVec2 overlayMin(overlayMax.x - overlayWidth, plotPos.y + margin);

    const auto& waveTokens = activeWaveStyleTokens();
    const ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(waveTokens.measurementOverlayBackground);
    const ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(waveTokens.measurementOverlayBorder);
    const ImU32 accentColor = ImGui::ColorConvertFloat4ToU32(waveTokens.measurementOverlayAccent);

    const ImU32 titleColor = ImGui::ColorConvertFloat4ToU32(waveTokens.measurementOverlayTitle);
    const ImU32 chipBgColor = ImGui::ColorConvertFloat4ToU32(waveTokens.measurementChipBackground);
    const ImU32 chipBorderColor = ImGui::ColorConvertFloat4ToU32(waveTokens.measurementChipBorder);
    const ImU32 chipLabelColor = ImGui::ColorConvertFloat4ToU32(waveTokens.measurementChipLabel);
    const ImU32 chipValueColor = ImGui::ColorConvertFloat4ToU32(waveTokens.measurementChipValue);

    drawList->AddRectFilled(overlayMin, overlayMax, bgColor, overlayRounding);
    drawList->AddRect(overlayMin, overlayMax, borderColor, overlayRounding);

    drawList->AddRectFilled(
        ImVec2(overlayMin.x, overlayMin.y + 9.0F), ImVec2(overlayMin.x + 3.0F, overlayMax.y - 9.0F), accentColor, 3.0F);

    ImVec2 cursor(overlayMin.x + padding, overlayMin.y + padding);

    drawList->AddText(cursor, titleColor, title.c_str());

    cursor.y += ImGui::GetTextLineHeight() + headerGap;

    drawChipGrid(drawList,
                 cursor,
                 chips,
                 overlayWidth - padding * 2.0F,
                 chipBgColor,
                 chipBorderColor,
                 chipLabelColor,
                 chipValueColor);
}

} // namespace protoscope::ui
