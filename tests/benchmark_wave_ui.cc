#include "../src/ui/wave/wave_render_service.hpp"

#include "protoscope/config/config.hpp"
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <thread>

using namespace protoscope;
using Clock = std::chrono::steady_clock;

int main(int argc, char** argv)
{
    const bool verify = argc > 1 && std::string_view(argv[1]) == "--verify";
    bool withGl = false;
    for (int i = 1; i < argc; ++i) withGl = withGl || std::string_view(argv[i]) == "--gl";
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
    if (withGl && !ImGui_ImplOpenGL3_Init("#version 330")) return 2;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1280, 900);
    io.DeltaTime = 1.0F / 60.0F;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
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
            const auto draw = [&] {
                if (withGl) ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(1200, 850));
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
            const auto drawMeasurementFrame = [&] {
                if (withGl) ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowSize(ImVec2(1200, 850));
                ImGui::Begin("measurement verification");
                auto frame = ui::prepareWaveFrame(wave, 1200);
                ui::drawOscilloscopePlot(wave, frame,
                    {.drawMeasurementOverlay = false, .drawLegendOverlay = false}, nullptr);
                ImGui::End();
                ImGui::Render();
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
            wave.buffer.setChannelSpec(4, {.label = "digital", .bitDisplay = {.enabled = true, .bitCount = 32}});
            for (int mode = 0; mode < 7; ++mode) {
                wave.view.glowEnabled = mode != 0;
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
                    io.MouseDown[ImGuiMouseButton_Left] = !warmBackend;
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
                    auto frame = ui::prepareWaveFrame(wave, 1200);
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
                    if (!warmBackend && (wave.fftSubmittedCount != fftSubmissions ||
                        wave.measurementSubmittedCount != measurementSubmissions))
                        throw std::runtime_error("analysis submitted during drag");
                    for (const auto& c : frame.displayData->channels)
                        if (c.samples.size() > frame.renderBudget.pointsPerChannel)
                            throw std::runtime_error("display point budget exceeded");
                    if (frameNumber >= 0)
                        times.push_back(ms);
                    if (warmBackend) backend = wave.view.lastRenderStats.phosphorBackendStatus;
                    vertices = (std::max)(vertices, ImGui::GetDrawData()->TotalVtxCount);
                }
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
