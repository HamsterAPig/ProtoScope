#include "protoscope/ui/ui_theme.hpp"
#include "protoscope/plot/wave_overview_color.hpp"

#include <implot.h>

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

    void finishPreset(UiThemeDefinition& d, bool light)
    {
        auto& w = d.wave;
        const auto& u = d.ui;
        w.statusOverlayBackground = w.legendOverlayBackground = w.measurementOverlayBackground = u.panelBackground;
        w.statusOverlayBorder = w.legendOverlayBorder = w.measurementOverlayBorder = u.panelBorder;
        w.statusOverlayText = w.legendOverlayTextPrimary = w.measurementOverlayTitle = u.textStrong;
        w.channelLabel = w.splitChannelLabel = w.bitLabel = w.legendOverlayTextSecondary = u.textMuted;
        w.channelSeparator = u.panelBorder;
        w.legendOverlayRowHover = u.panelBackgroundAlt;
        w.legendOverlayRowActive = u.panelBackgroundAlt;
        w.legendOverlayRowActiveBorder = w.measurementOverlayAccent = u.accent;
        w.measurementChipBackground = u.panelBackgroundAlt;
        w.measurementChipBorder = u.panelBorder;
        w.measurementChipLabel = u.textMuted;
        w.measurementChipValue = u.textStrong;
        w.lightPersistence = light;
        w.channelPalette = light
            ? std::vector<ImVec4>{rgb8(20, 120, 65), rgb8(0, 105, 175), rgb8(145, 101, 0), rgb8(120, 55, 183),
                                 rgb8(195, 42, 48), rgb8(0, 120, 119), rgb8(177, 78, 0), rgb8(57, 89, 157)}
            : std::vector<ImVec4>{rgb8(55, 226, 122), rgb8(51, 199, 255), rgb8(255, 194, 71), rgb8(182, 109, 255),
                                 rgb8(255, 93, 93), rgb8(85, 233, 226), rgb8(255, 143, 56), rgb8(158, 204, 255)};
        w.cursorPalette = light
            ? std::vector<ImVec4>{rgb8(146, 98, 0), rgb8(0, 103, 150), rgb8(51, 99, 150), rgb8(185, 60, 78),
                                 rgb8(34, 136, 51), rgb8(170, 51, 119), rgb8(95, 100, 110)}
            : std::vector<ImVec4>{rgb8(255, 209, 102), rgb8(102, 204, 238), rgb8(68, 119, 170), rgb8(238, 102, 119),
                                 rgb8(34, 136, 51), rgb8(170, 51, 119), rgb8(187, 187, 187)};
    }

    const UiThemeDefinition& professionalDarkTheme()
    {
        static const UiThemeDefinition definition = [] {
            UiThemeDefinition d;
            d.ui.accent = rgb8(46, 148, 224);
            d.ui.accentMuted = rgb8(46, 148, 224, .24F);
            d.ui.success = rgb8(61, 189, 122);
            d.ui.warning = rgb8(237, 179, 51);
            d.ui.danger = rgb8(239, 106, 106);
            d.ui.textStrong = rgb8(237, 245, 252);
            d.ui.appBackground = rgb8(27, 29, 33);
            d.ui.panelBackground = rgb8(36, 39, 44);
            d.ui.panelBackgroundAlt = rgb8(46, 49, 55);
            d.ui.panelBorder = rgb8(113, 123, 135);
            d.ui.textMuted = rgb8(180, 188, 200);
            d.ui.genericPlotBackground = d.wave.plotBackground = rgb8(21, 23, 27);
            d.wave.gridMajor = rgb8(90, 96, 106, .36F);
            d.wave.gridMinorTick = rgb8(130, 139, 151, .48F);
            d.wave.gridCenter = rgb8(145, 155, 167, .50F);
            finishPreset(d, false);
            return d;
        }();
        return definition;
    }

    const UiThemeDefinition& debugHighContrastTheme()
    {
        static const UiThemeDefinition definition = [] {
            auto d = professionalDarkTheme();
            d.theme = config::GuiTheme::DebugHighContrast;
            d.ui.accent = rgb8(46, 184, 250);
            d.ui.accentMuted = rgb8(46, 184, 250, .24F);
            d.ui.textStrong = rgb8(245, 250, 255);
            d.ui.windowRounding = 4.F;
            d.ui.frameRounding = d.ui.grabRounding = d.ui.tabRounding = 3.F;
            d.wave.gridMajorWidth = d.wave.gridMinorTickWidth = 1.2F;
            d.wave.gridCenterWidth = 1.8F;
            d.wave.gridMinorTickHalfLength = 2.5F;
            d.id = d.base = "debug_high_contrast";
            d.name = "仪器深黑（高对比）";
            d.ui.appBackground = rgb8(12, 13, 15);
            d.ui.panelBackground = rgb8(19, 21, 24);
            d.ui.panelBackgroundAlt = rgb8(28, 30, 34);
            d.ui.panelBorder = rgb8(125, 137, 151);
            d.ui.textMuted = rgb8(196, 203, 213);
            d.ui.genericPlotBackground = d.wave.plotBackground = rgb8(8, 9, 11);
            d.wave.gridMajor = rgb8(92, 100, 111, .42F);
            d.wave.gridMinorTick = rgb8(149, 162, 178, .50F);
            d.wave.gridCenter = rgb8(176, 188, 202, .55F);
            finishPreset(d, false);
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
            d.name = "专业浅色";
            d.ui.appBackground = rgb8(240, 242, 244);
            d.ui.panelBackground = rgb8(247, 248, 250);
            d.ui.panelBackgroundAlt = rgb8(229, 233, 239);
            d.ui.panelBorder = rgb8(120, 128, 139);
            d.ui.accent = rgb8(20, 91, 170);
            d.ui.accentMuted = rgb8(20, 91, 170, .16F);
            d.ui.success = rgb8(22, 112, 60);
            d.ui.warning = rgb8(131, 85, 0);
            d.ui.danger = rgb8(178, 35, 48);
            d.ui.textStrong = rgb8(36, 41, 47);
            d.ui.textMuted = rgb8(86, 97, 111);
            d.ui.genericPlotBackground = d.wave.plotBackground = rgb8(252, 253, 254);
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
        const auto interaction = [&](float alpha) {
            const plot::OverviewColor base{tokens.panelBackgroundAlt.x, tokens.panelBackgroundAlt.y,
                                            tokens.panelBackgroundAlt.z, 1};
            plot::OverviewColor mixed;
            // 交互填充按实际背景验证辅助文字，不让浅色主题悬停时出现低对比文字。
            for (alpha = (std::min)(alpha, .14F); ; alpha = (std::max)(0.F, alpha - .01F)) {
                mixed = plot::compositeOverviewColor({tokens.accent.x, tokens.accent.y, tokens.accent.z, alpha}, base);
                if (alpha == 0 || plot::overviewContrast(
                        plot::compositeOverviewColor({tokens.textMuted.x,tokens.textMuted.y,tokens.textMuted.z,tokens.textMuted.w},mixed),
                        mixed) >= 4.5) break;
            }
            return ImVec4(float(mixed.r),float(mixed.g),float(mixed.b),1);
        };
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
        style.WindowBorderSize = 1.0F;
        style.ChildBorderSize = 1.0F;
        style.FrameBorderSize = 1.0F;
        style.TabBorderSize = 0.0F;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = tokens.textStrong;
        colors[ImGuiCol_TextDisabled] = tokens.textMuted;
        colors[ImGuiCol_WindowBg] = tokens.appBackground;
        colors[ImGuiCol_ChildBg] = tokens.panelBackground;
        colors[ImGuiCol_PopupBg] = tokens.panelBackground;
        colors[ImGuiCol_Border] = tokens.panelBorder;
        colors[ImGuiCol_FrameBg] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_FrameBgHovered] = interaction(.10F);
        colors[ImGuiCol_FrameBgActive] = interaction(.16F);
        colors[ImGuiCol_TitleBg] = tokens.panelBackground;
        colors[ImGuiCol_TitleBgActive] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_MenuBarBg] = tokens.panelBackground;
        colors[ImGuiCol_ScrollbarBg] = tokens.appBackground;
        colors[ImGuiCol_ScrollbarGrab] = tokens.panelBorder;
        colors[ImGuiCol_ScrollbarGrabHovered] = tokens.textMuted;
        colors[ImGuiCol_ScrollbarGrabActive] = tokens.accent;
        colors[ImGuiCol_CheckMark] = tokens.accent;
        colors[ImGuiCol_SliderGrab] = tokens.accent;
        colors[ImGuiCol_SliderGrabActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.85F);
        colors[ImGuiCol_Button] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_ButtonHovered] = interaction(.10F);
        colors[ImGuiCol_ButtonActive] = interaction(.16F);
        colors[ImGuiCol_Header] = interaction(.06F);
        colors[ImGuiCol_HeaderHovered] = interaction(.10F);
        colors[ImGuiCol_HeaderActive] = interaction(.16F);
        colors[ImGuiCol_Separator] = tokens.panelBorder;
        colors[ImGuiCol_ResizeGrip] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.20F);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.40F);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.62F);
        colors[ImGuiCol_Tab] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_TabHovered] = interaction(.10F);
        colors[ImGuiCol_TabActive] = interaction(.16F);
        colors[ImGuiCol_TabUnfocused] = tokens.panelBackground;
        colors[ImGuiCol_TabUnfocusedActive] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_DockingPreview] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.45F);
        colors[ImGuiCol_DockingEmptyBg] = tokens.appBackground;
        colors[ImGuiCol_TableHeaderBg] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_TableBorderStrong] = tokens.panelBorder;
        colors[ImGuiCol_TableBorderLight] =
            ImVec4(tokens.panelBorder.x, tokens.panelBorder.y, tokens.panelBorder.z, 0.55F);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0F, 1.0F, 1.0F, 0.02F);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.30F);
        colors[ImGuiCol_TitleBgCollapsed] = tokens.panelBackground;
        colors[ImGuiCol_SeparatorHovered] = tokens.accent;
        colors[ImGuiCol_SeparatorActive] = tokens.accent;
        colors[ImGuiCol_PlotLines] = tokens.accent;
        colors[ImGuiCol_PlotLinesHovered] = tokens.warning;
        colors[ImGuiCol_PlotHistogram] = tokens.accent;
        colors[ImGuiCol_PlotHistogramHovered] = tokens.warning;
        colors[ImGuiCol_NavHighlight] = tokens.accent;
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
        colors[ImPlotCol_LegendBg] =
            ImVec4(tokens.panelBackgroundAlt.x, tokens.panelBackgroundAlt.y, tokens.panelBackgroundAlt.z, 0.92F);
        colors[ImPlotCol_LegendBorder] = tokens.panelBorder;
        colors[ImPlotCol_LegendText] = tokens.textStrong;
        colors[ImPlotCol_TitleText] = tokens.textStrong;
        colors[ImPlotCol_InlayText] = tokens.textMuted;
        colors[ImPlotCol_AxisText] = tokens.textMuted;
        colors[ImPlotCol_AxisGrid] = ImVec4(tokens.panelBorder.x, tokens.panelBorder.y, tokens.panelBorder.z, 0.28F);
        colors[ImPlotCol_AxisTick] = tokens.textMuted;
        colors[ImPlotCol_Crosshairs] = tokens.accent;
        colors[ImPlotCol_Selection] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.35F);
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
    const plot::OverviewColor back{background.x, background.y, background.z, 1};
    const auto ratio = [&](ImVec4 c) {
        return plot::overviewContrast(plot::compositeOverviewColor({c.x, c.y, c.z, c.w}, back), back);
    };
    if (ratio(color) >= minimumContrast) return color;
    const float end = plot::overviewContrast({0, 0, 0, 1}, back) >
                      plot::overviewContrast({1, 1, 1, 1}, back) ? 0.F : 1.F;
    const auto original = color;
    // 先沿明暗方向最小幅度修正 RGB；透明度不足时才增加 alpha，原始通道颜色不变。
    for (int step = 1; step <= 48; ++step) {
        const float t = step / 64.F;
        color.x = std::lerp(original.x, end, t);
        color.y = std::lerp(original.y, end, t);
        color.z = std::lerp(original.z, end, t);
        if (ratio(color) >= minimumContrast) return color;
    }
    for (int step = 1; step <= 64; ++step) {
        color.w = std::lerp(original.w, 1.F, step / 64.F);
        if (ratio(color) >= minimumContrast) return color;
    }
    // 先保留至少四分之一原色，避免低 alpha 直接褪为纯黑/纯白；极端背景再用剩余明暗范围。
    for (int step = 49; step <= 64; ++step) {
        color.x = std::lerp(original.x, end, step / 64.F);
        color.y = std::lerp(original.y, end, step / 64.F);
        color.z = std::lerp(original.z, end, step / 64.F);
        if (ratio(color) >= minimumContrast) return color;
    }
    return color;
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
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.panelBackgroundAlt);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.panelBorder);
    // 这里沿用 ImGui::BeginChild() 的原生语义：高度为 0 时占满剩余空间，调用方需要显式传入紧凑高度。
    const bool opened = ImGui::BeginChild("##group", ImVec2(0.0F, minHeight), true);
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
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

bool drawToolbarSectionButton(const char* label, const char* tooltip, bool active, const ImVec2& size)
{
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    }
    const bool clicked = ImGui::Button(label, size.x == 0.0F && size.y == 0.0F ? ImVec2(-1.0F, 0.0F) : size);
    if (active) {
        ImGui::PopStyleColor(3);
    }
    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

void drawHeaderBadge(const char* label, const ImVec4& color, bool filled)
{
    const auto& tokens = defaultUiStyleTokens();
    const ImVec4 background = filled ? color : ImVec4(color.x, color.y, color.z, 0.16F);
    const ImVec4 border = filled ? color : ImVec4(color.x, color.y, color.z, 0.55F);
    const auto composite = plot::compositeOverviewColor({background.x, background.y, background.z, background.w},
        {tokens.panelBackground.x, tokens.panelBackground.y, tokens.panelBackground.z, 1});
    const ImVec4 backdrop(float(composite.r), float(composite.g), float(composite.b), 1);
    const ImVec4 textColor = displayColor(filled ? tokens.textStrong : color, backdrop, 1.F, 4.5F);
    ImGui::PushStyleColor(ImGuiCol_Button, background);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, background);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, background);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    ImGui::PushStyleColor(ImGuiCol_Text, textColor);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
    ImGui::Button(label, ImVec2(0.0F, 0.0F));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(5);
}

bool drawDangerIconButton(const char* label, const char* tooltip)
{
    const auto& tokens = defaultUiStyleTokens();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(tokens.danger.x, tokens.danger.y, tokens.danger.z, 0.18F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(tokens.danger.x, tokens.danger.y, tokens.danger.z, 0.32F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(tokens.danger.x, tokens.danger.y, tokens.danger.z, 0.48F));
    ImGui::PushStyleColor(ImGuiCol_Text, tokens.danger);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(4);
    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

bool drawGhostIconButton(const char* label, const char* tooltip)
{
    const auto& tokens = defaultUiStyleTokens();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0F, 1.0F, 1.0F, 0.04F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.18F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.28F));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

} // namespace protoscope::ui
