#include "../src/ui/wave/wave_render_service.hpp"

#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace protoscope;
namespace {
constexpr int width = 1000, height = 720;
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }

std::vector<unsigned char> capture(const std::filesystem::path& directory, const std::string& name)
{
    std::vector<unsigned char> pixels(width * height * 3);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    check(glGetError() == GL_NO_ERROR, "framebuffer read failed");
    if (!directory.empty()) {
        std::filesystem::create_directories(directory);
        auto bgr = pixels;
        for (std::size_t i = 0; i < bgr.size(); i += 3) std::swap(bgr[i], bgr[i+2]);
        std::array<unsigned char, 54> header{};
        header[0] = 'B'; header[1] = 'M'; header[26] = 1; header[28] = 24;
        const auto put = [&](std::size_t offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) header[offset+i] = static_cast<unsigned char>(value >> (8*i));
        };
        put(2, static_cast<std::uint32_t>(54 + bgr.size())); put(10,54); put(14,40); put(18,width); put(22,height);
        std::ofstream output(directory / (name + ".bmp"), std::ios::binary);
        output.write(reinterpret_cast<const char*>(header.data()), header.size());
        output.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
        check(bool(output), "screenshot write failed");
    }
    return pixels;
}

std::size_t matchingPixels(const std::vector<unsigned char>& pixels, ImVec4 color, ImVec2 min, ImVec2 max)
{
    std::size_t count = 0;
    for (int y = (std::max)(0, int(min.y)); y < (std::min)(height, int(max.y)); ++y)
        for (int x = (std::max)(0, int(min.x)); x < (std::min)(width, int(max.x)); ++x) {
            const auto i = ((height-1-y)*width+x)*3;
            if (std::abs(pixels[i] - color.x*255) <= 2 &&
                std::abs(pixels[i+1] - color.y*255) <= 2 &&
                std::abs(pixels[i+2] - color.z*255) <= 2) ++count;
        }
    return count;
}

std::size_t readableTextPixels(const std::vector<unsigned char>& pixels, ImVec2 min, ImVec2 max, double target)
{
    const auto read = [&](int x, int y) {
        const auto i = ((height-1-y)*width+x)*3;
        return plot::OverviewColor{pixels[i]/255.,pixels[i+1]/255.,pixels[i+2]/255.,1};
    };
    // 字体抗锯齿会改变字形像素 RGB，按实测背景统计满足对比度的核心像素。
    const auto background = read((std::max)(0,int(min.x)-2), int(min.y));
    std::size_t count = 0;
    for (int y = int(min.y); y < int(max.y); ++y)
        for (int x = int(min.x); x < int(max.x); ++x)
            count += plot::overviewContrast(read(x,y),background) >= target;
    return count;
}

void verify(const std::filesystem::path& directory)
{
    plot::WaveDockState wave;
    auto& view = wave.view;
    view.initialized = true;
    view.defaultViewportPending = false;
    view.autoFollowLatest = false;
    view.viewMinTime = .2; view.viewMaxTime = .8; view.visibleDuration = .6;
    view.viewMinValue = -1.5; view.viewMaxValue = 1.5;
    view.sampleFrequencyHz = 2048;
    view.showCursors = true;
    view.cursors[0].time = .3; view.cursors[1].time = .65;
    view.showChannelLegend = true;
    view.interactionAnimationEnabled = false;
    view.glowEnabled = false;
    view.auxiliaryCursors.add(.45, .45);
    for (std::size_t c = 0; c < 3; ++c) {
        wave.buffer.setChannelSpec(c, {.label = "Signal " + std::to_string(c + 1)});
        plot::WaveAppendRequest request;
        for (int i = 0; i < 4096; ++i) request.samples.push_back({i/2048., c == 2 ? double((i/100)%4) : std::sin(i*.03+c)});
        wave.buffer.append(c, std::move(request));
    }
    wave.buffer.setChannelSpec(0, {.label = "Custom translucent", .color = std::array{.15F,.7F,.45F,.3F}});
    wave.buffer.setChannelSpec(2, {.label = "Digital", .bitDisplay = {.enabled = true, .bitCount = 2}});
    const auto colorBefore = wave.buffer.channelSpec(0)->color;
    const auto cursorIdentity = view.auxiliaryCursors.items.front().id;
    const auto dataRevision = wave.buffer.analysisRevision();
    ImVec2 textMin, textMax, mutedMin, mutedMax;
    auto render = [&] {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(ImVec2(width,height));
        ImGui::Begin("Theme verification", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
        ImGui::TextUnformatted("Signal acquisition 0123456789");
        textMin = ImGui::GetItemRectMin(); textMax = ImGui::GetItemRectMax();
        ImGui::TextDisabled("Frequency / Cursor / Measurement");
        mutedMin = ImGui::GetItemRectMin(); mutedMax = ImGui::GetItemRectMax();
        auto frame = ui::prepareWaveFrame(wave, width-40);
        ImGui::BeginChild("overview", ImVec2(0,100));
        ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot, *frame.overviewDisplayData,
            plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6), {0,1,2}, frame.renderBudget);
        ImGui::EndChild();
        if (view.fft.enabled) ui::drawWaveFftPlot(wave, frame, true, true);
        else ui::drawOscilloscopePlot(wave, frame, {.drawMeasurementOverlay = true, .drawLegendOverlay = true}, nullptr);
        ImGui::End();
        ImGui::Render();
        glViewport(0,0,width,height);
        const auto bg = ui::activeUiStyleTokens().appBackground;
        glClearColor(bg.x,bg.y,bg.z,1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glFinish();
    };
    for (const auto theme : {config::GuiTheme::ProfessionalDark, config::GuiTheme::DebugHighContrast,
                             config::GuiTheme::ProfessionalLight, config::GuiTheme::ProfessionalDark}) {
        ui::applyUiTheme(theme);
        for (int scenario = 0; scenario < 6; ++scenario) {
            view.viewMode = scenario == 1 ? plot::WaveViewMode::Split : plot::WaveViewMode::Overlay;
            view.phosphorEnabled = scenario >= 4;
            view.phosphorBackend = scenario == 4 ? plot::WavePhosphorBackend::CpuTexture : plot::WavePhosphorBackend::GpuFbo;
            view.fft.enabled = scenario == 3;
            view.fft.displayMode = plot::WaveFftDisplayMode::FullSpectrum;
            view.overviewShowBitChannels = true;
            view.overviewSelection.automatic = scenario != 2;
            view.viewMinTime = .2;
            view.viewMaxTime = scenario == 2 ? .200001 : .8;
            view.forceNextMainPlotLimits = true;
            if (view.fft.enabled) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do {
                    ui::prepareWaveFrame(wave, width);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while ((!wave.cachedFftFrame.valid || wave.fftRequestActive) && std::chrono::steady_clock::now() < deadline);
                check(wave.cachedFftFrame.valid, "FFT not ready");
            }
            ++view.phosphorResetGeneration;
            for (int i = 0; i < 8; ++i) render();
            const auto name = std::string(config::guiThemeId(theme)) + "-" + std::to_string(scenario);
            const auto pixels = capture(directory, name);
            const auto& ui = ui::activeUiStyleTokens();
            check(readableTextPixels(pixels, textMin, textMax,
                theme == config::GuiTheme::DebugHighContrast ? 12 : 4.5) > 20, "primary text pixel contrast");
            check(readableTextPixels(pixels, mutedMin, mutedMax,
                theme == config::GuiTheme::DebugHighContrast ? 7 : 4.5) > 20, "secondary text pixel contrast");
            check(matchingPixels(pixels, ui.genericPlotBackground, {0,160}, {width,height}) > 1000, "plot background pixels missing");
            if (scenario >= 4) {
                check(!view.autoFollowLatest, "theme change unfroze viewport");
                check(view.lastRenderStats.phosphorBackendStatus.find(scenario == 4 ? "CPU Texture" : "GPU FBO") != std::string::npos,
                      "requested phosphor backend missing");
                const auto bg = ui::activeWaveStyleTokens().plotBackground;
                std::size_t readableCore = 0;
                for (int y = 370; y < height-65; ++y) for (int x = 40; x < width-40; ++x) {
                    const auto i = ((height-1-y)*width+x)*3;
                    const plot::OverviewColor pixel{pixels[i]/255., pixels[i+1]/255., pixels[i+2]/255., 1};
                    if (pixel.b-pixel.r > .12 &&
                        plot::overviewContrast(pixel, {bg.x,bg.y,bg.z,1}) >= 2.95) ++readableCore;
                }
                check(readableCore > 20, "phosphor core pixels below 3:1 contrast");
            }
            check(wave.buffer.analysisRevision() == dataRevision && wave.buffer.channelSpec(0)->color == colorBefore,
                  "theme changed acquisition or source color");
            check(view.auxiliaryCursors.items.front().id == cursorIdentity, "theme changed cursor identity");
            std::cout << name << " pixels verified; backend=" << view.lastRenderStats.phosphorBackendStatus << '\n';
        }
    }
}
}

int main(int argc, char** argv)
{
    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    auto* window = glfwCreateWindow(width,height,"Theme verification",nullptr,nullptr);
    if (!window) { glfwTerminate(); return 2; }
    glfwMakeContextCurrent(window);
    ImGui::CreateContext(); ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = ImVec2(width,height);
    ImGui::GetIO().DeltaTime = 1.F/60;
    if (std::filesystem::exists("C:/Windows/Fonts/msyh.ttc"))
        ImGui::GetIO().Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 16, nullptr,
                                               ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon());
    ImGui_ImplOpenGL3_Init("#version 330");
    unsigned char* pixels; int w,h;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    int status = 0;
    try { verify(argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path{}); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    ImGui_ImplOpenGL3_Shutdown();
    ImPlot::DestroyContext(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate();
    return status;
}
