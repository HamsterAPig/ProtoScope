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
#include <limits>
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

void verifyMeasurementInputs()
{
    const auto check = [](bool ok, const char* message) {
        if (!ok) throw std::runtime_error(message);
    };
    plot::WaveDockState wave;
    wave.view.autoFollowLatest = false;
    std::vector<plot::WaveSample> samples(1000000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = {1.0 + i * 0.001, std::sin(i * 0.013)};
    wave.buffer.append(0, {{}, samples});
    plot::WaveDisplayData display;
    const auto refresh = [&] {
        const auto snapshot = wave.buffer.snapshot(-1e20, 1e20, false);
        plot::buildQueryDisplayDataInto(snapshot, wave.view.sampleFrequencyHz, 1200, display);
    };
    double begin = 2, end = 900;
    std::optional<std::size_t> reference;
    std::optional<double> manual;
    const auto tick = [&] { return ui::requestWaveMeasurement(wave, display, 0, begin, end, reference, manual); };
    const auto settle = [&] {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        do {
            tick();
            if (!wave.view.measurementUpdatePending && !wave.measurementRequestActive) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (Clock::now() < deadline);
        check(wave.cachedMeasurement && !wave.view.measurementUpdatePending && !wave.measurementRequestActive,
              "measurement did not converge");
    };
    refresh();
    const auto initialStart = Clock::now();
    const auto extractionStart = Clock::now();
    tick();
    const double extractionMs = std::chrono::duration<double, std::milli>(Clock::now() - extractionStart).count();
    // 首次任务在途时区间外追加，不应漏收或重复提交同一输入。
    wave.buffer.append(0, {.samples = {{1002, 99}}});
    refresh();
    settle();
    const double initialMs = std::chrono::duration<double, std::milli>(Clock::now() - initialStart).count();
    auto submitted = wave.measurementSubmittedCount;
    const auto stableStart = Clock::now();
    for (int i = 0; i < 1000; ++i) tick();
    const double stableUs = std::chrono::duration<double, std::micro>(Clock::now() - stableStart).count() / 1000;
    check(wave.measurementSubmittedCount == submitted, "stable million input resubmitted");
    wave.buffer.setMaxTotalSamples(999500);
    refresh();
    settle();
    check(wave.measurementSubmittedCount == submitted, "outside trim resubmitted");
    begin += 0.0001; end += 0.0001;
    settle();
    check(wave.measurementSubmittedCount == submitted + 1, "boundary sample change not measured");
    submitted = wave.measurementSubmittedCount;
    begin += 0.0001; end += 0.0001;
    settle();
    check(wave.measurementSubmittedCount == submitted, "same sample set resubmitted");
    const auto synchronousStart = Clock::now();
    const auto synchronous = wave.buffer.measureWindow(0, 2, 900);
    const double synchronousMs = std::chrono::duration<double, std::milli>(Clock::now() - synchronousStart).count();
    check(synchronous.valid, "million synchronous benchmark invalid");
    std::cout << "measurement million: extract_ms=" << extractionMs << " first_result_ms=" << initialMs
              << " stable_signature_us=" << stableUs << " synchronous_ms=" << synchronousMs << '\n';
    begin = 4; end = 5; settle();
    submitted = wave.measurementSubmittedCount;
    begin = 5; end = 6; settle();
    check(wave.measurementSubmittedCount == submitted + 1, "same span different interval reused");
    wave.view.viewMinTime = 7; wave.view.viewMaxTime = 8;
    settle();
    submitted = wave.measurementSubmittedCount;
    check(wave.cachedMeasurement->valid, "fixed interval outside view lost");
    wave.view.measurementVisibleWindowOnly = true;
    settle();
    check(!wave.cachedMeasurement->valid && wave.measurementSubmittedCount == submitted + 1,
          "visible-only empty intersection not measured");
    wave.view.measurementVisibleWindowOnly = false;
    // 在途追加区间内的新样本必须补算最终输入。
    begin = 1002; end = 1003;
    tick();
    wave.buffer.append(0, {.samples = {{1003, 101}}}); refresh();
    settle();
    check(wave.cachedMeasurement->sampleCount == 2, "inflight append lost");
    // 同数量同时间重新导入内容必须换代，旧结果不能填回。
    begin = 1; end = 2; tick();
    wave.buffer.clear();
    wave.buffer.append(0, {.samples = {{1, 7}, {2, 9}}}); refresh();
    settle();
    check(wave.cachedMeasurement->meanValue == 8, "clear reused stale data");
    wave.buffer.configureChannels(0);
    wave.buffer.append(0, {.samples = {{1, 20}, {2, 30}}}); refresh(); settle();
    check(wave.cachedMeasurement->meanValue == 25, "recreated channel reused stale data");
    manual = 10; settle();
    check(wave.cachedMeasurement->meanError == 15, "manual reference not measured");
    manual = 20; settle();
    check(wave.cachedMeasurement->meanError == 5, "manual reference change reused");
    manual.reset();
    wave.buffer.append(1, {.samples = {{1, 1}, {2, 2}}}); reference = 1; refresh(); settle();
    check(wave.cachedMeasurement->meanError == 23.5, "channel reference incorrect");
    wave.buffer.configureChannels(1);
    wave.buffer.append(1, {.samples = {{1, 10}, {2, 10}}}); refresh(); settle();
    check(wave.cachedMeasurement->meanError == 15, "reference generation reused");
    auto spec = *wave.buffer.channelSpec(0); spec.ratio = 2;
    wave.buffer.setChannelSpec(0, spec); refresh(); settle();
    check(wave.cachedMeasurement->meanValue == 50, "ratio change reused");
    wave.view.sampleFrequencyHz = 2000; refresh(); settle();
    check(!wave.cachedMeasurement->valid, "time axis change reused script interval");
    wave.view.sampleFrequencyHz = 0; refresh(); settle();
    reference.reset();
    // 连续滚动允许同上下文快照，间隔 0 时依然只保留一个在途任务。
    wave.view.autoFollowLatest = true; wave.view.followMeasurementCursorsOnScroll = true;
    for (const auto interval : {200ULL, 0ULL}) {
        wave.view.measurementFollowRefreshIntervalMs = interval;
        const auto count = wave.measurementSubmittedCount;
        const auto deadline = Clock::now() + std::chrono::milliseconds(450);
        std::size_t frames = 0;
        while (Clock::now() < deadline) {
            begin = 1.0 + (frames % 5) * 0.1; end = begin + 0.5;
            tick();
            ++frames;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(wave.measurementSubmittedCount > count && wave.cachedMeasurement,
              "continuous follow starved measurements");
        if (interval) check(wave.measurementSubmittedCount <= count + 4, "follow interval ignored");
        begin = 1; end = 2;
        settle();
        check(wave.measurementCompletedKey && *wave.measurementCompletedKey == wave.measurementKey,
              "follow stop missed final interval");
    }
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
        if (verify) verifyMeasurementInputs();
        {
            // 与生产入口相同的多帧输入，不直接写拖动状态；覆盖整框而非旧中心 8x8。
            plot::WaveDockState wave;
            auto& view = wave.view;
            view.initialized = true;
            view.defaultViewportPending = false;
            view.autoFollowLatest = false;
            view.showCursors = false;
            view.viewMinValue = -7;
            view.viewMaxValue = 9;
            wave.buffer.append(0, {{}, {{0, 0}, {100, 1}}});
            ImRect bounds;
            bool collapsed = false;
            const auto draw = [&] {
                if (withGl) ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0, 0));
                ImGui::SetNextWindowSize(ImVec2(1100, 300));
                ImGui::Begin("overview capture regression", nullptr, ImGuiWindowFlags_NoSavedSettings);
                auto frame = ui::prepareWaveFrame(wave, 1100);
                if (collapsed) ui::releaseOverviewDrag(wave);
                else {
                    ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot,
                        *frame.overviewDisplayData, plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6),
                        {0}, frame.renderBudget);
                    if (const auto* p = ImPlot::GetPlot("##wave_overview")) bounds = p->PlotRect;
                }
                ImGui::End();
                ImGui::Render();
            };
            const auto check = [](bool condition, const char* message) {
                if (!condition) throw std::runtime_error(message);
            };
            const auto reset = [&] {
                io.AddMouseButtonEvent(0, false);
                io.AddMousePosEvent(-100, -100);
                for (int n = 0; n < 24; ++n) draw(); // 隔离双击计时及事件队列。
                view.viewMinTime = 20;
                view.viewMaxTime = 60;
                view.visibleDuration = 40;
                view.autoFollowLatest = false;
                view.forceNextMainPlotLimits = false;
                draw();
            };
            const auto point = [&](double time, float y) {
                return ImVec2(bounds.Min.x + bounds.GetWidth() * float(time / 100),
                              bounds.Min.y + bounds.GetHeight() * y);
            };
            const auto press = [&](ImVec2 p) {
                io.AddMousePosEvent(p.x, p.y); draw();
                io.AddMouseButtonEvent(0, true); draw();
            };
            const auto move = [&](ImVec2 p) { io.AddMousePosEvent(p.x, p.y); draw(); };
            for (float dpi : {1.F, 1.5F, 2.F}) {
                ImGui::GetMainViewport()->DpiScale = dpi;
                for (float x : {.2F, .8F}) for (float y : {.2F, .8F}) {
                    reset();
                    const auto start = point(20 + 40 * x, y);
                    press(start);
                    check(view.overviewDrag.target == 3 && ImGui::GetActiveID() == view.overviewDrag.activeId,
                          "概览四象限未获得真实平移活动 ID");
                    // 让跟随已定位到同一范围，再检验 OverviewDrag 强制暂停，不依赖通用暂停开关。
                    view.pauseAutoFollowOnInteraction = false;
                    // ImGui UpdateMouseInputs 对坐标向下取整；以真实整像素输入独立计算预期，
                    // 不把107.1px指令误当成已送达107.1px，也不放宽时间断言容差。
                    const float destinationX = std::floor(start.x + bounds.GetWidth() * .1F);
                    const double expectedShift = (destinationX - std::floor(start.x)) * 100.0 / bounds.GetWidth();
                    move(ImVec2(destinationX, start.y));
                    check(std::abs(view.viewMinTime - (20 + expectedShift)) < 1e-4 &&
                          std::abs(view.viewMaxTime - (60 + expectedShift)) < 1e-4,
                          "概览总锚点平移错误或重复应用 delta");
                    check(view.viewMinValue == -7 && view.viewMaxValue == 9 && view.forceNextMainPlotLimits,
                          "概览平移改变 Y 或未刷新主图");
                    draw();
                    check(std::abs(view.viewMinTime - (20 + expectedShift)) < 1e-4, "静止持有累计位移");
                    move(ImVec2(destinationX + .75F, start.y));
                    check(io.MousePos.x == destinationX &&
                          std::abs(view.viewMinTime - (20 + expectedShift)) < 1e-4 &&
                          std::abs(view.viewMaxTime - (60 + expectedShift)) < 1e-4,
                          "同一逻辑像素的分数坐标不得移动概览");
                    move(ImVec2(bounds.Max.x + 100, bounds.Max.y + 50));
                    check(view.overviewWindowDragging && std::abs(view.viewMaxTime - 100) < 1e-4,
                          "图外捕获丢失或历史夹紧失败");
                    io.AddMouseButtonEvent(0, false); draw();
                    check(!view.overviewWindowDragging && !wave.overviewColorCache.dragging && !view.overviewDrag.activeId,
                          "图外释放未清理捕获和锁色");
                }
                for (int edge : {1, 2}) {
                    reset();
                    const auto start = point(edge == 1 ? 20 : 60, .5F);
                    press(start);
                    check(view.overviewDrag.target == edge, "左右把手未互斥命中");
                    const float destinationX=std::floor(start.x + bounds.GetWidth() * .05F);
                    const double expectedShift=(destinationX-std::floor(start.x))*100.0/bounds.GetWidth();
                    move(ImVec2(destinationX, start.y));
                    check(std::abs(view.viewMinTime - (edge == 1 ? 20+expectedShift : 20)) < 1e-4 &&
                          std::abs(view.viewMaxTime - (edge == 2 ? 60+expectedShift : 60)) < 1e-4, "边缘缩放移动错误端点");
                    reset();
                    const auto p = point(edge == 1 ? 20 : 60, .5F);
                    press(p); io.AddMouseButtonEvent(0, false); draw();
                    io.AddMouseButtonEvent(0, true); draw();
                    check(std::abs((edge == 1 ? view.viewMinTime : view.viewMaxTime) - (edge == 1 ? 0 : 100)) < 1e-4,
                          "双击边缘未恢复历史端点");
                }
                reset();
                view.viewMinTime = 40; view.viewMaxTime = 40.001; view.visibleDuration = .001;
                draw();
                const auto narrow = point(40.0005, .2F);
                press(narrow);
                check(view.overviewDrag.target == 3, "极窄框没有平移入口");
                check(std::abs(view.viewMinTime - 40) < 1e-8, "点击视觉扩展区发生跳转");
                move(ImVec2(narrow.x + 20 * dpi, narrow.y));
                check(std::abs(view.viewMaxTime - view.viewMinTime - .001) < 1e-8, "最小像素宽度污染时间跨度");
                io.AddFocusEvent(false); draw();
                check(!view.overviewDrag.activeId && !wave.overviewColorCache.dragging, "失焦未释放概览");
                io.AddFocusEvent(true);
                for (int edge : {1, 2}) {
                    reset();
                    view.viewMinTime = 40; view.viewMaxTime = 40.001; view.visibleDuration = .001;
                    draw();
                    auto handle = point(40.0005, .5F);
                    handle.x += (edge == 1 ? -2 : 2) * dpi;
                    press(handle);
                    check(view.overviewDrag.target == edge, "极窄框左右把手不可抓取");
                    move(ImVec2(handle.x + (edge == 1 ? -20 : 20) * dpi, handle.y));
                    check(edge == 1 ? view.viewMinTime < 40 : view.viewMaxTime > 40.001,
                          "极窄把手未扩大时间范围");
                }
                reset();
                press(point(5, .2F));
                move(point(35, .2F));
                check(!view.overviewDrag.activeId && view.viewMinTime == 20, "框外按下移入被劫持");
                reset();
                press(point(35, .2F));
                collapsed = true; draw();
                check(!view.overviewDrag.activeId && !wave.overviewColorCache.dragging, "折叠未释放");
                collapsed = false;
                reset();
                const auto anchor = point(30, .2F);
                move(anchor);
                const double actualAnchor=(std::floor(anchor.x)-bounds.Min.x)*100.0/bounds.GetWidth();
                const double anchorRatio=(actualAnchor-20)/40;
                io.AddMouseWheelEvent(0, 1); draw();
                check(view.viewMaxTime - view.viewMinTime < 40 &&
                      std::abs((actualAnchor - view.viewMinTime) / (view.viewMaxTime - view.viewMinTime) - anchorRatio) < 1e-5,
                      "概览滚轮锚点回归");
                reset();
                // 最大时间为100，跟随40宽窗口对应60..100。
                view.viewMinTime = 60; view.viewMaxTime = 100; view.autoFollowLatest = true;
                press(point(75, .2F));
                move(point(70, .2F));
                check(!view.autoFollowLatest, "概览拖动未强制停止自动跟随");
            }
            reset();
            press(point(35, .2F));
            wave.buffer.clear(); draw();
            check(!view.overviewDrag.activeId && !view.overviewWindowDragging, "无数据未释放概览");
            io.AddMouseButtonEvent(0, false); draw();
            ImGui::GetMainViewport()->DpiScale = 1;
            std::cout << "overview_capture: quadrants, edges, narrow, DPI, release, focus, wheel passed\n";
        }
        {
            // 真正通过概览捕获进入拖动；最终移动与抬起同帧，不能用直接改 view 代替释放链路。
            const auto check = [](bool condition, const char* message) {
                if (!condition) throw std::runtime_error(message);
            };
            const auto run = [&](int target, bool firstMoveOnRelease, double shift,
                                 bool outside, double minTime, double maxTime,
                                 bool staleTask, bool visibleSamples) {
                plot::WaveDockState wave;
                auto& view = wave.view;
                view.initialized = true;
                view.defaultViewportPending = false;
                view.autoFollowLatest = false;
                view.showCursors = false;
                view.viewMinTime = minTime;
                view.viewMaxTime = maxTime;
                view.visibleDuration = maxTime - minTime;
                view.sampleFrequencyHz = 1024;
                view.fft.enabled = true;
                view.fft.pointCount = visibleSamples ? plot::WaveFftPointCount::VisibleSamples
                                                     : plot::WaveFftPointCount::N1024;
                std::vector<plot::WaveSample> samples;
                for (int i = 0; i <= 4096; ++i) {
                    const double hz = i < 2048 ? 32 : 96;
                    const double value = i == 512 || i == 3072
                        ? std::numeric_limits<double>::quiet_NaN()
                        : std::sin(6.283185307179586 * hz * i / 1024.0);
                    samples.push_back({i / 1024.0, value});
                }
                check(wave.buffer.append(0, {{}, samples}), "频谱释放样本写入失败");
                ImRect bounds;
                bool rejectOldResult = false;
                const auto draw = [&] {
                    if (withGl) ImGui_ImplOpenGL3_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0));
                    ImGui::SetNextWindowSize(ImVec2(1100, 600));
                    ImGui::Begin("FFT overview release", nullptr, ImGuiWindowFlags_NoSavedSettings);
                    auto frame = ui::prepareWaveFrame(wave, 1100);
                    if (rejectOldResult && wave.cachedFftKeyValid)
                        check(wave.cachedFftKey.viewMinTime == view.fftSourceMinTime &&
                              wave.cachedFftKey.viewMaxTime == view.fftSourceMaxTime,
                              "等待最终频谱时发布了旧代次结果");
                    ImGui::BeginChild("overview", ImVec2(0, 150));
                    ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot,
                        *frame.overviewDisplayData, plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6),
                        {0}, frame.renderBudget);
                    if (const auto* p = ImPlot::GetPlot("##wave_overview")) bounds = p->PlotRect;
                    ImGui::EndChild();
                    ui::drawWaveFftPlot(wave, frame, true, false);
                    ImGui::End();
                    ImGui::Render();
                };
                const auto settle = [&] {
                    const auto deadline = Clock::now() + std::chrono::seconds(5);
                    do {
                        draw();
                        // 无效但已缓存的不足样本结果也是完成，不无限等待 valid。
                        if (wave.cachedFftKeyValid && !view.fftUpdatePending && !wave.fftRequestActive) return;
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    } while (Clock::now() < deadline);
                    throw std::runtime_error("概览释放后 FFT 未在期限内完成");
                };
                io.AddMouseButtonEvent(0, false);
                io.AddMousePosEvent(-100, -100);
                for (int i = 0; i < 24; ++i) draw(); // 隔离前一场景双击和事件队列。
                settle();
                const auto oldKey = wave.cachedFftKey;
                const auto oldGeneration = wave.fftRequestGeneration;
                const auto submissions = wave.fftSubmittedCount;
                const auto oldPeak = wave.cachedFftFrame.fundamentalHz;
                view.fftFrequencyMin = 10; view.fftFrequencyMax = 200;
                view.fftMagnitudeMin = -2; view.fftMagnitudeMax = 2;
                view.fftPhaseMin = -90; view.fftPhaseMax = 90;
                view.fftViewportInitialized = true;
                const double startTime = target == 1 ? minTime : target == 2 ? maxTime : (minTime + maxTime) / 2;
                const float startX = std::floor(bounds.Min.x + bounds.GetWidth() * float(startTime / 4));
                const float startY = std::floor(bounds.Min.y + bounds.GetHeight() * (target == 3 ? .2F : .5F));
                io.AddMousePosEvent(startX, startY); draw();
                io.AddMouseButtonEvent(0, true); draw();
                check(view.overviewDrag.target == target, "FFT 回归未捕获预期概览目标");
                const float endX = std::floor(startX + bounds.GetWidth() * float(shift / 4));
                if (!firstMoveOnRelease) {
                    io.AddMousePosEvent(std::floor((startX + endX) / 2), startY); draw();
                    for (int i = 0; i < 3; ++i) draw();
                }
                check(wave.fftSubmittedCount == submissions, "按住概览时提交了昂贵 FFT");
                if (staleTask) {
                    // 注入旧窗口的有效任务（可在执行或已完成但未取走），不依赖线程调度时长。
                    auto snapshot = wave.buffer.snapshot(oldKey.viewMinTime, oldKey.viewMaxTime, false);
                    auto display = plot::buildDisplayData(snapshot, view.sampleFrequencyHz);
                    wave.analysisWorker->submit(plot::WaveFftInput{oldGeneration, oldKey,
                        std::move(snapshot), std::move(display)});
                    wave.fftRequestActive = true;
                }
                // 独立预期：真实整数像素总位移，先限制边缘，再夹紧历史与最小跨度。
                const double delta = (endX - startX) * 4.0 / bounds.GetWidth();
                double expectedMin = minTime + (target != 2 ? delta : 0);
                double expectedMax = maxTime + (target != 1 ? delta : 0);
                if (target == 1) expectedMin = std::clamp(expectedMin, 0.0, maxTime - view.minVisibleTimeSpan);
                if (target == 2) expectedMax = std::clamp(expectedMax, minTime + view.minVisibleTimeSpan, 4.0);
                const double span = (std::min)(4.0, (std::max)(view.minVisibleTimeSpan, expectedMax - expectedMin));
                expectedMin = std::clamp((expectedMin + expectedMax - span) / 2, 0.0, 4.0 - span);
                expectedMax = expectedMin + span;
                io.AddMousePosEvent(endX, outside ? bounds.Max.y + 100 : startY);
                io.AddMouseButtonEvent(0, false);
                draw(); // 两个事件必须在同一次 NewFrame 前入队。
                check(!io.MouseDown[0] && io.MouseReleased[0] && io.MousePos.x == endX,
                      "最终移动与释放未在同帧送达");
                check(std::abs(view.viewMinTime - expectedMin) < 1e-9 &&
                      std::abs(view.viewMaxTime - expectedMax) < 1e-9, "释放遗漏最终总位移或归一化错误");
                check(!view.overviewDrag.activeId && !view.overviewWindowDragging,
                      "FFT 释放后概览捕获未清理");
                check(wave.fftSubmittedCount == submissions, "释放收口前提交了旧选区 FFT");
                rejectOldResult = true;
                settle();
                check(std::abs(wave.cachedFftKey.viewMinTime - expectedMin) < 1e-9 &&
                      std::abs(wave.cachedFftKey.viewMaxTime - expectedMax) < 1e-9 &&
                      view.fftSourceMinTime == view.viewMinTime && view.fftSourceMaxTime == view.viewMaxTime,
                      "最终选区未自动发布或旧任务覆盖新频谱");
                std::size_t finite = 0;
                for (const auto& sample : samples)
                    if (sample.time >= expectedMin && sample.time <= expectedMax && std::isfinite(sample.value)) ++finite;
                check(wave.cachedFftFrame.visibleSampleCount == finite, "FFT 未精确包含端点或未排除非有限值");
                const auto required = plot::resolveWaveFftPointCount(view.fft, finite);
                const bool enough = required >= 16 && finite >= required;
                check(wave.cachedFftFrame.valid == enough, "固定 N 不足样本未发布正确无效结果");
                if (!enough) check(!wave.cachedFftFrame.channels.empty() &&
                    wave.cachedFftFrame.channels[0].message == "当前可视区样本不足", "不足样本错误提示丢失");
                if (enough && expectedMin > 2 && !visibleSamples) {
                    check(oldPeak && std::abs(*oldPeak - 32) < 1 && wave.cachedFftFrame.fundamentalHz &&
                          std::abs(*wave.cachedFftFrame.fundamentalHz - 96) < 1,
                          "不同信号区段释放后谱峰没有自动改变");
                }
                check(view.fftFrequencyMin == 10 && view.fftFrequencyMax == 200 &&
                      view.fftMagnitudeMin == -2 && view.fftMagnitudeMax == 2 &&
                      view.fftPhaseMin == -90 && view.fftPhaseMax == 90 && !view.fftFitAllRequested,
                      "概览改变了独立频域视口");
                const bool changed = std::abs(expectedMin - minTime) > 1e-9 || std::abs(expectedMax - maxTime) > 1e-9;
                check(wave.fftSubmittedCount == submissions + (changed || staleTask ? 1 : 0),
                      "概览最终目标未合并成单次计算或无变化点击额外刷新");
                const auto settled = wave.fftSubmittedCount;
                for (int i = 0; i < 5; ++i) {
                    ui::releaseOverviewDrag(wave);
                    draw();
                    check(wave.fftSubmittedCount == settled, "稳定帧或重复清理重复计算 FFT");
                }
                // 停止跟随后的新数据不移动输入；实时恢复仍采用最新窗口。
                check(wave.buffer.append(0, {{}, {{5, 1}}}), "追加样本失败");
                settle();
                check(view.fftSourceMinTime == expectedMin && view.fftSourceMaxTime == expectedMax,
                      "暂停后的新数据移动了历史输入");
                view.autoFollowLatest = true;
                settle();
                check(std::abs(view.fftSourceMaxTime - 4097 / 1024.0) < 1e-9, "恢复跟随没有采用最新窗口");
            };
            for (int target : {1, 2, 3}) for (bool first : {false, true})
                run(target, first, target == 1 ? .125 : target == 2 ? 1.5 : 2, false, .25, 1.5,
                    target == 3 && !first, false);
            run(3, true, 10, true, .25, 1.5, false, false);
            run(3, true, -10, true, .25, 1.5, false, false);
            run(1, true, -10, true, .25, 1.5, false, true); // 精确包含历史左端和非有限样本。
            run(2, true, 10, true, .25, 1.5, false, true);  // 精确包含历史右端。
            run(1, true, 10, true, .25, 1.5, false, false); // 最小跨度且固定 N 不足。
            run(2, true, -10, true, .25, 1.5, false, false);
            run(3, true, .5, false, 1, 1.001, false, false); // 窄框视觉扩展不得污染真实跨度。
            run(3, true, 0, false, .25, 1.5, false, false); // 无变化点击不得强制刷新。
            for (int cancel : {0, 1, 2, 3, 4}) {
                // 取消不是放下：失焦、空历史、折叠、ID 丢失、抽屉输入隔离不能应用新 MousePos。
                plot::WaveDockState wave;
                auto& view = wave.view;
                view.initialized = true; view.defaultViewportPending = false;
                view.autoFollowLatest = false; view.showCursors = false;
                view.viewMinTime = 1; view.viewMaxTime = 2; view.visibleDuration = 1;
                wave.buffer.append(0, {{}, {{0, 0}, {4, 1}}});
                ImRect bounds;
                const auto draw = [&] {
                    if (withGl) ImGui_ImplOpenGL3_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0));
                    ImGui::SetNextWindowSize(ImVec2(1100, 300));
                    ImGui::Begin("overview release cancellation", nullptr, ImGuiWindowFlags_NoSavedSettings);
                    auto frame = ui::prepareWaveFrame(wave, 1100);
                    if (cancel == 4 && wave.toolsDrawerResizing) ui::releaseOverviewDrag(wave);
                    ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot,
                        *frame.overviewDisplayData, plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6),
                        {0}, frame.renderBudget);
                    if (const auto* p = ImPlot::GetPlot("##wave_overview")) bounds = p->PlotRect;
                    ImGui::End(); ImGui::Render();
                };
                io.AddMouseButtonEvent(0, false); io.AddMousePosEvent(-100, -100);
                for (int i = 0; i < 24; ++i) draw();
                const float x = std::floor(bounds.Min.x + bounds.GetWidth() * .375F);
                const float y = std::floor(bounds.Min.y + bounds.GetHeight() * .2F);
                io.AddMousePosEvent(x, y); draw();
                io.AddMouseButtonEvent(0, true); draw();
                check(view.overviewDrag.target == 3, "取消测试没有捕获概览");
                if (cancel == 0) io.AddFocusEvent(false);
                if (cancel == 1) wave.buffer.clear();
                if (cancel == 2) wave.overviewCollapsed = true;
                if (cancel == 3) ImGui::ClearActiveID();
                if (cancel == 4) wave.toolsDrawerResizing = true;
                io.AddMousePosEvent(x + 200, y);
                io.AddMouseButtonEvent(0, false);
                draw();
                check(view.viewMinTime == 1 && view.viewMaxTime == 2 && !view.overviewDrag.activeId,
                      "取消路径应用了无关的释放坐标");
                io.AddFocusEvent(true); draw();
            }
            {
                // 即使直接调用概览组件，CursorSplit 也必须保留 A/B 输入，而非被时域框覆盖。
                plot::WaveDockState wave;
                auto& view = wave.view;
                view.initialized = true; view.defaultViewportPending = false;
                view.autoFollowLatest = false; view.showCursors = false;
                view.viewMinTime = .25; view.viewMaxTime = 1.5; view.visibleDuration = 1.25;
                view.sampleFrequencyHz = 1024;
                view.fft.enabled = true; view.fft.displayMode = plot::WaveFftDisplayMode::CursorSplit;
                view.fft.pointCount = plot::WaveFftPointCount::N1024;
                view.cursors[1].time = 3;
                std::vector<plot::WaveSample> samples;
                for (int i = 0; i <= 4096; ++i) samples.push_back({i / 1024.0, std::sin(i * .1)});
                wave.buffer.append(0, {{}, samples});
                ImRect bounds;
                const auto draw = [&] {
                    if (withGl) ImGui_ImplOpenGL3_NewFrame();
                    ImGui::NewFrame();
                    ImGui::SetNextWindowPos(ImVec2(0, 0));
                    ImGui::SetNextWindowSize(ImVec2(1100, 300));
                    ImGui::Begin("split FFT input isolation", nullptr, ImGuiWindowFlags_NoSavedSettings);
                    auto frame = ui::prepareWaveFrame(wave, 1100);
                    ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot,
                        *frame.overviewDisplayData, plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6),
                        {0}, frame.renderBudget);
                    if (const auto* p = ImPlot::GetPlot("##wave_overview")) bounds = p->PlotRect;
                    ImGui::End(); ImGui::Render();
                };
                io.AddMouseButtonEvent(0, false); io.AddMousePosEvent(-100, -100);
                for (int i = 0; i < 24; ++i) draw();
                check(view.fftSourceMinTime == 2 && view.fftSourceMaxTime == 3, "分屏 A/B 测试输入错误");
                const float x = std::floor(bounds.Min.x + bounds.GetWidth() * .21875F);
                const float y = std::floor(bounds.Min.y + bounds.GetHeight() * .2F);
                io.AddMousePosEvent(x, y); draw();
                io.AddMouseButtonEvent(0, true); draw();
                check(view.overviewDrag.target == 3, "分屏隔离测试没有捕获概览");
                io.AddMousePosEvent(x + 100, y); io.AddMouseButtonEvent(0, false); draw();
                check(view.viewMinTime > .25 && view.fftSourceMinTime == 2 && view.fftSourceMaxTime == 3,
                      "概览拖动破坏 CursorSplit A/B 输入");
            }
            std::cout << "fft_overview_release: final events, clamp, stale work, finite samples and peaks passed\n";
        }
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
                            const float cursorX = plot->PlotRect.Min.x + plot->PlotRect.GetWidth() *
                                float(wave.view.cursors[c].time / 2.0);
                            if (cursorVertex < 0 && std::abs(drawList->VtxBuffer[v].pos.x - cursorX) < 3 && vertexColor ==
                                ImGui::ColorConvertFloat4ToU32(ui::measurementCursorColor(wave.view, c)))
                                cursorVertex = v;
                        }
                        for (std::size_t i = 0; i < colors.size(); ++i) {
                            const auto color = ImGui::ColorConvertFloat4ToU32(ui::displayColor(
                                ui::channelColor(*wave.buffer.channelSpec(i), i),
                                ui::activeWaveStyleTokens().plotBackground, .65F));
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
                if (wave.measurementRequestActive)
                    throw std::runtime_error("measurement submitted during drag");
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
            // 缩放出窗后普通和分屏读数仍绑定全历史，完整 A/B 统计保持可用。
            for (const auto mode : {plot::WaveViewMode::Overlay, plot::WaveViewMode::Split}) {
                wave.view.viewMode = mode;
                wave.view.autoFollowLatest = false;
                wave.view.viewMinTime = 6;
                wave.view.viewMaxTime = 7;
                wave.view.visibleDuration = 1;
                wave.view.forceNextMainPlotLimits = true;
                wave.view.cursors[0].time = 1;
                wave.view.cursors[1].time = 2;
                wave.view.lastCursorReadouts = {};
                const auto deadline = Clock::now() + std::chrono::seconds(5);
                do {
                    drawMeasurementFrame();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while (wave.view.measurementUpdatePending && Clock::now() < deadline);
                if (!wave.view.lastCursorReadouts[0] || !wave.view.lastCursorReadouts[1] ||
                    !wave.cachedMeasurement || !wave.cachedMeasurement->valid ||
                    wave.cachedMeasurement->sampleCount != reference.sampleCount) {
                    std::cerr << "offscreen mode=" << static_cast<int>(mode) << " A=" << wave.view.cursors[0].time
                        << " B=" << wave.view.cursors[1].time << " readouts="
                        << wave.view.lastCursorReadouts[0].has_value() << ',' << wave.view.lastCursorReadouts[1].has_value()
                        << " cached=" << wave.cachedMeasurement.has_value() << " count="
                        << (wave.cachedMeasurement ? wave.cachedMeasurement->sampleCount : 0)
                        << " expected=" << reference.sampleCount << " key=" << wave.measurementKey.begin << ','
                        << wave.measurementKey.end << '\n';
                    throw std::runtime_error("offscreen fixed cursor measurement lost");
                }
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
            // 同一数据往返切换：稳定模式不得二次压缩，旧模式必须恢复各布局原有分支。
            for (const auto downsample : {plot::WaveDownsampleMode::StableEdges,
                                          plot::WaveDownsampleMode::LegacyUniform,
                                          plot::WaveDownsampleMode::StableEdges})
            for (const auto mode : {plot::WaveViewMode::Overlay, plot::WaveViewMode::Stacked,
                                    plot::WaveViewMode::Split}) {
                view.downsampleMode = downsample;
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
                        if (downsample == plot::WaveDownsampleMode::StableEdges &&
                            (edge == indices.end() || edge + 1 == indices.end() || *(edge + 1) != 1234))
                            throw std::runtime_error("display cache lost fixed analog edge pair");
                        const auto rendered = ui::drawOscilloscopePlot(wave, frame,
                            {.drawMeasurementOverlay = false, .drawLegendOverlay = false}, nullptr);
                        ImGui::End();
                        ImGui::Render();
                        if (!rendered.plotRendered)
                            throw std::runtime_error("analog mode switch produced no plot");
                        if (downsample == plot::WaveDownsampleMode::StableEdges) {
                            if (!wave.renderEnvelopeCache.empty())
                                throw std::runtime_error("analog query trace was compressed again by renderer");
                        } else if (mode != plot::WaveViewMode::Split || !peakDetect) {
                            if (wave.renderEnvelopeCache.empty() || !wave.renderEnvelopeCache[0].valid)
                                throw std::runtime_error("legacy drawing branch was not restored");
                            const auto& cached = wave.renderEnvelopeCache[0];
                            if (cached.key.downsampleMode != downsample || cached.key.peakDetectDownsample != peakDetect ||
                                (peakDetect ? cached.peakDetectTrace.empty() : cached.envelope.empty()))
                                throw std::runtime_error("legacy peak/envelope cache used wrong mode");
                        }
                        if (view.lastRenderPointCount > frame.renderBudget.pointsPerChannel ||
                            ImGui::GetDrawData()->TotalVtxCount > int(view.maxRenderVertices))
                            throw std::runtime_error("analog stable rendering exceeds budget");
                        if (withGl && pan == 5) {
                            glViewport(0, 0, 1280, 900);
                            glClear(GL_COLOR_BUFFER_BIT);
                            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                            glFinish();
                            captureFrame(captureDirectory, "analog-mode-" + std::to_string(int(downsample)) +
                                "-" + std::to_string(int(mode)) +
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
            for (int scenario = 0; scenario < 16; ++scenario) {
                const int mode = scenario % 8;
                wave.view.downsampleMode = scenario < 8
                    ? plot::WaveDownsampleMode::StableEdges : plot::WaveDownsampleMode::LegacyUniform;
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
                    if (mode == 0) captureFrame(captureDirectory, "overview-" + std::to_string(count) +
                        "-" + std::to_string(int(wave.view.downsampleMode)));
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
                          << " downsample=" << int(wave.view.downsampleMode)
                          << " p95_ms=" << times[times.size() * 95 / 100] << " vertices=" << vertices
                          << " phosphor_status=" << wave.view.lastRenderStats.phosphorBackendStatus << '\n';
                if (withGl && (mode == 4 || mode == 5)) {
                    std::cout << "warm_backend mode=" << mode << " status=" << backend << '\n';
                    if (backend.find(mode == 4 ? "GPU FBO" : "CPU Texture") == std::string::npos)
                        throw std::runtime_error("requested phosphor backend did not render");
                    // 在冻结状态切换，下一帧必须重建而不能复用旧模式纹理。
                    wave.view.downsampleMode = wave.view.downsampleMode == plot::WaveDownsampleMode::StableEdges
                        ? plot::WaveDownsampleMode::LegacyUniform : plot::WaveDownsampleMode::StableEdges;
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
