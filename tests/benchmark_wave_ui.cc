#include "../src/ui/wave/wave_render_service.hpp"

#include "protoscope/config/config.hpp"
#include "protoscope/ui/ui_theme.hpp"
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace protoscope;
using Clock = std::chrono::steady_clock;

void captureFrame(const std::filesystem::path& directory, const std::string& name)
{
    if (directory.empty()) return;
    std::filesystem::create_directories(directory);
    std::vector<unsigned char> pixels(1280 * 900 * 3);
    glReadPixels(0, 0, 1280, 900, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    for (std::size_t i = 0; i < pixels.size(); i += 3) std::swap(pixels[i], pixels[i + 2]);
    const auto extrema = std::minmax_element(pixels.begin(), pixels.end());
    if (*extrema.first == *extrema.second || glGetError() != GL_NO_ERROR)
        throw std::runtime_error("blank verification framebuffer");
    std::array<unsigned char, 54> header{};
    header[0] = 'B'; header[1] = 'M';
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) header[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, static_cast<std::uint32_t>(54 + pixels.size()));
    put(10, 54); put(14, 40); put(18, 1280); put(22, 900);
    header[26] = 1; header[28] = 24;
    std::ofstream output(directory / (name + ".bmp"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!output) throw std::runtime_error("framebuffer capture write failed");
}

int main(int argc, char** argv)
{
    const bool verify = argc > 1 && std::string_view(argv[1]) == "--verify";
    bool withGl = false;
    for (int i = 1; i < argc; ++i) withGl = withGl || std::string_view(argv[i]) == "--gl";
    std::filesystem::path captureDirectory;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "--capture") captureDirectory = argv[i + 1];
    GLFWwindow* window = nullptr;
    if (withGl) {
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        window = glfwCreateWindow(1280, 900, "wave backend verification", nullptr, nullptr);
        if (!window) { glfwTerminate(); return 2; }
        glfwMakeContextCurrent(window);
        glfwSwapInterval(0);
    }
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ui::applyUiTheme(config::GuiTheme::ProfessionalDark);
    if (withGl && !ImGui_ImplOpenGL3_Init("#version 330")) return 2;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 900);
    io.DeltaTime = 1.0F / 60.0F;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    if (const auto* windows = std::getenv("SystemRoot")) {
        const auto font = std::filesystem::path(windows) / "Fonts" / "msyh.ttc";
        if (std::filesystem::exists(font))
            io.Fonts->AddFontFromFileTTF(font.string().c_str(), 16.0F, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int exitCode = 0;
    try {
        {
            plot::WaveDockState wave;
            wave.view.initialized = true;
            wave.view.defaultViewportPending = false;
            wave.view.autoFollowLatest = false;
            wave.view.viewMinTime = 0;
            wave.view.viewMaxTime = 2048;
            wave.view.visibleDuration = 2048;
            wave.view.showCursors = true;
            for (std::size_t channel = 0; channel < 2; ++channel) {
                wave.buffer.setChannelSpec(channel, {.bitDisplay = {.enabled = true, .bitCount = 8}});
                std::vector<plot::WaveSample> samples;
                for (int i = 0; i < 4096; ++i)
                    samples.push_back({double(i), channel == 0 ? double(i % 256) : double((i / 10) % 256)});
                wave.buffer.append(channel, {{}, samples});
            }
            ImVec2 verificationSize(1200, 850);
            const auto draw = [&] {
                if (withGl) ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(verificationSize);
                ImGui::Begin("digital verification");
                auto frame = ui::prepareWaveFrame(wave, 1200);
                ui::drawOscilloscopePlot(wave, frame,
                    {.drawMeasurementOverlay = false, .drawLegendOverlay = false}, nullptr);
                ImGui::End();
                ImGui::Render();
            };
            io.MouseDown[0] = false;
            draw();
            const auto initialQueries = wave.bitCountQueryCount;
            if (initialQueries != 16 || wave.bitCountCache[0].counts == wave.bitCountCache[1].counts)
                throw std::runtime_error("per-channel digital count initialization");
            const auto original = wave.bitCountCache[0].counts;
            draw();
            if (wave.bitCountQueryCount != initialQueries) throw std::runtime_error("count cache miss without changes");
            io.MouseDown[0] = true;
            for (int n = 0; n < 4; ++n) {
                wave.view.viewMinTime = 50.0 * n;
                wave.view.viewMaxTime = 1500.0 + 50.0 * n;
                wave.view.forceNextMainPlotLimits = true;
                wave.buffer.append(0, {{}, {{4096.0 + n, double(n)}}});
                draw();
                if (wave.bitCountQueryCount != initialQueries || wave.bitCountCache[0].counts != original)
                    throw std::runtime_error("digital counts updated during drag");
            }
            io.MouseDown[0] = false;
            wave.view.overviewWindowDragging = true;
            draw();
            wave.view.overviewWindowDragging = false;
            wave.view.viewportAnimation.active = true;
            draw();
            if (wave.bitCountQueryCount != initialQueries) throw std::runtime_error("digital counts updated in animation");
            wave.view.viewportAnimation.active = false;
            draw();
            if (wave.bitCountQueryCount != initialQueries + 16) throw std::runtime_error("release count must refresh once");
            draw();
            if (wave.bitCountQueryCount != initialQueries + 16) throw std::runtime_error("release count queried twice");
            if (withGl) {
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glFinish();
                captureFrame(captureDirectory, "digital-dark");
            }
            ui::applyUiTheme(config::GuiTheme::DebugHighContrast);
            draw();
            if (withGl) {
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glFinish();
                captureFrame(captureDirectory, "digital-contrast");
            }
            ui::applyUiTheme(config::GuiTheme::ProfessionalDark);
            verificationSize = ImVec2(480, 420);
            for (int i = 0; i < 2; ++i) {
                wave.view.forceNextMainPlotLimits = true;
                draw();
            }
            if (wave.bitCountQueryCount != initialQueries + 16)
                throw std::runtime_error("width-only change repeated total count");
            if (withGl) {
                glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glFinish();
                captureFrame(captureDirectory, "digital-narrow");
            }
            verificationSize = ImVec2(1200, 850);
            wave.view.viewMode = plot::WaveViewMode::Split;
            wave.view.viewMinTime = 200;
            wave.view.viewMaxTime = 800;
            wave.view.forceNextMainPlotLimits = true;
            draw();
            wave.view.forceNextMainPlotLimits = true;
            draw();
            if (wave.bitCountQueryCount != initialQueries + 32) {
                if (withGl) {
                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    glFinish();
                    captureFrame(captureDirectory, "split-diagnostic");
                }
                std::cerr << "split counts=" << wave.bitCountQueryCount << " expected=" << initialQueries + 32
                    << " interacting=" << wave.view.interactionActive << " range=" << wave.view.viewMinTime
                    << ',' << wave.view.viewMaxTime << " first=" << wave.bitCountCache[0].key.minTime
                    << ',' << wave.bitCountCache[0].key.maxTime << " second=" << wave.bitCountCache[1].key.minTime
                    << ',' << wave.bitCountCache[1].key.maxTime << '\n';
                for (const auto& c : wave.cachedDisplayData.channels)
                    std::cerr << "display samples=" << c.samples.size() << " raw="
                        << (c.source ? c.source->totalSamples : 0) << '\n';
                throw std::runtime_error("visible split digital counts did not update");
            }
            for (std::size_t c = 0; c < 2; ++c) {
                const auto& cached = wave.bitCountCache[c];
                const auto snap = wave.buffer.snapshot(-1e20, 1e20, false);
                const auto& channel = snap.channels[c];
                for (std::size_t bit = 0; bit < 8; ++bit) {
                    std::uint64_t expected = 0;
                    for (std::size_t i = 1; i < channel.totalSamples; ++i)
                        if (channel.samples[i].time >= cached.key.minTime && channel.samples[i].time <= cached.key.maxTime &&
                            ((unsigned(channel.samples[i].value) ^ unsigned(channel.samples[i - 1].value)) & (1U << bit)))
                            ++expected;
                    if (cached.counts[bit] != expected) throw std::runtime_error("rendered viewport count mismatch");
                }
            }
            wave.buffer.clear();
            ui::prepareWaveFrame(wave, 1200);
            if (!wave.bitCountCache.empty()) throw std::runtime_error("clear retained digital counts");
        }
        {
            plot::WaveDockState wave;
            std::vector<plot::WaveSample> samples;
            for (int i = 0; i < 20000; ++i)
                samples.push_back({i * 0.1, (0.2 + 0.8 * i / 20000.0) * std::sin(i * 0.7)});
            wave.buffer.append(0, {{}, samples});
            auto snap = wave.buffer.snapshot(-1e20, 1e20, false);
            const auto update = [&](std::size_t width = 500) -> const auto& {
                return ui::cachedOverviewChannel(wave, snap.channels[0], 0,
                    plot::WaveTimeAxisSource::ScriptTime, 0, 1999.9, width, width);
            };
            if (update().envelope.empty()) throw std::runtime_error("overview must query raw envelopes");
            const auto queries = wave.overviewQueryCount;
            wave.view.viewMinTime = 100;
            wave.view.viewMaxTime = 200;
            update();
            if (wave.overviewQueryCount != queries) throw std::runtime_error("main pan rebuilt overview");
            wave.view.overviewNormalizeChannels = true;
            const auto& normalized = update();
            if (wave.overviewQueryCount != queries + 1) throw std::runtime_error("normalization cache key");
            for (const auto& bucket : normalized.envelope)
                if (bucket.minValue < -1.000001 || bucket.maxValue > 1.000001)
                    throw std::runtime_error("normalized overview outside range");
            for (std::size_t i = 0; i < samples.size(); ++i)
                if (snap.channels[0].samples[i].value != samples[i].value)
                    throw std::runtime_error("normalization changed source");
            update(300);
            if (wave.overviewQueryCount != queries + 2) throw std::runtime_error("overview width cache key");
            wave.buffer.setChannelSpec(0, {.scale = -2, .offset = 3});
            snap = wave.buffer.snapshot(-1e20, 1e20, false);
            update(300);
            if (wave.overviewQueryCount != queries + 3) throw std::runtime_error("overview transform cache key");
            wave.buffer.append(0, {{}, {{2000, 12}}});
            snap = wave.buffer.snapshot(-1e20, 1e20, false);
            update(300);
            if (wave.overviewQueryCount != queries + 4) throw std::runtime_error("overview append cache key");
            const auto& rawOverview = ui::cachedOverviewChannel(wave, snap.channels[0], 0,
                plot::WaveTimeAxisSource::ScriptTime, 0, 2000, 30000, 30000);
            if (rawOverview.trace.size() != 20001 || !rawOverview.envelope.empty())
                throw std::runtime_error("low-density overview must retain real polyline");
            wave.buffer.clear();
            wave.buffer.append(0, {{}, samples});
            wave.view.initialized = true;
            wave.view.defaultViewportPending = false;
            wave.view.autoFollowLatest = false;
            wave.view.viewMinTime = 300;
            wave.view.viewMaxTime = 900;
            wave.view.visibleDuration = 600;
            const auto drawOverview = [&](const std::string& name) {
                if (withGl) ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(1100, 300));
                ImGui::Begin("overview verification");
                auto frame = ui::prepareWaveFrame(wave, 1100);
                ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot,
                    *frame.overviewDisplayData, plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6),
                    {0}, frame.renderBudget);
                ImGui::End();
                ImGui::Render();
                if (withGl) {
                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    glFinish();
                    captureFrame(captureDirectory, name);
                }
            };
            drawOverview("overview-am");
            wave.buffer.clear();
            for (auto& sample : samples) sample.value = 4;
            wave.buffer.append(0, {{}, samples});
            wave.view.overviewNormalizeChannels = false;
            drawOverview("overview-constant");
        }
        {
            dock::DockStore docks;
            auto& wave = docks.waveState();
            wave.buffer.configureChannels(4);
            wave.view.initialized = true;
            wave.view.defaultViewportPending = false;
            wave.view.autoFollowLatest = false;
            const std::array<std::array<float, 4>, 4> colors{{
                {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 0, 1, 1}}};
            for (std::size_t i = 0; i < colors.size(); ++i)
                wave.buffer.setChannelSpec(i, {.color = colors[i], .bitDisplay = {.enabled = i % 2 == 1}});
            wave.view.viewMinTime = 0.25;
            wave.view.viewMaxTime = 0.75;
            wave.view.visibleDuration = 0.5;
            wave.view.showCursors = true;
            wave.view.cursors[0] = {.enabled = true, .time = 0.4};
            wave.view.cursors[1] = {.enabled = true, .time = 0.6};
            config::ConfigStore configStore;
            const auto draw = [&](bool showBits, bool normalize, const std::vector<std::size_t>& indices,
                                  const std::string& name) {
                const auto loaded = configStore.loadText(
                    std::string("gui:\n  wave:\n    overview_show_bit_channels: ") +
                    (showBits ? "true\n" : "false\n"));
                configStore.applyToDock(loaded.config, docks);
                wave.view.overviewNormalizeChannels = normalize;
                const bool verifyNavigation = name == "overview-all-bit-navigation";
                const auto previousSpan = wave.view.viewMaxTime - wave.view.viewMinTime;
                // 同时验证真正的 ImPlot 坐标范围与提交给后端的顶点顺序。
                const int passes = verifyNavigation ? 3 : 2;
                for (int pass = 0; pass < passes; ++pass) {
                    if (withGl) ImGui_ImplOpenGL3_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0));
                    ImGui::SetNextWindowSize(ImVec2(1100, 300));
                    ImGui::Begin("overview bit layers");
                    auto frame = ui::prepareWaveFrame(wave, 1100);
                    ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot,
                        *frame.overviewDisplayData, plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6),
                        indices, frame.renderBudget);
                    const auto* plot = ImPlot::GetPlot("##wave_overview");
                    if (!plot || plot->Axes[ImAxis_X1].Range.Min != 0 || plot->Axes[ImAxis_X1].Range.Max != 2)
                        throw std::runtime_error("overview lost full history navigation");
                    if (!showBits && !normalize && indices.size() > 2 && plot->Axes[ImAxis_Y1].Range.Max > 2)
                        throw std::runtime_error("hidden Bit channels changed overview Y bounds");
                    const auto* drawList = ImGui::GetWindowDrawList();
                    std::array<int, 4> first{-1, -1, -1, -1}, last{-1, -1, -1, -1};
                    int selectionVertex = -1, cursorVertex = -1;
                    const auto rectangleColor = plot::overviewRgb(wave.overviewColorCache.selected);
                    for (int v = 0; v < drawList->VtxBuffer.Size; ++v) {
                        const auto vertexColor = drawList->VtxBuffer[v].col;
                        if (selectionVertex < 0 && vertexColor ==
                            ImGui::ColorConvertFloat4ToU32(ImVec4(float(rectangleColor.r),
                                float(rectangleColor.g), float(rectangleColor.b), 1)))
                            selectionVertex = v;
                        for (std::size_t c = 0; c < wave.view.cursors.size(); ++c) {
                            const auto time = wave.view.cursors[c].time;
                            const auto alpha = time >= wave.view.viewMinTime && time <= wave.view.viewMaxTime ? 0.95F : 0.35F;
                            if (cursorVertex < 0 && vertexColor ==
                                ImGui::ColorConvertFloat4ToU32(ui::withAlpha(ui::measurementCursorColor(c), alpha)))
                                cursorVertex = v;
                        }
                        for (std::size_t i = 0; i < colors.size(); ++i) {
                            const auto color = ImGui::ColorConvertFloat4ToU32(
                                ImVec4(colors[i][0], colors[i][1], colors[i][2], 0.65F));
                            if (drawList->VtxBuffer[v].col == color) {
                                if (first[i] < 0) first[i] = v;
                                last[i] = v;
                            }
                        }
                    }
                    for (std::size_t i = 0; i < colors.size(); ++i) {
                        const bool visible = std::find(indices.begin(), indices.end(), i) != indices.end() &&
                                             (showBits || !wave.buffer.channelSpec(i)->bitDisplay.enabled);
                        if ((first[i] >= 0) != visible) throw std::runtime_error("overview Bit/legend filtering failed");
                    }
                    if (showBits && indices.size() == 4 &&
                        (last[1] >= first[0] || last[3] >= first[0] || last[1] >= first[2] || last[3] >= first[2]))
                        throw std::runtime_error("Bit overview rendered above analog");
                    if (selectionVertex <= *std::max_element(last.begin(), last.end()) ||
                        cursorVertex <= selectionVertex)
                        throw std::runtime_error("overview selection/cursors order: " + name +
                            " wave=" + std::to_string(*std::max_element(last.begin(), last.end())) +
                            " selection=" + std::to_string(selectionVertex) + " cursor=" + std::to_string(cursorVertex));
                    if (verifyNavigation) {
                        if (pass == 0) {
                            const auto center = plot->PlotRect.GetCenter();
                            io.AddMousePosEvent(center.x, center.y);
                        } else if (pass == 1) {
                            io.AddMouseWheelEvent(0, 1);
                        } else if (wave.view.viewMaxTime - wave.view.viewMinTime >= previousSpan) {
                            throw std::runtime_error("empty Bit overview cannot zoom time window: wheel=" +
                                std::to_string(io.MouseWheel) + " hovered=" + std::to_string(plot->Hovered) +
                                " span=" + std::to_string(wave.view.viewMaxTime - wave.view.viewMinTime));
                        }
                    }
                    ImGui::End();
                    ImGui::Render();
                    if (withGl && pass == passes - 1) {
                        glClear(GL_COLOR_BUFFER_BIT);
                        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                        glFinish();
                        captureFrame(captureDirectory, name);
                    }
                }
            };
            for (bool dense : {false, true}) {
                wave.buffer.clear();
                for (std::size_t i = 0; i < colors.size(); ++i) {
                    wave.buffer.setChannelSpec(i, {.color = colors[i], .bitDisplay = {.enabled = i % 2 == 1}});
                    std::vector<plot::WaveSample> samples;
                    const int count = dense && (i == 1 || i == 2) ? 6000 : 20;
                    for (int j = 0; j <= count; ++j)
                        samples.push_back({2.0 * j / count,
                            i % 2 ? (j % 2 ? 100.0 : -100.0) :
                                    std::sin(6.283185307179586 * j / count * (i + 1))});
                    wave.buffer.append(i, {{}, samples});
                }
                const std::string mode = dense ? "-envelope" : "-trace";
                draw(false, false, {0, 1, 2, 3}, "overview-bit-hidden" + mode);
                draw(true, true, {0, 1, 2, 3}, "overview-bit-layers" + mode);
                draw(true, false, {1, 2, 3}, "overview-legend-hidden" + mode);
                draw(false, false, {1, 3}, "overview-bits-only" + mode);
                auto spec = *wave.buffer.channelSpec(1);
                spec.bitDisplay.enabled = false;
                wave.buffer.setChannelSpec(1, spec);
                draw(false, true, {1, 3}, "overview-bit-toggle" + mode);
                spec.bitDisplay.enabled = true;
                wave.buffer.setChannelSpec(1, spec);
            }
            for (std::size_t i : {0U, 2U}) {
                auto spec = *wave.buffer.channelSpec(i);
                spec.bitDisplay.enabled = true;
                wave.buffer.setChannelSpec(i, spec);
            }
            draw(false, false, {0, 1, 2, 3}, "overview-all-bit-navigation");
        }
        {
            plot::WaveDockState wave;
            wave.view.initialized = true;
            wave.view.defaultViewportPending = false;
            wave.view.autoFollowLatest = false;
            wave.view.sampleFrequencyHz = 1000;
            wave.view.viewMaxTime = 10;
            wave.view.fft = {.enabled = true, .pointCount = plot::WaveFftPointCount::N1024};
            plot::WaveAppendRequest input;
            for (int i = 0; i < 12000; ++i)
                input.samples.push_back({i / 1000.0, std::sin(i * 0.1)});
            wave.buffer.append(0, std::move(input));
            io.MouseDown[ImGuiMouseButton_Left] = true;
            for (int i = 0; i < 10; ++i) {
                wave.view.viewMinTime = i * 0.1;
                ui::prepareWaveFrame(wave, 1200);
                if (wave.fftRequestActive) throw std::runtime_error("FFT submitted during drag");
            }
            io.MouseDown[ImGuiMouseButton_Left] = false;
            ui::prepareWaveFrame(wave, 1200);
            if (!wave.fftRequestActive) throw std::runtime_error("FFT release did not submit");
            const auto generation = wave.fftRequestGeneration;
            wave.view.fftSourceMinTime = 2;
            wave.view.fftSourceMaxTime = 5;
            wave.view.fft.window = plot::WaveFftWindow::Hamming;
            ui::prepareWaveFrame(wave, 1200);
            if (wave.fftRequestGeneration <= generation) throw std::runtime_error("FFT query generation unchanged");
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while ((!wave.cachedFftFrame.valid || wave.fftRequestActive) && Clock::now() < deadline) {
                ui::prepareWaveFrame(wave, 1200);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!wave.cachedFftFrame.valid || wave.cachedFftKey.viewMinTime != 2 ||
                wave.cachedFftKey.config.window != plot::WaveFftWindow::Hamming)
                throw std::runtime_error("stale FFT replaced final query");
            const auto completedRevision = wave.cachedFftKey.dataRevision;
            for (int i = 0; i < 100; ++i) {
                wave.buffer.append(0, {.samples = {{12.0 + i / 1000.0, 1.0}}});
                ui::prepareWaveFrame(wave, 1200);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (wave.cachedFftKey.dataRevision <= completedRevision)
                throw std::runtime_error("continuous capture starved FFT");
            wave.view.fft.enabled = false;
            wave.view.cursors[0].time = 1;
            wave.view.cursors[1].time = 2;
            wave.view.cursors[0].value = std::sin(100.0);
            wave.view.cursors[1].value = std::sin(200.0);
            wave.view.cursors[0].pinned = wave.view.cursors[1].pinned = true;
            // 只保留统计项，直接检查悬浮层确实产生绘制顶点。
            wave.view.measurement.cursorA = wave.view.measurement.cursorB = false;
            wave.view.measurement.deltaTime = wave.view.measurement.deltaValue = false;
            wave.view.measurement.frequency = wave.view.measurement.period = false;
            wave.view.showMeasurementOverlay = true;
            bool hasSelectedMetrics = true;
            std::string measurementCapture;
            const auto drawMeasurementFrame = [&] {
                if (withGl) ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(1200, 850));
                ImGui::Begin("measurement verification");
                auto frame = ui::prepareWaveFrame(wave, 1200);
                const auto result = ui::drawOscilloscopePlot(wave, frame,
                    {.drawMeasurementOverlay = false, .drawLegendOverlay = false}, nullptr);
                auto* drawList = ImGui::GetForegroundDrawList();
                const auto vertexCount = drawList->VtxBuffer.Size;
                ui::drawMeasurementOverlay(wave.view, frame.snapshot, *frame.displayData, result,
                    ImVec2(100, 100), ImVec2(1000, 700), drawList);
                const bool visible = wave.view.showCursors && wave.view.showMeasurementOverlay && hasSelectedMetrics;
                if ((drawList->VtxBuffer.Size > vertexCount) != visible)
                    throw std::runtime_error("statistics-only overlay visibility mismatch");
                ImGui::End();
                ImGui::Render();
                if (withGl && !measurementCapture.empty()) {
                    glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    glFinish();
                    captureFrame(captureDirectory, measurementCapture);
                }
            };
            io.MouseDown[ImGuiMouseButton_Left] = true;
            drawMeasurementFrame();
            if (wave.measurementRequestActive || wave.cachedMeasurement)
                throw std::runtime_error("statistics submitted during drag");
            io.MouseDown[ImGuiMouseButton_Left] = false;
            const auto measurementDeadline = Clock::now() + std::chrono::seconds(5);
            do {
                drawMeasurementFrame();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (!wave.cachedMeasurement && Clock::now() < measurementDeadline);
            const auto reference = wave.buffer.measureWindow(0, 1, 2);
            if (!wave.cachedMeasurement || wave.cachedMeasurement->sampleCount != reference.sampleCount ||
                std::abs(wave.cachedMeasurement->rmsValue - reference.rmsValue) > 1e-12) {
                std::cerr << "measurement diagnostic cached=" << wave.cachedMeasurement.has_value()
                    << " active=" << wave.measurementRequestActive << " interacting=" << wave.view.interactionActive
                    << " cursorA=" << wave.view.cursors[0].time << " cursorB=" << wave.view.cursors[1].time
                    << " readouts=" << wave.view.lastCursorReadouts[0].has_value() << ','
                    << wave.view.lastCursorReadouts[1].has_value() << " key=" << wave.measurementKeyValid << '\n';
                throw std::runtime_error("statistics release result mismatch");
            }
            for (const auto mode : {plot::WaveViewMode::Overlay, plot::WaveViewMode::Split}) {
                wave.view.viewMode = mode;
                const auto submitted = wave.measurementSubmittedCount;
                const auto generation = wave.measurementRequestGeneration;
                io.MouseDown[ImGuiMouseButton_Left] = true;
                measurementCapture = mode == plot::WaveViewMode::Overlay ? "measurement-pending" : "measurement-split-pending";
                for (int i = 0; i < 8; ++i) {
                    wave.view.cursors[1].time = 3.0 + i * 0.1;
                    wave.view.cursors[1].value = std::sin(wave.view.cursors[1].time * 100.0);
                    wave.view.lastCursorReadouts[1].reset();
                    wave.view.measurementCursorReadoutRefreshPending = true;
                    drawMeasurementFrame();
                    if (wave.cachedMeasurement || wave.measurementSubmittedCount != submitted) {
                        std::cerr << "measurement drag mode=" << static_cast<int>(mode) << " frame=" << i
                            << " cached=" << wave.cachedMeasurement.has_value() << " submitted="
                            << wave.measurementSubmittedCount << " baseline=" << submitted
                            << " cursor=" << wave.view.cursors[1].time << " key=" << wave.measurementKey.end << '\n';
                        throw std::runtime_error("drag retained stale statistics or submitted work");
                    }
                }
                if (wave.measurementRequestGeneration <= generation)
                    throw std::runtime_error("measurement query generation unchanged");
                // 在新代次等待期间注入旧任务，不能将旧区间结果填回当前窗口。
                wave.analysisWorker->submit(plot::WaveMeasurementInput{generation, 0, {1, 2}, {999, 999}, {}});
                for (int i = 0; i < 8; ++i) {
                    drawMeasurementFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    if (wave.cachedMeasurement)
                        throw std::runtime_error("stale measurement accepted during drag");
                }
                io.MouseDown[ImGuiMouseButton_Left] = false;
                measurementCapture.clear();
                const auto deadline = Clock::now() + std::chrono::seconds(5);
                do {
                    drawMeasurementFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (!wave.cachedMeasurement && Clock::now() < deadline);
                const auto expected = wave.buffer.measureWindow(0, wave.measurementKey.begin, wave.measurementKey.end);
                if (!wave.cachedMeasurement || !wave.cachedMeasurement->valid ||
                    std::abs(wave.cachedMeasurement->meanValue - expected.meanValue) > 1e-12 ||
                    wave.measurementSubmittedCount != submitted + 1)
                    throw std::runtime_error("final measurement did not recover after drag");
                measurementCapture = mode == plot::WaveViewMode::Overlay ? "measurement-ready" : "measurement-split-ready";
                drawMeasurementFrame();
                wave.view.showMeasurementOverlay = false;
                measurementCapture = "measurement-closed";
                drawMeasurementFrame();
                wave.view.showMeasurementOverlay = true;
                wave.view.showCursors = false;
                drawMeasurementFrame();
                wave.view.showCursors = true;
                measurementCapture.clear();
            }
            // bit 模式仍只显示游标与时间项目，取消全部适用项应隐藏悬浮窗。
            wave.view.viewMode = plot::WaveViewMode::Overlay;
            auto bitSpec = *wave.buffer.channelSpec(0);
            bitSpec.bitDisplay.enabled = true;
            bitSpec.bitDisplay.bitCount = 2;
            wave.buffer.setChannelSpec(0, bitSpec);
            wave.view.measurement.cursorA = wave.view.measurement.cursorB = true;
            wave.view.measurement.deltaTime = wave.view.measurement.frequency = true;
            measurementCapture = "measurement-bit";
            drawMeasurementFrame();
            drawMeasurementFrame();
            wave.view.measurement.cursorA = wave.view.measurement.cursorB = false;
            wave.view.measurement.deltaTime = wave.view.measurement.frequency = false;
            wave.view.measurement.sampleCount = wave.view.measurement.span = false;
            wave.view.measurement.min = wave.view.measurement.max = false;
            wave.view.measurement.peakToPeak = wave.view.measurement.mean = false;
            wave.view.measurement.rms = wave.view.measurement.stddev = false;
            hasSelectedMetrics = false;
            measurementCapture = "measurement-none";
            drawMeasurementFrame();
            const auto oldGeneration = wave.fftRequestGeneration;
            wave.buffer.clear();
            ui::prepareWaveFrame(wave, 1200);
            if (wave.fftRequestGeneration <= oldGeneration || wave.cachedFftFrame.valid)
                throw std::runtime_error("history reset accepted old result");
        }
        config::ConfigStore configStore;
        for (const auto text : {"compressed_steps", "activity_band", "invalid"}) {
            const auto loaded = configStore.loadText(std::string("gui:\n  wave:\n    bit_dense_render_mode: ") + text);
            const auto expected = std::string_view(text) == "activity_band"
                                      ? plot::WaveBitDenseRenderMode::ActivityBand
                                      : plot::WaveBitDenseRenderMode::CompressedSteps;
            if (loaded.config.gui.wave.bitDenseRenderMode != expected)
                throw std::runtime_error("bit mode parse");
            std::string yaml, error;
            if (!configStore.saveText(loaded.config, yaml, error) ||
                configStore.loadText(yaml).config.gui.wave.bitDenseRenderMode != expected)
                throw std::runtime_error("bit mode roundtrip");
            dock::DockStore store;
            configStore.applyToDock(loaded.config, store);
            if (configStore.captureFromDock(store).gui.wave.bitDenseRenderMode != expected)
                throw std::runtime_error("bit mode runtime roundtrip");
        }
        {
            plot::WaveDockState wave;
            auto& view = wave.view;
            view.initialized = true;
            view.defaultViewportPending = false;
            view.autoFollowLatest = false;
            view.glowEnabled = false;
            view.showCursors = false;
            view.showChannelLegend = false;
            view.interactionAnimationEnabled = false;
            view.maxRenderVertices = 30000;
            std::vector<plot::WaveSample> samples;
            for (int i = 0; i < 10000; ++i) samples.push_back({double(i), double(i >= 1234)});
            wave.buffer.append(0, {{}, samples});
            // 主图、堆叠、分屏必须使用查询层的同一轨迹，不能再次丢掉跳变邻点。
            for (const auto mode : {plot::WaveViewMode::Overlay, plot::WaveViewMode::Stacked,
                                    plot::WaveViewMode::Split}) {
                view.viewMode = mode;
                for (const bool peakDetect : {false, true}) {
                    view.peakDetectDownsample = peakDetect;
                    for (int pan = 0; pan < 9; ++pan) {
                        view.autoFollowLatest = pan >= 6;
                        if (view.autoFollowLatest) {
                            const auto latest = wave.buffer.latestTime().value();
                            wave.buffer.append(0, {{}, {{latest + 20, 1}}});
                        }
                        view.viewMinTime = pan * 20;
                        view.viewMaxTime = view.viewMinTime + 10000;
                        view.visibleDuration = 10000;
                        view.forceNextMainPlotLimits = true;
                        io.MouseDown[0] = false;
                        if (withGl) ImGui_ImplOpenGL3_NewFrame();
                        ImGui::NewFrame();
                        ImGui::SetNextWindowSize(ImVec2(1200, 800));
                        ImGui::Begin("analog stability");
                        auto frame = ui::prepareWaveFrame(wave, 1100);
                        if (view.autoFollowLatest &&
                            view.viewMaxTime != wave.buffer.latestTime().value())
                            throw std::runtime_error("stable trace froze live viewport following");
                        const auto& indices = frame.displayData->channels[0].sourceIndices;
                        const auto edge = std::ranges::find(indices, 1233);
                        if (edge == indices.end() || edge + 1 == indices.end() || *(edge + 1) != 1234)
                            throw std::runtime_error("display cache lost fixed analog edge pair");
                        const auto rendered = ui::drawOscilloscopePlot(wave, frame,
                            {.drawMeasurementOverlay = false, .drawLegendOverlay = false}, nullptr);
                        ImGui::End();
                        ImGui::Render();
                        if (!rendered.plotRendered || !wave.renderEnvelopeCache.empty())
                            throw std::runtime_error("analog query trace was compressed again by renderer");
                        if (view.lastRenderPointCount > frame.renderBudget.pointsPerChannel ||
                            ImGui::GetDrawData()->TotalVtxCount > int(view.maxRenderVertices))
                            throw std::runtime_error("analog stable rendering exceeds budget");
                        if (withGl && pan == 5) {
                            glViewport(0, 0, 1280, 900);
                            glClear(GL_COLOR_BUFFER_BIT);
                            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                            glFinish();
                            captureFrame(captureDirectory, "analog-stable-" + std::to_string(int(mode)) +
                                "-" + std::to_string(peakDetect));
                        }
                    }
                }
            }
        }
        for (const std::size_t count : {100000U, 1000000U}) {
            plot::WaveDockState wave;
            wave.view.initialized = true;
            wave.view.defaultViewportPending = false;
            wave.view.autoFollowLatest = false;
            wave.view.viewMaxTime = static_cast<double>(count);
            wave.view.visibleDuration = static_cast<double>(count);
            wave.view.showCursors = true;
            wave.view.cursors[0].time = static_cast<double>(count) * 0.4;
            wave.view.cursors[1].time = static_cast<double>(count) * 0.6;
            for (auto& cursor : wave.view.cursors) {
                cursor.pinned = true;
                cursor.value = std::sin(cursor.time * 0.03);
            }
            wave.view.interactionAnimationEnabled = false;
            wave.view.overviewNormalizeChannels = true;
            const auto constructionStart = Clock::now();
            for (std::size_t c = 0; c < 5; ++c) {
                plot::WaveAppendRequest input;
                input.samples.reserve(count + 30000);
                for (std::size_t i = 0; i < count + 30000; ++i)
                    input.samples.push_back({static_cast<double>(i),
                                             c == 4
                                                 ? static_cast<double>((i * 2654435761ULL) & 0xffffffffULL)
                                                 : std::sin(static_cast<double>(i) * 0.03 + static_cast<double>(c))});
                wave.buffer.append(c, std::move(input));
            }
            const auto indexStart = Clock::now();
            wave.buffer.setChannelSpec(4, {.label = "digital", .bitDisplay = {.enabled = true, .bitCount = 32}});
            const auto indexEnd = Clock::now();
            const auto appendStart = Clock::now();
            wave.buffer.append(4, {{}, {{double(count + 30000), 17}}});
            const auto appendEnd = Clock::now();
            const auto memorySnapshot = wave.buffer.snapshot(-1e20, 1e20, false);
            std::size_t bytes = 0;
            for (const auto& channel : memorySnapshot.channels) bytes += channel.summaryIndex->memoryBytes();
            std::cout << "index samples=" << count << " initial_all_ms="
                << std::chrono::duration<double, std::milli>(indexEnd - constructionStart).count()
                << " enable_digital_ms=" << std::chrono::duration<double, std::milli>(indexEnd - indexStart).count()
                << " append_one_ms=" << std::chrono::duration<double, std::milli>(appendEnd - appendStart).count()
                << " summary_bytes=" << bytes << '\n';
            for (int mode = 0; mode < 8; ++mode) {
                wave.view.maxRenderVertices = mode == 7 ? 30000 : 60000;
                wave.view.glowEnabled = mode != 0 && mode != 7;
                wave.view.viewMode = mode == 2   ? plot::WaveViewMode::Stacked
                                     : mode == 3 ? plot::WaveViewMode::Split
                                                 : plot::WaveViewMode::Overlay;
                wave.view.phosphorEnabled = mode == 4 || mode == 5;
                wave.view.phosphorBackend =
                    mode == 4 ? plot::WavePhosphorBackend::GpuFbo : plot::WavePhosphorBackend::CpuTexture;
                wave.view.phosphorMode = mode == 5 ? plot::WavePhosphorMode::Triggered : plot::WavePhosphorMode::FreeRun;
                wave.view.fft.enabled = mode == 6;
                if (mode == 6) {
                    io.MouseDown[ImGuiMouseButton_Left] = false;
                    wave.view.sampleFrequencyHz = 1.0;
                    wave.view.viewMinTime = 0;
                    wave.view.viewMaxTime = static_cast<double>(count);
                    const auto fftStart = Clock::now();
                    const auto deadline = fftStart + std::chrono::seconds(10);
                    do {
                        ui::prepareWaveFrame(wave, 1200);
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    } while ((!wave.cachedFftFrame.valid || wave.fftRequestActive) && Clock::now() < deadline);
                    if (!wave.cachedFftFrame.valid)
                        throw std::runtime_error("FFT warmup failed");
                    std::cout << "fft_warmup samples=" << count << " elapsed_ms="
                        << std::chrono::duration<double, std::milli>(Clock::now() - fftStart).count() << '\n';
                }
                wave.view.bitDenseRenderMode = mode % 2 ? plot::WaveBitDenseRenderMode::ActivityBand
                                                        : plot::WaveBitDenseRenderMode::CompressedSteps;
                std::vector<double> times;
                int vertices = 0;
                std::string backend;
                for (int frameNumber = -7; frameNumber < (verify ? 6 : 300); ++frameNumber) {
                    const bool warmBackend = withGl && (mode == 4 || mode == 5) && frameNumber < -5;
                    wave.view.autoFollowLatest = warmBackend;
                    const bool stationary = frameNumber == -7;
                    io.MouseDown[ImGuiMouseButton_Left] = !warmBackend && !stationary;
                    if (withGl) ImGui_ImplOpenGL3_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0));
                    ImGui::SetNextWindowSize(ImVec2(1260, 880));
                    ImGui::Begin("wave pan benchmark", nullptr, ImGuiWindowFlags_NoSavedSettings);
                    wave.view.viewMinTime = static_cast<double>((std::max)(frameNumber, 0) * 100);
                    wave.view.viewMaxTime = wave.view.viewMinTime + static_cast<double>(count);
                    wave.view.forceNextMainPlotLimits = true;
                    const auto start = Clock::now();
                    const auto fftSubmissions = wave.fftSubmittedCount;
                    const auto measurementSubmissions = wave.measurementSubmittedCount;
                    const auto countQueries = wave.bitCountQueryCount;
                    auto frame = ui::prepareWaveFrame(wave, 1200);
                    if (mode != 6) {
                        ImGui::BeginChild("overview", ImVec2(0, 110));
                        ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot,
                            *frame.overviewDisplayData, plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6),
                            {0, 1, 2, 3, 4}, frame.renderBudget);
                        ImGui::EndChild();
                    }
                    const auto rendered =
                        mode == 6
                            ? ui::drawWaveFftPlot(wave, frame, true, false)
                            : ui::drawOscilloscopePlot(
                                  wave, frame, {.drawMeasurementOverlay = false, .drawLegendOverlay = false}, nullptr);
                    ImGui::End();
                    ImGui::Render();
                    if (withGl) {
                        glViewport(0, 0, 1280, 900);
                        glClearColor(0.05F, 0.05F, 0.05F, 1);
                        glClear(GL_COLOR_BUFFER_BIT);
                        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                        glFinish();
                        if (glGetError() != GL_NO_ERROR) throw std::runtime_error("OpenGL frame error");
                        glfwSwapBuffers(window);
                    }
                    const auto ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
                    if (!rendered.plotRendered || ImGui::GetDrawData()->TotalVtxCount == 0)
                        throw std::runtime_error("empty ImPlot frame");
                    if (ImGui::GetDrawData()->TotalVtxCount > static_cast<int>(wave.view.maxRenderVertices))
                        throw std::runtime_error("actual vertex budget exceeded");
                    if (!warmBackend && !stationary && (wave.fftSubmittedCount != fftSubmissions ||
                        wave.measurementSubmittedCount != measurementSubmissions || wave.bitCountQueryCount != countQueries))
                        throw std::runtime_error("analysis submitted during drag");
                    for (const auto& c : frame.displayData->channels)
                        if (c.samples.size() > frame.renderBudget.pointsPerChannel)
                            throw std::runtime_error("display point budget exceeded");
                    if (frameNumber >= 0)
                        times.push_back(ms);
                    if (warmBackend) backend = wave.view.lastRenderStats.phosphorBackendStatus;
                    vertices = (std::max)(vertices, ImGui::GetDrawData()->TotalVtxCount);
                }
                io.MouseDown[ImGuiMouseButton_Left] = false;
                if (withGl) ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImGui::Begin("wave pan benchmark");
                const auto releaseStart = Clock::now();
                const auto releaseQueries = wave.bitCountQueryCount;
                auto released = ui::prepareWaveFrame(wave, 1200);
                if (mode != 6) {
                    ImGui::BeginChild("overview", ImVec2(0, 110));
                    ui::drawOverviewWindow(wave, released.fullSnapshot->config, *released.fullSnapshot,
                        *released.overviewDisplayData, plot::computeDisplayBounds(*released.overviewDisplayData, 1e-6),
                        {0, 1, 2, 3, 4}, released.renderBudget);
                    ImGui::EndChild();
                    ui::drawOscilloscopePlot(wave, released,
                        {.drawMeasurementOverlay = false, .drawLegendOverlay = false}, nullptr);
                } else ui::drawWaveFftPlot(wave, released, true, false);
                ImGui::End();
                ImGui::Render();
                if (withGl) {
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    glFinish();
                    if (mode == 0) captureFrame(captureDirectory, "overview-" + std::to_string(count));
                    glfwSwapBuffers(window);
                }
                if (ImGui::GetDrawData()->TotalVtxCount > static_cast<int>(wave.view.maxRenderVertices))
                    throw std::runtime_error("release frame vertex budget exceeded");
                // 分屏默认四行，第五个数字通道在滚动区外；其可见路径由双通道多帧测试覆盖。
                if (mode != 6 && mode != 3 && wave.bitCountQueryCount != releaseQueries + 32)
                    throw std::runtime_error("benchmark release did not count final viewport");
                std::cout << "release samples=" << count << " mode=" << mode << " count_ms=" << wave.lastBitCountQueryMs
                    << " frame_ms=" << std::chrono::duration<double, std::milli>(Clock::now() - releaseStart).count() << '\n';
                std::ranges::sort(times);
                std::cout << "ui_pan samples=" << count << " mode=" << mode
                          << " p95_ms=" << times[times.size() * 95 / 100] << " vertices=" << vertices
                          << " phosphor_status=" << wave.view.lastRenderStats.phosphorBackendStatus << '\n';
                if (withGl && (mode == 4 || mode == 5)) {
                    std::cout << "warm_backend mode=" << mode << " status=" << backend << '\n';
                    if (backend.find(mode == 4 ? "GPU FBO" : "CPU Texture") == std::string::npos)
                        throw std::runtime_error("requested phosphor backend did not render");
                    io.MouseDown[ImGuiMouseButton_Left] = false;
                    ImGui_ImplOpenGL3_NewFrame();
                    ImGui::NewFrame();
                    ImGui::Begin("wave pan benchmark");
                    auto frame = ui::prepareWaveFrame(wave, 1200);
                    ui::drawOscilloscopePlot(wave, frame,
                        {.drawMeasurementOverlay = false, .drawLegendOverlay = false}, nullptr);
                    ImGui::End();
                    ImGui::Render();
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                    glFinish();
                    if (wave.view.autoFollowLatest || wave.view.lastRenderStats.phosphorBackendStatus.find("冻结") == std::string::npos)
                        throw std::runtime_error("phosphor release did not preserve freeze");
                    std::vector<unsigned char> image(1280 * 900 * 3);
                    glReadPixels(0, 0, 1280, 900, GL_RGB, GL_UNSIGNED_BYTE, image.data());
                    const auto extrema = std::minmax_element(image.begin(), image.end());
                    if (*extrema.first == *extrema.second || glGetError() != GL_NO_ERROR)
                        throw std::runtime_error("blank phosphor framebuffer");
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        exitCode = 1;
    }
    ImPlot::DestroyContext();
    if (withGl) ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    if (window) { glfwDestroyWindow(window); glfwTerminate(); }
    return exitCode;
}
