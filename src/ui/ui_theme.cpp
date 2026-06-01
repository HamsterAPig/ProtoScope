#include "protoscope/ui/ui_theme.hpp"

#include <implot.h>

namespace protoscope::ui {

namespace {

UiStyleTokens makeDefaultTokens() {
    return UiStyleTokens{
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
    };
}

} // namespace

const UiStyleTokens& defaultUiStyleTokens() {
    static const UiStyleTokens tokens = makeDefaultTokens();
    return tokens;
}

void applyImGuiProfessionalDarkTheme() {
    const auto& tokens = defaultUiStyleTokens();
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
    colors[ImGuiCol_MenuBarBg] = ImVec4(0.08F, 0.10F, 0.14F, 0.95F);
    colors[ImGuiCol_ScrollbarBg] = ImVec4(0.06F, 0.08F, 0.11F, 1.0F);
    colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.25F, 0.34F, 0.45F, 0.90F);
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
    colors[ImGuiCol_Tab] = ImVec4(0.10F, 0.13F, 0.18F, 1.0F);
    colors[ImGuiCol_TabHovered] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.45F);
    colors[ImGuiCol_TabActive] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.28F);
    colors[ImGuiCol_TabUnfocused] = ImVec4(0.08F, 0.11F, 0.16F, 1.0F);
    colors[ImGuiCol_TabUnfocusedActive] = ImVec4(0.11F, 0.15F, 0.22F, 1.0F);
    colors[ImGuiCol_DockingPreview] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.45F);
    colors[ImGuiCol_DockingEmptyBg] = ImVec4(0.05F, 0.07F, 0.10F, 1.0F);
    colors[ImGuiCol_TableHeaderBg] = ImVec4(0.10F, 0.13F, 0.18F, 1.0F);
    colors[ImGuiCol_TableBorderStrong] = tokens.panelBorder;
    colors[ImGuiCol_TableBorderLight] = ImVec4(tokens.panelBorder.x, tokens.panelBorder.y, tokens.panelBorder.z, 0.55F);
    colors[ImGuiCol_TableRowBgAlt] = ImVec4(1.0F, 1.0F, 1.0F, 0.02F);
    colors[ImGuiCol_TextSelectedBg] = ImVec4(tokens.accent.x, tokens.accent.y, tokens.accent.z, 0.30F);
}

void applyImPlotProfessionalDarkTheme() {
    const auto& tokens = defaultUiStyleTokens();
    ImPlotStyle& style = ImPlot::GetStyle();
    style.PlotBorderSize = 1.0F;
    style.MinorAlpha = 0.20F;
    style.MajorTickLen = ImVec2(10.0F, 10.0F);
    style.MinorTickLen = ImVec2(5.0F, 5.0F);
    style.PlotPadding = ImVec2(12.0F, 10.0F);

    ImVec4* colors = style.Colors;
    colors[ImPlotCol_FrameBg] = tokens.panelBackground;
    colors[ImPlotCol_PlotBg] = ImVec4(0.07F, 0.09F, 0.13F, 1.0F);
    colors[ImPlotCol_PlotBorder] = tokens.panelBorder;
    colors[ImPlotCol_LegendBg] = ImVec4(tokens.panelBackgroundAlt.x, tokens.panelBackgroundAlt.y, tokens.panelBackgroundAlt.z, 0.92F);
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
bool beginToolbarGroup(const char* id, const char* title, float minHeight) {
    const auto& tokens = defaultUiStyleTokens();
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, tokens.panelBackgroundAlt);
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.panelBorder);
    const bool opened = ImGui::BeginChild("##group", ImVec2(0.0F, minHeight), true);
    if (opened && title != nullptr && title[0] != '\0') {
        ImGui::PushStyleColor(ImGuiCol_Text, tokens.textMuted);
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Separator();
    }
    return opened;
}

void endToolbarGroup() {
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopID();
}

bool drawToolbarSectionButton(const char* label, const char* tooltip, bool active, const ImVec2& size) {
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

void drawHeaderBadge(const char* label, const ImVec4& color, bool filled) {
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

bool drawDangerIconButton(const char* label, const char* tooltip) {
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

bool drawGhostIconButton(const char* label, const char* tooltip) {
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



