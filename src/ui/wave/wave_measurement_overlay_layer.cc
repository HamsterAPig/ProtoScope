#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::ui {
namespace {

void appendOptionalMetric(std::vector<std::string>& lines,
                          const char* label,
                          const std::optional<double>& value,
                          const char* unit = nullptr) {
    lines.push_back(std::string(label) + "=" + (value.has_value() ? formatMetricText(*value, unit) : "N/A"));
}

void appendMetric(std::vector<std::string>& lines, const char* label, double value, const char* unit = nullptr) {
    lines.push_back(std::string(label) + "=" + formatMetricText(value, unit));
}

} // namespace

void drawMeasurementOverlay(const plot::WaveViewState& view,
                            const plot::WaveSnapshot& snapshot,
                            const plot::WaveDisplayData& displayData,
                            const PlotRenderResult& result) {
    if (!view.showMeasurementOverlay) {
        return;
    }
    std::vector<std::string> lines;
    const auto& selection = view.measurement;
    if (view.showCursors && selection.cursorA && result.cursorReadouts[0].has_value()) {
        const auto& c0 = *result.cursorReadouts[0];
        lines.push_back("Cursor A: " + snapshot.channels[c0.channelIndex].label + "  t="
            + formatMetricText(c0.time, displayData.timeUnit.c_str()) + "  y=" + formatMetricText(c0.value, nullptr));
    }
    if (view.showCursors && selection.cursorB && result.cursorReadouts[1].has_value()) {
        const auto& c1 = *result.cursorReadouts[1];
        lines.push_back("Cursor B: " + snapshot.channels[c1.channelIndex].label + "  t="
            + formatMetricText(c1.time, displayData.timeUnit.c_str()) + "  y=" + formatMetricText(c1.value, nullptr));
    }
    if (view.showCursors && result.cursorReadouts[0].has_value() && result.cursorReadouts[1].has_value()) {
        const auto delta = plot::OscilloscopeBuffer::makeDelta(*result.cursorReadouts[0], *result.cursorReadouts[1]);
        const auto intervalText = plot::makeCursorIntervalText(
            *result.cursorReadouts[0], *result.cursorReadouts[1], displayData.axisSource, displayData.timeUnit);
        if (intervalText.valid) {
            std::string line;
            if (selection.deltaTime) {
                line += (intervalText.showFrequency ? "Δt=" : "Δsample=");
                line += formatMetricText(intervalText.showFrequency ? delta.deltaTime : intervalText.delta,
                    intervalText.showFrequency ? displayData.timeUnit.c_str() : intervalText.deltaUnit.c_str());
            }
            if (selection.deltaValue) {
                if (!line.empty()) {
                    line += "  ";
                }
                line += "Δy=" + formatMetricText(delta.deltaValue, nullptr);
            }
            if (selection.frequency && intervalText.showFrequency) {
                if (!line.empty()) {
                    line += "  ";
                }
                line += "f=" + formatMetricText(intervalText.frequencyHz, "Hz");
            }
            if (selection.period && intervalText.showFrequency) {
                if (!line.empty()) {
                    line += "  ";
                }
                line += "T=" + formatMetricText(std::abs(delta.deltaTime), displayData.timeUnit.c_str());
            }
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
        }
    }
    if (view.showCursors && result.measurement.has_value() && result.measurement->valid) {
        const auto& m = *result.measurement;
        const auto& channel = snapshot.channels[m.channelIndex];
        if (selection.sampleCount || selection.span) {
            std::string line = "Measure " + channel.label + ":";
            if (selection.sampleCount) {
                line += " N=" + std::to_string(m.sampleCount);
            }
            if (selection.span) {
                line += " span=" + formatMetricText(m.duration, displayData.timeUnit.c_str());
            }
            lines.push_back(std::move(line));
        }
        if (selection.min) {
            appendMetric(lines, "min", m.minValue, channel.unit.c_str());
        }
        if (selection.max) {
            appendMetric(lines, "max", m.maxValue, channel.unit.c_str());
        }
        if (selection.peakToPeak) {
            appendMetric(lines, "Vpp", m.peakToPeak, channel.unit.c_str());
        }
        if (selection.mean) {
            appendMetric(lines, "mean", m.meanValue, channel.unit.c_str());
        }
        if (selection.rms) {
            appendMetric(lines, "rms", m.rmsValue, channel.unit.c_str());
        }
        if (selection.median) {
            appendMetric(lines, "median", m.medianValue, channel.unit.c_str());
        }
        if (selection.p95) {
            appendMetric(lines, "p95", m.p95Value, channel.unit.c_str());
        }
        if (selection.p99) {
            appendMetric(lines, "p99", m.p99Value, channel.unit.c_str());
        }
        if (selection.variance) {
            appendMetric(lines, "variance", m.variance);
        }
        if (selection.stddev) {
            appendMetric(lines, "stddev", m.stddev, channel.unit.c_str());
        }
        if (selection.cv) {
            appendOptionalMetric(lines, "cv", m.cv);
        }
        if (selection.mad) {
            appendMetric(lines, "mad", m.mad, channel.unit.c_str());
        }
        if (selection.medianAbsDev) {
            appendMetric(lines, "medianAbsDev", m.medianAbsDev, channel.unit.c_str());
        }
        if (selection.iqr) {
            appendMetric(lines, "iqr", m.iqr, channel.unit.c_str());
        }
        if (selection.p95Spread) {
            appendMetric(lines, "p95Spread", m.p95Spread, channel.unit.c_str());
        }
        if (selection.highWidth) {
            appendMetric(lines, "highWidth", m.highWidth, displayData.timeUnit.c_str());
        }
        if (selection.lowWidth) {
            appendMetric(lines, "lowWidth", m.lowWidth, displayData.timeUnit.c_str());
        }
        if (selection.dutyCycle) {
            appendOptionalMetric(lines, "duty", m.dutyCycle, "%");
        }
        if (selection.riseTime) {
            appendOptionalMetric(lines, "rise", m.riseTime, displayData.timeUnit.c_str());
        }
        if (selection.fallTime) {
            appendOptionalMetric(lines, "fall", m.fallTime, displayData.timeUnit.c_str());
        }
        if (selection.edgeCount) {
            lines.push_back("edgeCount=" + std::to_string(m.edgeCount));
        }
        if (selection.absoluteError) {
            appendOptionalMetric(lines, "absoluteError", m.absoluteError, channel.unit.c_str());
        }
        if (selection.relativeErrorPercent) {
            appendOptionalMetric(lines, "relativeError", m.relativeErrorPercent, "%");
        }
        if (selection.meanError) {
            appendOptionalMetric(lines, "meanError", m.meanError, channel.unit.c_str());
        }
        if (selection.mse) {
            appendOptionalMetric(lines, "MSE", m.mse);
        }
        if (selection.rmse) {
            appendOptionalMetric(lines, "RMSE", m.rmse, channel.unit.c_str());
        }
        if (selection.mae) {
            appendOptionalMetric(lines, "MAE", m.mae, channel.unit.c_str());
        }
        if (selection.maxAbsError) {
            appendOptionalMetric(lines, "maxAbsError", m.maxAbsError, channel.unit.c_str());
        }
        if (selection.bias) {
            appendOptionalMetric(lines, "bias", m.bias, channel.unit.c_str());
        }
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
