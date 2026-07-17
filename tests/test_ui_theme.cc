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

void test_ui_theme_professional_dark_preserves_existing_tokens()
{
    const auto& definition = protoscope::ui::uiThemeDefinition(protoscope::config::GuiTheme::ProfessionalDark);
    const auto& ui = definition.ui;
    const auto& wave = definition.wave;

    requireColor(ui.appBackground, ImVec4(0.05F, 0.07F, 0.10F, 1.0F), "专业深色全局背景不应变化");
    requireColor(ui.panelBackground, ImVec4(0.09F, 0.11F, 0.15F, 1.0F), "专业深色面板背景不应变化");
    requireColor(ui.panelBorder, ImVec4(0.22F, 0.29F, 0.38F, 0.95F), "专业深色面板边框不应变化");
    requireColor(ui.accent, ImVec4(0.18F, 0.58F, 0.88F, 1.0F), "专业深色强调色不应变化");
    requireColor(ui.genericPlotBackground, ImVec4(0.07F, 0.09F, 0.13F, 1.0F), "专业深色普通图表背景不应变化");
    requireColor(wave.plotBackground, ImVec4(0.043F, 0.067F, 0.094F, 1.0F), "专业深色波形背景不应变化");
    requireColor(wave.gridMajor, ImVec4(0.12F, 0.19F, 0.27F, 0.78F), "专业深色主网格色不应变化");
    requireColor(wave.gridMinorTick, ImVec4(0.30F, 0.46F, 0.60F, 0.76F), "专业深色五等分短刻度色不应变化");
    requireColor(wave.gridCenter, ImVec4(0.90F, 0.96F, 1.0F, 0.66F), "专业深色中心十字线色不应变化");
    requireColor(wave.splitChannelLabel,
                 ImVec4(0.90F, 0.94F, 0.98F, 0.86F),
                 "专业深色 Split 通道标签色不应变化");
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

    requireColor(ui.appBackground, rgb8(6, 9, 12), "高对比全局背景应匹配预设");
    requireColor(ui.panelBackground, rgb8(9, 12, 16), "高对比面板背景应匹配预设");
    requireColor(ui.panelBackgroundAlt, rgb8(14, 19, 25), "高对比次级面板应匹配预设");
    requireColor(ui.panelBorder, rgb8(46, 82, 107), "高对比面板边框应匹配预设");
    requireColor(ui.accent, rgb8(46, 184, 250), "高对比强调色应匹配预设");
    requireColor(ui.textStrong, rgb8(245, 250, 255), "高对比主文字应匹配预设");
    requireColor(ui.textMuted, rgb8(179, 194, 209), "高对比次文字应匹配预设");
    requireColor(ui.genericPlotBackground, rgb8(5, 7, 10), "高对比普通图表背景应匹配预设");
    requireColor(wave.plotBackground, rgb8(5, 7, 10), "高对比波形背景应匹配预设");
    requireColor(wave.gridMajor, rgb8(46, 82, 107, 0.92F), "高对比主网格应匹配预设");
    requireColor(wave.gridMinorTick, rgb8(88, 137, 174, 0.92F), "高对比五等分短刻度应匹配预设");
    requireColor(wave.gridCenter, rgb8(225, 245, 255, 0.96F), "高对比中心十字线应匹配预设");
    requireColor(wave.legendOverlayBackground, rgb8(8, 12, 17, 0.98F), "高对比图例背景应匹配预设");
    requireColor(wave.legendOverlayBorder, rgb8(56, 127, 170), "高对比图例边框应匹配预设");
    requireColor(wave.legendOverlayTextPrimary, rgb8(245, 250, 255), "高对比图例主文字应匹配预设");
    requireColor(wave.legendOverlayTextSecondary, rgb8(179, 194, 209), "高对比图例辅助文字应匹配预设");
    requireColor(wave.legendOverlayRowHover, rgb8(14, 26, 36), "高对比图例悬停行应匹配预设");
    requireColor(wave.legendOverlayRowActive, rgb8(14, 46, 66), "高对比图例激活行应匹配预设");
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
    protoscope::tests::require(compositedContrast(wave.gridMajor, wave.plotBackground) >= 2.10,
                               "高对比主网格与背景对比度不得低于 2.10:1");
    protoscope::tests::require(compositedContrast(wave.gridMinorTick, wave.plotBackground) >= 4.0,
                               "高对比短刻度与背景对比度不得低于 4:1");
    protoscope::tests::require(compositedContrast(wave.gridCenter, wave.plotBackground) >= 10.0,
                               "高对比中心线与背景对比度不得低于 10:1");

    ImGui::CreateContext();
    ImPlot::CreateContext();
    protoscope::ui::applyUiTheme(protoscope::config::GuiTheme::ProfessionalDark);
    requireColor(ImGui::GetStyle().Colors[ImGuiCol_WindowBg],
                 ImVec4(0.05F, 0.07F, 0.10F, 1.0F),
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
                 ImVec4(0.05F, 0.07F, 0.10F, 1.0F),
                 "往返切换后专业深色实际 ImGui 样式应恢复");
    requireColor(ImPlot::GetStyle().Colors[ImPlotCol_PlotBg],
                 ImVec4(0.07F, 0.09F, 0.13F, 1.0F),
                 "往返切换后专业深色实际 ImPlot 样式应恢复");
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}
