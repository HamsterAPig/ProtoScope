#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>

namespace protoscope::ui {
namespace {

struct FftGetterPayload {
    const plot::WaveFftBin* bins{nullptr};
};

ImPlotPoint fftBinGetter(int index, void* userData) {
    const auto* payload = static_cast<const FftGetterPayload*>(userData);
    const auto& bin = payload->bins[index];
    return {bin.frequencyHz, bin.displayMagnitude};
}

void drawCenteredHint(const char* message) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 textSize = ImGui::CalcTextSize(message);
    ImGui::SetCursorPosX((std::max)(0.0F, (available.x - textSize.x) * 0.5F));
    ImGui::SetCursorPosY((std::max)(0.0F, (available.y - textSize.y) * 0.5F));
    ImGui::TextUnformatted(message);
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

    const char* yLabel = wave.view.fft.magnitudeMode == plot::WaveFftMagnitudeMode::Decibel ? "幅值 (dB)" : "幅值";
    if (!ImPlot::BeginPlot("##wave_fft_plot", ImVec2(-1.0F, -1.0F), ImPlotFlags_NoMenus)) {
        return result;
    }

    result.plotRendered = true;
    ImPlot::SetupAxes("频率 (Hz)", yLabel);
    ImPlot::SetupAxisLimits(ImAxis_X1, 0.0, fftFrame->maxFrequencyHz, ImGuiCond_Always);
    const double padding = (std::max)(1e-9, (fftFrame->maxDisplayMagnitude - fftFrame->minDisplayMagnitude) * 0.08);
    ImPlot::SetupAxisLimits(ImAxis_Y1,
                            fftFrame->minDisplayMagnitude - padding,
                            fftFrame->maxDisplayMagnitude + padding,
                            ImGuiCond_Always);

    for (const auto& channel : fftFrame->channels) {
        if (!channel.enabled || !channel.valid || channel.bins.empty()) {
            continue;
        }
        const auto color = channelColor(frame.fullSnapshot->channels[channel.channelIndex], channel.channelIndex);
        FftGetterPayload payload{.bins = channel.bins.data()};
        const std::string label = channel.label + " FFT";
        ImPlotSpec spec{};
        spec.LineColor = color;
        spec.LineWeight = 1.5F;
        ImPlot::PlotLineG(label.c_str(), &fftBinGetter, &payload, static_cast<int>(channel.bins.size()), spec);
        if (channel.fundamental.has_value() && std::isfinite(channel.fundamental->frequencyHz)) {
            ImPlot::TagX(channel.fundamental->frequencyHz, color, "基波 %.4g Hz", channel.fundamental->frequencyHz);
        }
    }

    ImPlot::EndPlot();
    return result;
}

} // namespace protoscope::ui
