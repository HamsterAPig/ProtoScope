#include "../src/ui/wave/wave_render_service.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace protoscope;
namespace {
void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

void modelTests()
{
    plot::WaveAuxiliaryCursors cursors;
    cursors.random.seed(12345);
    std::array<std::size_t, 5> usage{};
    for (int n = 0; n < 40; ++n) {
        const auto previous = cursors.items;
        const auto id = cursors.add(4, 8);
        const auto& added = cursors.items.back();
        require(id == static_cast<std::uint64_t>(n + 1) && added.time >= 4 && added.time <= 8,
                "visible position or stable ID wrong");
        if (n == 0) require(added.time == 6, "first cursor must use midpoint");
        if (n < 20)
            for (const auto& cursor : previous)
                require(std::abs(added.time - cursor.time) >= 0.2 - 1e-12, "new cursor overlaps existing T");
        require(usage[added.colorIndex] == *std::min_element(usage.begin(), usage.end()),
                "color did not choose minimum occupancy");
        ++usage[added.colorIndex];
        const auto rgb = plot::kAuxiliaryCursorRgb[added.colorIndex];
        require(rgb != plot::kMeasurementCursorRgb[0] && rgb != plot::kMeasurementCursorRgb[1],
                "auxiliary cursor uses A/B color");
        for (std::size_t i = 0; i < previous.size(); ++i)
            require(previous[i].colorIndex == cursors.items[i].colorIndex && previous[i].id == cursors.items[i].id,
                    "creation recolored existing cursor");
    }
    const auto released = cursors.items[2].colorIndex;
    require(cursors.remove(3) && !cursors.remove(3), "delete identity wrong");
    require(cursors.add(8, 4) == 41 && cursors.items.back().colorIndex == released, "deleted color not released");
    cursors.clear();
    cursors.add(0, 0);
    cursors.add(4, 4);
    cursors.add(2, 2);
    auto intervals = cursors.intervals();
    require(intervals.size() == 2 && intervals[0].left.id == 1 && intervals[0].right.id == 3 &&
            intervals[1].right.id == 2 && intervals[0].delta == 2, "crossed cursor order wrong");
    cursors.items[2].time = 0;
    intervals = cursors.intervals();
    require(intervals[0].delta == 0, "coincident cursor delta must be zero");
    plot::WaveViewState view;
    view.auxiliaryCursors = cursors;
    view.followMeasurementCursorsOnScroll = true;
    view.showCursors = false;
    const plot::WaveViewport oldViewport{.minTime = 0, .maxTime = 10};
    const plot::WaveViewport panned{.minTime = 4, .maxTime = 14};
    plot::shiftMeasurementCursorsForViewportScroll(view, oldViewport, panned);
    require(view.auxiliaryCursors.items[0].time == 4, "auxiliary scroll depends on A/B visibility");
    view.followMeasurementCursorsOnScroll = false;
    plot::shiftMeasurementCursorsForViewportScroll(view, oldViewport, panned);
    require(view.auxiliaryCursors.items[0].time == 4, "fixed cursor followed scroll");
    view.followMeasurementCursorsOnScroll = true;
    plot::shiftMeasurementCursorsForViewportScroll(view, oldViewport, {.minTime = 4, .maxTime = 8});
    require(view.auxiliaryCursors.items[0].time == 4, "zoom moved auxiliary cursor");
    cursors.clear();
    const std::array<double, 2> ab{5, 5.5};
    cursors.add(0, 10, ab);
    require(std::abs(cursors.items.back().time - 6) < 1e-12, "new cursor must avoid visible A/B");
    cursors.clear();
    for (int i = 10; i <= 20; ++i) cursors.add(i * 0.5, i * 0.5);
    cursors.add(0, 10);
    require(std::abs(cursors.items.back().time - 4.5) < 1e-12, "full right side must search left");
    cursors.clear();
    for (int i = 0; i <= 40; ++i) cursors.add(i * 0.25, i * 0.25);
    cursors.items[20].time = 4.75;
    cursors.add(0, 10);
    require(std::abs(cursors.items.back().time - 5) < 1e-12, "crowded layout must choose largest gap midpoint");
    require(cursors.add(std::nan(""), 10) == 0, "nonfinite range accepted");
    cursors.add(3, 3);
    require(cursors.items.back().time == 3, "zero range did not terminate");
    cursors.add(-1e308, 1e308);
    require(std::isfinite(cursors.items.back().time), "extreme finite range overflow");
    for (const auto [unit, scale] : std::array<std::pair<const char*, double>, 7>{{
             {"s", 1}, {"ms", 1e-3}, {"us", 1e-6}, {"\xC2\xB5s", 1e-6},
             {"\xCE\xBCs", 1e-6}, {"ns", 1e-9}, {"ps", 1e-12}}}) {
        const auto hz = plot::cursorFrequencyHz(2, plot::WaveTimeAxisSource::ScriptTime, unit);
        require(std::abs(hz * (2 * scale) - 1) < 1e-12, "SI cursor frequency conversion");
        const auto interval = plot::makeCursorIntervalText(0, 2, plot::WaveTimeAxisSource::ScriptTime, unit);
        require(interval.frequencyHz == hz, "A/B and T frequency disagree");
    }
    for (const auto delta : {0.0, std::numeric_limits<double>::infinity(), std::nan("")})
        require(std::isnan(plot::cursorFrequencyHz(delta, plot::WaveTimeAxisSource::ScriptTime, "s")),
                "invalid interval must show N/A");
    require(std::isnan(plot::cursorFrequencyHz(2, plot::WaveTimeAxisSource::ScriptTime, "tick")),
            "unknown time unit must show N/A");
    require(std::isnan(plot::cursorFrequencyHz(2, plot::WaveTimeAxisSource::SampleIndex, "s")),
            "sample axis must not produce Hz");
    require(plot::cursorFrequencyHz(-2, plot::WaveTimeAxisSource::ScriptTime, "ms") == 500,
            "frequency must use absolute interval");
    require(std::isnan(plot::cursorFrequencyHz(1e-320, plot::WaveTimeAxisSource::ScriptTime, "ps")),
            "underflow interval must not produce infinite Hz");
}

void capture(const std::filesystem::path& directory, const std::string& name)
{
    if (directory.empty()) return;
    std::filesystem::create_directories(directory);
    std::vector<unsigned char> pixels(1200 * 850 * 3);
    glReadPixels(0, 0, 1200, 850, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    require(glGetError() == GL_NO_ERROR, "framebuffer read failed");
    const auto extrema = std::minmax_element(pixels.begin(), pixels.end());
    require(*extrema.first != *extrema.second, "blank framebuffer");
    for (std::size_t i = 0; i < pixels.size(); i += 3) std::swap(pixels[i], pixels[i + 2]);
    std::array<unsigned char, 54> header{};
    header[0] = 'B'; header[1] = 'M';
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) header[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, static_cast<std::uint32_t>(54 + pixels.size()));
    put(10, 54); put(14, 40); put(18, 1200); put(22, 850);
    header[26] = 1; header[28] = 24;
    std::ofstream file(directory / (name + ".bmp"), std::ios::binary);
    file.write(reinterpret_cast<const char*>(header.data()), header.size());
    file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    require(bool(file), "capture write failed");
}

void uiTests(bool withGl, const std::filesystem::path& directory)
{
    auto& io = ImGui::GetIO();
    plot::WaveDockState wave;
    auto& view = wave.view;
    view.initialized = true;
    view.defaultViewportPending = false;
    view.autoFollowLatest = false;
    view.viewMinTime = 0;
    view.viewMaxTime = 1;
    view.visibleDuration = 1;
    view.sampleFrequencyHz = 1024;
    view.showMeasurementOverlay = false;
    view.showChannelLegend = false;
    view.glowEnabled = false;
    for (std::size_t channel = 0; channel < 2; ++channel) {
        wave.buffer.setChannelSpec(channel, {.label = "Signal " + std::to_string(channel + 1)});
        plot::WaveAppendRequest request;
        for (int n = 0; n < 2048; ++n) request.samples.push_back({n / 1024.0, std::sin(n * 0.04 + channel)});
        wave.buffer.append(channel, std::move(request));
    }
    ImPlotPlot* plot = nullptr;
    ImVec2 windowSize(1180, 820);
    const auto render = [&]() {
        if (withGl) ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(windowSize);
        ImGui::Begin("Auxiliary cursor verification", nullptr, ImGuiWindowFlags_NoSavedSettings);
        auto frame = ui::prepareWaveFrame(wave, windowSize.x);
        if (view.fft.enabled && view.fft.displayMode == plot::WaveFftDisplayMode::FullSpectrum) {
            require(ui::drawWaveFftPlot(wave, frame, true).plotRendered, "full spectrum plot missing");
        } else {
            if (view.fft.enabled)
                ImGui::BeginChild("time", ImVec2(0, windowSize.y * 0.5F));
            const auto result = ui::drawOscilloscopePlot(wave, frame,
                {.drawMeasurementOverlay = view.showMeasurementOverlay, .drawLegendOverlay = false}, nullptr);
            require(result.plotRendered, "time-domain plot missing");
            plot = ImPlot::GetPlot("##oscilloscope");
            if (view.viewMode == plot::WaveViewMode::Split)
                for (auto* child : ImGui::GetCurrentWindow()->DC.ChildWindows)
                    if (std::string_view(child->Name).find("wave_split_scroll") != std::string_view::npos)
                        plot = GImPlot->Plots.GetByKey(child->GetID("##wave_split_0"));
            if (view.fft.enabled) {
                ImGui::EndChild();
                ImGui::BeginChild("spectrum");
                ui::drawWaveFftPlot(wave, frame, false, false);
                ImGui::EndChild();
            }
        }
        ImGui::End();
        ImGui::Render();
        if (withGl) {
            glViewport(0, 0, 1200, 850);
            glClearColor(0.05F, 0.05F, 0.05F, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glFinish();
        }
    };
    render();
    view.auxiliaryCursors.random.seed(99);
    view.auxiliaryCursors.add(0.3, 0.3);
    view.showCursors = false;
    view.cursorSnapMode = plot::WaveCursorSnapMode::ModifierSnap;
    for (int n = 0; n < 3; ++n) render();
    const auto pixel = [&](double time) {
        return ImVec2(plot->Axes[ImAxis_X1].PlotToPixels(time), plot->PlotRect.GetCenter().y);
    };
    auto mouse = pixel(0.3);
    io.AddMousePosEvent(mouse.x, mouse.y);
    render();
    io.AddMouseButtonEvent(0, true);
    render();
    mouse = pixel(0.62);
    io.AddMousePosEvent(mouse.x, mouse.y);
    render();
    io.AddMouseButtonEvent(0, false);
    render();
    require(std::abs(view.auxiliaryCursors.items[0].time - 0.62) < 0.005,
            "auxiliary mouse drag failed while A/B hidden");
    // 使用真实鼠标事件验证吸附与活动通道范围，预期取自现有 A/B 吸附查询。
    view.cursorSnapMode = plot::WaveCursorSnapMode::SmartSnap;
    view.cursorSnapScope = plot::WaveCursorSnapScope::ActiveChannel;
    view.measurementChannelIndex = 0;
    mouse = pixel(view.auxiliaryCursors.items[0].time);
    io.AddMousePosEvent(mouse.x, mouse.y);
    render();
    io.AddMouseButtonEvent(0, true);
    render();
    mouse = ImVec2(std::floor(plot->Axes[ImAxis_X1].PlotToPixels(0.185)),
                   std::floor(plot->Axes[ImAxis_Y1].PlotToPixels(0.99)));
    auto frame = ui::prepareWaveFrame(wave, windowSize.x);
    const ImPlotRect limits(plot->Axes[ImAxis_X1].Range.Min, plot->Axes[ImAxis_X1].Range.Max,
                            plot->Axes[ImAxis_Y1].Range.Min, plot->Axes[ImAxis_Y1].Range.Max);
    const auto expected = ui::findSmartCursorSnapByScope(*frame.displayData, view,
        plot->Axes[ImAxis_X1].PixelsToPlot(mouse.x), plot->Axes[ImAxis_Y1].PixelsToPlot(mouse.y),
        limits, (limits.X.Max - limits.X.Min) * 0.02, std::nullopt);
    require(expected.has_value(), "snap fixture has no target");
    io.AddMousePosEvent(mouse.x, mouse.y);
    render();
    io.AddMouseButtonEvent(0, false);
    render();
    require(std::abs(view.auxiliaryCursors.items[0].time - expected->readout.time) < 1e-9,
            "T drag did not reuse configured snap target");
    const auto ab = view.cursors;
    view.auxiliaryCursors.items[0].time = 0.5;
    view.auxiliaryCursors.add(0.5, 0.5);
    render();
    mouse = pixel(0.5);
    io.AddMousePosEvent(mouse.x, mouse.y);
    render();
    io.AddMouseButtonEvent(1, true);
    render();
    io.AddMouseButtonEvent(1, false);
    render();
    require(view.auxiliaryCursors.contextHits.size() == 2, "overlapping context menu omitted cursor");
    render();
    capture(directory, "overlap-menu");
    require(!GImGui->OpenPopupStack.empty() && GImGui->OpenPopupStack.back().Window, "cursor popup missing");
    const auto* popup = GImGui->OpenPopupStack.back().Window;
    const auto menuPoint = ImVec2(popup->Pos.x + 30, popup->Pos.y + ImGui::GetStyle().WindowPadding.y + 8);
    io.AddMousePosEvent(menuPoint.x, menuPoint.y);
    render();
    io.AddMouseButtonEvent(0, true);
    render();
    io.AddMouseButtonEvent(0, false);
    render();
    require(view.auxiliaryCursors.items.size() == 1 && view.auxiliaryCursors.items[0].id == 2,
            "context menu did not delete selected stable ID");
    require(view.cursors[0].time == ab[0].time && view.cursors[1].time == ab[1].time, "T interaction changed A/B");
    // 编号标签位于线右侧，点击标签文字也必须能打开同一删除菜单。
    mouse = ImVec2(pixel(0.5).x + 12, plot->PlotRect.Min.y + 10);
    io.AddMousePosEvent(mouse.x, mouse.y);
    render();
    io.AddMouseButtonEvent(1, true);
    render();
    io.AddMouseButtonEvent(1, false);
    render();
    require(view.auxiliaryCursors.contextHits == std::vector<std::uint64_t>{2},
            "right-click label did not resolve stable ID");
    io.AddKeyEvent(ImGuiKey_Escape, true);
    render();
    io.AddKeyEvent(ImGuiKey_Escape, false);
    render();
    // 通过真实按下/释放事件覆盖实段、虚线间隙、标签、重合和分屏；第一击不能吸附。
    view.showCursors = true;
    for (const auto mode : {plot::WaveViewMode::Overlay, plot::WaveViewMode::Split}) {
        view.viewMode = mode;
        view.forceNextMainPlotLimits = true;
        for (int scenario = 0; scenario < 5; ++scenario) {
            view.auxiliaryCursors.clear();
            const auto first = view.auxiliaryCursors.add(0.5, 0.5);
            const auto second = scenario == 3 ? view.auxiliaryCursors.add(0.5, 0.5) :
                scenario == 4 ? view.auxiliaryCursors.add(0.504, 0.504) : 0;
            view.zoomSelectionActive = scenario % 2 == 0;
            for (int n = 0; n < 25; ++n) render();
            require(plot != nullptr, "mouse fixture plot missing");
            const auto beforeView = ui::currentViewport(view);
            const auto beforeAb = view.cursors;
            auto target = pixel(scenario == 4 ? 0.501 : 0.5);
            target.y = plot->PlotRect.Min.y + (scenario == 1 ? 208 : 206);
            if (scenario == 2) target = ImVec2(pixel(0.5).x + 12, plot->PlotRect.Min.y + 10);
            io.AddMousePosEvent(target.x, target.y);
            render();
            io.AddMouseButtonEvent(0, true);
            render();
            require(view.auxiliaryCursors.items[0].time == 0.5, "first click snapped T cursor");
            io.AddMouseButtonEvent(0, false);
            render();
            io.AddMouseButtonEvent(0, true);
            render();
            io.AddMouseButtonEvent(0, false);
            render();
            capture(directory, "delete-" + std::to_string(int(mode)) + "-" + std::to_string(scenario));
            if (view.auxiliaryCursors.items.size() != (second ? 1U : 0U))
                throw std::runtime_error("double-click must remove exactly one T: mode=" +
                    std::to_string(int(mode)) + " scenario=" + std::to_string(scenario) +
                    " count=" + std::to_string(view.auxiliaryCursors.items.size()));
            if (second)
                require(view.auxiliaryCursors.items[0].id == (scenario == 3 ? first : second),
                        "overlap nearest/topmost deletion rule");
            const auto afterView = ui::currentViewport(view);
            require(beforeView.minTime == afterView.minTime && beforeView.maxTime == afterView.maxTime &&
                    beforeView.minValue == afterView.minValue && beforeView.maxValue == afterView.maxValue,
                    "cursor double-click changed viewport");
            require(view.cursors[0].time == beforeAb[0].time && view.cursors[1].time == beforeAb[1].time,
                    "cursor double-click changed A/B");
            require(!view.zoomSelectionDragging, "cursor double-click started box selection");
        }
    }
    view.zoomSelectionActive = false;
    view.viewMode = plot::WaveViewMode::Overlay;
    view.forceNextMainPlotLimits = true;
    view.auxiliaryCursors.clear();
    io.AddMousePosEvent(-100, -100);
    view.showCursors = true;
    view.cursors[0].time = 0.15;
    view.cursors[1].time = 0.85;
    for (int n = 0; n < 12; ++n) view.auxiliaryCursors.add(n * 0.075, n * 0.075);
    view.auxiliaryCursors.add(0.5, 0.5);
    const auto identities = view.auxiliaryCursors.items;
    render();
    const auto stableT = ui::cursorOverviewColor(ui::auxiliaryCursorColor(view, view.auxiliaryCursors.items.front()));
    const auto addedId = view.auxiliaryCursors.add(.91, .91);
    render();
    require(ui::cursorOverviewColor(ui::auxiliaryCursorColor(view, view.auxiliaryCursors.items.front())) == stableT,
            "新增 T 重新分配已有自动身份色");
    view.auxiliaryCursors.remove(addedId);
    render();
    require(ui::cursorOverviewColor(ui::auxiliaryCursorColor(view, view.auxiliaryCursors.items.front())) == stableT,
            "删除 T 重新分配已有自动身份色");
    for (auto theme : {config::GuiTheme::ProfessionalDark, config::GuiTheme::ProfessionalLight,
                       config::GuiTheme::DebugHighContrast}) {
        ui::applyUiTheme(theme);
        const auto& themeTokens = ui::activeUiStyleTokens();
        require(plot::overviewContrast(ui::cursorOverviewColor(themeTokens.textStrong),
                                       ui::cursorOverviewColor(themeTokens.panelBackgroundAlt)) >=
                    (theme == config::GuiTheme::DebugHighContrast ? 12 : 4.5),
                "游标标签回退正文在新主题背景上不达标");
        for (auto mode : {plot::WaveViewMode::Overlay, plot::WaveViewMode::Stacked, plot::WaveViewMode::Split}) {
            // 前面的拖动/缩放用例会留下旧Y锚点和读数。每个集成场景独立恢复确定夹具，
            // 不能只设 pinned 后任由旧读数影响最近点查询；采样索引256/768确实存在。
            view.viewMode = mode;
            view.initialized = true;
            view.defaultViewportPending = false;
            view.autoFollowLatest = false;
            view.viewMinTime = 0; view.viewMaxTime = 1; view.visibleDuration = 1;
            view.viewMinValue = -1.5; view.viewMaxValue = 1.5;
            view.cursorSnapMode = plot::WaveCursorSnapMode::ModifierSnap;
            view.cursorSnapScope = plot::WaveCursorSnapScope::ActiveChannel;
            view.measurementChannelIndex = 0;
            view.cursorIntervalLocked = false;
            view.lastCursorReadouts = {};
            view.measurementCursorReadoutRefreshPending = true;
            view.cursors[0] = {.enabled=true,.pinned=true,.channelIndex=0,.time=.25,.value=std::sin(256*.04)};
            view.cursors[1] = {.enabled=true,.pinned=true,.channelIndex=0,.time=.75,.value=std::sin(768*.04)};
            view.forceNextMainPlotLimits = true;
            for (int n = 0; n < 4; ++n) render();
            for (std::size_t c=0;c<2;++c) {
                const auto& readout=view.lastCursorReadouts[c];
                require(readout.has_value(),"集成场景缺少A/B有效通道读数");
                require(readout->channelIndex==0 && std::abs(readout->time-(c ? .75 : .25))<1e-9 &&
                        std::abs(readout->value-std::sin((c ? 768 : 256)*.04))<1e-9,
                        "集成场景A/B读数被旧锚点、初始化、跟随或吸附覆盖");
                require(plot && readout->time>plot->Axes[ImAxis_X1].Range.Min &&
                        readout->time<plot->Axes[ImAxis_X1].Range.Max,"A/B不在实际可见时间窗");
            }
            require(view.auxiliaryCursors.items.size() == identities.size(), "display mode cleared T cursors");
            const auto expectedColor = ImGui::ColorConvertFloat4ToU32(ui::auxiliaryCursorColor(view, view.auxiliaryCursors.items.front()));
            bool hasIdentityColor = false;
            for (const auto* list : ImGui::GetDrawData()->CmdLists)
                for (const auto& vertex : list->VtxBuffer) hasIdentityColor = hasIdentityColor || vertex.col == expectedColor;
            require(hasIdentityColor, "布局未使用本 Dock 冻结的 T 身份色");
            // 在真实 drawOscilloscopePlot 的最终draw data中匹配完整两行字形UV/颜色/相对坐标，
            // 不是搜索整帧任意同色背景，也不直接调用标签帮助函数充当集成结果。
            for (std::size_t c=0;c<2;++c) {
                const auto& readout=*view.lastCursorReadouts[c];
                char text[128];
                const auto time=ui::formatMetricText(readout.time,"s");
                std::snprintf(text,sizeof(text),"%c %s\nSignal 1 %.6g",c ? 'B' : 'A',time.c_str(),readout.value);
                ImDrawList expected(ImGui::GetDrawListSharedData());
                expected._ResetForNewFrame();
                expected.PushClipRectFullScreen();
                expected.PushTexture(ImGui::GetIO().Fonts->TexRef);
                expected.AddText({0,0},ImGui::ColorConvertFloat4ToU32(ui::cursorImVec(view.cursorColors.labelText)),text);
                bool found=false;
                for (const auto* list : ImGui::GetDrawData()->CmdLists)
                    for(int start=0;start+expected.VtxBuffer.Size<=list->VtxBuffer.Size;++start) {
                        if(expected.VtxBuffer.empty()) continue;
                        const ImVec2 origin(list->VtxBuffer[start].pos.x-expected.VtxBuffer[0].pos.x,
                                            list->VtxBuffer[start].pos.y-expected.VtxBuffer[0].pos.y);
                        bool same=true;
                        for(int n=0;n<expected.VtxBuffer.Size && same;++n) {
                            const auto& actual=list->VtxBuffer[start+n]; const auto& wanted=expected.VtxBuffer[n];
                            same=actual.col==wanted.col && std::abs(actual.uv.x-wanted.uv.x)<1e-6F &&
                                std::abs(actual.uv.y-wanted.uv.y)<1e-6F &&
                                std::abs(actual.pos.x-wanted.pos.x-origin.x)<.01F &&
                                std::abs(actual.pos.y-wanted.pos.y-origin.y)<.01F && plot->PlotRect.Contains(actual.pos);
                        }
                        if(same) {
                            // 字形必须被实际提交且位于命令裁剪内，不能只存在于未使用的顶点缓存。
                            for(int n=0;n<expected.VtxBuffer.Size && same;++n) {
                                bool visible=false;
                                const auto p=list->VtxBuffer[start+n].pos;
                                for(const auto& cmd:list->CmdBuffer) {
                                    if(cmd.UserCallback || p.x<cmd.ClipRect.x || p.x>cmd.ClipRect.z ||
                                       p.y<cmd.ClipRect.y || p.y>cmd.ClipRect.w) continue;
                                    for(unsigned i=cmd.IdxOffset;i<cmd.IdxOffset+cmd.ElemCount;++i)
                                        if(cmd.VtxOffset+list->IdxBuffer[i]==unsigned(start+n)) { visible=true; break; }
                                    if(visible) break;
                                }
                                same=visible;
                            }
                        }
                        found=found || same;
                    }
                if(!found) throw std::runtime_error("实际A/B两行标签缺失: theme="+std::to_string(int(theme))+
                    " mode="+std::to_string(int(mode))+" text="+text);
            }
            require(plot::overviewContrast(view.cursorColors.labelText,view.cursorColors.labelBackground)>=
                    (theme==config::GuiTheme::DebugHighContrast?12:4.5),"实际读数正文与标签背景阈值");
            for (std::size_t i = 0; i < identities.size(); ++i)
                require(view.auxiliaryCursors.items[i].id == identities[i].id &&
                        view.auxiliaryCursors.items[i].colorIndex == identities[i].colorIndex,
                        "display mode recolored T cursors");
            capture(directory, "theme-" + std::to_string(int(theme)) + "-mode-" + std::to_string(int(mode)));
        }
    }
    windowSize = ImVec2(480, 380);
    view.viewMode = plot::WaveViewMode::Overlay;
    for (int n = 0; n < 4; ++n) render();
    capture(directory, "dense-narrow");
    windowSize = ImVec2(1180, 820);
    view.showMeasurementOverlay = true;
    for (int n = 0; n < 4; ++n) render();
    capture(directory, "measurement-overlay");
    view.showMeasurementOverlay = false;
    // 该既有“频域没有 T”像素检查使用独占的手动身份色，避免自动候选与 FFT 波形同色误报。
    view.cursorAutoColor = false;
    view.fft.enabled = true;
    view.fft.displayMode = plot::WaveFftDisplayMode::CursorSplit;
    const auto settleFft = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        do {
            ui::prepareWaveFrame(wave, 900);
            if (wave.cachedFftFrame.valid && !view.fftUpdatePending) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("UI FFT fixture did not settle");
    };
    settleFft();
    for (int n = 0; n < 4; ++n) render();
    capture(directory, "fft-cursor-split");
    require(view.auxiliaryCursors.items.size() == identities.size(), "cursor-split FFT cleared T");
    const auto hasAuxiliaryColor = [&] {
        const auto* data = ImGui::GetDrawData();
        const auto found = std::ranges::find(view.auxiliaryCursors.items, std::size_t{3}, &plot::WaveAuxiliaryCursor::colorIndex);
        require(found != view.auxiliaryCursors.items.end(), "缺少手动色槽3的 T 测试样本");
        for (const auto* list : data->CmdLists)
            for (const auto& vertex : list->VtxBuffer)
                if (vertex.col == ImGui::ColorConvertFloat4ToU32(ui::auxiliaryCursorColor(view, *found))) return true;
        return false;
    };
    require(hasAuxiliaryColor(), "time plot did not draw T cursor color");
    view.fft.displayMode = plot::WaveFftDisplayMode::FullSpectrum;
    settleFft();
    for (int n = 0; n < 4; ++n) render();
    require(view.auxiliaryCursors.items.size() == identities.size(), "full spectrum destroyed T");
    require(!hasAuxiliaryColor(), "full spectrum drew hidden T cursors");
    capture(directory, "fft-full-spectrum");
    view.fft.enabled = false;
    for (int n = 0; n < 4; ++n) render();
    require(hasAuxiliaryColor(), "return to time domain did not restore T");
    view.sampleFrequencyHz = 2048;
    ui::prepareWaveFrame(wave, 900);
    require(view.auxiliaryCursors.items.empty(), "frequency change retained stale T");
    view.auxiliaryCursors.add(0, 1);
    wave.buffer.clear();
    ui::prepareWaveFrame(wave, 900);
    require(view.auxiliaryCursors.items.empty(), "history clear retained T");
    wave.buffer.appendImported(0, {{0, 1}, {1, 2}});
    ui::prepareWaveFrame(wave, 900);
    view.auxiliaryCursors.add(0, 1);
    wave.buffer.appendImported(0, {{0, 3}, {1, 4}});
    ui::prepareWaveFrame(wave, 900);
    require(view.auxiliaryCursors.items.size() == 1, "ordinary import append cleared T");
    wave.buffer.clear();
    wave.buffer.appendImported(0, {{0, 3}, {1, 4}});
    ui::prepareWaveFrame(wave, 900);
    require(view.auxiliaryCursors.items.empty(), "data replacement retained T");
    view.sampleFrequencyHz = 0;
    ui::prepareWaveFrame(wave, 900);
    view.auxiliaryCursors.add(0, 1);
    view.viewMinTime = 10;
    view.viewMaxTime = 20;
    ui::prepareWaveFrame(wave, 900);
    require(view.auxiliaryCursors.items.size() == 1, "empty viewport cleared T");
    view.viewMinTime = 0;
    view.viewMaxTime = 2;
    ui::prepareWaveFrame(wave, 900);
    require(view.auxiliaryCursors.items.size() == 1, "return from empty viewport cleared T");
    wave.buffer.clear();
    wave.buffer.appendImported(0, {{0, 3}, {0, 4}});
    view.viewMinTime = 0;
    view.viewMaxTime = 2;
    view.forceNextMainPlotLimits = true;
    ui::prepareWaveFrame(wave, 900);
    require(view.timeAxisSource == plot::WaveTimeAxisSource::SampleIndex, "sample-axis fixture wrong");
    view.auxiliaryCursors.add(0, 0);
    view.auxiliaryCursors.add(1, 1);
    for (int n = 0; n < 4; ++n) render();
    capture(directory, "sample-axis");
}
}

int main(int argc, char** argv)
{
    const bool withGl = argc == 3 && std::string_view(argv[1]) == "--capture";
    GLFWwindow* window = nullptr;
    if (withGl) {
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        window = glfwCreateWindow(1200, 850, "cursor verification", nullptr, nullptr);
        if (!window) { glfwTerminate(); return 2; }
        glfwMakeContextCurrent(window);
    }
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ui::applyUiTheme(config::GuiTheme::ProfessionalDark);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1200, 850);
    io.DeltaTime = 1.0F / 60.0F;
    if (withGl) ImGui_ImplOpenGL3_Init("#version 330");
    if (std::filesystem::exists("C:/Windows/Fonts/msyh.ttc"))
        io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 16, nullptr,
                                   io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int status = 0;
    try {
        modelTests();
        uiTests(withGl, withGl ? std::filesystem::path(argv[2]) : std::filesystem::path{});
        std::cout << "Auxiliary cursors: identity, color, deltas, scroll, mouse, layouts and lifetime passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    if (withGl) ImGui_ImplOpenGL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    if (window) { glfwDestroyWindow(window); glfwTerminate(); }
    return status;
}
