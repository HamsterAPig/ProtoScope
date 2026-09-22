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
    ImVec4 asVec(plot::OverviewColor c) { return {float(c.r), float(c.g), float(c.b), float(c.a)}; }

    // UI 强调与填充单独验收；wave.correct_contrast 只控制波形/游标的显示修正。
    ImVec4 surfaceTint(const UiThemeDefinition& d, ImVec4 accent, float alpha)
    {
        const auto base = plot::compositeOverviewColor(asColor(d.ui.panelBackgroundAlt), asColor(d.ui.panelBackground));
        const bool high = d.base == "debug_high_contrast";
        // 为>=90%覆盖字体核心保留滤波和8位量化余量，不能只让少数近乎不透明核心达标。
        auto coreText = asColor(d.ui.textStrong);
        coreText.a *= .9;
        for (;;) {
            accent.w = alpha;
            const auto mixed = plot::compositeOverviewColor(asColor(accent), base);
            if (alpha <= 0 || (plot::overviewContrast(plot::compositeOverviewColor(coreText, mixed), mixed) >= (high ? 12.2 : 4.7) &&
                plot::overviewContrast(plot::compositeOverviewColor(asColor(d.ui.textMuted), mixed), mixed) >= (high ? 7 : 4.5)))
                return asVec(mixed);
            alpha = (std::max)(0.F, alpha - .005F);
        }
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
        if (d.base == "debug_high_contrast")
            w.channelPalette = {rgb8(80, 255, 145), rgb8(72, 219, 255), rgb8(255, 218, 84), rgb8(213, 155, 255),
                                rgb8(255, 132, 140), rgb8(80, 247, 219), rgb8(255, 176, 82), rgb8(151, 201, 255)};
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
            d.ui.accent = rgb8(64, 183, 224);
            d.ui.accentMuted = rgb8(64, 183, 224, .20F);
            d.ui.success = rgb8(67, 202, 146);
            d.ui.warning = rgb8(242, 192, 86);
            d.ui.danger = rgb8(250, 127, 137);
            d.ui.textStrong = rgb8(234, 242, 250);
            d.ui.appBackground = rgb8(15, 23, 34);
            d.ui.panelBackground = rgb8(22, 33, 47);
            d.ui.panelBackgroundAlt = rgb8(30, 44, 60);
            d.ui.panelBorder = rgb8(58, 77, 96);
            d.ui.textMuted = rgb8(172, 190, 208);
            d.ui.genericPlotBackground = d.wave.plotBackground = rgb8(10, 18, 28);
            d.wave.gridMajor = rgb8(101, 132, 160, .22F);
            d.wave.gridMinorTick = rgb8(130, 156, 181, .36F);
            d.wave.gridCenter = rgb8(149, 179, 204, .40F);
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
            d.ui.appBackground = rgb8(5, 8, 12);
            d.ui.panelBackground = rgb8(10, 15, 21);
            d.ui.panelBackgroundAlt = rgb8(18, 26, 35);
            d.ui.panelBorder = rgb8(58, 72, 87);
            d.ui.textMuted = rgb8(200, 215, 230);
            d.ui.genericPlotBackground = d.wave.plotBackground = rgb8(2, 5, 9);
            d.wave.gridMajor = rgb8(103, 123, 145, .28F);
            d.wave.gridMinorTick = rgb8(157, 179, 200, .42F);
            d.wave.gridCenter = rgb8(188, 211, 231, .48F);
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
            d.ui.appBackground = rgb8(241, 246, 252);
            d.ui.panelBackground = rgb8(255, 255, 255);
            d.ui.panelBackgroundAlt = rgb8(246, 249, 253);
            d.ui.panelBorder = rgb8(200, 213, 227);
            d.ui.accent = rgb8(16, 92, 182);
            d.ui.accentMuted = rgb8(16, 92, 182, .12F);
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
        // 装饰线仍使用低调 panelBorder；必要的输入/按钮边界独立从辅助文字生成。
        const auto controlBorder = tokens.textMuted;
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
        // 原生 BeginDisabled 不自动使用 TextDisabled；业务经 beginDisabled 分离高对比正文/表面。
        // 此alpha只定义基础状态淡化，不能独自保证滤波后正文核心12:1。
        style.DisabledAlpha = definition.base == "debug_high_contrast" ? .86F : .72F;
        style.FrameBorderSize = 1.0F;
        style.TabBorderSize = 0.0F;

        ImVec4* colors = style.Colors;
        colors[ImGuiCol_Text] = tokens.textStrong;
        colors[ImGuiCol_TextDisabled] = tokens.textMuted;
        colors[ImGuiCol_WindowBg] = tokens.appBackground;
        colors[ImGuiCol_ChildBg] = tokens.panelBackground;
        colors[ImGuiCol_PopupBg] = tokens.panelBackground;
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
        colors[ImGuiCol_TabSelectedOverline] = tokens.accent;
        colors[ImGuiCol_TabDimmedSelectedOverline] = tokens.textMuted;
        colors[ImGuiCol_TextLink] = tokens.textStrong;
        colors[ImGuiCol_TreeLines] = tokens.panelBorder;
        colors[ImGuiCol_UnsavedMarker] = tokens.textStrong;
        colors[ImGuiCol_DragDropTarget] = tokens.accent;
        colors[ImGuiCol_DragDropTargetBg] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, .10F);
        colors[ImGuiCol_NavWindowingHighlight] = tokens.accent;
        colors[ImGuiCol_NavWindowingDimBg] = rgb8(3, 9, 18, .20F);
        colors[ImGuiCol_ModalWindowDimBg] = rgb8(3, 9, 18, .42F);
        colors[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
        colors[ImGuiCol_TableRowBgAlt] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_TextSelectedBg] = interaction(.22F);
        colors[ImGuiCol_SliderGrabActive] = tokens.accent;
        colors[ImGuiCol_ResizeGrip] = tokens.textMuted;
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
    const ImVec4 background = surfaceTint(activeDefinition, color, filled ? .20F : .08F);
    const ImVec4 border = color;
    const ImVec4 textColor = tokens.textStrong;
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
    ImGui::PushStyleColor(ImGuiCol_Button, surfaceTint(activeDefinition, tokens.danger, .08F));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, surfaceTint(activeDefinition, tokens.danger, .16F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, surfaceTint(activeDefinition, tokens.danger, .24F));
    ImGui::PushStyleColor(ImGuiCol_Text, tokens.textStrong);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.danger);
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(5);
    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

bool drawGhostIconButton(const char* label, const char* tooltip)
{
    const auto& tokens = defaultUiStyleTokens();
    ImGui::PushStyleColor(ImGuiCol_Button, tokens.panelBackground);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, surfaceTint(activeDefinition, tokens.accent, .10F));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, surfaceTint(activeDefinition, tokens.accent, .18F));
    const bool clicked = ImGui::Button(label);
    ImGui::PopStyleColor(3);
    if (tooltip != nullptr && tooltip[0] != '\0') {
        ImGui::SetItemTooltip("%s", tooltip);
    }
    return clicked;
}

void beginDisabled(bool disabled)
{
    ImGui::BeginDisabled(disabled);
    const bool high = activeDefinition.base == "debug_high_contrast" &&
                      (GImGui->CurrentItemFlags & ImGuiItemFlags_Disabled);
    auto text = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    auto button = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    auto frame = ImGui::GetStyleColorVec4(ImGuiCol_FrameBg);
    if (high) {
        // ImGui 在 GetColorU32 时才乘全局 Alpha：正文保留 .98，边框仍为 .86，
        // 并将可操作填充改为空黑表面。不能仅把所有元素的禁用alpha提高到接近1。
        text.w = .98F / ImGui::GetStyle().DisabledAlpha;
        button = frame = ImVec4(0,0,0,1);
    }
    // 固定压栈数量支持条件及嵌套禁用；EndDisabled 对称恢复原样式。
    ImGui::PushStyleColor(ImGuiCol_Text,text);
    ImGui::PushStyleColor(ImGuiCol_Button,button);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,frame);
}

void endDisabled()
{
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
}

} // namespace protoscope::ui
