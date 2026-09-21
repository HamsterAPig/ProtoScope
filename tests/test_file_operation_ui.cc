#include "protoscope/app/application.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include "../src/ui/wave/wave_render_service.hpp"
#include <imgui_internal.h>
#include <implot_internal.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
std::filesystem::path captureDirectory;
bool withGl = false;
void beginFrame()
{
    if (withGl) ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
}
void endFrame(const std::string& name = {})
{
    ImGui::Render();
    if (!withGl) return;
    const int width = static_cast<int>(ImGui::GetIO().DisplaySize.x);
    const int height = static_cast<int>(ImGui::GetIO().DisplaySize.y);
    glViewport(0, 0, width, height);
    glClearColor(0.05F, 0.05F, 0.05F, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glFinish();
    if (name.empty()) return;
    // BMP 行对齐到四字节，支持窄窗口验证截图。
    const auto stride = (width * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(stride * height));
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const auto extrema = std::minmax_element(pixels.begin(), pixels.end());
    if (*extrema.first == *extrema.second || glGetError() != GL_NO_ERROR)
        throw std::runtime_error("blank UI framebuffer");
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) std::swap(pixels[y * stride + x * 3], pixels[y * stride + x * 3 + 2]);
    std::array<unsigned char, 54> header{};
    header[0] = 'B'; header[1] = 'M';
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) header[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, static_cast<std::uint32_t>(54 + pixels.size()));
    put(10, 54); put(14, 40); put(18, width); put(22, height);
    header[26] = 1; header[28] = 24;
    std::filesystem::create_directories(captureDirectory);
    std::ofstream output(captureDirectory / (name + ".bmp"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!output) throw std::runtime_error("UI screenshot write failure");
}
}

namespace protoscope::ui {
struct GuiRuntimeTestAccess {
    static void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    static void verify(app::Application& application, GuiRuntime& runtime)
    {
        for (const auto mode : {config::GuiWaveFullscreenMode::Overlay, config::GuiWaveFullscreenMode::Focus}) {
            auto config = application.captureConfig();
            config.gui.wave.fullscreenMode = mode;
            application.applyConfig(config);
            runtime.showCommDock_ = true;
            runtime.showLogDock_ = false;
            runtime.luaDockVisibility_["test"] = true;
            runtime.enterWaveFullscreen();
            require(runtime.waveFullscreenActive_, "enter fullscreen");
            runtime.openUnifiedDataExport(-1, true);
            require(runtime.waveFullscreenActive_ && !runtime.pendingBuiltinFileOperation_, "rejected operation stays fullscreen");
            auto exportSettings = config.gui.lastDataExport;
            exportSettings.valid = true;
            exportSettings.waveRange = 2;
            application.rememberDataExport(exportSettings);
            application.docks().waveState().view.showCursors = false;
            runtime.openUnifiedDataExport(-1, true);
            require(runtime.waveFullscreenActive_ && !runtime.pendingBuiltinFileOperation_,
                    "invalid cursor export stays fullscreen");
            application.rememberDataExport({});
            application.docks().waveState().view.showCursors = true;
            bool called = false;
            runtime.deferBuiltinFileOperation([&] {
                require(!runtime.waveFullscreenActive_, "operation requires normal layout");
                require(runtime.showCommDock_ && !runtime.showLogDock_ && runtime.luaDockVisibility_["test"],
                        "restore visibility");
                called = true;
            });
            runtime.dispatchBuiltinFileOperation();
            require(!called && runtime.waveFullscreenActive_, "not dispatched in request frame");
            runtime.prepareBuiltinFileOperation();
            require(!called && !runtime.waveFullscreenActive_, "restore before normal frame");
            beginFrame();
            ImGui::Begin("normal layout");
            ImGui::TextUnformatted("normal");
            ImGui::End();
            endFrame();
            runtime.dispatchBuiltinFileOperation();
            require(called && !runtime.waveFullscreenActive_, "dispatch after normal frame");
            runtime.dispatchBuiltinFileOperation();
            require(!runtime.pendingBuiltinFileOperation_, "operation consumed");

            runtime.enterWaveFullscreen();
            runtime.openUnifiedDataExport();
            require(runtime.pendingBuiltinFileOperation_ && runtime.waveFullscreenActive_, "unified entry is deferred");
            runtime.prepareBuiltinFileOperation();
            beginFrame();
            endFrame();
            runtime.dispatchBuiltinFileOperation();
            require(runtime.unifiedDataDialogOpen_ && runtime.focusUnifiedDataDialog_, "open and focus once");
            beginFrame();
            runtime.drawUnifiedDataDialog();
            const auto* dataWindow = ImGui::FindWindowByName("数据导入导出");
            require(dataWindow && GImGui->NavWindow == dataWindow, "focus operation window");
            require(!runtime.focusUnifiedDataDialog_, "focus request consumed");
            endFrame();
            runtime.unifiedDataDialogOpen_ = false;
            require(!runtime.waveFullscreenActive_, "closing stays normal");
        }
    }
    static void verifyStatus(app::Application& application, GuiRuntime& runtime)
    {
        auto& config = application.docks().configState();
        auto& wave = application.docks().waveState();
        application.setStatusMessage("import result");
        config.dirty = false;
        wave.view.fft.enabled = true;
        wave.analysisEpoch = wave.buffer.historyEpoch();
        wave.cachedFftKeyValid = true;
        wave.cachedFftFrame.message = "FFT calculation failed with a long diagnostic message";
        for (float width : {1280.0F, 480.0F, 320.0F}) {
            ImGui::GetIO().DisplaySize = ImVec2(width, 700);
            for (int frame = 0; frame < 4; ++frame) {
                beginFrame();
                wave.view.fftUpdatePending = frame % 2 == 0;
                runtime.drawStatusBar();
                const auto* bar = ImGui::FindWindowByName("状态栏");
                require(bar && bar->Size.y == kStatusBarHeight && bar->ScrollMax.y == 0,
                        "fixed status height without vertical scroll");
                require(config.statusMessage == "import result" && !config.dirty,
                        "wave status must not overwrite general message or dirty configuration");
                require(runtime.waveStatusPresenter_.display().fftError, "FFT error shown in status bar");
                endFrame(frame == 3 ? "status-" + std::to_string(static_cast<int>(width)) : "");
            }
        }
        wave.view.fft.enabled = false;
        beginFrame();
        runtime.drawStatusBar();
        require(runtime.waveStatusPresenter_.display().fft.empty(), "disable clears status in same frame");
        endFrame();
        ImGui::GetIO().DisplaySize = ImVec2(1000, 700);
        for (int frame = 0; frame < 2; ++frame) {
            beginFrame();
            runtime.waveDockRenderer_.drawOverlay(true, nullptr);
            const auto* overlay = ImGui::FindWindowByName("波形全屏##wave_fullscreen_overlay");
            require(overlay && overlay->Pos.y + overlay->Size.y <= 700 - kStatusBarHeight,
                    "overlay reserves status bar");
            endFrame();
        }
    }
    static void verifyRememberedPaths(app::Application& application, GuiRuntime& runtime,
                                     const config::ConfigStore& store)
    {
        const auto root = std::filesystem::temp_directory_path() /
            ("protoscope-ui-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto importDir = root / std::filesystem::u8path("中文 import");
        std::filesystem::create_directories(importDir);
        const auto configPath = root / "config.yaml";
        auto& state = application.docks().configState();
        state.loadedFromPath = fileDialogPathText(configPath);
        state.dirty = false;
        runtime.importCsvDataFromPath(importDir / "missing.csv");
        require(!runtime.csvDataImportError_.empty(), "import fixture must fail");
        require(application.runtimeConfig().gui.fileDialogs.lastImportDirectory == fileDialogPathText(importDir),
                "confirmed directory survives import failure");
        require(store.load(configPath).config.gui.fileDialogs.lastImportDirectory == fileDialogPathText(importDir),
                "confirmed directory survives restart");
        require(!state.dirty, "directory persistence must not mark other settings dirty");
        const auto beforeCancel = application.runtimeConfig().gui.fileDialogs;
        runtime.deferBuiltinFileOperation([] {});
        runtime.prepareBuiltinFileOperation();
        beginFrame();
        endFrame();
        runtime.dispatchBuiltinFileOperation();
        require(application.runtimeConfig().gui.fileDialogs.lastImportDirectory == beforeCancel.lastImportDirectory,
                "canceled operation does not remember browsing");
        // 损坏配置保存失败仍允许继续读取用户确认的文件，并保留本次会话目录。
        { std::ofstream output(configPath); output << "gui: ["; }
        runtime.importCsvDataFromPath(root / "missing-again.csv");
        require(!runtime.fileDialogPreferenceError_.empty() && !runtime.csvDataImportError_.empty(),
                "preference save failure does not block file operation");
        require(application.runtimeConfig().gui.fileDialogs.lastImportDirectory == fileDialogPathText(root),
                "save failure preserves in-memory preference");
        require(!state.dirty, "save error must not mark configuration dirty");
        std::ifstream input(configPath);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        require(text == "gui: [", "broken configuration preserved");
        runtime.fileDialogPreferenceError_.clear();
    }
    static void verifyFullscreenFft(app::Application& application, GuiRuntime& runtime)
    {
        for (const auto mode : {config::GuiWaveFullscreenMode::Overlay, config::GuiWaveFullscreenMode::Focus}) {
            auto config = application.captureConfig();
            config.gui.wave.fullscreenMode = mode;
            application.applyConfig(config);
            auto& wave = application.docks().waveState();
            wave.buffer.clear();
            wave.view.initialized = true;
            wave.view.defaultViewportPending = false;
            wave.view.autoFollowLatest = false;
            wave.view.viewMinTime = 0;
            wave.view.viewMaxTime = 1;
            wave.view.sampleFrequencyHz = 1024;
            wave.view.fft.enabled = true;
            wave.view.overviewWindowDragging = true;
            std::vector<plot::WaveSample> samples;
            for (int i = 0; i < 1024; ++i)
                samples.push_back({double(i) / 1024, std::sin(double(i) * 0.1)});
            wave.buffer.append(0, {{}, samples});
            const auto prepared = prepareWaveFrame(wave, 1200);
            wave.cachedFftFrame = plot::buildWaveFftFrame(*prepared.fullSnapshot, *prepared.displayData,
                                                        wave.view.fft, {1}, 0, 1, 1024);
            require(wave.cachedFftFrame.valid, "fullscreen FFT fixture");
            wave.cachedFftKey = wave.fftRequestedKey;
            wave.cachedFftKeyValid = true;
            runtime.enterWaveFullscreen();
            for (const auto size : {ImVec2(1280, 900), ImVec2(480, 600)}) {
                ImGui::GetIO().DisplaySize = size;
                std::vector<ImRect> baseline;
                for (int n = 0; n < 8; ++n) {
                    beginFrame();
                    const bool pending = n % 2 == 0;
                    wave.view.overviewWindowDragging = true;
                    wave.cachedFftKey.dataRevision = wave.buffer.analysisRevision() + (pending ? 1 : 0);
                    if (mode == config::GuiWaveFullscreenMode::Overlay) {
                        runtime.waveDockRenderer_.drawOverlay(true, nullptr);
                    } else {
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(size.x, size.y - kStatusBarHeight));
                        runtime.waveDockRenderer_.draw(runtime.showWaveDock_, true, nullptr, true);
                    }
                    require(wave.view.fftUpdatePending == pending, "fullscreen pending state exercised");
                    std::vector<ImRect> current;
                    for (int i = 0; i < GImPlot->Plots.GetBufSize(); ++i)
                        current.push_back(GImPlot->Plots.GetByIndex(i)->FrameRect);
                    require(current.size() >= 2, "fullscreen magnitude and phase present");
                    if (n == 3) baseline = current;
                    if (n > 3) {
                        require(current.size() == baseline.size(), "plot count stable");
                        for (std::size_t i = 0; i < current.size(); ++i)
                            require(current[i].Min.x == baseline[i].Min.x && current[i].Min.y == baseline[i].Min.y &&
                                    current[i].Max.x == baseline[i].Max.x && current[i].Max.y == baseline[i].Max.y,
                                    "actual fullscreen FFT geometry stable");
                    }
                    runtime.drawStatusBar();
                    const std::string label = mode == config::GuiWaveFullscreenMode::Overlay ? "overlay" : "focus";
                    endFrame(n == 7 ? label + "-" + std::to_string(static_cast<int>(size.x)) : "");
                }
            }
            runtime.exitWaveFullscreen();
        }
    }
};
}

int main(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "--capture") captureDirectory = argv[i + 1];
    withGl = !captureDirectory.empty();
    GLFWwindow* window = nullptr;
    if (withGl) {
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(1280, 900, "UI verification", nullptr, nullptr);
        if (!window) return 2;
        glfwMakeContextCurrent(window);
    }
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1000, 700);
    io.DeltaTime = 1.0F / 60.0F;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    unsigned char* pixels;
    int width, height;
    if (const auto* windows = std::getenv("SystemRoot")) {
        const auto font = std::filesystem::path(windows) / "Fonts" / "msyh.ttc";
        if (std::filesystem::exists(font))
            io.Fonts->AddFontFromFileTTF(font.string().c_str(), 16.0F, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
    if (withGl) {
        if (!ImGui_ImplOpenGL3_Init("#version 130")) return 2;
    } else {
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    int result = 0;
    try {
        protoscope::app::Application app;
        protoscope::config::ConfigStore store;
        protoscope::ui::GuiRuntime runtime(app, store);
        protoscope::ui::GuiRuntimeTestAccess::verify(app, runtime);
        protoscope::ui::GuiRuntimeTestAccess::verifyRememberedPaths(app, runtime, store);
        protoscope::ui::GuiRuntimeTestAccess::verifyStatus(app, runtime);
        protoscope::ui::GuiRuntimeTestAccess::verifyFullscreenFft(app, runtime);
        std::cout << "file operation frame boundary and focus passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (withGl) ImGui_ImplOpenGL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    if (window) { glfwDestroyWindow(window); glfwTerminate(); }
    return result;
}
