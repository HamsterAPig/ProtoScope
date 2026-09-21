#include "protoscope/config/config.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>

#include <implot.h>

namespace {

bool nearlyEqual(const float left, const float right)
{
    return std::abs(left - right) <= 1e-6F;
}

void requireColor(const ImVec4& actual, const ImVec4& expected, const char* message)
{
    protoscope::tests::require(nearlyEqual(actual.x, expected.x) && nearlyEqual(actual.y, expected.y) &&
                                   nearlyEqual(actual.z, expected.z) && nearlyEqual(actual.w, expected.w),
                               message);
}

double linearSrgb(const double value)
{
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

double relativeLuminance(const ImVec4& color)
{
    return 0.2126 * linearSrgb(color.x) + 0.7152 * linearSrgb(color.y) + 0.0722 * linearSrgb(color.z);
}

double compositedContrast(const ImVec4& foreground, const ImVec4& background)
{
    const ImVec4 composited{
        foreground.x * foreground.w + background.x * (1.0F - foreground.w),
        foreground.y * foreground.w + background.y * (1.0F - foreground.w),
        foreground.z * foreground.w + background.z * (1.0F - foreground.w),
        1.0F,
    };
    const double foregroundLuminance = relativeLuminance(composited);
    const double backgroundLuminance = relativeLuminance(background);
    const double lighter = (std::max)(foregroundLuminance, backgroundLuminance);
    const double darker = (std::min)(foregroundLuminance, backgroundLuminance);
    return (lighter + 0.05) / (darker + 0.05);
}

ImVec4 rgb8(const int red, const int green, const int blue, const float alpha = 1.0F)
{
    constexpr float kColorScale = 1.0F / 255.0F;
    return ImVec4(static_cast<float>(red) * kColorScale,
                  static_cast<float>(green) * kColorScale,
                  static_cast<float>(blue) * kColorScale,
                  alpha);
}

} // namespace

void test_ui_theme_professional_dark_preset()
{
    const auto& definition = protoscope::ui::uiThemeDefinition(protoscope::config::GuiTheme::ProfessionalDark);
    const auto& ui = definition.ui;
    const auto& wave = definition.wave;

    requireColor(ui.appBackground, rgb8(27, 29, 33), "专业深色全局背景应匹配新预设");
    requireColor(ui.panelBackground, rgb8(36, 39, 44), "专业深色面板背景应匹配新预设");
    requireColor(wave.plotBackground, rgb8(21, 23, 27), "专业深色波形背景应匹配新预设");
    protoscope::tests::require(nearlyEqual(wave.gridMajorWidth, 1.0F) && nearlyEqual(wave.gridMinorTickWidth, 1.0F) &&
                                   nearlyEqual(wave.gridCenterWidth, 1.4F) &&
                                   nearlyEqual(wave.gridMinorTickHalfLength, 2.0F),
                               "专业深色网格线宽和短刻度长度不应变化");
}

void test_ui_theme_high_contrast_tokens_and_grid_contrast()
{
    const auto& definition = protoscope::ui::uiThemeDefinition(protoscope::config::GuiTheme::DebugHighContrast);
    const auto& ui = definition.ui;
    const auto& wave = definition.wave;

    requireColor(ui.appBackground, rgb8(12, 13, 15), "高对比全局背景应匹配预设");
    requireColor(ui.panelBackground, rgb8(19, 21, 24), "高对比面板背景应匹配预设");
    requireColor(ui.panelBackgroundAlt, rgb8(28, 30, 34), "高对比次级面板应匹配预设");
    requireColor(ui.panelBorder, rgb8(125, 137, 151), "高对比面板边框应匹配预设");
    requireColor(ui.accent, rgb8(46, 184, 250), "高对比强调色应匹配预设");
    requireColor(ui.textStrong, rgb8(245, 250, 255), "高对比主文字应匹配预设");
    requireColor(ui.textMuted, rgb8(196, 203, 213), "高对比次文字应匹配预设");
    requireColor(ui.genericPlotBackground, rgb8(8, 9, 11), "高对比普通图表背景应匹配预设");
    requireColor(wave.plotBackground, rgb8(8, 9, 11), "高对比波形背景应匹配预设");
    requireColor(wave.legendOverlayTextPrimary, rgb8(245, 250, 255), "高对比图例主文字应匹配预设");
    requireColor(wave.legendOverlayTextSecondary, ui.textMuted, "高对比图例辅助文字应匹配预设");
    requireColor(wave.legendOverlayRowActiveBorder, rgb8(46, 184, 250), "高对比图例激活边框应匹配预设");
    protoscope::tests::require(nearlyEqual(wave.gridMajorWidth, 1.2F) && nearlyEqual(wave.gridMinorTickWidth, 1.2F) &&
                                   nearlyEqual(wave.gridCenterWidth, 1.8F) &&
                                   nearlyEqual(wave.gridMinorTickHalfLength, 2.5F),
                               "高对比网格线宽和短刻度长度应匹配预设");

    protoscope::tests::require(compositedContrast(wave.legendOverlayTextPrimary, wave.legendOverlayBackground) >= 12.0,
                               "图例主文字与背景对比度不得低于 12:1");
    protoscope::tests::require(
        compositedContrast(wave.legendOverlayTextSecondary, wave.legendOverlayBackground) >= 7.0,
        "图例辅助文字与普通背景对比度不得低于 7:1");
    protoscope::tests::require(
        compositedContrast(wave.legendOverlayTextSecondary, wave.legendOverlayRowActive) >= 7.0,
        "图例辅助文字与激活行背景对比度不得低于 7:1");
    protoscope::tests::require(compositedContrast(wave.gridMajor, wave.plotBackground) <
                               compositedContrast(wave.gridCenter, wave.plotBackground), "网格应有层次");

    ImGui::CreateContext();
    ImPlot::CreateContext();
    protoscope::ui::applyUiTheme(protoscope::config::GuiTheme::ProfessionalDark);
    requireColor(ImGui::GetStyle().Colors[ImGuiCol_WindowBg],
                 rgb8(27, 29, 33),
                 "专业深色实际 ImGui 样式应先应用");
    protoscope::ui::applyUiTheme(protoscope::config::GuiTheme::DebugHighContrast);
    requireColor(protoscope::ui::activeWaveStyleTokens().plotBackground,
                 wave.plotBackground,
                 "applyUiTheme 后活动波形令牌应立即切换");
    requireColor(ImGui::GetStyle().Colors[ImGuiCol_WindowBg], ui.appBackground, "动态切换应立即更新 ImGui 窗口背景");
    requireColor(ImGui::GetStyle().Colors[ImGuiCol_Button],
                 ui.panelBackgroundAlt,
                 "高对比按钮默认态应使用中性深色");
    requireColor(ImPlot::GetStyle().Colors[ImPlotCol_PlotBg],
                 ui.genericPlotBackground,
                 "动态切换应立即更新 ImPlot 绘图区背景");
    protoscope::tests::require(nearlyEqual(ImGui::GetStyle().WindowRounding, 4.0F) &&
                                   nearlyEqual(ImGui::GetStyle().FrameRounding, 3.0F),
                               "高对比实际 ImGui 样式应使用 3-4px 圆角");
    protoscope::ui::applyUiTheme(protoscope::config::GuiTheme::ProfessionalDark);
    requireColor(ImGui::GetStyle().Colors[ImGuiCol_WindowBg],
                 rgb8(27, 29, 33),
                 "往返切换后专业深色实际 ImGui 样式应恢复");
    requireColor(ImPlot::GetStyle().Colors[ImPlotCol_PlotBg],
                 rgb8(21, 23, 27),
                 "往返切换后专业深色实际 ImPlot 样式应恢复");
    for (int cycle = 0; cycle < 3; ++cycle) {
        for (const auto theme : {protoscope::config::GuiTheme::ProfessionalLight,
                                protoscope::config::GuiTheme::ProfessionalDark,
                                protoscope::config::GuiTheme::DebugHighContrast}) {
            protoscope::ui::applyUiTheme(theme);
            const auto& d = protoscope::ui::uiThemeDefinition(theme);
            requireColor(ImGui::GetStyle().Colors[ImGuiCol_MenuBarBg], d.ui.panelBackground, "菜单背景不应残留");
            requireColor(ImPlot::GetStyle().Colors[ImPlotCol_PlotBg], d.wave.plotBackground, "绘图区不应残留");
            protoscope::tests::require(nearlyEqual(ImGui::GetStyle().WindowPadding.x, d.ui.windowPaddingX),
                                       "主题切换不能累积尺寸缩放");
            for (auto bg : {d.ui.appBackground, d.ui.panelBackground, d.ui.panelBackgroundAlt}) {
                protoscope::tests::require(compositedContrast(d.ui.textStrong, bg) >= 4.5, "正文对比度");
                protoscope::tests::require(compositedContrast(d.ui.textMuted, bg) >= 4.5, "辅助文字对比度");
            }
            for (auto slot : {ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_ButtonHovered,
                              ImGuiCol_ButtonActive, ImGuiCol_HeaderActive, ImGuiCol_TabActive}) {
                const auto bg = ImGui::GetStyle().Colors[slot];
                protoscope::tests::require(compositedContrast(d.ui.textMuted, bg) >= 4.5, "控件状态辅助文字对比度");
                if (theme == protoscope::config::GuiTheme::DebugHighContrast) {
                    protoscope::tests::require(compositedContrast(d.ui.textMuted, bg) >= 7, "高对比控件辅助文字");
                    protoscope::tests::require(compositedContrast(d.ui.textStrong, bg) >= 12, "高对比控件主文字");
                }
            }
            for (auto c : d.wave.channelPalette) {
                const auto shown = protoscope::ui::displayColor(c, d.wave.plotBackground, .65F);
                protoscope::tests::require(compositedContrast(shown, d.wave.plotBackground) >= 3., "通道显示对比度");
            }
            const auto transparent = protoscope::ui::displayColor(ImVec4(1,1,1,0), d.wave.plotBackground);
            protoscope::tests::require(transparent.w == 0, "显式全透明色必须保留");
        }
    }
    protoscope::ui::applyUiTheme(protoscope::config::GuiTheme::ProfessionalDark);
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}
