#include "protoscope/app/application.hpp"
#include "protoscope/plot/wave_state.hpp"
#include "protoscope/ui/icons.hpp"
#include "protoscope/ui/ui_theme.hpp"
#include "protoscope/ui/wave_dock_renderer.hpp"

#include "wave_component.hpp"
#include "wave_detail.hpp"
#include "wave_render_service.hpp"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>

namespace protoscope::ui {

const char* zoomSelectionHelpText(const plot::WaveViewState& view) {
    return view.zoomSelectionAutoExit ? "框选主视图局部放大；框选完成后自动退出。"
                                      : "框选主视图局部放大；框选完成后保留框选模式，需手动退出。";
}

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
    return drawToolbarSectionButton(label, help, false, size);
}

bool drawToolbarToggleButton(const char* label, bool active, const char* help, const ImVec2& size) {
    return drawToolbarSectionButton(label, help, active, size);
}

float calcToolbarButtonWidth(const char* label) {
    const auto textSize = ImGui::CalcTextSize(label);
    const auto& style = ImGui::GetStyle();
    return textSize.x + style.FramePadding.x * 2.0F;
}

std::string buildToolbarButtonHelp(const char* label, const char* help) {
    if (help == nullptr || help[0] == '\0') {
        return std::string(label);
    }
    return std::string(label) + "： " + help;
}

bool drawAdaptiveToolbarButton(const char* fullLabel,
                               const char* shortLabel,
                               const char* help,
                               bool active,
                               bool placeNextOnSameLine = false) {
    const float fullWidth = calcToolbarButtonWidth(fullLabel);
    const float shortWidth = calcToolbarButtonWidth(shortLabel);

    float availableWidth = ImGui::GetContentRegionAvail().x;
    if (shortWidth > availableWidth && ImGui::GetCursorPosX() > 0.0F) {
        ImGui::NewLine();
        availableWidth = ImGui::GetContentRegionAvail().x;
    }

    const bool useFullLabel = fullWidth <= availableWidth || shortWidth > availableWidth;
    const char* visibleLabel = useFullLabel ? fullLabel : shortLabel;
    const float buttonWidth = useFullLabel ? fullWidth : shortWidth;
    const std::string tooltip = buildToolbarButtonHelp(fullLabel, help);
    const bool clicked = drawToolbarSectionButton(visibleLabel, tooltip.c_str(), active, ImVec2(buttonWidth, 0.0F));
    if (placeNextOnSameLine) {
        ImGui::SameLine();
    }
    return clicked;
}

void ensureFftChannelState(plot::WaveDockState& wave) {
    const auto channelCount = wave.buffer.channelCount();
    if (wave.fftChannelEnabled.size() != channelCount) {
        const auto oldSize = wave.fftChannelEnabled.size();
        wave.fftChannelEnabled.resize(channelCount, 0);
        if (oldSize == 0 && channelCount > 0) {
            const auto preferredChannel = (std::min)(wave.view.measurementChannelIndex, channelCount - 1);
            wave.fftChannelEnabled[preferredChannel] = 1;
        }
    }
}

void drawFftToolbarSectionContent(plot::WaveDockState& wave) {
    auto& view = wave.view;
    ensureFftChannelState(wave);

    if (!ImGui::CollapsingHeader("FFT")) {
        return;
    }

    bool enabled = view.fft.enabled;
    if (drawAdaptiveToolbarButton("启用 FFT 频谱模式",
                                  "FFT",
                                  "启用后主图横坐标切换为频率 Hz，输入数据来自当前可视区。",
                                  enabled)) {
        enabled = !enabled;
        view.fft.enabled = enabled;
        if (enabled) {
            view.fftSourceMinTime = view.viewMinTime;
            view.fftSourceMaxTime = view.viewMaxTime;
            view.fftSourceWindowValid = true;
            view.fftViewportInitialized = false;
        } else {
            view.fftSourceWindowValid = false;
            view.fftViewportInitialized = false;
        }
        wave.cachedFftKeyValid = false;
    }

    const char* pointItems[] = {"全部可视样本", "Auto 2^n", "手动", "256", "512", "1024", "2048", "4096", "8192", "16384"};
    int pointIndex = static_cast<int>(view.fft.pointCount);
    if (ImGui::Combo("点数", &pointIndex, pointItems, IM_ARRAYSIZE(pointItems))) {
        view.fft.pointCount = static_cast<plot::WaveFftPointCount>(pointIndex);
        wave.cachedFftKeyValid = false;
    }
    addItemHelp("点数 N 决定频率分辨率：Δf = Fs / N。全部可视样本和手动点数允许非 2^n，由 pocketfft 计算。");

    int manualPointCount = static_cast<int>(view.fft.manualPointCount);
    if (view.fft.pointCount == plot::WaveFftPointCount::Manual
        && ImGui::InputInt("手动点数", &manualPointCount, 128, 1024)) {
        // 核心流程：手动 N 强制使用用户输入的点数，样本不足时由 FFT 计算层给出不足提示，不做隐式补零。
        view.fft.manualPointCount = static_cast<std::size_t>((std::clamp)(manualPointCount, 16, 16384));
        wave.cachedFftKeyValid = false;
    }

    int autoMaxPointCount = static_cast<int>(view.fft.autoMaxPointCount);
    if (view.fft.pointCount == plot::WaveFftPointCount::Auto
        && ImGui::InputInt("Auto 上限", &autoMaxPointCount, 256, 1024)) {
        view.fft.autoMaxPointCount = static_cast<std::size_t>((std::clamp)(autoMaxPointCount, 256, 16384));
        wave.cachedFftKeyValid = false;
    }

    if (ImGui::Button("刷新输入窗口")) {
        view.fftSourceMinTime = view.viewMinTime;
        view.fftSourceMaxTime = view.viewMaxTime;
        view.fftSourceWindowValid = true;
        view.fftViewportInitialized = false;
        wave.cachedFftKeyValid = false;
    }
    addItemHelp("重新使用当前时域主视图范围作为 FFT 输入；频域缩放不会改变这个输入窗口。");
    ImGui::SameLine();
    if (ImGui::Button("显示全部频谱")) {
        view.fftFitAllRequested = true;
    }
    addItemHelp("重置频率、幅值和相位轴范围，不改变 FFT 输入窗口。");

    const char* windowItems[] = {"Rectangular", "Hann", "Hamming", "Blackman-Harris"};
    int windowIndex = static_cast<int>(view.fft.window);
    if (ImGui::Combo("窗函数", &windowIndex, windowItems, IM_ARRAYSIZE(windowItems))) {
        view.fft.window = static_cast<plot::WaveFftWindow>(windowIndex);
        wave.cachedFftKeyValid = false;
    }
    addItemHelp("默认 Hann，适合常规频谱观察；Rectangular 适合整周期采样，Blackman-Harris 旁瓣更低。");

    const char* magnitudeItems[] = {"幅值", "dB"};
    int magnitudeIndex = static_cast<int>(view.fft.magnitudeMode);
    if (ImGui::Combo("幅值模式", &magnitudeIndex, magnitudeItems, IM_ARRAYSIZE(magnitudeItems))) {
        view.fft.magnitudeMode = static_cast<plot::WaveFftMagnitudeMode>(magnitudeIndex);
        wave.cachedFftKeyValid = false;
    }

    const char* fundamentalItems[] = {"自动峰值", "手动输入"};
    int fundamentalIndex = static_cast<int>(view.fft.fundamentalMode);
    if (ImGui::Combo("基波", &fundamentalIndex, fundamentalItems, IM_ARRAYSIZE(fundamentalItems))) {
        view.fft.fundamentalMode = static_cast<plot::WaveFftFundamentalMode>(fundamentalIndex);
        wave.cachedFftKeyValid = false;
    }
    if (view.fft.fundamentalMode == plot::WaveFftFundamentalMode::Manual
        && ImGui::InputDouble("手动基波 Hz", &view.fft.manualFundamentalHz, 1.0, 10.0, "%.6g")) {
        view.fft.manualFundamentalHz = (std::max)(0.0, view.fft.manualFundamentalHz);
        wave.cachedFftKeyValid = false;
    }

    if (ImGui::TreeNode("Legend")) {
        if (drawAdaptiveToolbarButton("显示频谱图例",
                                      "例",
                                      "显示或隐藏 FFT 幅值/相位图例；不会持久化每条曲线的临时隐藏状态。",
                                      view.showFftLegend)) {
            view.showFftLegend = !view.showFftLegend;
        }
        ImGui::TreePop();
    }

    if (ImGui::TreeNode("参与通道")) {
        if (ImGui::Button("启用全部通道")) {
            std::fill(wave.fftChannelEnabled.begin(), wave.fftChannelEnabled.end(), 1);
            wave.cachedFftKeyValid = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("仅当前测量通道")) {
            std::fill(wave.fftChannelEnabled.begin(), wave.fftChannelEnabled.end(), 0);
            if (!wave.fftChannelEnabled.empty()) {
                const auto channelIndex = (std::min)(view.measurementChannelIndex, wave.fftChannelEnabled.size() - 1);
                wave.fftChannelEnabled[channelIndex] = 1;
            }
            wave.cachedFftKeyValid = false;
        }
        for (std::size_t channelIndex = 0; channelIndex < wave.fftChannelEnabled.size(); ++channelIndex) {
            const bool channelEnabled = wave.fftChannelEnabled[channelIndex] != 0;
            std::string label = "CH" + std::to_string(channelIndex + 1);
            if (const auto spec = wave.buffer.channelSpec(channelIndex); spec.has_value() && !spec->label.empty()) {
                label = spec->label;
            }
            char shortLabelBuffer[16]{};
            std::snprintf(shortLabelBuffer, sizeof(shortLabelBuffer), "C%zu", channelIndex + 1);
            if (drawAdaptiveToolbarButton(label.c_str(),
                                          shortLabelBuffer,
                                          "切换该通道是否参与 FFT 计算。",
                                          channelEnabled,
                                          channelIndex + 1 < wave.fftChannelEnabled.size())) {
                wave.fftChannelEnabled[channelIndex] = channelEnabled ? 0 : 1;
                wave.cachedFftKeyValid = false;
            }
        }
        ImGui::NewLine();
        ImGui::TreePop();
    }

    const auto& fftFrame = wave.cachedFftFrame;
    ImGui::SeparatorText("频率换算");
    ImGui::Text("Fs: %s", formatMetricText(view.sampleFrequencyHz, "Hz").c_str());
    if (view.fftSourceWindowValid) {
        ImGui::Text("输入窗口: %s ~ %s",
                    formatMetricText(view.fftSourceMinTime, "s").c_str(),
                    formatMetricText(view.fftSourceMaxTime, "s").c_str());
    }
    if (fftFrame.valid) {
        ImGui::Text("N: %zu", fftFrame.pointCount);
        ImGui::Text("N类型: %s", (fftFrame.pointCount & (fftFrame.pointCount - 1U)) == 0U ? "2^n" : "任意点数");
        ImGui::Text("Δf: %s/bin", formatMetricText(fftFrame.frequencyResolutionHz, "Hz").c_str());
        ImGui::Text("显示范围: 0 ~ %s", formatMetricText(fftFrame.maxFrequencyHz, "Hz").c_str());
        ImGui::Text("当前可视区样本: %zu", fftFrame.visibleSampleCount);
        ImGui::Text("实际使用样本: %zu", fftFrame.usedSampleCount);
        if (fftFrame.fundamentalHz.has_value()) {
            ImGui::Text("基波: %s", formatMetricText(*fftFrame.fundamentalHz, "Hz").c_str());
        }
    } else {
        ImGui::TextUnformatted(fftFrame.message.empty() ? "启用后显示 N、Δf 和频率范围。" : fftFrame.message.c_str());
    }
}

class WaveFftToolbarSection final : public IWaveToolbarSection {
public:
    std::string_view id() const override { return "wave_fft_toolbar"; }

    void draw(app::Application&, plot::WaveDockState& wave) override {
        drawFftToolbarSectionContent(wave);
    }
};

static_assert(std::is_base_of_v<IWaveToolbarSection, WaveFftToolbarSection>, "WaveFftToolbarSection 必须通过工具栏段基类接入");

void drawWaveToolbar(app::Application& application,
                     plot::WaveDockState& wave,
                     const plot::ViewConfig& config,
                     const plot::WaveDisplayData& displayData) {
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
                                    zoomSelectionHelpText(view),
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

    ImGui::SeparatorText("主视图控制");
    // 核心流程：快捷操作改为自适应流式按钮，避免 Child/Table 吃满纵向空间并引入额外作用域配对风险。
    if (drawAdaptiveToolbarButton(view.autoFollowLatest ? "跟随最新数据" : "暂停跟随",
                                  "跟/停",
                                  "切换自动跟随最新数据。关闭后当前视口会停留在手动浏览位置。",
                                  view.autoFollowLatest,
                                  true)) {
        view.autoFollowLatest = !view.autoFollowLatest;
    }
    if (drawAdaptiveToolbarButton(view.lockVerticalRange ? "纵轴锁定" : "纵轴自动",
                                  "纵/自",
                                  "锁定或释放纵轴范围。锁定后使用手动纵轴最小/最大值。",
                                  view.lockVerticalRange,
                                  true)) {
        view.lockVerticalRange = !view.lockVerticalRange;
    }
    if (drawAdaptiveToolbarButton(view.showCursors ? "显示游标" : "隐藏游标",
                                  "游",
                                  "显示或隐藏测量游标。游标隐藏时不会显示游标读数。",
                                  view.showCursors,
                                  true)) {
        view.showCursors = !view.showCursors;
    }
    if (drawAdaptiveToolbarButton("C1 到视窗",
                                  "C1",
                                  "仅移动 C1 游标到当前主视窗，不改变当前视窗范围。",
                                  false,
                                  true)) {
        placeCursorInViewport(view, config, displayData, 0, 0.5);
    }
    if (drawAdaptiveToolbarButton("C2 到视窗",
                                  "C2",
                                  "仅移动 C2 游标到当前主视窗，不改变当前视窗范围。",
                                  false,
                                  true)) {
        placeCursorInViewport(view, config, displayData, 1, 0.5);
    }
    if (drawAdaptiveToolbarButton("C1+C2 到视窗",
                                  "C1+C2",
                                  "仅移动双游标到当前主视窗，不改变当前视窗范围。",
                                  false,
                                  true)) {
        placeCursorPairInViewport(view, config, displayData);
    }
    if (drawAdaptiveToolbarButton(view.showHoverReadout ? "显示悬停读数" : "隐藏悬停读数",
                                  "读",
                                  "显示或隐藏鼠标悬停读数。开启后鼠标靠近曲线会显示最近采样点。",
                                  view.showHoverReadout,
                                  true)) {
        view.showHoverReadout = !view.showHoverReadout;
    }
    if (drawAdaptiveToolbarButton(PROTOSCOPE_ICON_MAGNIFYING_GLASS " 框选放大",
                                  "框",
                                  zoomSelectionHelpText(view),
                                  view.zoomSelectionActive,
                                  true)) {
        view.zoomSelectionActive = !view.zoomSelectionActive;
        view.zoomSelectionDragging = false;
    }
    if (drawAdaptiveToolbarButton(PROTOSCOPE_ICON_EXPAND " 显示全部",
                                  "全",
                                  "适配当前可见波形到完整视图。",
                                  false,
                                  true)) {
        view.fitVisibleWaveformsRequested = true;
    }
    if (drawAdaptiveToolbarButton("清空历史",
                                  "清",
                                  "清空当前波形历史缓存；不会修改协议脚本或串口连接状态。",
                                  false,
                                  true)) {
        application.resetWaveHistory();
    }
    if (drawAdaptiveToolbarButton("折叠工具栏",
                                  "收",
                                  "收起为窄按钮列，保留常用操作入口。",
                                  false)) {
        wave.toolsCollapsed = true;
    }

    ImGui::Spacing();
    WaveFftToolbarSection fftToolbarSection;
    std::array<IWaveToolbarSection*, 1> toolbarSections{&fftToolbarSection};
    for (auto* section : toolbarSections) {
        section->draw(application, wave);
    }

    ImGui::Spacing();
    if (ImGui::CollapsingHeader("视图", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (drawAdaptiveToolbarButton("交互后暂停跟随", "停", "拖动、缩放或手动浏览后自动关闭跟随，避免视口被新数据拉回末尾。", view.pauseAutoFollowOnInteraction, true)) {
            view.pauseAutoFollowOnInteraction = !view.pauseAutoFollowOnInteraction;
        }
        if (drawAdaptiveToolbarButton("稀疏时显示点", "点", "样本较少时显示采样点，便于观察离散数据。", view.showPointsWhenSparse, true)) {
            view.showPointsWhenSparse = !view.showPointsWhenSparse;
        }
        if (drawAdaptiveToolbarButton("显示坐标轴标签", "轴", "显示或隐藏主波形图的时间轴/数值轴标签。", view.showAxisLabels, true)) {
            view.showAxisLabels = !view.showAxisLabels;
        }
        if (drawAdaptiveToolbarButton("显示图例", "例", "显示或隐藏顶部通道图例栏；不会持久化每个通道的临时隐藏状态。", view.showChannelLegend)) {
            view.showChannelLegend = !view.showChannelLegend;
        }
        ImGui::TextUnformatted("可视时长");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputDouble("##visible_duration", &view.visibleDuration, minVisibleTimeSpan, minVisibleTimeSpan * 10.0, "%.6f");
        addItemHelp("当前主视图横向可见时间范围，单位与波形时间轴一致。");
        view.visibleDuration = (std::max)(view.visibleDuration, minVisibleTimeSpan);

        ImGui::TextUnformatted("最小可视跨度");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputDouble("##min_visible_span", &view.minVisibleTimeSpan, 0.001, 0.01, "%.6f");
        addItemHelp("限制横向缩放的最小时长，防止缩放到过小范围。");
        view.minVisibleTimeSpan = (std::max)(view.minVisibleTimeSpan, 1e-6);
        view.visibleDuration = (std::max)(view.visibleDuration, view.minVisibleTimeSpan);
    }

    if (ImGui::CollapsingHeader("游标", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (drawAdaptiveToolbarButton("测量浮层", "测", "显示游标间隔、测量通道和读数摘要。", view.showMeasurementOverlay, true)) {
            view.showMeasurementOverlay = !view.showMeasurementOverlay;
        }
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
        if (drawAdaptiveToolbarButton(view.cursorIntervalLocked ? "锁定游标间隔" : "解锁游标间隔",
                                      view.cursorIntervalLocked ? "锁" : "解",
                                      "锁定后拖动单个游标会保持两个游标之间的时间间隔。",
                                      view.cursorIntervalLocked)) {
            view.cursorIntervalLocked = !view.cursorIntervalLocked;
            view.lockedCursorInterval = std::abs(view.cursors[1].time - view.cursors[0].time);
        }
    }

    if (ImGui::CollapsingHeader("渲染", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (drawAdaptiveToolbarButton("磷光辉光", "辉", "开启后使用类似示波器余辉的曲线显示效果。", view.phosphorGlowEnabled)) {
            view.phosphorGlowEnabled = !view.phosphorGlowEnabled;
        }
        ImGui::Text("渲染点: %zu / 源样本: %zu", view.lastRenderPointCount, view.lastRenderSourceSampleCount);
        addItemHelp("本帧实际参与绘制的点数与原始显示样本数，用于判断降采样是否生效。");
        char frequencyBuffer[64]{};
        std::strncpy(frequencyBuffer, view.sampleFrequencyInput.c_str(), sizeof(frequencyBuffer) - 1);

        ImGui::TextUnformatted("发送频率 Hz");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputText("##sample_frequency", frequencyBuffer, sizeof(frequencyBuffer))) {
            view.sampleFrequencyInput = frequencyBuffer;
            wave_detail::applyFrequencyInput(view);
        }
        addItemHelp("用于把样本序号换算成时间轴的采样频率。");

        ImGui::TextUnformatted("余辉时间窗");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputDouble("##persistence_window", &view.persistenceWindow, minVisibleTimeSpan, minVisibleTimeSpan * 10.0, "%.6f");
        addItemHelp("余辉模式保留历史亮度的时间窗口。");
        view.persistenceWindow = (std::max)(view.persistenceWindow, minVisibleTimeSpan);

        ImGui::TextUnformatted("降采样启动倍数");
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputDouble("##downsample_multiplier", &view.downsampleStartMultiplier, 0.1, 0.5, "%.2f");
        addItemHelp("可见点数超过渲染预算一定倍数后开始降采样，数值越大越晚触发。");
        view.downsampleStartMultiplier = (std::max)(view.downsampleStartMultiplier, 1.0);

        ImGui::TextUnformatted("辉光强度");
        ImGui::SetNextItemWidth(-1.0F);
        const double glowMin = 0.2;
        const double glowMax = 2.5;
        ImGui::SliderScalar("##glow_intensity", ImGuiDataType_Double, &view.glowIntensity, &glowMin, &glowMax, "%.2f");
        addItemHelp("调整磷光辉光的亮度强度，仅影响显示效果。");

        if (view.lockVerticalRange) {
            ImGui::TextUnformatted("纵轴范围");
            const float verticalInputsWidth = ImGui::GetContentRegionAvail().x;
            if (verticalInputsWidth >= 240.0F) {
                ImGui::SetNextItemWidth((verticalInputsWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5F);
                ImGui::InputDouble("##manual_vertical_min", &view.manualVerticalMin, 0.1, 1.0, "%.6f");
                addItemHelp("纵轴锁定时使用的显示下限。");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.0F);
                ImGui::InputDouble("##manual_vertical_max", &view.manualVerticalMax, 0.1, 1.0, "%.6f");
                addItemHelp("纵轴锁定时使用的显示上限。");
            } else {
                ImGui::SetNextItemWidth(-1.0F);
                ImGui::InputDouble("##manual_vertical_min", &view.manualVerticalMin, 0.1, 1.0, "%.6f");
                addItemHelp("纵轴锁定时使用的显示下限。");
                ImGui::SetNextItemWidth(-1.0F);
                ImGui::InputDouble("##manual_vertical_max", &view.manualVerticalMax, 0.1, 1.0, "%.6f");
                addItemHelp("纵轴锁定时使用的显示上限。");
            }
        }
        if (!view.sampleFrequencyError.empty()) {
            ImGui::TextColored(ImVec4(1.0F, 0.35F, 0.25F, 1.0F), "%s", view.sampleFrequencyError.c_str());
        }
    }

    if (ImGui::CollapsingHeader("概览设置", ImGuiTreeNodeFlags_DefaultOpen)) {
        int maxSamplesInput = static_cast<int>((std::min)(view.overviewMaxSamples, static_cast<std::size_t>(1000000)));
        ImGui::TextUnformatted("概览最大样本/通道");
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputInt("##overview_max_samples", &maxSamplesInput, 1000, 10000)) {
            view.overviewMaxSamples = static_cast<std::size_t>((std::max)(0, maxSamplesInput));
        }
        addItemHelp("限制概览图每个通道保留的最大样本数，避免概览绘制过重。");
    }
}

} // namespace protoscope::ui
