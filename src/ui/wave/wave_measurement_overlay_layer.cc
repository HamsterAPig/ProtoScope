#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace protoscope::ui {
namespace {

    struct MetricChip {
        std::string label;
        std::string value;
    };

    using MetricChips = std::vector<MetricChip>;

    void addChip(MetricChips& chips, std::string label, std::string value)
    {
        if (!value.empty()) {
            chips.push_back({std::move(label), std::move(value)});
        }
    }

    void addMetricChip(MetricChips& chips, const char* label, double value, const char* unit = nullptr)
    { addChip(chips, label, formatMetricText(value, unit)); }

    void addOptionalMetricChip(MetricChips& chips,
                               const char* label,
                               const std::optional<double>& value,
                               const char* unit = nullptr)
    { addChip(chips, label, value.has_value() ? formatMetricText(*value, unit) : "N/A"); }

    const char* safeUnit(const std::string& unit)
    { return unit.empty() ? nullptr : unit.c_str(); }

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
                size.x = (std::max) (size.x, rowWidth);
                size.y += rowHeight + gapY;

                rowWidth = 0.0F;
                rowHeight = 0.0F;
            }

            if (rowWidth > 0.0F) {
                rowWidth += gapX;
            }

            rowWidth += chipSize.x;
            rowHeight = (std::max) (rowHeight, chipSize.y);
        }

        size.x = (std::max) (size.x, rowWidth);
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
            rowHeight = (std::max) (rowHeight, chipSize.y);
        }
    }

    void appendCursorChips(MetricChips& chips,
                           const plot::WaveViewState& view,
                           const plot::WaveSnapshot& snapshot,
                           const plot::WaveDisplayData& displayData,
                           const PlotRenderResult& result)
    {
        const auto& selection = view.measurement;
        const bool bitMode = result.bitMeasurementActive;

        if (!view.showCursors) {
            return;
        }

        if (selection.cursorA && result.cursorReadouts[0].has_value()) {
            const auto& cursor = *result.cursorReadouts[0];
            const auto& channel = snapshot.channels[cursor.channelIndex];

            addChip(chips, "A·t", formatMetricText(cursor.time, displayData.timeUnit.c_str()));
            if (bitMode && cursor.bit.has_value()) {
                addChip(chips, "A·val", cursor.bit->value ? "1" : "0");
            } else {
                addChip(chips, "A·y", formatMetricText(cursor.value, safeUnit(channel.unit)));
            }
        }

        if (selection.cursorB && result.cursorReadouts[1].has_value()) {
            const auto& cursor = *result.cursorReadouts[1];
            const auto& channel = snapshot.channels[cursor.channelIndex];

            addChip(chips, "B·t", formatMetricText(cursor.time, displayData.timeUnit.c_str()));
            if (bitMode && cursor.bit.has_value()) {
                addChip(chips, "B·val", cursor.bit->value ? "1" : "0");
            } else {
                addChip(chips, "B·y", formatMetricText(cursor.value, safeUnit(channel.unit)));
            }
        }

        if (!result.cursorReadouts[0].has_value() || !result.cursorReadouts[1].has_value()) {
            return;
        }

        const auto delta = plot::OscilloscopeBuffer::makeDelta(*result.cursorReadouts[0], *result.cursorReadouts[1]);

        const auto intervalText = plot::makeCursorIntervalText(
            *result.cursorReadouts[0], *result.cursorReadouts[1], displayData.axisSource, displayData.timeUnit);

        if (!intervalText.valid) {
            return;
        }

        if (selection.deltaTime) {
            addChip(chips,
                    intervalText.showFrequency ? "Δt" : "ΔS",
                    formatMetricText(
                        intervalText.showFrequency ? delta.deltaTime : intervalText.delta,
                        intervalText.showFrequency ? displayData.timeUnit.c_str() : intervalText.deltaUnit.c_str()));
        }

        if (selection.deltaValue) {
            if (!bitMode) {
                addChip(chips, "Δy", formatMetricText(delta.deltaValue, nullptr));
            }
        }

        if (selection.frequency && intervalText.showFrequency) {
            addChip(chips, "Freq", formatMetricText(intervalText.frequencyHz, "Hz"));
        }

        if (selection.period && intervalText.showFrequency) {
            addChip(chips, "T", formatMetricText(std::abs(delta.deltaTime), displayData.timeUnit.c_str()));
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

void drawMeasurementOverlay(const plot::WaveViewState& view,
                            const plot::WaveSnapshot& snapshot,
                            const plot::WaveDisplayData& displayData,
                            const PlotRenderResult& result)
{
    if (!view.showMeasurementOverlay) {
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

    const ImVec2 plotPos = ImPlot::GetPlotPos();
    const ImVec2 plotSize = ImPlot::GetPlotSize();

    constexpr float margin = 8.0F;
    constexpr float padding = 9.0F;
    constexpr float headerGap = 7.0F;
    constexpr float overlayRounding = 8.0F;
    constexpr float maxOverlayWidth = 360.0F;

    constexpr float chipGapX = 6.0F;
    constexpr float chipGapY = 6.0F;
    constexpr float chipPadX = 8.0F;
    constexpr float chipPadY = 4.0F;

    const float gridMaxWidth = (std::min) (maxOverlayWidth, plotSize.x * 0.52F) - padding * 2.0F;

    const ImVec2 gridSize = calcChipGridSize(chips, gridMaxWidth, chipGapX, chipGapY, chipPadX, chipPadY);

    std::string title = "测量";
    if (result.bitMeasurementActive) {
        title = "周期测量";
    }
    if (!measuredChannel.empty()) {
        title += " · " + measuredChannel;
    }

    const float titleWidth = ImGui::CalcTextSize(title.c_str()).x;
    const float contentWidth = (std::max) (gridSize.x, titleWidth);

    const float overlayWidth = contentWidth + padding * 2.0F;
    const float overlayHeight = padding * 2.0F + ImGui::GetTextLineHeight() + headerGap + gridSize.y;

    const ImVec2 overlayMax(plotPos.x + plotSize.x - margin, plotPos.y + margin + overlayHeight);

    const ImVec2 overlayMin(overlayMax.x - overlayWidth, plotPos.y + margin);

    auto* drawList = ImPlot::GetPlotDrawList();

    const ImU32 bgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.035F, 0.040F, 0.050F, 0.72F));
    const ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.000F, 1.000F, 1.000F, 0.15F));
    const ImU32 accentColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.300F, 0.620F, 1.000F, 0.85F));

    const ImU32 titleColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.960F, 0.970F, 1.000F, 0.98F));
    const ImU32 chipBgColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.000F, 1.000F, 1.000F, 0.075F));
    const ImU32 chipBorderColor = ImGui::ColorConvertFloat4ToU32(ImVec4(1.000F, 1.000F, 1.000F, 0.12F));
    const ImU32 chipLabelColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.660F, 0.720F, 0.800F, 0.94F));
    const ImU32 chipValueColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.940F, 0.960F, 0.990F, 0.98F));

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
