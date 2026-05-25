#include "protoscope/ui/wave_dock_renderer.hpp"

#include <imgui.h>
#include <implot.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::ui {

namespace {

struct PlotGetterPayload {
    const plot::EnvelopePoint* points{nullptr};
};

ImPlotPoint envelopeLineMinGetter(int index, void* data) {
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].minValue};
}

ImPlotPoint envelopeLineMaxGetter(int index, void* data) {
    const auto* payload = static_cast<const PlotGetterPayload*>(data);
    return ImPlotPoint{payload->points[index].time, payload->points[index].maxValue};
}

void renderEnvelopeAsBars(const std::vector<plot::EnvelopePoint>& points, const ImVec4& color) {
    if (points.empty()) {
        return;
    }
    auto* drawList = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();
    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(color);
    for (const auto& point : points) {
        const ImVec2 minPos = ImPlot::PlotToPixels(point.time, point.minValue);
        const ImVec2 maxPos = ImPlot::PlotToPixels(point.time, point.maxValue);
        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y), lineColor, 1.0F);
    }
    ImPlot::PopPlotClipRect();
}

ImVec4 withAlpha(ImVec4 color, float alphaScale) {
    color.w *= alphaScale;
    return color;
}

float phosphorFade(double latestTime, double pointTime, double persistenceWindow) {
    if (persistenceWindow <= 1e-12) {
        return 1.0F;
    }
    const double age = (std::max)(0.0, latestTime - pointTime);
    const double fade = 1.0 - age / persistenceWindow;
    return static_cast<float>((std::clamp)(fade, 0.08, 1.0));
}

float densityStrength(std::size_t sampleCount) {
    const double strength = std::log2(static_cast<double>(sampleCount) + 1.0) / 4.0;
    return static_cast<float>((std::clamp)(0.35 + strength, 0.35, 1.0));
}

void renderPhosphorEnvelope(const std::vector<plot::EnvelopePoint>& points,
                            const ImVec4& color,
                            double latestTime,
                            double persistenceWindow,
                            double glowIntensity) {
    if (points.empty()) {
        return;
    }

    auto* drawList = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    bool hasPrevMid = false;
    ImVec2 prevMid{};
    float prevAlpha = 0.0F;
    for (const auto& point : points) {
        const float fade = phosphorFade(latestTime, point.time, persistenceWindow);
        const float density = densityStrength(point.sampleCount);
        const float alpha = static_cast<float>((std::clamp)(fade * density * glowIntensity, 0.05, 1.0));

        const ImVec2 minPos = ImPlot::PlotToPixels(point.time, point.minValue);
        const ImVec2 maxPos = ImPlot::PlotToPixels(point.time, point.maxValue);
        const ImVec2 midPos = ImVec2(minPos.x, 0.5F * (minPos.y + maxPos.y));

        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.12F)), 7.0F);
        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.28F)), 3.0F);
        drawList->AddLine(ImVec2(minPos.x, minPos.y), ImVec2(maxPos.x, maxPos.y),
                          ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.9F)), 1.0F);
        drawList->AddCircleFilled(midPos, 1.5F + 1.5F * alpha,
                                  ImGui::ColorConvertFloat4ToU32(withAlpha(color, alpha * 0.85F)));

        if (hasPrevMid) {
            const float lineAlpha = (std::min)(prevAlpha, alpha);
            drawList->AddLine(prevMid, midPos,
                              ImGui::ColorConvertFloat4ToU32(withAlpha(color, lineAlpha * 0.18F)), 5.0F);
            drawList->AddLine(prevMid, midPos,
                              ImGui::ColorConvertFloat4ToU32(withAlpha(color, lineAlpha * 0.75F)), 1.2F);
        }
        hasPrevMid = true;
        prevMid = midPos;
        prevAlpha = alpha;
    }

    ImPlot::PopPlotClipRect();
}

} // namespace

WaveDockRenderer::WaveDockRenderer(app::Application& application)
    : application_(application) {}

std::string WaveDockRenderer::formatMetric(double value, const char* baseUnit) {
    const char* unit = baseUnit != nullptr ? baseUnit : "";
    const double absValue = std::abs(value);
    double scaled = value;
    const char* prefix = "";
    if (absValue >= 1e9) {
        scaled = value / 1e9;
        prefix = "G";
    } else if (absValue >= 1e6) {
        scaled = value / 1e6;
        prefix = "M";
    } else if (absValue >= 1e3) {
        scaled = value / 1e3;
        prefix = "k";
    } else if (absValue > 0.0 && absValue < 1e-9) {
        scaled = value * 1e12;
        prefix = "p";
    } else if (absValue > 0.0 && absValue < 1e-6) {
        scaled = value * 1e9;
        prefix = "n";
    } else if (absValue > 0.0 && absValue < 1e-3) {
        scaled = value * 1e6;
        prefix = "u";
    } else if (absValue > 0.0 && absValue < 1.0) {
        scaled = value * 1e3;
        prefix = "m";
    }

    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%.4g %s%s", scaled, prefix, unit);
    return buffer;
}

void WaveDockRenderer::draw(bool& showWaveDock) {
    if (!showWaveDock) {
        return;
    }

    if (ImGui::Begin("波形", &showWaveDock)) {
        auto& wave = application_.docks().waveState();
        auto& view = wave.view;
        const auto& config = wave.buffer.viewConfig();

        ImGui::Checkbox("自动跟随最新数据", &view.autoFollowLatest);
        ImGui::SameLine();
        ImGui::Checkbox("交互后暂停跟随", &view.pauseAutoFollowOnInteraction);
        ImGui::SameLine();
        ImGui::Checkbox("锁定纵轴", &view.lockVerticalRange);
        ImGui::SameLine();
        if (ImGui::Button("清空历史")) {
            application_.resetWaveHistory();
        }

        ImGui::SetNextItemWidth(180.0F);
        ImGui::InputDouble("可视时长", &view.visibleDuration, config.timeScale, config.timeScale * 10.0, "%.6f");
        if (view.visibleDuration <= 0.0) {
            view.visibleDuration = (std::max)(config.timeScale, 1e-6);
        }
        ImGui::SameLine();
        ImGui::Checkbox("磷光辉光", &view.phosphorGlowEnabled);

        ImGui::SetNextItemWidth(180.0F);
        ImGui::InputDouble("余辉时间窗", &view.persistenceWindow, config.timeScale, config.timeScale * 10.0, "%.6f");
        if (view.persistenceWindow <= 0.0) {
            view.persistenceWindow = (std::max)(config.timeScale, 1e-6);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(140.0F);
        const double glowMin = 0.2;
        const double glowMax = 2.5;
        ImGui::SliderScalar("辉光强度", ImGuiDataType_Double, &view.glowIntensity, &glowMin, &glowMax, "%.2f");

        if (view.lockVerticalRange) {
            ImGui::SetNextItemWidth(140.0F);
            ImGui::InputDouble("纵轴最小", &view.manualVerticalMin, 0.1, 1.0, "%.6f");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(140.0F);
            ImGui::InputDouble("纵轴最大", &view.manualVerticalMax, 0.1, 1.0, "%.6f");
        }

        syncWaveViewToLatest();
        if (!view.initialized) {
            const double halfDuration = view.visibleDuration * 0.5;
            view.viewMinTime = view.centerTime - halfDuration;
            view.viewMaxTime = view.centerTime + halfDuration;
            view.viewMinValue = view.manualVerticalMin;
            view.viewMaxValue = view.manualVerticalMax;
            view.initialized = true;
        }
        auto snapshot = wave.buffer.snapshot(view.viewMinTime, view.viewMaxTime);

        if (snapshot.channels.empty()) {
            ImGui::TextUnformatted("Lua 尚未通过 proto.plot.setup / proto.plot.push 提供波形数据。");
        } else if (ImPlot::BeginPlot("##oscilloscope", ImVec2(-1.0F, -1.0F), ImPlotFlags_None)) {
            ImPlot::SetupAxis(ImAxis_X1, "Time", ImPlotAxisFlags_None);
            ImPlot::SetupAxis(ImAxis_Y1, snapshot.config.verticalUnit.c_str(), ImPlotAxisFlags_None);
            if (view.autoFollowLatest) {
                ImPlot::SetupAxisLimits(ImAxis_X1, view.viewMinTime, view.viewMaxTime, ImPlotCond_Always);
            } else {
                ImPlot::SetupAxisLimits(ImAxis_X1, view.viewMinTime, view.viewMaxTime, ImPlotCond_Once);
            }
            if (view.lockVerticalRange) {
                ImPlot::SetupAxisLimits(ImAxis_Y1, view.manualVerticalMin, view.manualVerticalMax, ImPlotCond_Always);
            } else {
                ImPlot::SetupAxisLimits(ImAxis_Y1, view.viewMinValue, view.viewMaxValue, ImPlotCond_Once);
            }

            const ImPlotPoint mousePos = ImPlot::GetPlotMousePos();
            const ImPlotRect limits = ImPlot::GetPlotLimits();
            const double timeSnapDistance = (limits.X.Max - limits.X.Min) / 80.0;
            const double valueSnapDistance = (limits.Y.Max - limits.Y.Min) / 30.0;
            std::array<std::optional<plot::CursorReadout>, 2> cursorReadouts{};
            std::optional<plot::MeasurementReadout> measurement;

            for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
                const auto& channel = snapshot.channels[channelIndex];
                const float contentWidth = ImGui::GetContentRegionAvail().x;
                const auto envelope = wave.buffer.buildEnvelope(channelIndex, limits.X.Min, limits.X.Max, static_cast<std::size_t>((std::max)(contentWidth, 64.0F)));
                if (envelope.points.empty()) {
                    continue;
                }

                PlotGetterPayload payload{.points = envelope.points.data()};
                const ImVec4 channelColor = ImVec4(0.15F + 0.25F * static_cast<float>(channelIndex % 3),
                                                   0.75F,
                                                   0.35F + 0.2F * static_cast<float>((channelIndex + 1) % 3),
                                                   1.0F);
                ImPlot::PlotLineG((channel.label + " min").c_str(), &envelopeLineMinGetter, &payload, static_cast<int>(envelope.points.size()));
                ImPlot::PlotLineG((channel.label + " max").c_str(), &envelopeLineMaxGetter, &payload, static_cast<int>(envelope.points.size()));
                if (view.phosphorGlowEnabled) {
                    renderPhosphorEnvelope(envelope.points, channelColor, limits.X.Max, view.persistenceWindow, view.glowIntensity);
                } else {
                    renderEnvelopeAsBars(envelope.points, channelColor);
                }

                if (ImPlot::IsPlotHovered() && view.showHoverReadout) {
                    auto hovered = wave.buffer.findNearest(channelIndex, mousePos.x, mousePos.y, timeSnapDistance, valueSnapDistance);
                    if (hovered.has_value()) {
                        ImPlot::Annotation(hovered->time, hovered->value, ImVec4(1.0F, 1.0F, 0.2F, 1.0F), ImVec2(12.0F, -12.0F), true,
                            "%s t=%s y=%.6g %s",
                            channel.label.c_str(),
                            formatMetric(hovered->time, config.timeUnit.c_str()).c_str(),
                            hovered->value,
                            channel.unit.c_str());
                    }
                }
            }

            bool anyCursorHeld = false;
            for (std::size_t cursorIndex = 0; cursorIndex < view.cursors.size(); ++cursorIndex) {
                auto& cursor = view.cursors[cursorIndex];
                if (!cursor.enabled) {
                    continue;
                }
                bool clicked = false;
                bool hovered = false;
                bool held = false;
                ImPlot::DragLineX(static_cast<int>(100 + cursorIndex), &cursor.time,
                    ImVec4(cursorIndex == 0 ? 0.2F : 1.0F, 0.9F, 0.3F, 1.0F),
                    1.5F,
                    ImPlotDragToolFlags_NoFit,
                    &clicked,
                    &hovered,
                    &held);
                anyCursorHeld = anyCursorHeld || held;

                if (held || hovered || cursor.pinned) {
                    std::optional<plot::CursorReadout> best;
                    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
                        auto nearest = wave.buffer.findNearestByTime(channelIndex, cursor.time, timeSnapDistance);
                        if (!nearest.has_value()) {
                            continue;
                        }
                        if (!best.has_value() || std::abs(nearest->time - cursor.time) < std::abs(best->time - cursor.time)) {
                            best = nearest;
                        }
                    }
                    if (best.has_value()) {
                        // 核心流程：拖动中保持用户手里的连续时间，避免被最近采样点每帧抢写导致抖动。
                        cursor.channelIndex = best->channelIndex;
                        if (!held) {
                            cursor.time = best->time;
                        }
                        cursor.value = best->value;
                        if (held) {
                            best->time = cursor.time;
                        }
                        cursorReadouts[cursorIndex] = best;
                        ImPlot::TagX(cursor.time, ImVec4(1.0F, 1.0F, 1.0F, 0.85F), "C%zu %s", cursorIndex + 1, formatMetric(cursor.time, config.timeUnit.c_str()).c_str());
                        ImPlot::TagY(cursor.value, ImVec4(1.0F, 1.0F, 1.0F, 0.85F), "%.6g", cursor.value);
                    }
                }
            }

            if (!view.lockVerticalRange) {
                view.viewMinValue = limits.Y.Min;
                view.viewMaxValue = limits.Y.Max;
            }
            view.viewMinTime = limits.X.Min;
            view.viewMaxTime = limits.X.Max;
            view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, config.timeScale);
            view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
            // 核心流程：游标拖动也算用户交互，确保自动跟随暂停，避免拖动时轴范围变化引发闪烁。
            const bool userInteracting = anyCursorHeld || (ImPlot::IsPlotHovered() && (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::GetIO().MouseWheel != 0.0F || ImGui::IsMouseClicked(ImGuiMouseButton_Right)));
            if (userInteracting && view.pauseAutoFollowOnInteraction) {
                view.autoFollowLatest = false;
            }

            ImPlot::EndPlot();

            if (cursorReadouts[0].has_value()) {
                const auto& c0 = *cursorReadouts[0];
                ImGui::Text("Cursor A: %s  t=%s  y=%.6g",
                    snapshot.channels[c0.channelIndex].label.c_str(),
                    formatMetric(c0.time, config.timeUnit.c_str()).c_str(),
                    c0.value);
            }
            if (cursorReadouts[1].has_value()) {
                const auto& c1 = *cursorReadouts[1];
                ImGui::Text("Cursor B: %s  t=%s  y=%.6g",
                    snapshot.channels[c1.channelIndex].label.c_str(),
                    formatMetric(c1.time, config.timeUnit.c_str()).c_str(),
                    c1.value);
            }
            if (cursorReadouts[0].has_value() && cursorReadouts[1].has_value()) {
                const auto delta = plot::OscilloscopeBuffer::makeDelta(*cursorReadouts[0], *cursorReadouts[1]);
                measurement = wave.buffer.measureWindow(cursorReadouts[0]->channelIndex, cursorReadouts[0]->time, cursorReadouts[1]->time);
                ImGui::Text("Δt=%s  Δy=%.6g  f=%s",
                    formatMetric(delta.deltaTime, config.timeUnit.c_str()).c_str(),
                    delta.deltaValue,
                    formatMetric(delta.frequencyHz, "Hz").c_str());
            }
            if (measurement.has_value() && measurement->valid) {
                const auto& m = *measurement;
                const auto& channel = snapshot.channels[m.channelIndex];
                ImGui::Text("Measure %s: N=%zu  span=%s  Vpp=%.6g %s",
                    channel.label.c_str(),
                    m.sampleCount,
                    formatMetric(m.duration, config.timeUnit.c_str()).c_str(),
                    m.peakToPeak,
                    channel.unit.c_str());
                ImGui::Text("min=%.6g  max=%.6g  mean=%.6g  rms=%.6g",
                    m.minValue,
                    m.maxValue,
                    m.meanValue,
                    m.rmsValue);
            }
        }

        if (!wave.channelSummaries.empty()) {
            ImGui::Separator();
            for (const auto& summary : wave.channelSummaries) {
                ImGui::TextUnformatted(summary.c_str());
            }
        }
    }
    ImGui::End();
}

void WaveDockRenderer::syncWaveViewToLatest() {
    auto& wave = application_.docks().waveState();
    if (!wave.view.autoFollowLatest) {
        return;
    }

    const auto snapshot = wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
    double latestTime = 0.0;
    bool found = false;
    for (const auto& channel : snapshot.channels) {
        if (channel.totalSamples == 0 || channel.samples == nullptr) {
            continue;
        }
        latestTime = (std::max)(latestTime, channel.samples[channel.totalSamples - 1].time);
        found = true;
    }
    if (found) {
        wave.view.viewMaxTime = latestTime;
        wave.view.viewMinTime = latestTime - wave.view.visibleDuration;
        wave.view.centerTime = 0.5 * (wave.view.viewMinTime + wave.view.viewMaxTime);
        if (!wave.view.lockVerticalRange && !wave.view.initialized) {
            wave.view.viewMinValue = wave.view.manualVerticalMin;
            wave.view.viewMaxValue = wave.view.manualVerticalMax;
        }
    }
}

} // namespace protoscope::ui
