#include "../src/ui/wave/wave_render_service.hpp"
#include <implot_internal.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace protoscope;

int main()
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 900);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int result = 0;
    try {
        plot::WaveDockState wave;
        wave.view.initialized = true;
        wave.view.defaultViewportPending = false;
        wave.view.autoFollowLatest = false;
        wave.view.viewMinTime = 0;
        wave.view.viewMaxTime = 1;
        wave.view.sampleFrequencyHz = 1024;
        wave.view.fft.enabled = true;
        std::vector<plot::WaveSample> samples;
        for (int i = 0; i < 1024; ++i)
            samples.push_back({double(i) / 1024, std::sin(double(i) * 0.1)});
        wave.buffer.append(0, {{}, samples});
        auto frame = ui::prepareWaveFrame(wave, 1200);
        const auto fft = plot::buildWaveFftFrame(*frame.fullSnapshot, *frame.displayData,
                                                wave.view.fft, {1}, 0, 1, 1024);
        if (!fft.valid) throw std::runtime_error("FFT fixture invalid");
        frame.fftFrame = &fft;
        for (const auto size : {ImVec2(1200, 800), ImVec2(640, 420), ImVec2(420, 360)}) {
            for (const char* layout : {"normal", "split", "focus", "overlay"}) {
                std::array<ImRect, 2> baseline{};
                float baselineScroll = 0;
                for (int n = 0; n < 10; ++n) {
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0));
                    ImGui::SetNextWindowSize(size);
                    ImGui::Begin(layout, nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
                    if (std::string_view(layout) == "split")
                        ImGui::BeginChild("split FFT", ImVec2(size.x * 0.6F, 0));
                    wave.view.fftUpdatePending = n % 2 == 0;
                    const auto rendered = ui::drawWaveFftPlot(wave, frame, true, false);
                    auto* magnitude = ImPlot::GetPlot("##wave_fft_magnitude");
                    auto* phase = ImPlot::GetPlot("##wave_fft_phase");
                    if (!rendered.plotRendered || !magnitude || !phase)
                        throw std::runtime_error("FFT plots missing");
                    const std::array<ImRect, 2> rectangles{magnitude->FrameRect, phase->FrameRect};
                    const float scroll = ImGui::GetCurrentWindow()->ScrollMax.y;
                    if (n == 2) { baseline = rectangles; baselineScroll = scroll; }
                    if (n > 2) {
                        for (std::size_t i = 0; i < rectangles.size(); ++i) {
                            const auto& a = rectangles[i];
                            const auto& b = baseline[i];
                            if (a.Min.x != b.Min.x || a.Min.y != b.Min.y ||
                                a.Max.x != b.Max.x || a.Max.y != b.Max.y || scroll != baselineScroll)
                                throw std::runtime_error("FFT pending state changed geometry or scroll");
                        }
                    }
                    if (std::string_view(layout) == "split") ImGui::EndChild();
                    ImGui::End();
                    ImGui::Render();
                }
            }
        }
        std::cout << "FFT pending/ready geometry stable across sizes and layouts\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return result;
}
