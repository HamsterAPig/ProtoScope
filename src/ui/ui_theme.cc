#include "protoscope/ui/ui_theme.hpp"
#include "protoscope/plot/wave_overview_color.hpp"

#include <implot.h>
#include <imgui_internal.h>
#include <limits>

namespace protoscope::ui {

namespace {

    ImVec4 rgb8(const int red, const int green, const int blue, const float alpha = 1.0F)
    {
        constexpr float kColorScale = 1.0F / 255.0F;
        return ImVec4(static_cast<float>(red) * kColorScale,
                      static_cast<float>(green) * kColorScale,
                      static_cast<float>(blue) * kColorScale,
                      alpha);
    }

    plot::OverviewColor asColor(ImVec4 c) { return {c.x, c.y, c.z, c.w}; }

    // 普通状态只做确定性叠色，不按文字对比度搜索；保留自定义透明表面的 alpha。
    ImVec4 surfaceTint(const UiThemeDefinition& d, ImVec4 accent, float strength)
    {
        const auto composite = [](ImVec4 front, ImVec4 back) {
            const float remaining = back.w * (1.F - front.w);
            const float alpha = front.w + remaining;
            if (alpha <= 0.F) return ImVec4(0, 0, 0, 0);
            return ImVec4((front.x * front.w + back.x * remaining) / alpha,
                          (front.y * front.w + back.y * remaining) / alpha,
                          (front.z * front.w + back.z * remaining) / alpha, alpha);
        };
        const auto base = composite(d.ui.panelBackgroundAlt, d.ui.panelBackground);
        accent.w *= strength;
        return composite(accent, base);
    }

    void finishPreset(UiThemeDefinition& d, bool light)
    {
        auto& w = d.wave;
        const auto& u = d.ui;
        w.statusOverlayBackground = w.legendOverlayBackground = w.measurementOverlayBackground = u.panelBackground;
        w.statusOverlayBorder = w.legendOverlayBorder = w.measurementOverlayBorder = u.panelBorder;
        w.statusOverlayText = w.legendOverlayTextPrimary = w.measurementOverlayTitle = u.textStrong;
        w.channelLabel = w.splitChannelLabel = w.bitLabel = w.legendOverlayTextSecondary = u.textMuted;
        w.channelSeparator = u.panelBorder;
        w.legendOverlayRowHover = surfaceTint(d, u.accent, .10F);
        w.legendOverlayRowActive = surfaceTint(d, u.accent, .18F);
        w.legendOverlayRowActiveBorder = w.measurementOverlayAccent = u.accent;
        w.measurementChipBackground = u.panelBackgroundAlt;
        w.measurementChipBorder = u.panelBorder;
        w.measurementChipLabel = u.textMuted;
        w.measurementChipValue = u.textStrong;
        w.lightPersistence = light;
        w.channelPalette = light
            ? std::vector<ImVec4>{rgb8(15, 120, 65), rgb8(0, 107, 173), rgb8(148, 103, 0), rgb8(125, 60, 186),
                                 rgb8(194, 43, 62), rgb8(0, 120, 117), rgb8(181, 79, 0), rgb8(50, 85, 167)}
            : std::vector<ImVec4>{rgb8(61, 222, 133), rgb8(57, 195, 246), rgb8(250, 197, 83), rgb8(190, 135, 250),
                                 rgb8(250, 112, 125), rgb8(65, 218, 200), rgb8(255, 156, 72), rgb8(126, 179, 255)};
        w.cursorPalette = light
            ? std::vector<ImVec4>{rgb8(137, 91, 0), rgb8(0, 99, 145), rgb8(44, 91, 153), rgb8(177, 48, 71),
                                 rgb8(22, 113, 54), rgb8(153, 41, 106), rgb8(79, 91, 108)}
            : std::vector<ImVec4>{rgb8(255, 214, 111), rgb8(105, 216, 242), rgb8(130, 175, 237), rgb8(251, 132, 155),
                                 rgb8(114, 215, 139), rgb8(221, 143, 215), rgb8(202, 212, 225)};
    }

    const UiThemeDefinition& professionalDarkTheme()
    {
        static const UiThemeDefinition definition = [] {
            UiThemeDefinition d;
            d.name = "石墨青绿";
            // 石墨灰按宿主、内容、浮层分层，青绿仅用于操作与选中身份。
            d.ui.accent = rgb8(85, 166, 155);
            d.ui.accentMuted = rgb8(85, 166, 155, .16F);
            d.ui.success = rgb8(116, 180, 144);
            d.ui.warning = rgb8(202, 174, 112);
            d.ui.danger = rgb8(210, 139, 143);
            d.ui.textStrong = rgb8(216, 223, 225);
            d.ui.appBackground = rgb8(25, 29, 32);
            d.ui.panelBackground = rgb8(33, 38, 42);
            d.ui.panelBackgroundAlt = rgb8(42, 48, 53);
            d.ui.panelBorder = rgb8(65, 75, 81);
            d.ui.controlBorder = rgb8(80, 92, 100);
            d.ui.floatingBackground = rgb8(43, 50, 56);
            d.ui.floatingBorder = rgb8(95, 109, 118);
            d.ui.tabBackground = rgb8(36, 42, 46);
            d.ui.tabHovered = rgb8(51, 71, 72);
            d.ui.tabSelected = rgb8(43, 70, 68);
            d.ui.tabDimmed = rgb8(29, 34, 38);
            d.ui.tabDimmedSelected = rgb8(45, 55, 59);
            d.ui.tabSelectedOverline = d.ui.accent;
            d.ui.tabDimmedSelectedOverline = rgb8(102, 130, 130);
            d.ui.textMuted = rgb8(158, 170, 177);
            d.ui.genericPlotBackground = d.wave.plotBackground = rgb8(23, 28, 31);
            d.wave.gridMajor = rgb8(112, 118, 120, .16F);
            d.wave.gridMinorTick = rgb8(124, 130, 132, .24F);
            d.wave.gridCenter = rgb8(140, 148, 150, .28F);
            finishPreset(d, false);
            d.wave.channelPalette = {rgb8(116, 190, 149), rgb8(113, 177, 204), rgb8(208, 180, 117), rgb8(174, 148, 202),
                                     rgb8(210, 142, 151), rgb8(112, 186, 177), rgb8(211, 161, 120), rgb8(143, 167, 207)};
            d.wave.cursorPalette = {rgb8(208, 183, 128), rgb8(120, 185, 196), rgb8(144, 170, 207), rgb8(209, 148, 161),
                                    rgb8(134, 186, 151), rgb8(192, 153, 189), rgb8(174, 183, 190)};
            return d;
        }();
        return definition;
    }

    const UiThemeDefinition& debugHighContrastTheme()
    {
        static const UiThemeDefinition definition = [] {
            auto d = professionalDarkTheme();
            d.theme = config::GuiTheme::DebugHighContrast;
            d.id = d.base = "debug_high_contrast";
            d.name = "石墨琥珀";
            // 历史配置 ID 保留，外观不再承载高对比特判。
            d.ui.accent = rgb8(213, 174, 99);
            d.ui.accentMuted = rgb8(213, 174, 99, .16F);
            d.ui.textStrong = rgb8(224, 224, 221);
            d.ui.windowRounding = 4.F;
            d.ui.frameRounding = d.ui.grabRounding = d.ui.tabRounding = 3.F;
            d.wave.gridMajorWidth = d.wave.gridMinorTickWidth = 1.2F;
            d.wave.gridCenterWidth = 1.8F;
            d.wave.gridMinorTickHalfLength = 2.5F;
            d.ui.appBackground = rgb8(29, 31, 33);
            d.ui.panelBackground = rgb8(37, 40, 43);
            d.ui.panelBackgroundAlt = rgb8(47, 51, 55);
            d.ui.panelBorder = rgb8(70, 75, 80);
            d.ui.controlBorder = rgb8(88, 94, 100);
            d.ui.floatingBackground = rgb8(48, 53, 58);
            d.ui.floatingBorder = rgb8(106, 115, 122);
            d.ui.tabBackground = rgb8(40, 44, 48);
            d.ui.tabHovered = rgb8(77, 70, 55);
            d.ui.tabSelected = rgb8(79, 67, 45);
            d.ui.tabDimmed = rgb8(33, 36, 39);
            d.ui.tabDimmedSelected = rgb8(59, 57, 51);
            d.ui.tabSelectedOverline = d.ui.accent;
            d.ui.tabDimmedSelectedOverline = rgb8(151, 136, 105);
            d.ui.textMuted = rgb8(168, 174, 177);
            d.ui.genericPlotBackground = d.wave.plotBackground = rgb8(26, 30, 33);
            d.wave.gridMajor = rgb8(120, 126, 130, .16F);
            d.wave.gridMinorTick = rgb8(132, 138, 142, .24F);
            d.wave.gridCenter = rgb8(148, 156, 160, .28F);
            finishPreset(d, false);
            d.wave.channelPalette = professionalDarkTheme().wave.channelPalette;
            d.wave.cursorPalette = professionalDarkTheme().wave.cursorPalette;
            return d;
        }();
        return definition;
    }

    const UiThemeDefinition& professionalLightTheme()
    {
        static const UiThemeDefinition definition = [] {
            auto d = professionalDarkTheme();
            d.theme = config::GuiTheme::ProfessionalLight;
            d.id = d.base = "professional_light";
            d.name = "冷白青蓝";
            d.ui.appBackground = rgb8(232, 239, 244);
            d.ui.panelBackground = rgb8(247, 250, 252);
            d.ui.panelBackgroundAlt = rgb8(235, 242, 246);
            d.ui.panelBorder = rgb8(204, 216, 224);
            d.ui.controlBorder = rgb8(177, 195, 207);
            d.ui.floatingBackground = rgb8(239, 246, 250);
            d.ui.floatingBorder = rgb8(149, 172, 188);
            d.ui.accent = rgb8(39, 126, 146);
            d.ui.accentMuted = rgb8(39, 126, 146, .12F);
            d.ui.tabBackground = rgb8(227, 235, 241);
            d.ui.tabHovered = rgb8(203, 225, 234);
            d.ui.tabSelected = rgb8(207, 230, 238);
            d.ui.tabDimmed = rgb8(235, 241, 245);
            d.ui.tabDimmedSelected = rgb8(222, 233, 240);
            d.ui.tabSelectedOverline = d.ui.accent;
            d.ui.tabDimmedSelectedOverline = rgb8(117, 153, 170);
            d.ui.success = rgb8(22, 112, 60);
            d.ui.warning = rgb8(131, 85, 0);
            d.ui.danger = rgb8(178, 35, 48);
            d.ui.textStrong = rgb8(22, 37, 56);
            d.ui.textMuted = rgb8(67, 86, 108);
            d.ui.genericPlotBackground = d.wave.plotBackground = rgb8(255, 255, 255);
            d.wave.gridMajor = rgb8(119, 132, 148, .24F);
            d.wave.gridMinorTick = rgb8(106, 120, 136, .38F);
            d.wave.gridCenter = rgb8(88, 102, 118, .45F);
            finishPreset(d, true);
            return d;
        }();
        return definition;
    }

    UiThemeDefinition activeDefinition = professionalDarkTheme();
    std::uint64_t themeRevision{0};

    void applyImGuiTheme(const UiThemeDefinition& definition)
    {
        const auto& tokens = definition.ui;
        const auto interaction = [&](float alpha) { return surfaceTint(definition, tokens.accent, alpha); };
        const auto controlBorder = tokens.controlBorder;
        ImGuiStyle& style = ImGui::GetStyle();
        // 每次从同一基准重建样式，防止旧主题颜色或重复缩放残留。
        style = ImGuiStyle{};
        ImGui::StyleColorsDark(&style);
        style.WindowRounding = tokens.windowRounding;
        style.ChildRounding = tokens.windowRounding;
        style.FrameRounding = tokens.frameRounding;
        style.GrabRounding = tokens.grabRounding;
        style.TabRounding = tokens.tabRounding;
        style.PopupRounding = tokens.frameRounding;
        style.ScrollbarRounding = tokens.frameRounding;
        style.WindowPadding = ImVec2(tokens.windowPaddingX, tokens.windowPaddingY);
        style.FramePadding = ImVec2(tokens.framePaddingX, tokens.framePaddingY);
        style.ItemSpacing = ImVec2(tokens.itemSpacingX, tokens.itemSpacingY);
        style.ItemInnerSpacing = ImVec2(8.0F, 6.0F);
        style.CellPadding = ImVec2(8.0F, 6.0F);
        style.WindowBorderSize = 0.0F;
        style.ChildBorderSize = 0.0F;
        // 原生 Popup 与内部控件共用 Border；不在 ColorEdit 外压入浮层边框污染输入。
        style.PopupBorderSize = 1.0F;
        style.DisabledAlpha = .72F;
        style.FrameBorderSize = 1.0F;
        style.TabBorderSize = 0.0F;
        style.TabBarOverlineSize = 1.0F;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = tokens.textStrong;
        colors[ImGuiCol_TextDisabled] = tokens.textMuted;
        colors[ImGuiCol_WindowBg] = tokens.appBackground;
        colors[ImGuiCol_ChildBg] = tokens.panelBackground;
        colors[ImGuiCol_PopupBg] = tokens.floatingBackground;
        colors[ImGuiCol_Border] = controlBorder;
        colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_FrameBg] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_FrameBgHovered] = interaction(.10F);
        colors[ImGuiCol_FrameBgActive] = interaction(.16F);
        colors[ImGuiCol_TitleBg] = tokens.panelBackground;
        colors[ImGuiCol_TitleBgActive] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_MenuBarBg] = tokens.panelBackground;
        colors[ImGuiCol_ScrollbarBg] = tokens.appBackground;
        colors[ImGuiCol_ScrollbarGrab] = controlBorder;
        colors[ImGuiCol_ScrollbarGrabHovered] = tokens.textMuted;
        colors[ImGuiCol_ScrollbarGrabActive] = tokens.accent;
        colors[ImGuiCol_CheckMark] = tokens.accent;
        colors[ImGuiCol_SliderGrab] = tokens.accent;
        colors[ImGuiCol_Button] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_ButtonHovered] = interaction(.10F);
        colors[ImGuiCol_ButtonActive] = interaction(.16F);
        colors[ImGuiCol_Header] = interaction(.06F);
        colors[ImGuiCol_HeaderHovered] = interaction(.10F);
        colors[ImGuiCol_HeaderActive] = interaction(.16F);
        colors[ImGuiCol_Separator] = tokens.panelBorder;
        colors[ImGuiCol_Tab] = tokens.tabBackground;
        colors[ImGuiCol_TabHovered] = tokens.tabHovered;
        colors[ImGuiCol_TabSelected] = tokens.tabSelected;
        colors[ImGuiCol_TabDimmed] = tokens.tabDimmed;
        colors[ImGuiCol_TabDimmedSelected] = tokens.tabDimmedSelected;
        colors[ImGuiCol_DockingPreview] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, tokens.accent.w * .45F);
        colors[ImGuiCol_DockingEmptyBg] = tokens.appBackground;
        colors[ImGuiCol_TableHeaderBg] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_TableBorderStrong] = tokens.panelBorder;
        colors[ImGuiCol_TableBorderLight] =
            ImVec4(tokens.panelBorder.x, tokens.panelBorder.y, tokens.panelBorder.z, tokens.panelBorder.w * .55F);
        colors[ImGuiCol_TitleBgCollapsed] = tokens.panelBackground;
        colors[ImGuiCol_SeparatorHovered] = tokens.accent;
        colors[ImGuiCol_SeparatorActive] = tokens.accent;
        colors[ImGuiCol_PlotLines] = tokens.accent;
        colors[ImGuiCol_PlotLinesHovered] = tokens.warning;
        colors[ImGuiCol_PlotHistogram] = tokens.accent;
        colors[ImGuiCol_PlotHistogramHovered] = tokens.warning;
        colors[ImGuiCol_NavHighlight] = tokens.accent;
        colors[ImGuiCol_InputTextCursor] = tokens.textStrong;
        colors[ImGuiCol_CheckboxSelectedBg] = interaction(.10F);
        colors[ImGuiCol_TabSelectedOverline] = tokens.tabSelectedOverline;
        colors[ImGuiCol_TabDimmedSelectedOverline] = tokens.tabDimmedSelectedOverline;
        colors[ImGuiCol_TextLink] = tokens.textStrong;
        colors[ImGuiCol_TreeLines] = tokens.panelBorder;
        colors[ImGuiCol_UnsavedMarker] = tokens.textStrong;
        colors[ImGuiCol_DragDropTarget] = tokens.accent;
        colors[ImGuiCol_DragDropTargetBg] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, tokens.accent.w * .10F);
        colors[ImGuiCol_NavWindowingHighlight] = tokens.accent;
        colors[ImGuiCol_NavWindowingDimBg] = rgb8(3, 9, 18, .20F);
        colors[ImGuiCol_ModalWindowDimBg] = rgb8(3, 9, 18, .42F);
        colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_TableRowBgAlt] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_TextSelectedBg] = interaction(.22F);
        colors[ImGuiCol_SliderGrabActive] = tokens.accent;
        colors[ImGuiCol_ResizeGrip] = controlBorder;
        colors[ImGuiCol_ResizeGripHovered] = colors[ImGuiCol_ResizeGripActive] = tokens.accent;
    }

    void applyImPlotTheme(const UiThemeDefinition& definition)
    {
        const auto& tokens = definition.ui;
        ImPlotStyle& style = ImPlot::GetStyle();
        style.PlotBorderSize = 1.0F;
        style.MinorAlpha = 0.20F;
        style.MajorTickLen = ImVec2(10.0F, 10.0F);
        style.MinorTickLen = ImVec2(5.0F, 5.0F);
        style.PlotPadding = ImVec2(12.0F, 10.0F);

        ImVec4* colors = style.Colors;
        colors[ImPlotCol_FrameBg] = tokens.panelBackground;
        colors[ImPlotCol_PlotBg] = tokens.genericPlotBackground;
        colors[ImPlotCol_PlotBorder] = tokens.panelBorder;
        colors[ImPlotCol_LegendBg] = tokens.panelBackground;
        colors[ImPlotCol_LegendBorder] = tokens.panelBorder;
        colors[ImPlotCol_LegendText] = tokens.textStrong;
        colors[ImPlotCol_TitleText] = tokens.textStrong;
        colors[ImPlotCol_InlayText] = tokens.textMuted;
        colors[ImPlotCol_AxisText] = tokens.textMuted;
        colors[ImPlotCol_AxisGrid] = ImVec4(tokens.panelBorder.x, tokens.panelBorder.y, tokens.panelBorder.z, tokens.panelBorder.w * .28F);
        colors[ImPlotCol_AxisTick] = tokens.textMuted;
        colors[ImPlotCol_Crosshairs] = tokens.accent;
        colors[ImPlotCol_Selection] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, tokens.accent.w * .35F);
    }

} // namespace

const UiThemeDefinition& uiThemeDefinition(const config::GuiTheme theme)
{
    switch (theme) {
        case config::GuiTheme::DebugHighContrast:
            return debugHighContrastTheme();
        case config::GuiTheme::ProfessionalLight:
            return professionalLightTheme();
        case config::GuiTheme::ProfessionalDark:
        default:
            return professionalDarkTheme();
    }
}

const UiStyleTokens& activeUiStyleTokens()
{
    return activeDefinition.ui;
}

const WaveStyleTokens& activeWaveStyleTokens()
{
    return activeDefinition.wave;
}

void applyUiTheme(const config::GuiTheme theme)
{
    applyUiTheme(uiThemeDefinition(theme));
}

const UiThemeDefinition& activeThemeDefinition() { return activeDefinition; }
std::uint64_t activeThemeRevision() { return themeRevision; }

ImVec4 displayColor(ImVec4 color, ImVec4 background, float opacity, float minimumContrast)
{
    color.w *= std::clamp(opacity, 0.F, 1.F);
    if (!activeWaveStyleTokens().correctContrast || color.w <= 0.F) return color;
    // 半透明表面先合成到当前宿主表面；无 ImGui 上下文时使用活动主题应用底。
    auto parent = asColor(activeUiStyleTokens().appBackground);
    if (ImGui::GetCurrentContext()) {
        parent = plot::compositeOverviewColor(asColor(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg)), parent);
        if (const auto* window = GImGui->CurrentWindow) {
            if (window->Flags & ImGuiWindowFlags_ChildWindow)
                parent = plot::compositeOverviewColor(asColor(ImGui::GetStyleColorVec4(ImGuiCol_ChildBg)), parent);
            else if (window->Flags & ImGuiWindowFlags_Popup)
                parent = plot::compositeOverviewColor(asColor(ImGui::GetStyleColorVec4(ImGuiCol_PopupBg)), parent);
        }
    }
    const auto back = plot::compositeOverviewColor(asColor(background), parent);
    const auto ratio = [&](ImVec4 c) {
        return plot::overviewContrast(plot::compositeOverviewColor({c.x, c.y, c.z, c.w}, back), back);
    };
    if (ratio(color) >= minimumContrast) return color;
    const auto original = color;
    // 先只提高 alpha，能保留 RGB 时绝不混白/混黑；二分求最小可行覆盖量。
    color.w = 1.F;
    if (ratio(color) >= minimumContrast) {
        float low = original.w, high = 1.F;
        for (int i = 0; i < 20; ++i) {
            color.w = (low + high) * .5F;
            if (ratio(color) >= minimumContrast) high = color.w;
            else low = color.w;
        }
        color.w = high;
        return color;
    }
    // 两个方向分别求可行解，按真实 RGB 距离而非端点对比或混合比例择优。
    ImVec4 best = color;
    float bestDistance = std::numeric_limits<float>::infinity();
    double bestRatio = ratio(color);
    for (float end : {0.F, 1.F}) {
        const auto adjusted = [&](float t) {
            return ImVec4(std::lerp(original.x, end, t), std::lerp(original.y, end, t),
                          std::lerp(original.z, end, t), 1.F);
        };
        const auto endpoint = adjusted(1);
        if (ratio(endpoint) < minimumContrast) {
            if (!std::isfinite(bestDistance) && ratio(endpoint) > bestRatio) {
                best = endpoint;
                bestRatio = ratio(endpoint);
            }
            continue;
        }
        float low = 0.F, high = 1.F;
        for (int i = 0; i < 20; ++i) {
            const float middle = (low + high) * .5F;
            if (ratio(adjusted(middle)) >= minimumContrast) high = middle;
            else low = middle;
        }
        const auto candidate = adjusted(high);
        const auto distance = std::hypot(candidate.x-original.x, candidate.y-original.y, candidate.z-original.z);
        if (distance < bestDistance) { best = candidate; bestDistance = distance; }
    }
    return best;
}

void applyUiTheme(const UiThemeDefinition& definition)
{
    activeDefinition = definition;
    ++themeRevision;
    if (ImGui::GetCurrentContext() != nullptr) {
        applyImGuiTheme(definition);
    }
    if (ImPlot::GetCurrentContext() != nullptr) {
        applyImPlotTheme(definition);
    }
}

const UiStyleTokens& defaultUiStyleTokens()
{
    return activeUiStyleTokens();
}

void applyImGuiProfessionalDarkTheme()
{
    activeDefinition = professionalDarkTheme();
    ++themeRevision;
    applyImGuiTheme(professionalDarkTheme());
}

void applyImPlotProfessionalDarkTheme()
{
    activeDefinition = professionalDarkTheme();
    ++themeRevision;
    applyImPlotTheme(professionalDarkTheme());
}

bool beginToolbarGroup(const char* id, const char* title, float minHeight)
{
    const auto& tokens = defaultUiStyleTokens();
    const bool hasTitle = title != nullptr && title[0] != '\0';
    ImGui::PushID(id);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(tokens.windowPaddingX, hasTitle ? tokens.windowPaddingY : tokens.framePaddingY));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.panelBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.panelBorder);
    // 这里沿用 ImGui::BeginChild() 的原生语义：高度为 0 时占满剩余空间，调用方需要显式传入紧凑高度。
    const bool opened = ImGui::BeginChild("##group", ImVec2(0.0F, minHeight), true);
    // 容器装饰已完成；内部输入与原生弹窗恢复控件边框。
    ImGui::PopStyleColor(2);
    if (opened && hasTitle) {
        ImGui::PushStyleColor(ImGuiCol_Text, tokens.textMuted);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    return opened;
}

void endToolbarGroup()
{
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopID();
}

bool drawUiButton(const char* label, const ImVec2& size, UiButtonRole role, bool active)
{
    const auto& tokens = activeDefinition.ui;
    const auto identity = role == UiButtonRole::Danger ? tokens.danger : tokens.accent;
    // 角色只改变按钮局部填充与边界，所有主题共用原生交互与焦点路径。
    const bool disabled = (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled) != 0;
    const auto normal = disabled ? ImGui::GetStyleColorVec4(ImGuiCol_Button)
        : role == UiButtonRole::Toolbar && !active ? ImVec4(0, 0, 0, 0)
        : role == UiButtonRole::Primary || active || role == UiButtonRole::Danger
            ? surfaceTint(activeDefinition, identity, .16F)
            : ImGui::GetStyleColorVec4(ImGuiCol_Button);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.F);
    ImGui::PushStyleColor(ImGuiCol_Button, normal);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, role == UiButtonRole::Secondary
        ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered) : surfaceTint(activeDefinition, identity, .18F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, role == UiButtonRole::Secondary
        ? ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive) : surfaceTint(activeDefinition, identity, .22F));
    const bool clicked = ImGui::ButtonEx(label, size,
        ImGui::GetStyle().FramePadding.y == 0.F ? ImGuiButtonFlags_AlignTextBaseLine : ImGuiButtonFlags_None);
    // 当前 ImGui 原生先绘焦点再绘填充；再次绘制确保自绘侧栏等焦点不被表面遮挡。
    ImGui::RenderNavCursor(GImGui->LastItemData.Rect, GImGui->LastItemData.ID);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    return clicked;
}

bool drawUiSmallButton(const char* label, bool active)
{
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0.F));
    const bool clicked = drawUiButton(label, ImVec2(0, 0), UiButtonRole::Toolbar, active);
    ImGui::PopStyleVar();
    return clicked;
}

bool drawToolbarSectionButton(const char* label, const char* tooltip, bool active, const ImVec2& size)
{
    const bool clicked = drawUiButton(label, size.x == 0.0F && size.y == 0.0F ? ImVec2(-1.0F, 0.0F) : size,
                                      UiButtonRole::Toolbar, active);
    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

void drawHeaderBadge(const char* label, const ImVec4& color, bool filled)
{
    const auto& tokens = defaultUiStyleTokens();
    const ImVec4 background = surfaceTint(activeDefinition, color, filled ? .20F : .08F);
    const ImVec4 textColor = tokens.textStrong;
    ImGui::PushStyleColor(ImGuiCol_Button, background);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, background);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, background);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.F);
    ImGui::Button(label, ImVec2(0.0F, 0.0F));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
}

bool drawDangerIconButton(const char* label, const char* tooltip)
{
    const auto& tokens = defaultUiStyleTokens();
    ImGui::PushStyleColor(ImGuiCol_Button, surfaceTint(activeDefinition, tokens.danger, .08F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, surfaceTint(activeDefinition, tokens.danger, .16F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, surfaceTint(activeDefinition, tokens.danger, .24F));
    ImGui::PushStyleColor(ImGuiCol_Text, tokens.textStrong);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.danger);
    const bool clicked = drawUiButton(label, ImVec2(0, 0), UiButtonRole::Danger);
    ImGui::PopStyleColor(5);
    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

bool drawGhostIconButton(const char* label, const char* tooltip)
{
    const bool clicked = drawUiButton(label, ImVec2(0, 0), UiButtonRole::Toolbar);
    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

void beginDisabled(bool disabled)
{
    ImGui::BeginDisabled(disabled);
}

void endDisabled()
{
    ImGui::EndDisabled();
}

} // namespace protoscope::ui
