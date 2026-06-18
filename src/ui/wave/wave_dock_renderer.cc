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

namespace {

    constexpr float kTopToolbarHeight = 38.0F;

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
        view.viewMinTime = center - nextSpan * 0.5;
        view.viewMaxTime = center + nextSpan * 0.5;
        view.visibleDuration = nextSpan;
        view.centerTime = center;
        view.autoFollowLatest = false;
        view.forceNextMainPlotLimits = true;
    }

    void drawCompactMainToolbar(app::Application& application,
                                plot::WaveDockState& wave,
                                const plot::ViewConfig& config,
                                const plot::WaveDisplayData& displayData,
                                bool fullscreenActive,
                                bool* fullscreenToggleRequested)
    {
        auto& view = wave.view;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.059F, 0.086F, 0.125F, 1.0F));
        ImGui::BeginChild("##wave_main_toolbar", ImVec2(0.0F, kTopToolbarHeight), true, ImGuiWindowFlags_NoScrollbar);
        ImGui::AlignTextToFramePadding();

        if (drawTopToolbarButton("叠加", view.viewMode == plot::WaveViewMode::Overlay, "多通道共用同一个波形区域。")) {
            view.viewMode = plot::WaveViewMode::Overlay;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("堆叠", view.viewMode == plot::WaveViewMode::Stacked, "按通道纵向错开显示，不修改原始采样。")) {
            view.viewMode = plot::WaveViewMode::Stacked;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("分屏", view.viewMode == plot::WaveViewMode::Split, "每个可见通道使用独立子图，共享时间轴。")) {
            view.viewMode = plot::WaveViewMode::Split;
        }

        drawTopToolbarSeparator();
        if (drawTopToolbarButton("-", false, "缩小时间轴。")) {
            zoomTimeAroundCenter(view, 1.25);
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("+", false, "放大时间轴。")) {
            zoomTimeAroundCenter(view, 0.80);
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("适配", false, "适配当前可见波形到完整视图。")) {
            view.fitVisibleWaveformsRequested = true;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(view.zoomSelectionActive ? "框选" : "平移", view.zoomSelectionActive, "切换框选放大模式。")) {
            view.zoomSelectionActive = !view.zoomSelectionActive;
            view.zoomSelectionDragging = false;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(view.autoFollowLatest ? "跟随" : "停跟", view.autoFollowLatest, "切换自动跟随最新数据。")) {
            view.autoFollowLatest = !view.autoFollowLatest;
        }

        drawTopToolbarSeparator();
        if (drawTopToolbarButton("A", view.cursors[0].enabled, "显示或隐藏 A 游标。")) {
            view.cursors[0].enabled = !view.cursors[0].enabled;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("B", view.cursors[1].enabled, "显示或隐藏 B 游标。")) {
            view.cursors[1].enabled = !view.cursors[1].enabled;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton(view.cursorIntervalLocked ? "锁定" : "间隔", view.cursorIntervalLocked, "锁定 A/B 游标间隔。")) {
            view.cursorIntervalLocked = !view.cursorIntervalLocked;
            view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("同步", false, "把 A/B 游标移入当前视窗。")) {
            placeCursorPairInViewport(view, config, displayData);
        }

        drawTopToolbarSeparator();
        if (drawTopToolbarButton("dt", view.measurement.deltaTime, "切换时间差测量。")) {
            view.measurement.deltaTime = !view.measurement.deltaTime;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("Hz", view.measurement.frequency, "切换等效频率测量。")) {
            view.measurement.frequency = !view.measurement.frequency;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("峰峰", view.measurement.peakToPeak, "切换峰峰值测量。")) {
            view.measurement.peakToPeak = !view.measurement.peakToPeak;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("均值", view.measurement.mean, "切换均值测量。")) {
            view.measurement.mean = !view.measurement.mean;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("更多", !wave.toolsCollapsed, "展开右侧高级工具轨。")) {
            wave.toolsCollapsed = false;
        }

        drawTopToolbarSeparator();
        if (drawTopToolbarButton("概览", !wave.overviewCollapsed, "展开或折叠概览图。")) {
            wave.overviewCollapsed = !wave.overviewCollapsed;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("图例", view.showChannelLegend, "显示或隐藏图内通道图例。")) {
            view.showChannelLegend = !view.showChannelLegend;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("展开", wave.legendOverlay.expanded, "展开图内通道图例详情面板。")) {
            wave.legendOverlay.expanded = !wave.legendOverlay.expanded;
        }
        ImGui::SameLine();
        if (drawTopToolbarButton("恢复", false, "恢复所有通道显示设置，不清空波形数据。")) {
            if (plot::resetAllChannelViewSettings(wave)) {
                invalidateWaveDisplayCaches(wave);
            }
        }
        ImGui::SameLine();
        if (fullscreenToggleRequested != nullptr &&
            drawTopToolbarButton(fullscreenActive ? "退全" : "全屏", fullscreenActive, "切换波形全屏显示。")) {
            *fullscreenToggleRequested = true;
            application.setStatusMessage(fullscreenActive ? "已请求退出波形全屏" : "已请求进入波形全屏", false);
        }

        ImGui::EndChild();
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

bool drawRightPanelSplitter(
    const char* id, float& rightWidth, float minRightWidth, float minLeftWidth, float totalWidth, float thickness)
{
    const float safeThickness = (std::max)(thickness, 4.0F);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Separator));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorActive));
    ImGui::Button(id, ImVec2(safeThickness, ImGui::GetContentRegionAvail().y));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive()) {
        // 右侧 splitter 位于工具栏左边界：手柄向左时右侧宽度变大，向右时变小。
        rightWidth -= ImGui::GetIO().MouseDelta.x;
        rightWidth = (std::clamp)(
            rightWidth, minRightWidth, (std::max)(minRightWidth, totalWidth - minLeftWidth - safeThickness));
        return true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    return false;
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
    view.viewMinTime = limits.X.Min;
    view.viewMaxTime = limits.X.Max;
    view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, minVisibleTimeSpan);
    view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    if (!view.lockVerticalRange) {
        view.viewMinValue = limits.Y.Min;
        view.viewMaxValue = limits.Y.Max;
    }
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
        view.autoFollowLatest = false;
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

bool handleMainPlotAxisDoubleClick(plot::WaveViewState& view,
                                   const plot::WaveSnapshot& snapshot,
                                   const plot::WaveDataBounds& visibleWindowBounds,
                                   const plot::WaveDataBounds& fullHistoryBounds,
                                   const plot::WaveDataBounds& yAutoFitBounds)
{
    if (!isFitDoubleClicked()) {
        return false;
    }

    bool changed = false;
    if (ImPlot::IsAxisHovered(ImAxis_X1)) {
        const auto& xBounds =
            plot::selectXAxisDoubleClickBounds(view.xAxisDoubleClickAction, visibleWindowBounds, fullHistoryBounds);
        if (!xBounds.valid) {
            return false;
        }
        // 核心流程：横轴双击要同步应用层视口；否则 autoFollow/Once 条件会在下一帧把 ImPlot autofit 覆盖回旧范围。
        view.viewMinTime = xBounds.minTime;
        view.viewMaxTime = xBounds.maxTime;
        view.visibleDuration =
            (std::max)(view.viewMaxTime - view.viewMinTime, (std::max)(view.minVisibleTimeSpan, 1e-6));
        view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
        view.autoFollowLatest = false;
        changed = true;
    }
    if (ImPlot::IsAxisHovered(ImAxis_Y1) && selectedChannelUsesBitDisplay(view, snapshot)) {
        // Bit lanes are laid out in plot pixels. Consume the fit gesture so ImPlot does not persist a normal Y fit.
        changed = true;
    } else if (ImPlot::IsAxisHovered(ImAxis_Y1) && !view.lockVerticalRange && yAutoFitBounds.valid) {
        // 核心流程：Y 轴双击 auto fit 在所有控制模式下都统一走倍率逻辑，避免默认 oscilloscope 模式退回 ImPlot 的 1x
        // 紧贴范围。
        const auto range = plot::makeVerticalAutoFitRange(
            yAutoFitBounds.minValue, yAutoFitBounds.maxValue, view.verticalAutoFitMultiplier);
        view.viewMinValue = range.minValue;
        view.viewMaxValue = range.maxValue;
        changed = true;
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

bool applyFullViewport(plot::WaveViewState& view, double minTime, double maxTime, double minValue, double maxValue);

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
            view.autoFollowLatest = false;
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
            view.autoFollowLatest = false;
            view.forceNextMainPlotLimits = true;
            return true;
        case ZoomSelectionAxisMode::XY:
            // 核心流程：斜向框选保留原有 XY 矩形缩放语义，两个轴一起回写到视口。
            return applyFullViewport(view, minTime, maxTime, minValue, maxValue);
    }
    return false;
}

bool applyFullViewport(plot::WaveViewState& view, double minTime, double maxTime, double minValue, double maxValue)
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
    view.autoFollowLatest = false;
    return true;
}

bool applyFitVisibleWaveforms(plot::WaveViewState& view,
                              const plot::WaveDisplayData& displayData,
                              const std::vector<std::size_t>& visibleChannelIndices)
{
    if (!view.fitVisibleWaveformsRequested) {
        return false;
    }
    view.fitVisibleWaveformsRequested = false;

    // 核心流程：显示全部只根据图例当前可见通道计算显示范围，不读取或修改原始采样与游标状态。
    const auto bounds = plot::computeDisplayBoundsForChannels(
        displayData, visibleChannelIndices, (std::max)(view.minVisibleTimeSpan, 1e-6));
    if (!bounds.valid) {
        return false;
    }

    const auto yRange =
        plot::makeVerticalAutoFitRange(bounds.minValue, bounds.maxValue, view.verticalAutoFitMultiplier);
    return applyFullViewport(view, bounds.minTime, bounds.maxTime, yRange.minValue, yRange.maxValue);
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
                                                const ImPlotPoint& mousePos,
                                                double timeSnapDistance,
                                                double valueSnapDistance)
{
    auto& view = wave.view;
    if (!ImPlot::IsPlotHovered() || !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        return false;
    }

    auto bitLaneContainsValue = [](const BitLaneLayoutEntry& lane, double plotY, double maxDistance) {
        const double minY = (std::min)(lane.lowY, lane.highY);
        const double maxY = (std::max)(lane.lowY, lane.highY);
        return plotY >= minY - maxDistance && plotY <= maxY + maxDistance;
    };
    if (view.measurementChannelIndex < snapshot.channels.size() &&
        bitDisplayEnabled(snapshot.channels[view.measurementChannelIndex].bitDisplay)) {
        for (const auto& lane : bitLayout.lanes) {
            if (lane.parentChannelIndex == view.measurementChannelIndex &&
                bitLaneContainsValue(lane, mousePos.y, valueSnapDistance)) {
                return resetChannelBitYOffsetToZero(wave, view.measurementChannelIndex);
            }
        }
    }
    if (const auto bitLane = findBitLaneAtPlotValue(bitLayout, mousePos.y, valueSnapDistance)) {
        return resetChannelBitYOffsetToZero(wave, bitLane->lane.parentChannelIndex);
    }

    const auto activePoint =
        plot::findNearestDisplayByTime(displayData, view.measurementChannelIndex, mousePos.x, timeSnapDistance);
    if (!activePoint.has_value() || std::abs(activePoint->displayValue - mousePos.y) > valueSnapDistance) {
        return false;
    }
    // 核心流程：双击只复位当前激活通道 offset override，保留 label/ratio/scale 的用户覆盖。
    if (!plot::resetChannelOffsetToDefault(wave, view.measurementChannelIndex)) {
        return false;
    }
    invalidateWaveDisplayCaches(wave);
    return true;
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

void applyViewport(plot::WaveViewState& view, const plot::WaveViewport& viewport)
{
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    view.viewMinTime = viewport.minTime;
    view.viewMaxTime = viewport.maxTime;
    view.visibleDuration = (std::max)(view.viewMaxTime - view.viewMinTime, minVisibleTimeSpan);
    view.centerTime = 0.5 * (view.viewMinTime + view.viewMaxTime);
    if (!view.lockVerticalRange) {
        view.viewMinValue = viewport.minValue;
        view.viewMaxValue = viewport.maxValue;
    }
    view.autoFollowLatest = false;
    view.forceNextMainPlotLimits = true;
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
        drawOscilloscopePlot(context.wave, *context.renderFrame);
        ImGui::EndChild();
    }
};

class WaveCursorSplitComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_cursor_split"; }

    void draw(WaveContext& context) override
    {
        const float gap = ImGui::GetStyle().ItemSpacing.y;
        const float totalHeight = (std::max)(context.layout->mainHeight, context.wave.minMainPanelHeight);
        const float panelHeight = (std::max)(80.0F, (totalHeight - gap) * 0.5F);

        ImGui::BeginChild("##wave_cursor_split_time", ImVec2(0.0F, panelHeight), false, ImGuiWindowFlags_NoScrollbar);
        drawOscilloscopePlot(context.wave, *context.renderFrame);
        ImGui::EndChild();

        ImGui::BeginChild("##wave_cursor_split_fft", ImVec2(0.0F, panelHeight), false, ImGuiWindowFlags_NoScrollbar);
        drawWaveFftPlot(context.wave, *context.renderFrame, false, false);
        ImGui::EndChild();
    }
};

class WaveMeasurementOverlayComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_measurement_overlay"; }
};

class WaveToolbarComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_toolbar"; }

    void draw(WaveContext& context) override
    {
        auto& wave = context.wave;
        auto& view = context.view;
        const auto& config = *context.config;
        const auto& displayData = *context.renderFrame->displayData;

        ImGui::SameLine();
        drawRightPanelSplitter("##wave_tools_splitter",
                               wave.toolsExpandedWidth,
                               wave.minToolsExpandedWidth,
                               (std::max)(240.0F, context.contentWidth * 0.35F),
                               context.availableWidth,
                               wave.contentToolsSplitterWidth);
        ImGui::SameLine();
        ImGui::BeginChild("##wave_tools", ImVec2(context.toolsWidth, context.availableHeight), true);
        if (ImGui::Button(wave.toolsCollapsed ? "<" : ">")) {
            wave.toolsCollapsed = !wave.toolsCollapsed;
        }
        if (!wave.toolsCollapsed) {
            ImGui::Separator();
            drawWaveToolbar(context.application,
                            wave,
                            config,
                            displayData,
                            context.fullscreenActive,
                            context.fullscreenToggleRequested);
            ImGui::Separator();
            drawCursorToolbar(view, config, displayData);
        } else {
            drawWaveToolbar(context.application,
                            wave,
                            config,
                            displayData,
                            context.fullscreenActive,
                            context.fullscreenToggleRequested);
        }
        drawChannelControls(wave, *context.renderFrame->fullSnapshot);
        ImGui::EndChild();
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
    WaveMeasurementOverlayComponent measurementOverlay;
    WaveToolbarComponent toolbar;
    std::array<IWaveComponent*, 7> all;

    WaveComponentSet() : all{&overview, &mainToolbar, &plot, &cursorSplit, &fft, &measurementOverlay, &toolbar} {}
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
    const float toolbarHeight = cursorSplitMode ? 0.0F : kTopToolbarHeight;
    const float overviewRequestedHeight =
        cursorSplitMode ? 0.0F : (wave.overviewCollapsed ? wave.overviewCollapsedHeight : wave.overviewPanelHeight);
    const float overviewMinHeight =
        cursorSplitMode ? 0.0F : (wave.overviewCollapsed ? wave.overviewCollapsedHeight : wave.minOverviewPanelHeight);
    const float mainPlotAxisReserve = ImGui::GetTextLineHeightWithSpacing() + spacingHeight;
    const float fixedContentHeight = toolbarHeight + spacingHeight * 2.0F + mainPlotAxisReserve;

    WaveContentPlan plan;
    plan.layout = plot::solveWaveLayout(available.x,
                                        available.y,
                                        overviewRequestedHeight,
                                        wave.toolsExpandedWidth,
                                        wave.toolsCollapsedWidth,
                                        wave.toolsCollapsed,
                                        wave.contentToolsSplitterWidth + spacingWidth * 2.0F,
                                        cursorSplitMode || wave.overviewCollapsed ? 0.0F : wave.overviewMainSplitterHeight,
                                        overviewMinHeight,
                                        wave.minMainPanelHeight,
                                        wave.minToolsExpandedWidth,
                                        wave.maxToolsExpandedWidth,
                                        fixedContentHeight);
    wave.toolsExpandedWidth = wave.toolsCollapsed ? wave.toolsExpandedWidth : plan.layout.toolsWidth;
    plan.toolsWidth = plan.layout.toolsWidth;
    plan.contentWidth =
        (std::max)(0.0F, available.x - plan.toolsWidth - wave.contentToolsSplitterWidth - spacingWidth * 2.0F);
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
        components.measurementOverlay.draw(context);
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
}

void WaveDockRenderer::drawOverlay(bool fullscreenActive, bool* fullscreenToggleRequested)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoCollapse;
    if (ImGui::Begin("波形全屏##wave_fullscreen_overlay", nullptr, flags)) {
        drawContent(ImGui::GetContentRegionAvail(), fullscreenActive, fullscreenToggleRequested, true);
    }
    ImGui::End();
}

void WaveDockRenderer::drawContent(const ImVec2& available,
                                   bool fullscreenActive,
                                   bool* fullscreenToggleRequested,
                                   bool shortcutFocusOverride)
{
    auto& wave = application_.docks().waveState();
    auto& view = wave.view;
    const auto& config = wave.buffer.viewConfig();
    wave.suppressZoomSelectionEscapeThisFrame = wave.suppressZoomSelectionEscapeThisFrame || fullscreenActive;

    syncWaveViewToLatest();
    initializeWaveViewIfNeeded(view);
    const bool dockFocused = shortcutFocusOverride || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    handleWaveShortcuts(dockFocused, fullscreenToggleRequested);

    auto plan = buildWaveContentPlan(wave, view, available);
    auto context =
        makeWaveContext(application_, wave, view, config, plan, available, fullscreenActive, fullscreenToggleRequested);
    WaveComponentSet components;
    prepareWaveComponents(components, context);
    drawWaveContentComponents(components, context);
    commitWaveComponents(components, context);
    wave.suppressZoomSelectionEscapeThisFrame = false;
}

void WaveDockRenderer::handleWaveShortcuts(const bool dockFocused, bool* fullscreenToggleRequested)
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
            *fullscreenToggleRequested = true;
            application_.setStatusMessage("已请求切换波形全屏", false);
        }
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveTogglePauseFollow)) {
        view.autoFollowLatest = !view.autoFollowLatest;
        application_.setStatusMessage(view.autoFollowLatest ? "波形自动跟随已恢复" : "波形自动跟随已暂停", false);
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveFitVisible)) {
        view.fitVisibleWaveformsRequested = true;
        application_.setStatusMessage("已请求适配当前可见波形", false);
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveToggleZoomSelection)) {
        view.zoomSelectionActive = !view.zoomSelectionActive;
        view.zoomSelectionDragging = false;
        application_.setStatusMessage(view.zoomSelectionActive ? "框选放大已开启" : "框选放大已关闭", false);
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
        application_.setStatusMessage(view.fft.enabled ? "FFT 频谱模式已开启" : "FFT 频谱模式已关闭", false);
        return;
    }
    if (waveShortcutPressed(ShortcutAction::WaveClearHistory)) {
        // 核心流程：清空历史使用带修饰键组合，避免裸字母误清实时采样缓存。
        application_.resetWaveHistory();
        application_.setStatusMessage("当前波形历史已清空", false);
    }
}

void WaveDockRenderer::syncWaveViewToLatest()
{
    auto& wave = application_.docks().waveState();
    if (!wave.view.autoFollowLatest) {
        return;
    }

    if (const auto latestTime = wave.buffer.latestTime()) {
        wave.view.viewMaxTime = *latestTime;
        wave.view.viewMinTime = *latestTime - wave.view.visibleDuration;
        wave.view.centerTime = 0.5 * (wave.view.viewMinTime + wave.view.viewMaxTime);
        if (!wave.view.lockVerticalRange && !wave.view.initialized) {
            wave.view.viewMinValue = wave.view.manualVerticalMin;
            wave.view.viewMaxValue = wave.view.manualVerticalMax;
        }
    }
}

} // namespace protoscope::ui
