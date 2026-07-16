#include "protoscope/ui/wave_dock_renderer.hpp"

#include "protoscope/app/application.hpp"
#include "protoscope/ui/icons.hpp"
#include "protoscope/ui/keyboard_shortcuts.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include "wave_component.hpp"
#include "wave_context.hpp"
#include "wave_detail.hpp"
#include "wave_fft_component.hpp"
#include "wave_render_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>
#include <implot.h>
#include <implot_internal.h>

namespace protoscope::ui {

struct WaveOverlayFrame {
    PlotRenderResult plotResult{};
    plot::WaveSnapshot snapshot{};
    const plot::WaveDisplayData* displayData{nullptr};
    ImGuiViewport* hostViewport{nullptr};
    bool hasPlotResult{false};
};

namespace {

    constexpr float kCompactMainToolbarFramePaddingY = 2.0F;
    constexpr float kCompactMainToolbarVerticalPadding = 8.0F;
    constexpr float kWaveToolsRailButtonWidth = 28.0F;
    constexpr float kWaveToolsRailButtonHeight = 30.0F;
    constexpr float kWaveToolsRailTopPadding = 8.0F;
    constexpr float kWaveToolsRailButtonGap = 8.0F;
    constexpr float kWaveToolsRailButtonRadius = 8.0F;

    float animateProgress(float current, float target, bool enabled, float deltaSec, float speed = 12.0F)
    {
        if (!enabled) {
            return target;
        }
        const float step = (std::max)(0.0F, deltaSec) * speed;
        if (current < target) {
            return (std::min)(target, current + step);
        }
        return (std::max)(target, current - step);
    }

    void advanceWavePanelAnimations(plot::WaveDockState& wave, float deltaSec)
    {
        const bool enabled = wave.view.interactionAnimationEnabled && wave.view.effectiveInteractionAnimationEnabled;
        wave.toolsDrawerProgress =
            animateProgress(wave.toolsDrawerProgress, wave.toolsCollapsed ? 0.0F : 1.0F, enabled, deltaSec);
        wave.overviewPanelProgress =
            animateProgress(wave.overviewPanelProgress, wave.overviewCollapsed ? 0.0F : 1.0F, enabled, deltaSec);
    }

    struct ToolsDrawerResizeState {
        ImVec2 min{};
        ImVec2 max{};
        bool hovered{false};
        bool active{false};
    };

    bool pointInRect(const ImVec2& point, const ImVec2& min, const ImVec2& max)
    {
        return point.x >= min.x && point.x <= max.x && point.y >= min.y && point.y <= max.y;
    }

    void captureWaveOverlayFrame(WaveOverlayFrame& overlayFrame,
                                 const PlotRenderResult& plotResult,
                                 const WaveFrameData& renderFrame)
    {
        overlayFrame.plotResult = plotResult;
        overlayFrame.snapshot = renderFrame.snapshot;
        overlayFrame.displayData = renderFrame.displayData;
        overlayFrame.hostViewport = ImGui::GetWindowViewport();
        overlayFrame.hasPlotResult = plotResult.plotRendered;
    }

    void drawWaveOverlayPass(plot::WaveDockState& wave, const WaveOverlayFrame& overlayFrame)
    {
        if (!overlayFrame.hasPlotResult || !overlayFrame.plotResult.plotRendered) {
            return;
        }

        const auto& result = overlayFrame.plotResult;
        auto* hostViewport =
            overlayFrame.hostViewport != nullptr ? overlayFrame.hostViewport : ImGui::GetMainViewport();
        if (result.legendOverlay.valid) {
            drawChannelLegendOverlay(wave,
                                     overlayFrame.snapshot,
                                     result.legendOverlay.pos,
                                     result.legendOverlay.size,
                                     hostViewport,
                                     WaveLegendOverlayLayerPolicy::ForceDisplayFront);
        }
    }

    void resetLegendOverlayTransientForFullscreenEntry(plot::WaveDockState& wave)
    {
        auto& overlay = wave.legendOverlay;
        overlay.hoverFloating = false;
        overlay.hoverInteractionLocked = false;
        overlay.hoverCloseRemainingSec = 0.0F;
        wave.legendVisibilityRestorePending = true;
    }

    bool hasSampleFrequencyTimebase(const plot::WaveViewState& view)
    {
        return view.sampleFrequencyHz > 0.0 && std::isfinite(view.sampleFrequencyHz);
    }

    std::optional<double> latestWaveViewTime(const plot::WaveDockState& wave)
    {
        if (!hasSampleFrequencyTimebase(wave.view)) {
            return wave.buffer.latestTime();
        }

        // 核心流程：采样频率时间轴以全局样本序号为准，避免跟随逻辑先落到脚本时间。
        const auto snapshot = wave.buffer.snapshot(
            -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), false);
        std::optional<std::size_t> latestSampleIndex;
        for (const auto& channel : snapshot.channels) {
            if (channel.totalSamples == 0) {
                continue;
            }
            const auto candidate = channel.sampleIndexOffset + channel.totalSamples - 1;
            if (!latestSampleIndex.has_value() || candidate > *latestSampleIndex) {
                latestSampleIndex = candidate;
            }
        }
        if (!latestSampleIndex.has_value()) {
            return std::nullopt;
        }
        return static_cast<double>(*latestSampleIndex) / wave.view.sampleFrequencyHz;
    }

    void clampWaveViewLowerBoundToZero(plot::WaveViewState& view)
    {
        if (view.viewMinTime >= 0.0) {
            return;
        }
        view.viewMinTime = 0.0;
        view.viewMaxTime = (std::max)(view.viewMaxTime, view.visibleDuration);
    }

    ImGuiKey toImGuiKey(const ShortcutKey key)
    {
        switch (key) {
            case ShortcutKey::A:
                return ImGuiKey_A;
            case ShortcutKey::C:
                return ImGuiKey_C;
            case ShortcutKey::F:
                return ImGuiKey_F;
            case ShortcutKey::Z:
                return ImGuiKey_Z;
            case ShortcutKey::Space:
                return ImGuiKey_Space;
            case ShortcutKey::F11:
                return ImGuiKey_F11;
            default:
                return ImGuiKey_None;
        }
    }

    bool chordPressed(const ShortcutChord& chord)
    {
        const auto& io = ImGui::GetIO();
        if (io.KeyCtrl != chord.ctrl || io.KeyShift != chord.shift || io.KeyAlt != chord.alt) {
            return false;
        }
        return ImGui::IsKeyPressed(toImGuiKey(chord.key), false);
    }

    bool waveShortcutPressed(const ShortcutAction action)
    {
        const auto* descriptor = findShortcut(action);
        return descriptor != nullptr && chordPressed(descriptor->chord);
    }

    bool drawTopToolbarButton(const char* label, bool active, const char* tooltip)
    {
        const float width = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2.0F;
        return drawToolbarSectionButton(label, tooltip, active, ImVec2(width, 0.0F));
    }

    [[nodiscard]] float compactMainToolbarHeight(bool reserveHorizontalScrollbar)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float buttonRowHeight = ImGui::GetTextLineHeight() + kCompactMainToolbarFramePaddingY * 2.0F;
        const float scrollbarReserve = reserveHorizontalScrollbar ? style.ScrollbarSize : 0.0F;
        return std::ceil(buttonRowHeight + scrollbarReserve + kCompactMainToolbarVerticalPadding +
                         style.ChildBorderSize * 2.0F);
    }

    const char* toolsDrawerTitle(const plot::WaveToolsDrawer drawer)
    {
        switch (drawer) {
            case plot::WaveToolsDrawer::Main:
                return "主视图控制";
            case plot::WaveToolsDrawer::Cursor:
                return "游标设置";
            case plot::WaveToolsDrawer::Measure:
                return "测量设置";
            case plot::WaveToolsDrawer::View:
                return "显示设置";
            case plot::WaveToolsDrawer::FFT:
                return "FFT设置";
            case plot::WaveToolsDrawer::Renderer:
                return "渲染设置";
            default:
                return "主视图控制";
        }
    }

    void openToolsDrawer(plot::WaveDockState& wave, plot::WaveToolsDrawer drawer)
    {
        if (!wave.toolsCollapsed && wave.activeToolsDrawer == drawer) {
            wave.toolsCollapsed = true;
            return;
        }
        wave.activeToolsDrawer = drawer;
        wave.toolsCollapsed = false;
    }

    [[nodiscard]] float centeredItemXInCurrentWindow(float itemWidth)
    {
        const ImGuiStyle& style = ImGui::GetStyle();

        const float windowWidth = ImGui::GetWindowSize().x;
        const float border = style.ChildBorderSize;

        const float innerWidth = (std::max)(0.0F, windowWidth - border * 2.0F);
        const float x = border + (std::max)(0.0F, (innerWidth - itemWidth) * 0.5F);

        return std::floor(x);
    }

    void setNextRailItemCentered(float itemWidth)
    {
        ImGui::SetCursorPosX(centeredItemXInCurrentWindow(itemWidth));
    }

    void addRailVerticalSpace(float height)
    {
        if (height > 0.0F) {
            ImGui::Dummy(ImVec2(1.0F, height));
        }
    }

    [[nodiscard]] ImVec2 centeredTextPos(const ImVec2& itemMin, const ImVec2& itemMax, const char* label)
    {
        const ImVec2 textSize = ImGui::CalcTextSize(label, nullptr, true);

        const ImVec2 itemSize(itemMax.x - itemMin.x, itemMax.y - itemMin.y);

        return ImVec2(std::floor(itemMin.x + (std::max)(0.0F, itemSize.x - textSize.x) * 0.5F),
                      std::floor(itemMin.y + (std::max)(0.0F, itemSize.y - textSize.y) * 0.5F));
    }

    bool drawRailToggleButton(const char* label, bool active, const char* tooltip, const ImVec2& size)
    {
        ImGui::InvisibleButton("##rail_toggle_button", size);

        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        const ImVec2 itemMin = ImGui::GetItemRectMin();
        const ImVec2 itemMax = ImGui::GetItemRectMax();

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        ImU32 bgColor = ImGui::GetColorU32(ImGuiCol_Button);
        ImU32 borderColor = ImGui::GetColorU32(ImGuiCol_Border);
        ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);

        if (active) {
            bgColor = ImGui::GetColorU32(ImGuiCol_ButtonActive);
            borderColor = ImGui::GetColorU32(ImGuiCol_HeaderActive);
        } else if (held) {
            bgColor = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        } else if (hovered) {
            bgColor = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
        }

        drawList->AddRectFilled(itemMin, itemMax, bgColor, kWaveToolsRailButtonRadius);

        drawList->AddRect(itemMin, itemMax, borderColor, kWaveToolsRailButtonRadius);

        const ImVec2 textPos = centeredTextPos(itemMin, itemMax, label);

        drawList->AddText(textPos, textColor, label);

        if (tooltip != nullptr && tooltip[0] != '\0' && hovered && ImGui::IsMouseHoveringRect(itemMin, itemMax) &&
            ImGui::GetIO().MouseDelta.x == ImGui::GetIO().MouseDelta.x) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
                ImGui::SetTooltip("%s", tooltip);
            }
        }

        return clicked;
    }

    bool drawToolsRailButton(plot::WaveDockState& wave,
                             plot::WaveToolsDrawer drawer,
                             const char* label,
                             const char* tooltip)
    {
        const bool active = !wave.toolsCollapsed && wave.activeToolsDrawer == drawer;

        constexpr ImVec2 buttonSize(kWaveToolsRailButtonWidth, kWaveToolsRailButtonHeight);

        setNextRailItemCentered(buttonSize.x);

        ImGui::PushID(static_cast<int>(drawer));

        const bool clicked = drawRailToggleButton(label, active, tooltip, buttonSize);

        ImGui::PopID();

        if (clicked) {
            openToolsDrawer(wave, drawer);
        }

        return clicked;
    }

    void drawToolsRail(plot::WaveDockState& wave)
    {
        const ImGuiStyle& style = ImGui::GetStyle();

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(style.ItemSpacing.x, kWaveToolsRailButtonGap));

        addRailVerticalSpace(kWaveToolsRailTopPadding);

        drawToolsRailButton(wave, plot::WaveToolsDrawer::Main, "...", "打开主视图设置：跟随、坐标轴和交互行为。");

        drawToolsRailButton(wave, plot::WaveToolsDrawer::FFT, "F", "打开 FFT 设置：频谱坐标、幅值模式和窗口参数。");

        drawToolsRailButton(wave, plot::WaveToolsDrawer::Renderer, "R", "打开渲染设置：降采样、余辉和性能显示。");

        drawToolsRailButton(wave, plot::WaveToolsDrawer::Cursor, "A", "打开游标设置：吸附、锁定和 A/B 游标定位。");

        drawToolsRailButton(
            wave, plot::WaveToolsDrawer::Measure, "dt", "打开测量设置：时间差、频率、峰峰值和误差参考。");

        drawToolsRailButton(
            wave, plot::WaveToolsDrawer::View, PROTOSCOPE_ICON_EXPAND, "打开显示设置：概览图、通道图例和展开策略。");

        ImGui::PopStyleVar();
    }

    ToolsDrawerResizeState drawToolsDrawerResizeHandle(
        plot::WaveDockState& wave, const ImVec2& drawerPos, float height, float contentLeft, float thickness)
    {
        const float safeThickness = (std::max)(thickness, 4.0F);
        const ImVec2 handlePos((std::max)(contentLeft, drawerPos.x - safeThickness), drawerPos.y);
        ImGui::SetCursorScreenPos(handlePos);
        ImGui::InvisibleButton("##wave_tools_drawer_splitter", ImVec2(safeThickness, height));
        ToolsDrawerResizeState state{
            .min = handlePos,
            .max = ImVec2(handlePos.x + safeThickness, handlePos.y + height),
            .hovered = ImGui::IsItemHovered(),
            .active = ImGui::IsItemActive(),
        };
        if (state.active) {
            wave.toolsExpandedWidth = (std::clamp)(wave.toolsExpandedWidth - ImGui::GetIO().MouseDelta.x,
                                                   wave.minToolsExpandedWidth,
                                                   wave.maxToolsExpandedWidth);
        }
        const ImU32 color = state.active || state.hovered ? ImGui::GetColorU32(ImGuiCol_SliderGrabActive)
                                                          : ImGui::GetColorU32(ImGuiCol_Border);
        ImGui::GetWindowDrawList()->AddRectFilled(
            handlePos, ImVec2(handlePos.x + safeThickness, handlePos.y + height), color, 2.0F);
        ImGui::SetItemTooltip("拖动调整右侧抽屉宽度");
        return state;
    }

    void drawTopToolbarSeparator()
    {
        ImGui::SameLine();
        ImGui::TextDisabled("|");
        ImGui::SameLine();
    }

    void zoomTimeAroundCenter(plot::WaveViewState& view, double factor)
    {
        const double span = (std::max)(view.viewMaxTime - view.viewMinTime, view.minVisibleTimeSpan);
        const double nextSpan = (std::max)(span * factor, view.minVisibleTimeSpan);
        const double center = 0.5 * (view.viewMinTime + view.viewMaxTime);
        auto target = currentViewport(view);
        target.minTime = center - nextSpan * 0.5;
        target.maxTime = center + nextSpan * 0.5;
        startViewportAnimation(view, target, WaveViewportAutoFollowPolicy::ExplicitCommand);
    }

    plot::WaveLegendOverlayOpenMode nextLegendOverlayOpenMode(plot::WaveLegendOverlayOpenMode mode)
    {
        switch (mode) {
            case plot::WaveLegendOverlayOpenMode::Hover:
                return plot::WaveLegendOverlayOpenMode::DoubleClick;
            case plot::WaveLegendOverlayOpenMode::DoubleClick:
                return plot::WaveLegendOverlayOpenMode::Disabled;
            case plot::WaveLegendOverlayOpenMode::Disabled:
                return plot::WaveLegendOverlayOpenMode::Hover;
        }
        return plot::WaveLegendOverlayOpenMode::Hover;
    }

    const char* legendOverlayOpenModeButtonLabel(plot::WaveLegendOverlayOpenMode mode)
    {
        switch (mode) {
            case plot::WaveLegendOverlayOpenMode::Hover:
                return "悬浮";
            case plot::WaveLegendOverlayOpenMode::DoubleClick:
                return "双击";
            case plot::WaveLegendOverlayOpenMode::Disabled:
                return "禁展";
        }
        return "悬浮";
    }

    const char* legendOverlayOpenModeTooltipLabel(plot::WaveLegendOverlayOpenMode mode)
    {
        switch (mode) {
            case plot::WaveLegendOverlayOpenMode::Hover:
                return "悬浮展开";
            case plot::WaveLegendOverlayOpenMode::DoubleClick:
                return "双击展开";
            case plot::WaveLegendOverlayOpenMode::Disabled:
                return "禁用展开";
        }
        return "悬浮展开";
    }

    void drawCompactAcquisitionControls(plot::WaveDockState& wave, WaveFrameState& frame)
    {
        const bool currentRunning = wave.oscilloscopeRunning;
        const bool targetRunning = !currentRunning;
        if (drawTopToolbarButton(currentRunning ? PROTOSCOPE_ICON_PAUSE : PROTOSCOPE_ICON_PLAY,
                                 currentRunning,
                                 currentRunning ? "请求协议脚本暂停示波器采集；脚本确认后状态才会改变。"
                                                : "请求协议脚本启动示波器采集；脚本确认后开始写入波形。")) {
            // 核心流程：延迟 Lua 启停回调，避免同一帧清空 buffer 后继续使用旧波形快照。
            deferOscilloscopeToggle(frame, currentRunning, targetRunning);
        }
    }

    void drawCompactViewModeControls(plot::WaveViewState& view)
    {
        drawTopToolbarSeparator();
        if (drawTopToolbarButton("叠加", view.viewMode == plot::WaveViewMode::Overlay, "多通道共用同一个波形区域。")) {
            view.viewMode = plot::WaveViewMode::Overlay;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(
                "堆叠", view.viewMode == plot::WaveViewMode::Stacked, "按通道纵向错开显示，不修改原始采样。")) {
            view.viewMode = plot::WaveViewMode::Stacked;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(
                "分屏", view.viewMode == plot::WaveViewMode::Split, "每个可见通道使用独立子图，共享时间轴。")) {
            view.viewMode = plot::WaveViewMode::Split;
        }
    }

    void drawCompactTimeControls(plot::WaveViewState& view,
                                 const plot::ViewConfig& config,
                                 const plot::WaveDisplayData& displayData)
    {
        static_cast<void>(config);
        static_cast<void>(displayData);
        drawTopToolbarSeparator();
        if (drawTopToolbarButton("-", false, "缩小时间轴视图，显示更长时间范围。")) {
            zoomTimeAroundCenter(view, 1.25);
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("+", false, "放大时间轴视图，查看更短时间范围内的细节。")) {
            zoomTimeAroundCenter(view, 0.80);
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("适配", false, "把当前可见通道的全部波形放入主视图。")) {
            view.fitVisibleWaveformsRequested = true;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(view.zoomSelectionActive ? "框选" : "平移",
                                 view.zoomSelectionActive,
                                 view.zoomSelectionActive ? "当前为框选放大；点击后回到拖拽平移。"
                                                          : "当前为拖拽平移；点击后用鼠标框选局部放大。")) {
            view.zoomSelectionActive = !view.zoomSelectionActive;
            view.zoomSelectionDragging = false;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(view.autoFollowLatest ? "跟随" : "停跟",
                                 view.autoFollowLatest,
                                 view.autoFollowLatest ? "当前自动跟随最新数据；点击后停在手动浏览位置。"
                                                       : "当前不跟随最新数据；点击后视图回到最新数据末尾。")) {
            view.autoFollowLatest = !view.autoFollowLatest;
        }
    }

    void drawCompactCursorControls(plot::WaveViewState& view,
                                   const plot::ViewConfig& config,
                                   const plot::WaveDisplayData& displayData)
    {
        drawTopToolbarSeparator();
        if (drawTopToolbarButton(
                "A",
                view.cursors[0].enabled,
                view.cursors[0].enabled ? "A 游标已显示；点击后隐藏 A 游标。" : "A 游标已隐藏；点击后显示 A 游标。")) {
            view.cursors[0].enabled = !view.cursors[0].enabled;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(
                "B",
                view.cursors[1].enabled,
                view.cursors[1].enabled ? "B 游标已显示；点击后隐藏 B 游标。" : "B 游标已隐藏；点击后显示 B 游标。")) {
            view.cursors[1].enabled = !view.cursors[1].enabled;
        }
        ImGui::SameLine();
        ImGui::PushID("cursor_intersection_readouts");
        if (drawTopToolbarButton("交点",
                                 view.showCursorIntersectionReadouts,
                                 view.showCursorIntersectionReadouts
                                     ? "当前显示 A/B 游标与各可见曲线的交点读数；点击后隐藏。"
                                     : "当前隐藏游标交点读数；点击后显示每个可见通道的采样读数。")) {
            view.showCursorIntersectionReadouts = !view.showCursorIntersectionReadouts;
        }
        ImGui::PopID();
        ImGui::SameLine();
        if (drawTopToolbarButton(view.cursorIntervalLocked ? "锁定" : "间隔",
                                 view.cursorIntervalLocked,
                                 view.cursorIntervalLocked
                                     ? "A/B 游标间隔已锁定；点击后解除联动。"
                                     : "点击后锁定当前 A/B 间隔，拖动单个游标时另一个同步移动。")) {
            view.cursorIntervalLocked = !view.cursorIntervalLocked;
            view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("同步", false, "把 A/B 游标重新放到当前可见时间范围内。")) {
            placeCursorPairInViewport(view, config, displayData);
        }
    }

    void drawCompactMeasurementControls(plot::WaveDockState& wave)
    {
        auto& view = wave.view;
        drawTopToolbarSeparator();
        if (drawTopToolbarButton(
                "dt",
                view.measurement.deltaTime,
                view.measurement.deltaTime ? "时间差测量已显示；点击后隐藏 dt。" : "显示 A/B 游标之间的时间差 dt。")) {
            view.measurement.deltaTime = !view.measurement.deltaTime;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(
                "Hz",
                view.measurement.frequency,
                view.measurement.frequency ? "等效频率已显示；点击后隐藏 Hz。" : "根据 A/B 时间差显示等效频率 Hz。")) {
            view.measurement.frequency = !view.measurement.frequency;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("峰峰",
                                 view.measurement.peakToPeak,
                                 view.measurement.peakToPeak ? "峰峰值测量已显示；点击后隐藏。"
                                                             : "显示当前测量通道在 A/B 范围内的峰峰值。")) {
            view.measurement.peakToPeak = !view.measurement.peakToPeak;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(
                "均值",
                view.measurement.mean,
                view.measurement.mean ? "均值测量已显示；点击后隐藏。" : "显示当前测量通道在 A/B 范围内的均值。")) {
            view.measurement.mean = !view.measurement.mean;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("更多", !wave.toolsCollapsed, "打开右侧高级工具抽屉。")) {
            wave.activeToolsDrawer = plot::WaveToolsDrawer::Main;
            wave.toolsCollapsed = false;
        }
    }

    void drawCompactOverlayControls(app::Application& application,
                                    plot::WaveDockState& wave,
                                    bool fullscreenActive,
                                    bool* fullscreenToggleRequested)
    {
        auto& view = wave.view;
        drawTopToolbarSeparator();
        if (drawTopToolbarButton("概览",
                                 !wave.overviewCollapsed,
                                 wave.overviewCollapsed ? "概览图已折叠；点击后显示全局时间轴概览。"
                                                        : "概览图已显示；点击后折叠概览区域。")) {
            wave.overviewCollapsed = !wave.overviewCollapsed;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(
                "图例",
                view.showChannelLegend,
                view.showChannelLegend ? "图内通道图例已显示；点击后隐藏。" : "图内通道图例已隐藏；点击后显示。")) {
            view.showChannelLegend = !view.showChannelLegend;
        }
        ImGui::SameLine();
        const auto nextLegendMode = nextLegendOverlayOpenMode(wave.legendOverlay.openMode);
        const std::string legendModeTooltip =
            std::string("图内图例展开方式：当前 ") + legendOverlayOpenModeTooltipLabel(wave.legendOverlay.openMode) +
            "，单击切换到 " + legendOverlayOpenModeTooltipLabel(nextLegendMode) + "。";
        if (drawTopToolbarButton(legendOverlayOpenModeButtonLabel(wave.legendOverlay.openMode),
                                 wave.legendOverlay.openMode != plot::WaveLegendOverlayOpenMode::Disabled,
                                 legendModeTooltip.c_str())) {
            wave.legendOverlay.openMode = nextLegendMode;
            if (wave.legendOverlay.openMode != plot::WaveLegendOverlayOpenMode::DoubleClick) {
                wave.legendOverlay.expanded = false;
            }
            wave.legendOverlay.hoverFloating = false;
            wave.legendOverlay.hoverInteractionLocked = false;
            wave.legendOverlay.hoverCloseRemainingSec = 0.0F;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("恢复", false, "恢复所有通道的颜色、名称、比例和隐藏状态；不清空波形数据。")) {
            if (plot::resetAllChannelViewSettings(wave)) {
                invalidateWaveDisplayCaches(wave);
            }
        }
        ImGui::SameLine();
        if (fullscreenToggleRequested != nullptr &&
            drawTopToolbarButton(
                fullscreenActive ? "退全" : "全屏",
                fullscreenActive,
                fullscreenActive ? "退出波形全屏，恢复普通 Dock 布局。" : "进入波形全屏，只聚焦当前波形视图。")) {
            if (!fullscreenActive) {
                resetLegendOverlayTransientForFullscreenEntry(wave);
            }
            *fullscreenToggleRequested = true;
            application.setTransientStatusMessage(fullscreenActive ? "已请求退出波形全屏" : "已请求进入波形全屏");
        }
    }

    void drawCompactMainToolbar(app::Application& application,
                                plot::WaveDockState& wave,
                                const plot::ViewConfig& config,
                                const plot::WaveDisplayData& displayData,
                                WaveFrameState& frame,
                                bool fullscreenActive,
                                bool* fullscreenToggleRequested)
    {
        // todo)) Flow Layout
        auto& view = wave.view;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.059F, 0.086F, 0.125F, 1.0F));

        const ImGuiStyle& style = ImGui::GetStyle();
        const bool reserveHorizontalScrollbar = wave.mainToolbarNeedsHorizontalScroll;
        const float toolbarHeight = compactMainToolbarHeight(reserveHorizontalScrollbar);
        const float availableWidth = ImGui::GetContentRegionAvail().x;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(style.WindowPadding.x, 0.0F));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, kCompactMainToolbarFramePaddingY));
        if (wave.mainToolbarContentWidth > 0.0F) {
            ImGui::SetNextWindowContentSize(ImVec2(wave.mainToolbarContentWidth, 0.0F));
        }

        ImGui::BeginChild("##wave_main_toolbar",
                          ImVec2(0.0F, toolbarHeight),
                          true,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const float rowHeight = ImGui::GetFrameHeight();
        const float scrollbarReserve = reserveHorizontalScrollbar ? style.ScrollbarSize : 0.0F;
        const float buttonAreaHeight =
            (std::max)(rowHeight, toolbarHeight - scrollbarReserve - style.ChildBorderSize * 2.0F);
        const float offsetY =
            style.ChildBorderSize + (std::max)(0.0F, std::floor((buttonAreaHeight - rowHeight) * 0.5F));
        ImGui::SetCursorPosY(offsetY);
        ImGui::AlignTextToFramePadding();

        ImGui::BeginGroup();
        drawCompactAcquisitionControls(wave, frame);
        drawCompactViewModeControls(view);
        drawCompactTimeControls(view, config, displayData);
        drawCompactCursorControls(view, config, displayData);
        drawCompactMeasurementControls(wave);
        drawCompactOverlayControls(application, wave, fullscreenActive, fullscreenToggleRequested);
        ImGui::EndGroup();

        const ImVec2 groupMin = ImGui::GetItemRectMin();
        const ImVec2 groupMax = ImGui::GetItemRectMax();
        const float groupWidth = (std::max)(0.0F, groupMax.x - groupMin.x);
        wave.mainToolbarContentWidth = std::ceil(groupWidth);
        wave.mainToolbarNeedsHorizontalScroll =
            groupWidth + style.WindowPadding.x * 2.0F > availableWidth - style.ChildBorderSize * 2.0F + 0.5F;

        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

} // namespace

std::string formatMetricText(double value, const char* baseUnit)
{
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

bool plotInteractionActive(bool toolHeld)
{
    const auto& io = ImGui::GetIO();
    const bool mouseAction =
        ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Right) || ImPlot::IsPlotSelected() || io.MouseWheel != 0.0F;
    const bool interactionAreaHovered =
        ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_X1) || ImPlot::IsAxisHovered(ImAxis_Y1);
    return toolHeld || (mouseAction && interactionAreaHovered);
}

bool drawHorizontalSplitter(
    const char* id, float& topHeight, float minTopHeight, float minBottomHeight, float totalHeight, float thickness)
{
    const float safeThickness = (std::max)(thickness, 4.0F);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Separator));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorActive));
    ImGui::Button(id, ImVec2(-1.0F, safeThickness));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive()) {
        topHeight += ImGui::GetIO().MouseDelta.y;
        topHeight = (std::clamp)(
            topHeight, minTopHeight, (std::max)(minTopHeight, totalHeight - minBottomHeight - safeThickness));
        return true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    return false;
}

void recordMainPlotLimits(plot::WaveViewState& view, const ImPlotRect& limits)
{
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    const auto oldViewport = currentViewport(view);
    view.viewMinTime = limits.X.Min;
    view.viewMaxTime = limits.X.Max;
    view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, minVisibleTimeSpan);
    view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    if (!view.lockVerticalRange) {
        view.viewMinValue = limits.Y.Min;
        view.viewMaxValue = limits.Y.Max;
    }
    plot::shiftMeasurementCursorsForViewportScroll(view, oldViewport, currentViewport(view));
}

bool syncAutoFitAxisLimits(plot::WaveViewState& view, const ImPlotRect& limits)
{
    constexpr double kLimitEpsilon = 1e-9;
    const bool xChanged = std::abs(limits.X.Min - view.viewMinTime) > kLimitEpsilon ||
                          std::abs(limits.X.Max - view.viewMaxTime) > kLimitEpsilon;
    const bool yChanged = !view.lockVerticalRange && (std::abs(limits.Y.Min - view.viewMinValue) > kLimitEpsilon ||
                                                      std::abs(limits.Y.Max - view.viewMaxValue) > kLimitEpsilon);
    if (!xChanged && !yChanged) {
        return false;
    }

    // 核心流程：ImPlot 双击坐标轴会直接改当前帧轴限，这里回写视口状态，避免下一帧被旧的 viewMin/viewMax 覆盖。
    recordMainPlotLimits(view, limits);
    if (xChanged) {
        applyAutoFollowPausePolicy(view, WaveViewportAutoFollowPolicy::UserInteraction);
    }
    return true;
}

bool isFitDoubleClicked()
{
    const int fitButton = ImPlot::GetInputMap().Fit;
    const bool fitButtonDoubleClicked =
        fitButton >= 0 && fitButton < ImGuiMouseButton_COUNT && ImGui::IsMouseDoubleClicked(fitButton);
    return ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) || fitButtonDoubleClicked;
}

bool selectedChannelUsesBitDisplay(const plot::WaveViewState& view, const plot::WaveSnapshot& snapshot)
{
    return view.measurementChannelIndex < snapshot.channels.size() &&
           bitDisplayEnabled(snapshot.channels[view.measurementChannelIndex].bitDisplay);
}

std::vector<std::size_t> selectableAnalogWaveformChannels(const plot::WaveSnapshot& snapshot,
                                                          const plot::WaveDisplayData& displayData,
                                                          const std::vector<std::size_t>& visibleChannelIndices)
{
    std::vector<std::size_t> channels;
    channels.reserve(visibleChannelIndices.size());
    for (const std::size_t channelIndex : visibleChannelIndices) {
        if (channelIndex >= snapshot.channels.size() || channelIndex >= displayData.channels.size()) {
            continue;
        }
        if (bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay)) {
            continue;
        }
        channels.push_back(channelIndex);
    }
    return channels;
}

std::vector<std::size_t> yAxisSingleSideScaleTargets(const plot::WaveDockState& wave,
                                                     const plot::WaveSnapshot& snapshot,
                                                     const std::vector<std::size_t>& visibleChannelIndices)
{
    const auto analogVisible = [&snapshot, &visibleChannelIndices](std::size_t channelIndex) {
        return std::find(visibleChannelIndices.begin(), visibleChannelIndices.end(), channelIndex) !=
                   visibleChannelIndices.end() &&
               channelIndex < snapshot.channels.size() &&
               !bitDisplayEnabled(snapshot.channels[channelIndex].bitDisplay);
    };

    if (wave.view.yAxisDoubleClickAction == plot::WaveYAxisDoubleClickAction::FitActiveChannel &&
        analogVisible(wave.view.measurementChannelIndex)) {
        return {wave.view.measurementChannelIndex};
    }

    std::vector<std::size_t> targets;
    targets.reserve(visibleChannelIndices.size());
    for (const std::size_t channelIndex : visibleChannelIndices) {
        if (analogVisible(channelIndex)) {
            targets.push_back(channelIndex);
        }
    }
    return targets;
}

struct VisibleActualRange {
    double min{0.0};
    double max{0.0};
    double span{0.0};
    double center{0.0};
};

std::optional<VisibleActualRange> visibleActualRangeForChannel(const plot::ChannelView& channel,
                                                               const plot::ChannelSpec& spec)
{
    if (bitDisplayEnabled(channel.bitDisplay) || channel.samples == nullptr) {
        return std::nullopt;
    }

    std::size_t begin = (std::min)(channel.visibleBegin, channel.totalSamples);
    std::size_t end = (std::min)(channel.visibleEnd, channel.totalSamples);
    if (end < begin) {
        end = begin;
    }

    double actualMin = std::numeric_limits<double>::infinity();
    double actualMax = -std::numeric_limits<double>::infinity();
    for (std::size_t sampleIndex = begin; sampleIndex < end; ++sampleIndex) {
        const double actual = channel.samples[sampleIndex].value * spec.ratio;
        if (!std::isfinite(actual)) {
            continue;
        }
        actualMin = (std::min)(actualMin, actual);
        actualMax = (std::max)(actualMax, actual);
    }
    if (!std::isfinite(actualMin) || !std::isfinite(actualMax)) {
        return std::nullopt;
    }
    const double actualSpan = actualMax - actualMin;
    if (!std::isfinite(actualSpan) || actualSpan <= 1e-12) {
        return std::nullopt;
    }
    return VisibleActualRange{
        .min = actualMin,
        .max = actualMax,
        .span = actualSpan,
        .center = (actualMin + actualMax) * 0.5,
    };
}

bool applyYAxisSingleSideScaleToChannels(plot::WaveDockState& wave,
                                         const plot::WaveSnapshot& snapshot,
                                         const std::vector<std::size_t>& visibleChannelIndices)
{
    const double viewMin = (std::min)(wave.view.viewMinValue, wave.view.viewMaxValue);
    const double viewMax = (std::max)(wave.view.viewMinValue, wave.view.viewMaxValue);
    const double yHeight = viewMax - viewMin;
    if (wave.view.lockVerticalRange || !std::isfinite(yHeight) || yHeight <= 1e-12) {
        return false;
    }
    const double safeMultiplier =
        wave.view.verticalAutoFitMultiplier > 0.0 && std::isfinite(wave.view.verticalAutoFitMultiplier)
            ? wave.view.verticalAutoFitMultiplier
            : 1.25;
    const double targetHeight = yHeight / safeMultiplier;
    if (!std::isfinite(targetHeight) || targetHeight <= 1e-12) {
        return false;
    }
    const double viewCenter = (viewMin + viewMax) * 0.5;

    bool changed = false;
    const auto targets = yAxisSingleSideScaleTargets(wave, snapshot, visibleChannelIndices);
    for (const std::size_t channelIndex : targets) {
        if (channelIndex >= snapshot.channels.size()) {
            continue;
        }
        const auto& channel = snapshot.channels[channelIndex];
        if (bitDisplayEnabled(channel.bitDisplay) || channel.samples == nullptr) {
            continue;
        }
        const auto currentSpec = wave.buffer.channelSpec(channelIndex);
        if (!currentSpec.has_value()) {
            continue;
        }

        const auto actual = visibleActualRangeForChannel(channel, *currentSpec);
        if (!actual.has_value()) {
            continue;
        }

        auto updated = *currentSpec;
        const double sign = updated.scale < 0.0 ? -1.0 : 1.0;
        const double targetMagnitude = targetHeight / actual->span;
        if (!std::isfinite(targetMagnitude) || targetMagnitude <= 1e-12) {
            continue;
        }
        updated.scale = sign * targetMagnitude;

        if (wave.view.yAxisDoubleClickAdjustOffset) {
            // 显式开启兼容模式时同步反推 offset，让实际值区间落在当前 Y 视口内部目标区间。
            if (wave.view.displayFormula == plot::WaveDisplayFormula::ScaleThenOffset) {
                updated.offset = viewCenter - actual->center * updated.scale;
            } else {
                updated.offset = viewCenter / updated.scale - actual->center;
            }
        }
        applyChannelTransformOverride(
            wave, channelIndex, updated, channelDefaultSpec(wave, channelIndex, *currentSpec));
        changed = true;
    }
    return changed;
}

bool handleMainPlotAxisDoubleClick(plot::WaveDockState& wave,
                                   const plot::WaveSnapshot& snapshot,
                                   const plot::WaveDataBounds& visibleWindowBounds,
                                   const plot::WaveDataBounds& fullHistoryBounds,
                                   const std::vector<std::size_t>& visibleChannelIndices)
{
    if (!isFitDoubleClicked()) {
        return false;
    }

    auto& view = wave.view;
    bool changed = false;
    if (ImPlot::IsAxisHovered(ImAxis_X1)) {
        const auto& xBounds =
            plot::selectXAxisDoubleClickBounds(view.xAxisDoubleClickAction, visibleWindowBounds, fullHistoryBounds);
        if (!xBounds.valid) {
            return false;
        }
        auto target = currentViewport(view);
        target.minTime = xBounds.minTime;
        target.maxTime = xBounds.maxTime;
        // 核心流程：横轴双击要同步应用层视口；否则 autoFollow/Once 条件会在下一帧把 ImPlot autofit 覆盖回旧范围。
        startViewportAnimation(view, target, WaveViewportAutoFollowPolicy::UserInteraction);
        changed = true;
    }
    if (ImPlot::IsAxisHovered(ImAxis_Y1) && selectedChannelUsesBitDisplay(view, snapshot)) {
        // Bit lanes are laid out in plot pixels. Consume the fit gesture so ImPlot does not persist a normal Y fit.
        changed = true;
    } else if (ImPlot::IsAxisHovered(ImAxis_Y1)) {
        // 核心流程：Y 轴双击保持当前视口不变，通过通道 scale/offset 把目标模拟波形拟合进视口。
        changed = applyYAxisSingleSideScaleToChannels(wave, snapshot, visibleChannelIndices) || changed;
    }

    if (changed) {
        view.forceNextMainPlotLimits = true;
    }
    return changed;
}

void cancelZoomSelection(plot::WaveViewState& view)
{
    view.zoomSelectionActive = false;
    view.zoomSelectionDragging = false;
}

bool applyFullViewport(plot::WaveViewState& view,
                       double minTime,
                       double maxTime,
                       double minValue,
                       double maxValue,
                       WaveViewportAutoFollowPolicy policy);

enum class ZoomSelectionAxisMode {
    XOnly,
    YOnly,
    XY,
};

ZoomSelectionAxisMode resolveZoomSelectionAxisMode(float pixelWidth, float pixelHeight)
{
    constexpr float kDirectionalBias = 2.0F;
    if (pixelWidth >= pixelHeight * kDirectionalBias) {
        return ZoomSelectionAxisMode::XOnly;
    }
    if (pixelHeight >= pixelWidth * kDirectionalBias) {
        return ZoomSelectionAxisMode::YOnly;
    }
    return ZoomSelectionAxisMode::XY;
}

void drawZoomSelectionPreview(ImDrawList& drawList,
                              ZoomSelectionAxisMode mode,
                              const ImVec2& rectMin,
                              const ImVec2& rectMax,
                              const ImVec2& selectionStart,
                              const ImVec2& selectionCurrent)
{
    const ImU32 fillColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.2F, 0.55F, 1.0F, 0.16F));
    const ImU32 lineColor = ImGui::ColorConvertFloat4ToU32(ImVec4(0.45F, 0.75F, 1.0F, 0.95F));
    switch (mode) {
        case ZoomSelectionAxisMode::XOnly: {
            const float lineY = 0.5F * (selectionStart.y + selectionCurrent.y);
            // 核心流程：水平拖动只展示一条水平线，明确这次框选只会缩放 X 轴范围。
            drawList.AddLine(ImVec2(rectMin.x, lineY), ImVec2(rectMax.x, lineY), lineColor, 2.0F);
            break;
        }
        case ZoomSelectionAxisMode::YOnly: {
            const float lineX = 0.5F * (selectionStart.x + selectionCurrent.x);
            // 核心流程：垂直拖动只展示一条竖直线，未选中的 X 轴视口不会被改写。
            drawList.AddLine(ImVec2(lineX, rectMin.y), ImVec2(lineX, rectMax.y), lineColor, 2.0F);
            break;
        }
        case ZoomSelectionAxisMode::XY:
            drawList.AddRectFilled(rectMin, rectMax, fillColor);
            drawList.AddRect(rectMin, rectMax, lineColor, 0.0F, 0, 2.0F);
            break;
    }
}

bool applyZoomSelectionViewport(plot::WaveViewState& view,
                                ZoomSelectionAxisMode mode,
                                const ImPlotPoint& plotStart,
                                const ImPlotPoint& plotEnd)
{
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    const double minTime = (std::min)(plotStart.x, plotEnd.x);
    const double maxTime = (std::max)(plotStart.x, plotEnd.x);
    const double minValue = (std::min)(plotStart.y, plotEnd.y);
    const double maxValue = (std::max)(plotStart.y, plotEnd.y);

    switch (mode) {
        case ZoomSelectionAxisMode::XOnly:
            if (!std::isfinite(minTime) || !std::isfinite(maxTime) || maxTime - minTime < minVisibleTimeSpan) {
                return false;
            }
            // 核心流程：水平框选只写回时间轴视口，垂直轴保持当前范围不变，避免轻微竖向抖动影响缩放结果。
            view.viewMinTime = minTime;
            view.viewMaxTime = maxTime;
            view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, minVisibleTimeSpan);
            view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
            applyAutoFollowPausePolicy(view, WaveViewportAutoFollowPolicy::UserInteraction);
            view.forceNextMainPlotLimits = true;
            return true;
        case ZoomSelectionAxisMode::YOnly:
            if (!std::isfinite(minValue) || !std::isfinite(maxValue) || maxValue <= minValue) {
                return false;
            }
            // 核心流程：垂直框选只写回电压/幅值轴视口，时间轴保持当前范围不变。
            view.viewMinValue = minValue;
            view.viewMaxValue = maxValue;
            if (view.lockVerticalRange) {
                view.manualVerticalMin = minValue;
                view.manualVerticalMax = maxValue;
            }
            applyAutoFollowPausePolicy(view, WaveViewportAutoFollowPolicy::UserInteraction);
            view.forceNextMainPlotLimits = true;
            return true;
        case ZoomSelectionAxisMode::XY:
            // 核心流程：斜向框选保留原有 XY 矩形缩放语义，两个轴一起回写到视口。
            return applyFullViewport(
                view, minTime, maxTime, minValue, maxValue, WaveViewportAutoFollowPolicy::UserInteraction);
    }
    return false;
}

void applyAutoFollowPausePolicy(plot::WaveViewState& view, WaveViewportAutoFollowPolicy policy)
{
    switch (policy) {
        case WaveViewportAutoFollowPolicy::Preserve:
            return;
        case WaveViewportAutoFollowPolicy::UserInteraction:
            if (view.pauseAutoFollowOnInteraction) {
                view.autoFollowLatest = false;
            }
            return;
        case WaveViewportAutoFollowPolicy::OverviewDrag:
            // 概览框拖动代表用户明确选择历史窗口，始终停留在放下位置。
            view.autoFollowLatest = false;
            return;
        case WaveViewportAutoFollowPolicy::ExplicitCommand:
            view.autoFollowLatest = false;
            return;
    }
}

bool applyFullViewport(plot::WaveViewState& view,
                       double minTime,
                       double maxTime,
                       double minValue,
                       double maxValue,
                       WaveViewportAutoFollowPolicy policy)
{
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    if (!std::isfinite(minTime) || !std::isfinite(maxTime) || maxTime - minTime < minVisibleTimeSpan) {
        return false;
    }
    if (!std::isfinite(minValue) || !std::isfinite(maxValue) || maxValue <= minValue) {
        return false;
    }

    view.viewMinTime = minTime;
    view.viewMaxTime = maxTime;
    view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, minVisibleTimeSpan);
    view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    view.viewMinValue = minValue;
    view.viewMaxValue = maxValue;
    if (view.lockVerticalRange) {
        view.manualVerticalMin = minValue;
        view.manualVerticalMax = maxValue;
    }
    view.forceNextMainPlotLimits = true;
    applyAutoFollowPausePolicy(view, policy);
    return true;
}

bool startViewportAnimation(plot::WaveViewState& view,
                            const plot::WaveViewport& target,
                            WaveViewportAutoFollowPolicy policy,
                            double durationSec)
{
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    if (!std::isfinite(target.minTime) || !std::isfinite(target.maxTime) ||
        target.maxTime - target.minTime < minVisibleTimeSpan || !std::isfinite(target.minValue) ||
        !std::isfinite(target.maxValue) || target.maxValue <= target.minValue) {
        return false;
    }

    if (!(view.interactionAnimationEnabled && view.effectiveInteractionAnimationEnabled) || durationSec <= 0.0) {
        view.viewportAnimation.active = false;
        return applyFullViewport(view, target.minTime, target.maxTime, target.minValue, target.maxValue, policy);
    }

    applyAutoFollowPausePolicy(view, policy);
    view.viewportAnimation.active = true;
    view.viewportAnimation.start = currentViewport(view);
    view.viewportAnimation.target = target;
    view.viewportAnimation.elapsedSec = 0.0;
    view.viewportAnimation.durationSec = (std::max)(durationSec, 1e-6);
    view.forceNextMainPlotLimits = true;
    return true;
}

bool advanceViewportAnimation(plot::WaveViewState& view, double deltaSec)
{
    auto& animation = view.viewportAnimation;
    if (!animation.active) {
        return false;
    }

    const auto oldViewport = currentViewport(view);
    animation.elapsedSec = (std::min)(animation.elapsedSec + (std::max)(deltaSec, 0.0), animation.durationSec);
    const double linear = (std::clamp)(animation.elapsedSec / animation.durationSec, 0.0, 1.0);
    const double progress = 1.0 - std::pow(1.0 - linear, 3.0);
    const auto lerp = [progress](double from, double to) { return from + (to - from) * progress; };

    view.viewMinTime = lerp(animation.start.minTime, animation.target.minTime);
    view.viewMaxTime = lerp(animation.start.maxTime, animation.target.maxTime);
    view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, (std::max)(view.minVisibleTimeSpan, 1e-6));
    view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    if (!view.lockVerticalRange) {
        view.viewMinValue = lerp(animation.start.minValue, animation.target.minValue);
        view.viewMaxValue = lerp(animation.start.maxValue, animation.target.maxValue);
    }

    if (linear >= 1.0) {
        view.viewMinTime = animation.target.minTime;
        view.viewMaxTime = animation.target.maxTime;
        view.visibleDuration =
            (std::max)(view.viewMaxTime - view.viewMinTime, (std::max)(view.minVisibleTimeSpan, 1e-6));
        view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
        if (!view.lockVerticalRange) {
            view.viewMinValue = animation.target.minValue;
            view.viewMaxValue = animation.target.maxValue;
        }
        animation.active = false;
    }

    plot::shiftMeasurementCursorsForViewportScroll(view, oldViewport, currentViewport(view));
    view.forceNextMainPlotLimits = true;
    return true;
}

namespace {

    struct ExactWaveTimeBounds {
        bool valid{false};
        double minTime{std::numeric_limits<double>::infinity()};
        double maxTime{-std::numeric_limits<double>::infinity()};
    };

    std::optional<double> exactWaveSampleTime(const plot::ChannelView& channel,
                                              std::size_t sampleIndex,
                                              plot::WaveTimeAxisSource axisSource,
                                              double sampleFrequencyHz)
    {
        const std::size_t globalSampleIndex = channel.sampleIndexOffset + sampleIndex;
        if (axisSource == plot::WaveTimeAxisSource::SampleFrequency) {
            if (sampleFrequencyHz <= 0.0 || !std::isfinite(sampleFrequencyHz)) {
                return std::nullopt;
            }
            return static_cast<double>(globalSampleIndex) / sampleFrequencyHz;
        }
        if (axisSource == plot::WaveTimeAxisSource::SampleIndex) {
            return static_cast<double>(globalSampleIndex);
        }
        if (channel.samples == nullptr || sampleIndex >= channel.totalSamples) {
            return std::nullopt;
        }
        const double time = channel.samples[sampleIndex].time;
        if (!std::isfinite(time)) {
            return std::nullopt;
        }
        return time;
    }

    ExactWaveTimeBounds exactVisibleWaveTimeBounds(const plot::WaveSnapshot& snapshot,
                                                   plot::WaveTimeAxisSource axisSource,
                                                   double sampleFrequencyHz,
                                                   const std::vector<std::size_t>& channelIndices)
    {
        ExactWaveTimeBounds bounds;
        for (const std::size_t channelIndex : channelIndices) {
            if (channelIndex >= snapshot.channels.size()) {
                continue;
            }
            const auto& channel = snapshot.channels[channelIndex];
            if (channel.totalSamples == 0) {
                continue;
            }
            const std::size_t begin = (std::min)(channel.visibleBegin, channel.totalSamples);
            std::size_t end = (std::min)(channel.visibleEnd, channel.totalSamples);
            if (end < begin) {
                end = begin;
            }
            if (begin >= end) {
                continue;
            }
            const auto firstTime = exactWaveSampleTime(channel, begin, axisSource, sampleFrequencyHz);
            const auto lastTime = exactWaveSampleTime(channel, end - 1U, axisSource, sampleFrequencyHz);
            if (!firstTime.has_value() || !lastTime.has_value()) {
                continue;
            }
            bounds.minTime = (std::min)(bounds.minTime, (std::min)(*firstTime, *lastTime));
            bounds.maxTime = (std::max)(bounds.maxTime, (std::max)(*firstTime, *lastTime));
            bounds.valid = true;
        }
        return bounds;
    }

} // namespace

bool applyFitVisibleWaveforms(plot::WaveViewState& view,
                              const plot::WaveSnapshot& fullSnapshot,
                              const plot::WaveDisplayData& displayData,
                              const std::vector<std::size_t>& visibleChannelIndices)
{
    if (!view.fitVisibleWaveformsRequested) {
        return false;
    }
    view.fitVisibleWaveformsRequested = false;

    // 核心流程：X 轴取完整历史的真实首尾样本；Y 轴复用显示包络，避免概览桶平均时间把边界内缩。
    const auto timeBounds =
        exactVisibleWaveTimeBounds(fullSnapshot, displayData.axisSource, view.sampleFrequencyHz, visibleChannelIndices);
    auto bounds = boundsForVisibleWaveforms(view, fullSnapshot, displayData, visibleChannelIndices);
    if (!bounds.valid) {
        return false;
    }
    if (!timeBounds.valid) {
        return false;
    }
    bounds.minTime = timeBounds.minTime;
    bounds.maxTime = timeBounds.maxTime;

    const auto yRange =
        plot::makeVerticalAutoFitRange(bounds.minValue, bounds.maxValue, view.verticalAutoFitMultiplier);
    return startViewportAnimation(view,
                                  plot::WaveViewport{
                                      .minTime = bounds.minTime,
                                      .maxTime = bounds.maxTime,
                                      .minValue = yRange.minValue,
                                      .maxValue = yRange.maxValue,
                                  },
                                  WaveViewportAutoFollowPolicy::ExplicitCommand);
}

ZoomSelectionResult handleMainPlotZoomSelection(plot::WaveViewState& view, bool suppressEscapeCancel)
{
    ZoomSelectionResult result;
    if (!view.zoomSelectionActive && !view.zoomSelectionDragging) {
        return result;
    }
    result.consumed = true;

    const bool cancelByEscape = ImGui::IsKeyPressed(ImGuiKey_Escape) && !suppressEscapeCancel;
    if (cancelByEscape || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
        ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        cancelZoomSelection(view);
        return result;
    }

    const ImVec2 mouse = ImGui::GetMousePos();
    if (view.zoomSelectionActive && !view.zoomSelectionDragging && ImPlot::IsPlotHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        view.zoomSelectionDragging = true;
        view.zoomSelectionStartX = mouse.x;
        view.zoomSelectionStartY = mouse.y;
        view.zoomSelectionCurrentX = mouse.x;
        view.zoomSelectionCurrentY = mouse.y;
    }

    if (!view.zoomSelectionDragging) {
        return result;
    }

    view.zoomSelectionCurrentX = mouse.x;
    view.zoomSelectionCurrentY = mouse.y;
    const ImVec2 rectMin(static_cast<float>((std::min)(view.zoomSelectionStartX, view.zoomSelectionCurrentX)),
                         static_cast<float>((std::min)(view.zoomSelectionStartY, view.zoomSelectionCurrentY)));
    const ImVec2 rectMax(static_cast<float>((std::max)(view.zoomSelectionStartX, view.zoomSelectionCurrentX)),
                         static_cast<float>((std::max)(view.zoomSelectionStartY, view.zoomSelectionCurrentY)));
    const float pixelWidth = rectMax.x - rectMin.x;
    const float pixelHeight = rectMax.y - rectMin.y;
    const ZoomSelectionAxisMode mode = resolveZoomSelectionAxisMode(pixelWidth, pixelHeight);
    auto* drawList = ImPlot::GetPlotDrawList();
    drawZoomSelectionPreview(
        *drawList,
        mode,
        rectMin,
        rectMax,
        ImVec2(static_cast<float>(view.zoomSelectionStartX), static_cast<float>(view.zoomSelectionStartY)),
        ImVec2(static_cast<float>(view.zoomSelectionCurrentX), static_cast<float>(view.zoomSelectionCurrentY)));

    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        return result;
    }

    constexpr float kSelectionThreshold = 4.0F;
    const bool validSelection =
        (mode == ZoomSelectionAxisMode::XOnly && pixelWidth >= kSelectionThreshold) ||
        (mode == ZoomSelectionAxisMode::YOnly && pixelHeight >= kSelectionThreshold) ||
        (mode == ZoomSelectionAxisMode::XY && pixelWidth >= kSelectionThreshold && pixelHeight >= kSelectionThreshold);
    if (validSelection) {
        const ImPlotPoint plotStart = ImPlot::PixelsToPlot(
            ImVec2(static_cast<float>(view.zoomSelectionStartX), static_cast<float>(view.zoomSelectionStartY)));
        const ImPlotPoint plotEnd = ImPlot::PixelsToPlot(
            ImVec2(static_cast<float>(view.zoomSelectionCurrentX), static_cast<float>(view.zoomSelectionCurrentY)));
        result.viewportChanged = applyZoomSelectionViewport(view, mode, plotStart, plotEnd);
    }
    // 核心流程：框选完成后是否退出交互模式由配置控制，便于连续框选或一次性框选两种习惯共存。
    view.zoomSelectionDragging = false;
    if (view.zoomSelectionAutoExit) {
        cancelZoomSelection(view);
    }
    return result;
}

bool handleActiveWaveformDoubleClickOffsetReset(plot::WaveDockState& wave,
                                                const plot::WaveSnapshot& snapshot,
                                                const BitLaneLayout& bitLayout,
                                                const plot::WaveDisplayData& displayData,
                                                const std::vector<std::size_t>& visibleChannelIndices,
                                                const ImPlotPoint& mousePos,
                                                double timeSnapDistance,
                                                double valueSnapDistance)
{
    auto& view = wave.view;
    if (!ImPlot::IsPlotHovered() || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        return false;
    }

    const auto waveformChannels = selectableAnalogWaveformChannels(snapshot, displayData, visibleChannelIndices);
    if (const auto waveform = plot::findNearestDisplayPointInChannels(
            displayData, waveformChannels, mousePos.x, mousePos.y, timeSnapDistance, valueSnapDistance)) {
        if (view.measurementChannelIndex != waveform->channelIndex) {
            // 核心流程：双击非当前模拟波形只切换激活 CH，不顺手复位用户配置。
            view.measurementChannelIndex = waveform->channelIndex;
            view.activeBitLane = {};
            return true;
        }

        // 核心流程：双击当前激活模拟波形才执行 offset 复位。
        if (!plot::resetChannelOffsetToDefault(wave, view.measurementChannelIndex)) {
            return false;
        }
        view.activeBitLane = {};
        invalidateWaveDisplayCaches(wave);
        return true;
    }

    if (const auto bitLane = findBitLaneAtPlotValue(bitLayout, mousePos.y, valueSnapDistance)) {
        return resetBitLaneYOffsetFromHit(wave, bitLane->lane);
    }

    return false;
}

bool resetBitLaneYOffsetFromHit(plot::WaveDockState& wave, const BitLaneLayoutEntry& lane)
{
    auto& view = wave.view;
    // 核心流程：bit lane 双击不依赖预先选中，也不受 Y offset 拖动模式影响。
    view.measurementChannelIndex = lane.parentChannelIndex;
    view.activeBitLane = {
        .active = true,
        .parentChannelIndex = lane.parentChannelIndex,
        .bitIndex = lane.bitIndex,
        .laneIndex = lane.laneIndex,
    };
    return resetChannelBitYOffsetToZero(wave, lane.parentChannelIndex);
}

const char* axisSourceName(plot::WaveTimeAxisSource source)
{
    switch (source) {
        case plot::WaveTimeAxisSource::SampleFrequency:
            return "控件频率";
        case plot::WaveTimeAxisSource::ScriptTime:
            return "脚本时间";
        case plot::WaveTimeAxisSource::SampleIndex:
            return "点数轴";
    }
    return "未知";
}

void applyViewport(plot::WaveViewState& view, const plot::WaveViewport& viewport, WaveViewportAutoFollowPolicy policy)
{
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    const auto oldViewport = currentViewport(view);
    view.viewMinTime = viewport.minTime;
    view.viewMaxTime = viewport.maxTime;
    view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, minVisibleTimeSpan);
    view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    if (!view.lockVerticalRange) {
        view.viewMinValue = viewport.minValue;
        view.viewMaxValue = viewport.maxValue;
    }
    applyAutoFollowPausePolicy(view, policy);
    view.forceNextMainPlotLimits = true;
    plot::shiftMeasurementCursorsForViewportScroll(view, oldViewport, currentViewport(view));
}

plot::WaveViewport currentViewport(const plot::WaveViewState& view)
{
    return {
        .minTime = view.viewMinTime,
        .maxTime = view.viewMaxTime,
        .minValue = view.lockVerticalRange ? view.manualVerticalMin : view.viewMinValue,
        .maxValue = view.lockVerticalRange ? view.manualVerticalMax : view.viewMaxValue,
    };
}

plot::ChannelSpec fallbackChannelDefaultSpec(const plot::ChannelSpec& spec)
{
    return plot::ChannelSpec{
        .label = spec.label,
        .unit = spec.unit,
        .ratio = 1.0,
        .scale = 1.0,
        .offset = 0.0,
        .color = spec.color,
        .lineWidth = spec.lineWidth,
        .bitDisplay = spec.bitDisplay,
    };
}

plot::ChannelSpec channelDefaultSpec(const plot::WaveDockState& wave,
                                     std::size_t channelIndex,
                                     const plot::ChannelSpec& current)
{
    if (channelIndex < wave.defaultChannelSpecs.size()) {
        return wave.defaultChannelSpecs[channelIndex];
    }
    return fallbackChannelDefaultSpec(current);
}

void applyChannelTransformOverride(plot::WaveDockState& wave,
                                   std::size_t channelIndex,
                                   const plot::ChannelSpec& updated,
                                   const plot::ChannelSpec& defaultSpec)
{
    if (channelIndex >= wave.channelOverrides.size()) {
        wave.channelOverrides.resize(channelIndex + 1);
    }
    auto& overrideState = wave.channelOverrides[channelIndex];
    overrideState.labelOverridden = updated.label != defaultSpec.label;
    overrideState.ratioOverridden = std::abs(updated.ratio - defaultSpec.ratio) > 1e-12;
    overrideState.scaleOverridden = std::abs(updated.scale - defaultSpec.scale) > 1e-12;
    overrideState.offsetOverridden = std::abs(updated.offset - defaultSpec.offset) > 1e-12;
    overrideState.colorOverridden = updated.color != defaultSpec.color;
    overrideState.bitYOffsetOverridden = std::abs(updated.bitDisplay.yOffset - defaultSpec.bitDisplay.yOffset) > 1e-12;
    overrideState.label = updated.label;
    overrideState.ratio = updated.ratio;
    overrideState.scale = updated.scale;
    overrideState.offset = updated.offset;
    overrideState.color = updated.color;
    overrideState.bitYOffset = updated.bitDisplay.yOffset;
    wave.buffer.setChannelSpec(channelIndex, updated);
    invalidateWaveDisplayCaches(wave);
}

void invalidateWaveDisplayCaches(plot::WaveDockState& wave)
{
    wave.cachedDisplayKeyValid = false;
    wave.cachedOverviewKeyValid = false;
    wave.renderEnvelopeCache.clear();
    wave.bitRenderCache.clear();
    wave.cachedFftKeyValid = false;
}

class WaveOverviewComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_overview"; }

    void draw(WaveContext& context) override
    {
        auto& wave = context.wave;
        auto& view = context.view;
        const auto& layout = *context.layout;
        const auto& config = *context.config;
        const auto& frame = *context.renderFrame;

        ImGui::BeginChild(
            "##wave_overview_panel", ImVec2(0.0F, layout.overviewHeight), true, ImGuiWindowFlags_NoScrollbar);
        const ImVec2 overviewPanelCursor = ImGui::GetCursorPos();
        if (!wave.overviewCollapsed) {
            // 核心流程：概览图必须基于完整历史数据，而不是主图当前视口，否则它会退化成“缩小版主图”。
            const auto derivedChannelIndices = channelIndicesForDerivedViews(wave, *frame.fullSnapshot);
            const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
            const auto overviewBounds =
                excludesLegendHiddenChannels(view)
                    ? plot::computeDisplayBoundsForChannels(
                          *frame.overviewDisplayData, derivedChannelIndices, minVisibleTimeSpan)
                    : plot::computeDisplayBounds(*frame.overviewDisplayData, minVisibleTimeSpan);
            drawOverviewWindow(view,
                               config,
                               *frame.fullSnapshot,
                               *frame.overviewDisplayData,
                               overviewBounds,
                               derivedChannelIndices,
                               frame.renderBudget);
        }
        ImGui::SetCursorPos(overviewPanelCursor);
        if (ImGui::Button(wave.overviewCollapsed ? "v" : "^", ImVec2(20.0F, 18.0F))) {
            wave.overviewCollapsed = !wave.overviewCollapsed;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(wave.overviewCollapsed ? "展开概览图" : "折叠概览图");
        }
        ImGui::EndChild();
        if (!wave.overviewCollapsed) {
            drawHorizontalSplitter("##wave_overview_splitter",
                                   wave.overviewPanelHeight,
                                   wave.minOverviewPanelHeight,
                                   wave.minMainPanelHeight,
                                   layout.overviewHeight + layout.mainHeight + wave.overviewMainSplitterHeight,
                                   wave.overviewMainSplitterHeight);
        }
    }
};

class WaveMainToolbarComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_main_toolbar"; }

    void draw(WaveContext& context) override
    {
        drawCompactMainToolbar(context.application,
                               context.wave,
                               *context.config,
                               *context.renderFrame->displayData,
                               context.frame,
                               context.fullscreenActive,
                               context.fullscreenToggleRequested);
    }
};

class WavePlotComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_plot"; }

    void draw(WaveContext& context) override
    {
        ImGui::BeginChild(
            "##wave_main_panel", ImVec2(0.0F, context.layout->mainHeight), false, ImGuiWindowFlags_NoScrollbar);
        const WavePlotOverlayPolicy overlayPolicy{
            .drawMeasurementOverlay = true,
            .drawLegendOverlay = context.overlayFrame == nullptr,
            .measurementSafeRightX = context.measurementSafeRightX,
        };
        auto result = drawOscilloscopePlot(context.wave, *context.renderFrame, overlayPolicy, &context.frame);
        if (context.overlayFrame != nullptr) {
            captureWaveOverlayFrame(*context.overlayFrame, result, *context.renderFrame);
        }
        ImGui::EndChild();
    }
};

class WaveCursorSplitComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_cursor_split"; }

    void draw(WaveContext& context) override
    {
        const float plotAreaHeight = (std::max)(2.0F, context.layout->mainHeight);
        const float timePanelHeight = (std::max)(1.0F, std::floor(plotAreaHeight * 0.5F));
        const float fftPanelHeight = (std::max)(1.0F, plotAreaHeight - timePanelHeight);
        const WavePlotOverlayPolicy overlayPolicy{
            .drawMeasurementOverlay = true,
            .drawLegendOverlay = context.overlayFrame == nullptr,
            .measurementSafeRightX = context.measurementSafeRightX,
        };

        ImGui::BeginChild(
            "##wave_cursor_split_time", ImVec2(0.0F, timePanelHeight), false, ImGuiWindowFlags_NoScrollbar);
        auto result = drawOscilloscopePlot(context.wave, *context.renderFrame, overlayPolicy, &context.frame);
        if (context.overlayFrame != nullptr) {
            captureWaveOverlayFrame(*context.overlayFrame, result, *context.renderFrame);
        }
        ImGui::EndChild();

        drawCompactMainToolbar(context.application,
                               context.wave,
                               *context.config,
                               *context.renderFrame->displayData,
                               context.frame,
                               context.fullscreenActive,
                               context.fullscreenToggleRequested);

        ImGui::BeginChild("##wave_cursor_split_fft", ImVec2(0.0F, fftPanelHeight), false, ImGuiWindowFlags_NoScrollbar);
        drawWaveFftPlot(context.wave, *context.renderFrame, false, false);
        ImGui::EndChild();
    }
};

class WaveToolbarComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_toolbar"; }

    void draw(WaveContext& context) override
    {
        auto& wave = context.wave;
        const auto& config = *context.config;
        const auto& displayData = *context.renderFrame->displayData;
        const auto& style = ImGui::GetStyle();

        ImGui::SameLine();

        const ImVec2 railPos = ImGui::GetCursorScreenPos();
        const ImVec2 railMax(railPos.x + context.toolsWidth, railPos.y + context.availableHeight);

        ScopedStyleVars railStyle;
        railStyle.push(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, style.WindowPadding.y));
        railStyle.push(ImGuiStyleVar_ItemSpacing, ImVec2(0.0F, style.ItemSpacing.y));

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, style.WindowPadding.y));

        ImGui::BeginChild("##wave_tools_rail",
                          ImVec2(context.toolsWidth, context.availableHeight),
                          true,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        drawToolsRail(wave);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        const bool railHovered = pointInRect(context.io.MousePos, railPos, railMax);

        if (!wave.toolsCollapsed && ImGui::IsKeyPressed(ImGuiKey_Escape) && !context.io.WantTextInput) {
            wave.toolsCollapsed = true;
        }

        const float drawerProgress = (std::clamp)(wave.toolsDrawerProgress, 0.0F, 1.0F);
        if (drawerProgress <= 0.0F) {
            return;
        }

        wave.toolsExpandedWidth =
            (std::clamp)(wave.toolsExpandedWidth, wave.minToolsExpandedWidth, wave.maxToolsExpandedWidth);

        const ToolsDrawerGeometry drawerGeometry =
            calculateToolsDrawerGeometry(railPos,
                                         context.contentWidth,
                                         context.availableHeight,
                                         wave.toolsExpandedWidth * drawerProgress,
                                         style.ItemSpacing.x);

        if (!drawerGeometry.valid) {
            return;
        }

        const ToolsDrawerResizeState resizeState = wave.toolsCollapsed
                                                       ? ToolsDrawerResizeState{}
                                                       : drawToolsDrawerResizeHandle(wave,
                                                                                     drawerGeometry.pos,
                                                                                     context.availableHeight,
                                                                                     drawerGeometry.contentLeft,
                                                                                     wave.contentToolsSplitterWidth);

        bool drawerOpen = true;
        const std::string title = std::string(toolsDrawerTitle(wave.activeToolsDrawer)) + "##wave_tools_drawer";

        ImGui::SetNextWindowPos(drawerGeometry.pos, ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(drawerGeometry.width, context.availableHeight), ImGuiCond_Always);

        constexpr ImGuiWindowFlags drawerFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                                                 ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                                 ImGuiWindowFlags_NoCollapse;

        bool drawerHovered = false;
        bool drawerFocused = false;
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, drawerProgress);
        if (ImGui::Begin(title.c_str(), &drawerOpen, drawerFlags)) {
            drawerHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                                   ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
            drawerFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
            drawWaveToolsDrawer(context.application,
                                wave,
                                config,
                                displayData,
                                wave.activeToolsDrawer,
                                context.fullscreenActive,
                                context.fullscreenToggleRequested);
        }

        ImGui::End();
        ImGui::PopStyleVar();

        if (!drawerOpen) {
            wave.toolsCollapsed = true;
        }

        const ImVec2 drawerMax(drawerGeometry.pos.x + drawerGeometry.width,
                               drawerGeometry.pos.y + context.availableHeight);
        const bool drawerContainsMouse = pointInRect(context.io.MousePos, drawerGeometry.pos, drawerMax);
        const bool resizeContainsMouse = pointInRect(context.io.MousePos, resizeState.min, resizeState.max);
        const bool popupOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const bool drawerKeyboardActive = drawerFocused && context.io.WantTextInput;
        const bool insideToolsArea = railHovered || drawerContainsMouse || resizeContainsMouse || drawerHovered ||
                                     drawerKeyboardActive || resizeState.hovered || resizeState.active;
        if (!wave.toolsCollapsed && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !insideToolsArea && !popupOpen &&
            !context.io.WantTextInput) {
            wave.toolsCollapsed = true;
        }
    }

private:
    static constexpr float kToolsRailButtonWidth = 28.0F;

    class ScopedStyleVars final {
    public:
        ScopedStyleVars() = default;

        ScopedStyleVars(const ScopedStyleVars&) = delete;
        ScopedStyleVars& operator=(const ScopedStyleVars&) = delete;

        ~ScopedStyleVars()
        {
            if (count_ > 0) {
                ImGui::PopStyleVar(count_);
            }
        }

        void push(ImGuiStyleVar idx, const ImVec2& value)
        {
            ImGui::PushStyleVar(idx, value);
            ++count_;
        }

        void push(ImGuiStyleVar idx, float value)
        {
            ImGui::PushStyleVar(idx, value);
            ++count_;
        }

    private:
        int count_ = 0;
    };

    struct ToolsDrawerGeometry final {
        ImVec2 pos{};
        float width = 0.0F;
        float contentLeft = 0.0F;
        float right = 0.0F;
        bool valid = false;
    };

    static float centeredOffset(float containerWidth, float itemWidth)
    {
        return std::floor((std::max)(0.0F, containerWidth - itemWidth) * 0.5F);
    }

    static void drawToolsRailCentered(plot::WaveDockState& wave)
    {
        const float railContentWidth = ImGui::GetContentRegionAvail().x;
        const float indentX = centeredOffset(railContentWidth, kToolsRailButtonWidth);

        if (indentX > 0.0F) {
            ImGui::Indent(indentX);
        }

        drawToolsRail(wave);

        if (indentX > 0.0F) {
            ImGui::Unindent(indentX);
        }
    }

    static ToolsDrawerGeometry calculateToolsDrawerGeometry(const ImVec2& railPos,
                                                            float contentWidth,
                                                            float availableHeight,
                                                            float requestedDrawerWidth,
                                                            float itemSpacingX)
    {
        ToolsDrawerGeometry geometry{};

        if (contentWidth <= 0.0F || availableHeight <= 0.0F) {
            return geometry;
        }

        geometry.contentLeft = railPos.x - itemSpacingX - contentWidth;
        geometry.right = railPos.x - itemSpacingX;
        geometry.width = (std::min)(requestedDrawerWidth, (std::max)(0.0F, contentWidth));

        if (geometry.width <= 0.0F) {
            return geometry;
        }

        geometry.pos = ImVec2((std::max)(geometry.contentLeft, geometry.right - geometry.width), railPos.y);

        geometry.valid = true;
        return geometry;
    }
};

struct WaveContentPlan {
    plot::WaveLayoutSizes layout{};
    WaveFrameData frame{};
    WaveFrameState frameState{};
    float contentWidth{0.0F};
    float toolsWidth{0.0F};
};

struct WaveComponentSet {
    WaveOverviewComponent overview;
    WaveMainToolbarComponent mainToolbar;
    WavePlotComponent plot;
    WaveCursorSplitComponent cursorSplit;
    WaveFftComponent fft;
    WaveToolbarComponent toolbar;
    std::array<IWaveComponent*, 6> all;

    WaveComponentSet() : all{&overview, &mainToolbar, &plot, &cursorSplit, &fft, &toolbar} {}
};

bool isCursorSplitFftMode(const plot::WaveViewState& view)
{
    return view.fft.enabled && view.fft.displayMode == plot::WaveFftDisplayMode::CursorSplit;
}

WaveContentPlan buildWaveContentPlan(plot::WaveDockState& wave, plot::WaveViewState& view, const ImVec2& available)
{
    const float spacingWidth = ImGui::GetStyle().ItemSpacing.x;
    const float spacingHeight = ImGui::GetStyle().ItemSpacing.y;
    const bool cursorSplitMode = isCursorSplitFftMode(view);
    const float toolbarHeight = compactMainToolbarHeight(wave.mainToolbarNeedsHorizontalScroll);
    const float overviewProgress = (std::clamp)(wave.overviewPanelProgress, 0.0F, 1.0F);
    const float overviewRequestedHeight =
        cursorSplitMode ? 0.0F
                        : wave.overviewCollapsedHeight +
                              (wave.overviewPanelHeight - wave.overviewCollapsedHeight) * overviewProgress;
    const float overviewMinHeight =
        cursorSplitMode ? 0.0F
                        : wave.overviewCollapsedHeight +
                              (wave.minOverviewPanelHeight - wave.overviewCollapsedHeight) * overviewProgress;
    const float mainPlotAxisReserve = ImGui::GetTextLineHeightWithSpacing() + spacingHeight;
    const float fixedContentHeight = toolbarHeight + spacingHeight * 2.0F + mainPlotAxisReserve;
    constexpr bool toolsLayoutCollapsed = true;

    WaveContentPlan plan;
    plan.layout =
        plot::solveWaveLayout(available.x,
                              available.y,
                              overviewRequestedHeight,
                              wave.toolsExpandedWidth,
                              wave.toolsCollapsedWidth,
                              toolsLayoutCollapsed,
                              spacingWidth,
                              cursorSplitMode || overviewProgress <= 0.0F ? 0.0F : wave.overviewMainSplitterHeight,
                              overviewMinHeight,
                              wave.minMainPanelHeight,
                              wave.minToolsExpandedWidth,
                              wave.maxToolsExpandedWidth,
                              fixedContentHeight);
    wave.toolsExpandedWidth =
        (std::clamp)(wave.toolsExpandedWidth, wave.minToolsExpandedWidth, wave.maxToolsExpandedWidth);
    plan.toolsWidth = plan.layout.toolsWidth;
    plan.contentWidth = (std::max)(0.0F, available.x - plan.toolsWidth - spacingWidth);
    plan.frame = prepareWaveFrame(wave, plan.contentWidth);
    return plan;
}

WaveContext makeWaveContext(app::Application& application,
                            plot::WaveDockState& wave,
                            plot::WaveViewState& view,
                            const plot::ViewConfig& config,
                            WaveContentPlan& plan,
                            const ImVec2& available,
                            bool fullscreenActive,
                            bool* fullscreenToggleRequested)
{
    WaveContext context{application, wave, view, *plan.frame.fullSnapshot, ImGui::GetIO(), plan.frameState};
    context.config = &config;
    context.layout = &plan.layout;
    context.renderFrame = &plan.frame;
    context.availableWidth = available.x;
    context.availableHeight = available.y;
    context.contentWidth = plan.contentWidth;
    context.toolsWidth = plan.toolsWidth;
    context.fullscreenActive = fullscreenActive;
    context.fullscreenToggleRequested = fullscreenToggleRequested;
    return context;
}

void prepareWaveComponents(WaveComponentSet& components, WaveContext& context)
{
    for (auto* component : components.all) {
        component->prepare(context);
    }
}

void drawWaveContentComponents(WaveComponentSet& components, WaveContext& context)
{
    ImGui::BeginChild(
        "##wave_content", ImVec2(context.contentWidth, context.availableHeight), false, ImGuiWindowFlags_NoScrollbar);
    const ImVec2 contentPos = ImGui::GetWindowPos();
    context.measurementSafeRightX = resolveMeasurementSafeRightX(contentPos.x,
                                                                 context.contentWidth,
                                                                 context.wave.toolsCollapsed,
                                                                 context.wave.toolsExpandedWidth,
                                                                 context.wave.minToolsExpandedWidth,
                                                                 context.wave.maxToolsExpandedWidth,
                                                                 context.wave.contentToolsSplitterWidth);
    const bool cursorSplitMode = isCursorSplitFftMode(context.view);
    if (!cursorSplitMode) {
        components.overview.draw(context);
        components.mainToolbar.draw(context);
    }
    if (cursorSplitMode) {
        components.cursorSplit.draw(context);
    } else if (context.view.fft.enabled) {
        components.fft.draw(context);
    } else {
        components.plot.draw(context);
    }
    ImGui::EndChild();

    components.toolbar.draw(context);
}

void commitWaveComponents(WaveComponentSet& components, WaveContext& context)
{
    for (auto* component : components.all) {
        component->handleInput(context);
        component->commit(context);
    }
}

WaveDockRenderer::WaveDockRenderer(app::Application& application) : application_(application) {}

std::string WaveDockRenderer::formatMetric(double value, const char* baseUnit)
{
    return formatMetricText(value, baseUnit);
}

void WaveDockRenderer::draw(bool& showWaveDock,
                            bool fullscreenActive,
                            bool* fullscreenToggleRequested,
                            bool shortcutFocusOverride)
{
    if (!showWaveDock) {
        return;
    }

    if (ImGui::Begin("波形", &showWaveDock)) {
        drawContent(ImGui::GetContentRegionAvail(), fullscreenActive, fullscreenToggleRequested, shortcutFocusOverride);
    }
    ImGui::End();
    flushPendingOscilloscopeToggle();
}

void WaveDockRenderer::drawOverlay(bool fullscreenActive, bool* fullscreenToggleRequested)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoNavFocus;
    WaveOverlayFrame overlayFrame;
    if (ImGui::Begin("波形全屏##wave_fullscreen_overlay", nullptr, flags)) {
        drawContent(ImGui::GetContentRegionAvail(), fullscreenActive, fullscreenToggleRequested, true, &overlayFrame);
    }
    ImGui::End();
    drawWaveOverlayPass(application_.docks().waveState(), overlayFrame);
    flushPendingOscilloscopeToggle();
}

void WaveDockRenderer::drawContent(const ImVec2& available,
                                   bool fullscreenActive,
                                   bool* fullscreenToggleRequested,
                                   bool shortcutFocusOverride,
                                   WaveOverlayFrame* overlayFrame)
{
    auto& wave = application_.docks().waveState();
    auto& view = wave.view;
    const auto& config = wave.buffer.viewConfig();
    view.effectiveInteractionAnimationEnabled =
        application_.runtimeConfig().gui.interactionFeedback.enabled && view.interactionAnimationEnabled;
    wave.suppressZoomSelectionEscapeThisFrame = wave.suppressZoomSelectionEscapeThisFrame || fullscreenActive;

    syncWaveViewToLatest();
    initializeWaveViewIfNeeded(view);
    advanceWavePanelAnimations(wave, ImGui::GetIO().DeltaTime);
    const bool dockFocused = shortcutFocusOverride || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    handleWaveShortcuts(dockFocused, fullscreenActive, fullscreenToggleRequested);
    advanceViewportAnimation(view, ImGui::GetIO().DeltaTime);

    auto plan = buildWaveContentPlan(wave, view, available);
    auto context =
        makeWaveContext(application_, wave, view, config, plan, available, fullscreenActive, fullscreenToggleRequested);
    context.overlayFrame = overlayFrame;
    WaveComponentSet components;
    prepareWaveComponents(components, context);
    drawWaveContentComponents(components, context);
    commitWaveComponents(components, context);
    if (plan.frameState.oscilloscopeToggleRequest.has_value()) {
        const auto& request = *plan.frameState.oscilloscopeToggleRequest;
        pendingOscilloscopeToggle_ = PendingOscilloscopeToggle{request.currentRunning, request.targetRunning};
    }
    if (plan.frameState.resetHistoryRequested) {
        application_.resetWaveHistory();
        application_.setTransientStatusMessage("当前波形历史已清空");
    }
    wave.suppressZoomSelectionEscapeThisFrame = false;
}

void WaveDockRenderer::flushPendingOscilloscopeToggle()
{
    if (!pendingOscilloscopeToggle_.has_value()) {
        return;
    }

    const auto request = *pendingOscilloscopeToggle_;
    pendingOscilloscopeToggle_.reset();
    application_.requestOscilloscopeToggle(request.currentRunning, request.targetRunning);
}

void WaveDockRenderer::handleWaveShortcuts(const bool dockFocused,
                                           const bool fullscreenActive,
                                           bool* fullscreenToggleRequested)
{
    const auto& io = ImGui::GetIO();
    if (!dockFocused || io.WantTextInput ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }

    auto& wave = application_.docks().waveState();
    auto& view = wave.view;
    if (waveShortcutPressed(ShortcutAction::WaveToggleFullscreen)) {
        if (fullscreenToggleRequested != nullptr) {
            if (!fullscreenActive) {
                resetLegendOverlayTransientForFullscreenEntry(wave);
            }
            *fullscreenToggleRequested = true;
            application_.setTransientStatusMessage("已请求切换波形全屏");
        }
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveTogglePauseFollow)) {
        view.autoFollowLatest = !view.autoFollowLatest;
        application_.setTransientStatusMessage(view.autoFollowLatest ? "波形自动跟随已恢复" : "波形自动跟随已暂停");
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveFitVisible)) {
        view.fitVisibleWaveformsRequested = true;
        application_.setTransientStatusMessage("已请求适配当前可见波形");
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveToggleZoomSelection)) {
        view.zoomSelectionActive = !view.zoomSelectionActive;
        view.zoomSelectionDragging = false;
        application_.setTransientStatusMessage(view.zoomSelectionActive ? "框选放大已开启" : "框选放大已关闭");
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveToggleFft)) {
        view.fft.enabled = !view.fft.enabled;
        if (view.fft.enabled) {
            view.fftSourceMinTime = view.viewMinTime;
            view.fftSourceMaxTime = view.viewMaxTime;
            view.fftSourceWindowValid = true;
            view.fftViewportInitialized = false;
        } else {
            view.fftSourceWindowValid = false;
            view.fftViewportInitialized = false;
        }
        wave.cachedFftKeyValid = false;
        application_.setTransientStatusMessage(view.fft.enabled ? "FFT 频谱模式已开启" : "FFT 频谱模式已关闭");
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveClearHistory)) {
        // 核心流程：清空历史使用带修饰键组合，避免裸字母误清实时采样缓存。
        application_.resetWaveHistory();
        application_.setTransientStatusMessage("当前波形历史已清空");
    }
}

void WaveDockRenderer::syncWaveViewToLatest()
{
    auto& wave = application_.docks().waveState();
    if (!wave.view.autoFollowLatest) {
        return;
    }

    if (const auto latestTime = latestWaveViewTime(wave)) {
        const auto oldViewport = currentViewport(wave.view);
        wave.view.viewMaxTime = *latestTime;
        wave.view.viewMinTime = *latestTime - wave.view.visibleDuration;
        clampWaveViewLowerBoundToZero(wave.view);
        wave.view.centerTime = 0.5 * (wave.view.viewMinTime + wave.view.viewMaxTime);
        plot::shiftMeasurementCursorsForViewportScroll(wave.view, oldViewport, currentViewport(wave.view));
        if (!wave.view.lockVerticalRange && !wave.view.initialized) {
            wave.view.viewMinValue = wave.view.manualVerticalMin;
            wave.view.viewMaxValue = wave.view.manualVerticalMax;
        }
    }
}

} // namespace protoscope::ui
