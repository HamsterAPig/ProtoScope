#pragma once

#include "protoscope/ui/ui_theme.hpp"
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <implot.h>

namespace protoscope::tests::theme {
inline bool same(ImVec4 a, ImVec4 b, float tolerance = 1e-6F)
{
    return std::abs(a.x-b.x)<=tolerance && std::abs(a.y-b.y)<=tolerance &&
           std::abs(a.z-b.z)<=tolerance && std::abs(a.w-b.w)<=tolerance;
}
inline void color(ImVec4 actual, ImVec4 expected, const std::string& label)
{
    if (!same(actual,expected)) throw std::runtime_error("主题 RGBA 不符: "+label);
}
struct Field { const char* yaml; ImVec4 ui::UiStyleTokens::* member; };
// 独立列出公开契约，防止生产字段表遗漏后解析和导出一起漏掉而自证通过。
inline constexpr std::array<Field,22> fields{{
    {"app_background",&ui::UiStyleTokens::appBackground},
    {"panel_background",&ui::UiStyleTokens::panelBackground},
    {"panel_background_alt",&ui::UiStyleTokens::panelBackgroundAlt},
    {"panel_border",&ui::UiStyleTokens::panelBorder},
    {"accent",&ui::UiStyleTokens::accent},{"accent_muted",&ui::UiStyleTokens::accentMuted},
    {"success",&ui::UiStyleTokens::success},{"warning",&ui::UiStyleTokens::warning},
    {"danger",&ui::UiStyleTokens::danger},{"text_strong",&ui::UiStyleTokens::textStrong},
    {"text_muted",&ui::UiStyleTokens::textMuted},{"generic_plot_background",&ui::UiStyleTokens::genericPlotBackground},
    {"control_border",&ui::UiStyleTokens::controlBorder},
    {"floating_background",&ui::UiStyleTokens::floatingBackground},
    {"floating_border",&ui::UiStyleTokens::floatingBorder},
    {"tab_background",&ui::UiStyleTokens::tabBackground},{"tab_hovered",&ui::UiStyleTokens::tabHovered},
    {"tab_selected",&ui::UiStyleTokens::tabSelected},{"tab_dimmed",&ui::UiStyleTokens::tabDimmed},
    {"tab_dimmed_selected",&ui::UiStyleTokens::tabDimmedSelected},
    {"tab_selected_overline",&ui::UiStyleTokens::tabSelectedOverline},
    {"tab_dimmed_selected_overline",&ui::UiStyleTokens::tabDimmedSelectedOverline},
}};
inline void mapped(const ui::UiThemeDefinition& d)
{
    struct Mapping { ImGuiCol slot; ImVec4 ui::UiStyleTokens::* member; };
    const Mapping mappings[]{
        {ImGuiCol_Text,&ui::UiStyleTokens::textStrong},{ImGuiCol_TextDisabled,&ui::UiStyleTokens::textMuted},
        {ImGuiCol_WindowBg,&ui::UiStyleTokens::appBackground},{ImGuiCol_ChildBg,&ui::UiStyleTokens::panelBackground},
        {ImGuiCol_FrameBg,&ui::UiStyleTokens::panelBackgroundAlt},{ImGuiCol_Button,&ui::UiStyleTokens::panelBackgroundAlt},
        {ImGuiCol_Border,&ui::UiStyleTokens::controlBorder},{ImGuiCol_PopupBg,&ui::UiStyleTokens::floatingBackground},
        {ImGuiCol_Separator,&ui::UiStyleTokens::panelBorder},{ImGuiCol_TableBorderStrong,&ui::UiStyleTokens::panelBorder},
        {ImGuiCol_Tab,&ui::UiStyleTokens::tabBackground},{ImGuiCol_TabHovered,&ui::UiStyleTokens::tabHovered},
        {ImGuiCol_TabSelected,&ui::UiStyleTokens::tabSelected},{ImGuiCol_TabDimmed,&ui::UiStyleTokens::tabDimmed},
        {ImGuiCol_TabDimmedSelected,&ui::UiStyleTokens::tabDimmedSelected},
        {ImGuiCol_TabSelectedOverline,&ui::UiStyleTokens::tabSelectedOverline},
        {ImGuiCol_TabDimmedSelectedOverline,&ui::UiStyleTokens::tabDimmedSelectedOverline}
    };
    for (const auto& m:mappings) color(ImGui::GetStyle().Colors[m.slot],d.ui.*m.member,ImGui::GetStyleColorName(m.slot));
    if (ImPlot::GetCurrentContext()) {
        color(ImPlot::GetStyle().Colors[ImPlotCol_PlotBg],d.ui.genericPlotBackground,"ImPlot.PlotBg");
        color(ImPlot::GetStyle().Colors[ImPlotCol_PlotBorder],d.ui.panelBorder,"ImPlot.PlotBorder");
    }
}
inline void sameStyles(const ImGuiStyle& a, const ImGuiStyle& b)
{
    for (int slot=0;slot<ImGuiCol_COUNT;++slot) color(a.Colors[slot],b.Colors[slot],ImGui::GetStyleColorName(slot));
    if (a.FrameBorderSize!=b.FrameBorderSize || a.WindowBorderSize!=b.WindowBorderSize ||
        a.ChildBorderSize!=b.ChildBorderSize || a.PopupBorderSize!=b.PopupBorderSize ||
        a.TabBorderSize!=b.TabBorderSize || a.TabBarOverlineSize!=b.TabBarOverlineSize ||
        a.FrameRounding!=b.FrameRounding || a.WindowRounding!=b.WindowRounding ||
        a.WindowPadding.x!=b.WindowPadding.x || a.WindowPadding.y!=b.WindowPadding.y ||
        a.FramePadding.x!=b.FramePadding.x || a.FramePadding.y!=b.FramePadding.y ||
        a.ItemSpacing.x!=b.ItemSpacing.x || a.ItemSpacing.y!=b.ItemSpacing.y ||
        a.Alpha!=b.Alpha || a.DisabledAlpha!=b.DisabledAlpha)
        throw std::runtime_error("主题样式尺寸或透明度未恢复");
}
// 仅用于期望像素的标准 source-over，不调用生产校色函数求自己的预期。
inline ImVec4 over(ImVec4 foreground, ImVec4 background)
{
    const float alpha=foreground.w+background.w*(1-foreground.w);
    if (alpha==0) return {0,0,0,0};
    return {(foreground.x*foreground.w+background.x*background.w*(1-foreground.w))/alpha,
            (foreground.y*foreground.w+background.y*background.w*(1-foreground.w))/alpha,
            (foreground.z*foreground.w+background.z*background.w*(1-foreground.w))/alpha,alpha};
}
} // namespace protoscope::tests::theme
