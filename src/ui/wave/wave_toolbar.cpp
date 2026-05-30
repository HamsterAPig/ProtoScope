#include "protoscope/app/application.hpp"
#include "protoscope/plot/wave_state.hpp"
#include "protoscope/ui/icons.hpp"
#include "protoscope/ui/wave_dock_renderer.hpp"

#include "wave_detail.hpp"

#include <imgui.h>

#include <algorithm>
#include <cstring>

namespace protoscope::ui {

void wave_detail::applyFrequencyInput(plot::WaveViewState& view) {
    const auto parsed = plot::parseSampleFrequencyText(view.sampleFrequencyInput);
    if (parsed.accepted) {
        view.sampleFrequencyHz = parsed.valueHz;
        view.sampleFrequencyError.clear();
    } else {
        view.sampleFrequencyError = parsed.error;
    }
}

// 核心流程：按钮帮助统一走 tooltip，避免工具栏里重复散落说明文字。
void addItemHelp(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    ImGui::SetItemTooltip("%s", text);
}

bool drawToolbarActionButton(const char* label, const char* help, const ImVec2& size) {
    const bool clicked = ImGui::Button(label, size);
    addItemHelp(help);
    return clicked;
}

bool drawToolbarToggleButton(const char* label, bool active, const char* help, const ImVec2& size) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool clicked = ImGui::Button(label, size);
    if (active) {
        ImGui::PopStyleColor();
    }
    addItemHelp(help);
    return clicked;
}

bool drawToolbarCheckbox(const char* label, bool* value, const char* help) {
    const bool changed = ImGui::Checkbox(label, value);
    addItemHelp(help);
    return changed;
}

void drawWaveToolbar(app::Application& application, plot::WaveDockState& wave) {
    auto& view = wave.view;
    const double minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
    if (view.visibleDuration <= 0.0) {
        view.visibleDuration = minVisibleTimeSpan;
    }
    view.visibleDuration = (std::max)(view.visibleDuration, minVisibleTimeSpan);
    if (view.persistenceWindow <= 0.0) {
        view.persistenceWindow = minVisibleTimeSpan;
    }

    if (wave.toolsCollapsed) {
        const ImVec2 collapsedButtonSize(28.0F, 0.0F);
        if (drawToolbarToggleButton(view.autoFollowLatest ? "跟" : "停",
                                    view.autoFollowLatest,
                                    "切换自动跟随最新数据。关闭后当前视口会停留在手动浏览位置。",
                                    collapsedButtonSize)) {
            view.autoFollowLatest = !view.autoFollowLatest;
        }
        if (drawToolbarToggleButton(view.lockVerticalRange ? "锁" : "轴",
                                    view.lockVerticalRange,
                                    "锁定或释放纵轴范围。锁定后使用手动纵轴最小/最大值。",
                                    collapsedButtonSize)) {
            view.lockVerticalRange = !view.lockVerticalRange;
        }
        if (drawToolbarToggleButton(view.showCursors ? "游" : "标",
                                    view.showCursors,
                                    "显示或隐藏测量游标。游标隐藏时不会显示游标读数。",
                                    collapsedButtonSize)) {
            view.showCursors = !view.showCursors;
        }
        if (drawToolbarToggleButton(view.showHoverReadout ? "读" : "点",
                                    view.showHoverReadout,
                                    "显示或隐藏鼠标悬停读数。开启后鼠标靠近曲线会显示最近采样点。",
                                    collapsedButtonSize)) {
            view.showHoverReadout = !view.showHoverReadout;
        }
        if (drawToolbarToggleButton(PROTOSCOPE_ICON_MAGNIFYING_GLASS,
                                    view.zoomSelectionActive,
                                    "框选主视图局部放大；框选完成后自动退出。",
                                    collapsedButtonSize)) {
            view.zoomSelectionActive = !view.zoomSelectionActive;
            view.zoomSelectionDragging = false;
        }
        if (drawToolbarActionButton(PROTOSCOPE_ICON_EXPAND,
                                    "适配当前可见波形到完整视图。",
                                    collapsedButtonSize)) {
            view.fitVisibleWaveformsRequested = true;
        }
        if (drawToolbarActionButton("清",
                                    "清空当前波形历史缓存；不会修改协议脚本或串口连接状态。",
                                    collapsedButtonSize)) {
            application.resetWaveHistory();
        }
        if (drawToolbarActionButton(">", "展开右侧工具栏。", collapsedButtonSize)) {
            wave.toolsCollapsed = false;
        }
        return;
    }

    ImGui::TextUnformatted("快捷操作");
    ImGui::Separator();
    // 核心流程：右侧工具栏先放高频动作，细项参数继续留在下方分组，避免工具区变成长表单。
    if (ImGui::BeginTable("##wave_toolbar_quick_actions", 2, ImGuiTableFlags_SizingStretchSame)) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (drawToolbarToggleButton(view.autoFollowLatest ? "跟随中" : "已暂停",
                                    view.autoFollowLatest,
                                    "切换自动跟随最新数据。关闭后当前视口会停留在手动浏览位置。",
                                    ImVec2(-1.0F, 0.0F))) {
            view.autoFollowLatest = !view.autoFollowLatest;
        }
        ImGui::TableSetColumnIndex(1);
        if (drawToolbarToggleButton(view.lockVerticalRange ? "纵轴锁定" : "纵轴自动",
                                    view.lockVerticalRange,
                                    "锁定或释放纵轴范围。锁定后使用手动纵轴最小/最大值。",
                                    ImVec2(-1.0F, 0.0F))) {
            view.lockVerticalRange = !view.lockVerticalRange;
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (drawToolbarToggleButton(view.showCursors ? "游标开" : "游标关",
                                    view.showCursors,
                                    "显示或隐藏测量游标。游标隐藏时不会显示游标读数。",
                                    ImVec2(-1.0F, 0.0F))) {
            view.showCursors = !view.showCursors;
        }
        ImGui::TableSetColumnIndex(1);
        if (drawToolbarToggleButton(view.showHoverReadout ? "读数开" : "读数关",
                                    view.showHoverReadout,
                                    "显示或隐藏鼠标悬停读数。开启后鼠标靠近曲线会显示最近采样点。",
                                    ImVec2(-1.0F, 0.0F))) {
            view.showHoverReadout = !view.showHoverReadout;
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (drawToolbarToggleButton(PROTOSCOPE_ICON_MAGNIFYING_GLASS " 框选放大",
                                    view.zoomSelectionActive,
                                    "框选主视图局部放大；框选完成后自动退出。",
                                    ImVec2(-1.0F, 0.0F))) {
            view.zoomSelectionActive = !view.zoomSelectionActive;
            view.zoomSelectionDragging = false;
        }
        ImGui::TableSetColumnIndex(1);
        if (drawToolbarActionButton(PROTOSCOPE_ICON_EXPAND " 显示全部",
                                    "适配当前可见波形到完整视图。",
                                    ImVec2(-1.0F, 0.0F))) {
            view.fitVisibleWaveformsRequested = true;
        }

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (drawToolbarActionButton("清空历史",
                                    "清空当前波形历史缓存；不会修改协议脚本或串口连接状态。",
                                    ImVec2(-1.0F, 0.0F))) {
            application.resetWaveHistory();
        }
        ImGui::TableSetColumnIndex(1);
        if (drawToolbarActionButton("折叠工具栏", "收起为窄按钮列，保留常用操作入口。", ImVec2(-1.0F, 0.0F))) {
            wave.toolsCollapsed = true;
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("视图", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawToolbarCheckbox("交互后暂停跟随",
                            &view.pauseAutoFollowOnInteraction,
                            "拖动、缩放或手动浏览后自动关闭跟随，避免视口被新数据拉回末尾。");
        drawToolbarCheckbox("稀疏时显示点", &view.showPointsWhenSparse, "样本较少时显示采样点，便于观察离散数据。");
        drawToolbarCheckbox("显示坐标轴标签", &view.showAxisLabels, "显示或隐藏主波形图的时间轴/数值轴标签。");
        if (ImGui::BeginTable("##view_controls", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("可视时长");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputDouble("##visible_duration", &view.visibleDuration, minVisibleTimeSpan, minVisibleTimeSpan * 10.0, "%.6f");
            addItemHelp("当前主视图横向可见时间范围，单位与波形时间轴一致。");
            view.visibleDuration = (std::max)(view.visibleDuration, minVisibleTimeSpan);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("最小可视跨度");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputDouble("##min_visible_span", &view.minVisibleTimeSpan, 0.001, 0.01, "%.6f");
            addItemHelp("限制横向缩放的最小时长，防止缩放到过小范围。");
            view.minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
            view.visibleDuration = (std::max)(view.visibleDuration, view.minVisibleTimeSpan);
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("游标", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawToolbarCheckbox("测量浮层", &view.showMeasurementOverlay, "显示游标间隔、测量通道和读数摘要。");
        const bool smartSnapMode = view.cursorSnapMode == plot::WaveCursorSnapMode::SmartSnap;
        if (drawToolbarActionButton(smartSnapMode ? "智能吸附" : "按键吸附",
                                    "切换游标吸附触发方式：智能吸附会自动贴近边沿/极值，按键吸附需按住 Shift 或 Ctrl。")) {
            view.cursorSnapMode = smartSnapMode ? plot::WaveCursorSnapMode::ModifierSnap : plot::WaveCursorSnapMode::SmartSnap;
        }
        int scopeIndex = view.cursorSnapScope == plot::WaveCursorSnapScope::AllChannels ? 0 : 1;
        const char* scopeItems[] = {"全部波形", "当前激活波形"};
        if (ImGui::Combo("吸附范围", &scopeIndex, scopeItems, IM_ARRAYSIZE(scopeItems))) {
            view.cursorSnapScope = scopeIndex == 0 ? plot::WaveCursorSnapScope::AllChannels : plot::WaveCursorSnapScope::ActiveChannel;
        }
        addItemHelp("选择游标吸附时搜索采样点的通道范围。");
        if (drawToolbarCheckbox(view.cursorIntervalLocked ? "锁定游标间隔" : "解锁游标间隔",
                                &view.cursorIntervalLocked,
                                "锁定后拖动单个游标会保持两个游标之间的时间间隔。")) {
            view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
        }
    }

    if (ImGui::CollapsingHeader("渲染", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawToolbarCheckbox("磷光辉光", &view.phosphorGlowEnabled, "开启后使用类似示波器余辉的曲线显示效果。");
        ImGui::Text("渲染点: %zu / 源样本: %zu", view.lastRenderPointCount, view.lastRenderSourceSampleCount);
        addItemHelp("本帧实际参与绘制的点数与原始显示样本数，用于判断降采样是否生效。");
        char frequencyBuffer[64]{};
        std::strncpy(frequencyBuffer, view.sampleFrequencyInput.c_str(), sizeof(frequencyBuffer) - 1);
        if (ImGui::BeginTable("##render_controls", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("发送频率 Hz");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::InputText("##sample_frequency", frequencyBuffer, sizeof(frequencyBuffer))) {
                view.sampleFrequencyInput = frequencyBuffer;
                wave_detail::applyFrequencyInput(view);
            }
            addItemHelp("用于把样本序号换算成时间轴的采样频率。");

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("余辉时间窗");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputDouble("##persistence_window", &view.persistenceWindow, minVisibleTimeSpan, minVisibleTimeSpan * 10.0, "%.6f");
            addItemHelp("余辉模式保留历史亮度的时间窗口。");
            view.persistenceWindow = (std::max)(view.persistenceWindow, minVisibleTimeSpan);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("降采样启动倍数");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputDouble("##downsample_multiplier", &view.downsampleStartMultiplier, 0.1, 0.5, "%.2f");
            addItemHelp("可见点数超过渲染预算一定倍数后开始降采样，数值越大越晚触发。");
            view.downsampleStartMultiplier = (std::max)(view.downsampleStartMultiplier, 1.0);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("辉光强度");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0F);
            const double glowMin = 0.2;
            const double glowMax = 2.5;
            ImGui::SliderScalar("##glow_intensity", ImGuiDataType_Double, &view.glowIntensity, &glowMin, &glowMax, "%.2f");
            addItemHelp("调整磷光辉光的亮度强度，仅影响显示效果。");

            if (view.lockVerticalRange) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("纵轴最小");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0F);
                ImGui::InputDouble("##manual_vertical_min", &view.manualVerticalMin, 0.1, 1.0, "%.6f");
                addItemHelp("纵轴锁定时使用的显示下限。");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted("纵轴最大");
                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(-1.0F);
                ImGui::InputDouble("##manual_vertical_max", &view.manualVerticalMax, 0.1, 1.0, "%.6f");
                addItemHelp("纵轴锁定时使用的显示上限。");
            }
            ImGui::EndTable();
        }
        if (!view.sampleFrequencyError.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.25F, 1.0F), "%s", view.sampleFrequencyError.c_str());
        }
    }

    if (ImGui::CollapsingHeader("概览设置", ImGuiTreeNodeFlags_DefaultOpen)) {
        int maxSamplesInput = static_cast<int>((std::min)(view.overviewMaxSamples, static_cast<std::size_t>(1000000)));
        if (ImGui::BeginTable("##overview_controls", 2, ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted("概览最大样本/通道");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::InputInt("##overview_max_samples", &maxSamplesInput, 1000, 10000)) {
                view.overviewMaxSamples = static_cast<std::size_t>((std::max)(0, maxSamplesInput));
            }
            addItemHelp("限制概览图每个通道保留的最大样本数，避免概览绘制过重。");
            ImGui::EndTable();
        }
    }
}


} // namespace protoscope::ui
