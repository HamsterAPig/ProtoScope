#include "wave_render_service.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace protoscope::ui {

void drawMeasurementOverlay(const plot::WaveViewState& view,
                            const plot::WaveSnapshot& snapshot,
                            const plot::WaveDisplayData& displayData,
                            const PlotRenderResult& result) {
    if (!view.showMeasurementOverlay) {
        return;
    }
    std::vector<std::string> lines;
    if (view.showCursors && result.cursorReadouts[0].has_value()) {
        const auto& c0 = *result.cursorReadouts[0];
        lines.push_back("Cursor A: " + snapshot.channels[c0.channelIndex].label + "  t="
            + formatMetricText(c0.time, displayData.timeUnit.c_str()) + "  y=" + formatMetricText(c0.value, nullptr));
    }
    if (view.showCursors && result.cursorReadouts[1].has_value()) {
        const auto& c1 = *result.cursorReadouts[1];
        lines.push_back("Cursor B: " + snapshot.channels[c1.channelIndex].label + "  t="
            + formatMetricText(c1.time, displayData.timeUnit.c_str()) + "  y=" + formatMetricText(c1.value, nullptr));
    }
    if (view.showCursors && result.cursorReadouts[0].has_value() && result.cursorReadouts[1].has_value()) {
        const auto delta = plot::OscilloscopeBuffer::makeDelta(*result.cursorReadouts[0], *result.cursorReadouts[1]);
        const auto intervalText = plot::makeCursorIntervalText(
            *result.cursorReadouts[0], *result.cursorReadouts[1], displayData.axisSource, displayData.timeUnit);
        if (intervalText.valid && intervalText.showFrequency) {
            lines.push_back("Δt=" + formatMetricText(delta.deltaTime, displayData.timeUnit.c_str()) + "  Δy="
                + formatMetricText(delta.deltaValue, nullptr) + "  f=" + formatMetricText(intervalText.frequencyHz, "Hz"));
        } else if (intervalText.valid) {
            lines.push_back("Δsample=" + formatMetricText(intervalText.delta, intervalText.deltaUnit.c_str()) + "  Δy="
                + formatMetricText(delta.deltaValue, nullptr));
        }
    }
    if (view.showCursors && result.measurement.has_value() && result.measurement->valid) {
        const auto& m = *result.measurement;
        const auto& channel = snapshot.channels[m.channelIndex];
        lines.push_back("Measure " + channel.label + ": N=" + std::to_string(m.sampleCount) + "  span="
            + formatMetricText(m.duration, displayData.timeUnit.c_str()) + "  Vpp=" + formatMetricText(m.peakToPeak, channel.unit.c_str()));
        lines.push_back("min=" + formatMetricText(m.minValue, channel.unit.c_str()) + "  max="
            + formatMetricText(m.maxValue, channel.unit.c_str()) + "  mean=" + formatMetricText(m.meanValue, channel.unit.c_str())
            + "  rms=" + formatMetricText(m.rmsValue, channel.unit.c_str()));
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
    drawList->AddRectFilled(overlayMin, overlayMax, ImGui::ColorConvertFloat4ToU32(ImVec4(0.04F, 0.045F, 0.05F, 0.68F)), 5.0F);
    drawList->AddRect(overlayMin, overlayMax, ImGui::ColorConvertFloat4ToU32(ImVec4(1.0F, 1.0F, 1.0F, 0.18F)), 5.0F);
    ImVec2 textPos(overlayMin.x + padding, overlayMin.y + padding);
    const ImU32 textColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.92F, 0.94F, 0.98F, 0.95F));
    for (const auto& line : lines) {
        drawList->AddText(textPos, textColor, line.c_str());
        textPos.y += lineSpacing;
    }
}

} // namespace protoscope::ui
