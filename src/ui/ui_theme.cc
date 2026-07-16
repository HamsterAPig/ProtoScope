#include "protoscope/ui/ui_theme.hpp"

#include <implot.h>

namespace protoscope::ui {

namespace {

    UiThemeDefinition makeProfessionalDarkTheme()
    {
        return UiThemeDefinition{
            .theme = config::GuiTheme::ProfessionalDark,
            .ui =
                {
                    .appBackground = ImVec4(0.05F, 0.07F, 0.10F, 1.0F),
                    .panelBackground = ImVec4(0.09F, 0.11F, 0.15F, 1.0F),
                    .panelBackgroundAlt = ImVec4(0.12F, 0.14F, 0.19F, 1.0F),
                    .panelBorder = ImVec4(0.22F, 0.29F, 0.38F, 0.95F),
                    .accent = ImVec4(0.18F, 0.58F, 0.88F, 1.0F),
                    .accentMuted = ImVec4(0.18F, 0.58F, 0.88F, 0.24F),
                    .success = ImVec4(0.24F, 0.74F, 0.48F, 1.0F),
                    .warning = ImVec4(0.93F, 0.70F, 0.20F, 1.0F),
                    .danger = ImVec4(0.91F, 0.33F, 0.33F, 1.0F),
                    .textStrong = ImVec4(0.93F, 0.96F, 0.99F, 1.0F),
                    .textMuted = ImVec4(0.58F, 0.67F, 0.76F, 1.0F),
                    .genericPlotBackground = ImVec4(0.07F, 0.09F, 0.13F, 1.0F),
                },
            .wave =
                {
                    .plotBackground = ImVec4(0.043F, 0.067F, 0.094F, 1.0F),
                    .gridMajor = ImVec4(0.12F, 0.19F, 0.27F, 0.78F),
                    .gridMinorTick = ImVec4(0.30F, 0.46F, 0.60F, 0.76F),
                    .gridCenter = ImVec4(0.90F, 0.96F, 1.0F, 0.66F),
                    .statusOverlayBackground = ImVec4(0.04F, 0.045F, 0.05F, 0.68F),
                    .statusOverlayBorder = ImVec4(1.0F, 1.0F, 1.0F, 0.18F),
                    .statusOverlayText = ImVec4(0.92F, 0.94F, 0.98F, 0.95F),
                    .channelSeparator = ImVec4(0.16F, 0.24F, 0.31F, 0.70F),
                    .channelLabel = ImVec4(0.84F, 0.89F, 0.94F, 0.76F),
                    .splitChannelLabel = ImVec4(0.90F, 0.94F, 0.98F, 0.86F),
                    .bitLabel = ImVec4(0.84F, 0.88F, 0.92F, 0.72F),
                    .legendOverlayBackground = ImVec4(0.051F, 0.075F, 0.106F, 1.0F),
                    .legendOverlayBorder = ImVec4(0.30F, 0.42F, 0.54F, 0.55F),
                    .measurementOverlayBackground = ImVec4(0.035F, 0.040F, 0.050F, 0.72F),
                    .measurementOverlayBorder = ImVec4(1.000F, 1.000F, 1.000F, 0.15F),
                    .measurementOverlayAccent = ImVec4(0.300F, 0.620F, 1.000F, 0.85F),
                    .measurementOverlayTitle = ImVec4(0.960F, 0.970F, 1.000F, 0.98F),
                    .measurementChipBackground = ImVec4(1.000F, 1.000F, 1.000F, 0.075F),
                    .measurementChipBorder = ImVec4(1.000F, 1.000F, 1.000F, 0.12F),
                    .measurementChipLabel = ImVec4(0.660F, 0.720F, 0.800F, 0.94F),
                    .measurementChipValue = ImVec4(0.940F, 0.960F, 0.990F, 0.98F),
                },
        };
    }

    UiThemeDefinition makeDebugHighContrastTheme()
    {
        return UiThemeDefinition{
            .theme = config::GuiTheme::DebugHighContrast,
            .ui =
                {
                    .appBackground = ImVec4(0.035F, 0.047F, 0.067F, 1.0F),
                    .panelBackground = ImVec4(0.055F, 0.071F, 0.096F, 1.0F),
                    .panelBackgroundAlt = ImVec4(0.080F, 0.104F, 0.140F, 1.0F),
                    .panelBorder = ImVec4(0.300F, 0.400F, 0.520F, 0.95F),
                    .accent = ImVec4(0.200F, 0.650F, 0.980F, 1.0F),
                    .accentMuted = ImVec4(0.200F, 0.650F, 0.980F, 0.24F),
                    .success = ImVec4(0.24F, 0.74F, 0.48F, 1.0F),
                    .warning = ImVec4(0.93F, 0.70F, 0.20F, 1.0F),
                    .danger = ImVec4(0.91F, 0.33F, 0.33F, 1.0F),
                    .textStrong = ImVec4(0.960F, 0.980F, 1.000F, 1.0F),
                    .textMuted = ImVec4(0.680F, 0.760F, 0.840F, 1.0F),
                    .genericPlotBackground = ImVec4(0.045F, 0.058F, 0.078F, 1.0F),
                },
            .wave =
                {
                    .plotBackground = ImVec4(0.025F, 0.040F, 0.058F, 1.0F),
                    .gridMajor = ImVec4(0.210F, 0.310F, 0.420F, 0.82F),
                    .gridMinorTick = ImVec4(0.320F, 0.490F, 0.640F, 0.86F),
                    .gridCenter = ImVec4(0.750F, 0.840F, 0.920F, 0.84F),
                    .statusOverlayBackground = ImVec4(0.025F, 0.040F, 0.058F, 0.84F),
                    .statusOverlayBorder = ImVec4(0.300F, 0.400F, 0.520F, 0.72F),
                    .statusOverlayText = ImVec4(0.960F, 0.980F, 1.000F, 0.98F),
                    .channelSeparator = ImVec4(0.300F, 0.400F, 0.520F, 0.78F),
                    .channelLabel = ImVec4(0.680F, 0.760F, 0.840F, 0.94F),
                    .splitChannelLabel = ImVec4(0.960F, 0.980F, 1.000F, 0.94F),
                    .bitLabel = ImVec4(0.750F, 0.840F, 0.920F, 0.92F),
                    .legendOverlayBackground = ImVec4(0.055F, 0.071F, 0.096F, 1.0F),
                    .legendOverlayBorder = ImVec4(0.300F, 0.400F, 0.520F, 0.72F),
                    .measurementOverlayBackground = ImVec4(0.025F, 0.040F, 0.058F, 0.86F),
                    .measurementOverlayBorder = ImVec4(0.300F, 0.400F, 0.520F, 0.68F),
                    .measurementOverlayAccent = ImVec4(0.300F, 0.620F, 1.000F, 0.85F),
                    .measurementOverlayTitle = ImVec4(0.960F, 0.980F, 1.000F, 0.98F),
                    .measurementChipBackground = ImVec4(0.080F, 0.104F, 0.140F, 0.82F),
                    .measurementChipBorder = ImVec4(0.300F, 0.400F, 0.520F, 0.55F),
                    .measurementChipLabel = ImVec4(0.680F, 0.760F, 0.840F, 0.96F),
                    .measurementChipValue = ImVec4(0.960F, 0.980F, 1.000F, 0.99F),
                    .gridMajorWidth = 1.2F,
                    .gridMinorTickWidth = 1.2F,
                    .gridCenterWidth = 1.8F,
                    .gridMinorTickHalfLength = 2.5F,
                },
        };
    }

    const UiThemeDefinition& professionalDarkTheme()
    {
        static const UiThemeDefinition definition = makeProfessionalDarkTheme();
        return definition;
    }

    const UiThemeDefinition& debugHighContrastTheme()
    {
        static const UiThemeDefinition definition = makeDebugHighContrastTheme();
        return definition;
    }

    config::GuiTheme activeTheme{config::GuiTheme::ProfessionalDark};

    ImVec4 withAlpha(const ImVec4& color, float alpha)
    {
        return ImVec4(color.x, color.y, color.z, alpha);
    }

    void applyImGuiTheme(const UiThemeDefinition& definition)
    {
        const auto& tokens = definition.ui;
        const bool highContrast = definition.theme == config::GuiTheme::DebugHighContrast;
        ImGuiStyle& style = ImGui::GetStyle();
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
        colors[ImGuiCol_FrameBgHovered] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.28F);
        colors[ImGuiCol_FrameBgActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.38F);
        colors[ImGuiCol_TitleBg] = tokens.panelBackground;
        colors[ImGuiCol_TitleBgActive] = tokens.panelBackgroundAlt;
        colors[ImGuiCol_MenuBarBg] =
            highContrast ? withAlpha(tokens.panelBackground, 0.95F) : ImVec4(0.08F, 0.10F, 0.14F, 0.95F);
        colors[ImGuiCol_ScrollbarBg] = highContrast ? tokens.appBackground : ImVec4(0.06F, 0.08F, 0.11F, 1.0F);
        colors[ImGuiCol_ScrollbarGrab] =
            highContrast ? withAlpha(tokens.panelBorder, 0.90F) : ImVec4(0.25F, 0.34F, 0.45F, 0.90F);
        colors[ImGuiCol_CheckMark] = tokens.accent;
        colors[ImGuiCol_SliderGrab] = tokens.accent;
        colors[ImGuiCol_SliderGrabActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.85F);
        colors[ImGuiCol_Button] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.20F);
        colors[ImGuiCol_ButtonHovered] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.36F);
        colors[ImGuiCol_ButtonActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.52F);
        colors[ImGuiCol_Header] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.18F);
        colors[ImGuiCol_HeaderHovered] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.28F);
        colors[ImGuiCol_HeaderActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.38F);
        colors[ImGuiCol_Separator] = tokens.panelBorder;
        colors[ImGuiCol_ResizeGrip] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.20F);
        colors[ImGuiCol_ResizeGripHovered] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.40F);
        colors[ImGuiCol_ResizeGripActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.62F);
        colors[ImGuiCol_Tab] = highContrast ? tokens.panelBackgroundAlt : ImVec4(0.10F, 0.13F, 0.18F, 1.0F);
        colors[ImGuiCol_TabHovered] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.45F);
        colors[ImGuiCol_TabActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.28F);
        colors[ImGuiCol_TabUnfocused] = highContrast ? tokens.panelBackground : ImVec4(0.08F, 0.11F, 0.16F, 1.0F);
        colors[ImGuiCol_TabUnfocusedActive] =
            highContrast ? tokens.panelBackgroundAlt : ImVec4(0.11F, 0.15F, 0.22F, 1.0F);
        colors[ImGuiCol_DockingPreview] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.45F);
        colors[ImGuiCol_DockingEmptyBg] = tokens.appBackground;
        colors[ImGuiCol_TableHeaderBg] = highContrast ? tokens.panelBackgroundAlt : ImVec4(0.10F, 0.13F, 0.18F, 1.0F);
        colors[ImGuiCol_TableBorderStrong] = tokens.panelBorder;
        colors[ImGuiCol_TableBorderLight] =
            ImVec4(tokens.panelBorder.x, tokens.panelBorder.y, tokens.panelBorder.z, 0.55F);
        colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0F, 1.0F, 1.0F, 0.02F);
        colors[ImGuiCol_TextSelectedBg] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.30F);
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
        case config::GuiTheme::ProfessionalDark:
        default:
            return professionalDarkTheme();
    }
}

const UiStyleTokens& activeUiStyleTokens()
{
    return uiThemeDefinition(activeTheme).ui;
}

const WaveStyleTokens& activeWaveStyleTokens()
{
    return uiThemeDefinition(activeTheme).wave;
}

void applyUiTheme(const config::GuiTheme theme)
{
    activeTheme = theme;
    const auto& definition = uiThemeDefinition(activeTheme);
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
    activeTheme = config::GuiTheme::ProfessionalDark;
    applyImGuiTheme(professionalDarkTheme());
}

void applyImPlotProfessionalDarkTheme()
{
    activeTheme = config::GuiTheme::ProfessionalDark;
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
    const auto& tokens = defaultUiStyleTokens();
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.42F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.58F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.74F));
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
    const ImVec4 textColor = filled ? tokens.textStrong : color;
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
