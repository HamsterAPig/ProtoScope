#include "protoscope/config/config.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>

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

    requireColor(ui.appBackground, ImVec4(0.035F, 0.047F, 0.067F, 1.0F), "高对比全局背景应匹配预设");
    requireColor(ui.panelBackground, ImVec4(0.055F, 0.071F, 0.096F, 1.0F), "高对比面板背景应匹配预设");
    requireColor(ui.panelBackgroundAlt, ImVec4(0.080F, 0.104F, 0.140F, 1.0F), "高对比次级面板应匹配预设");
    requireColor(ui.panelBorder, ImVec4(0.300F, 0.400F, 0.520F, 0.95F), "高对比面板边框应匹配预设");
    requireColor(ui.accent, ImVec4(0.200F, 0.650F, 0.980F, 1.0F), "高对比强调色应匹配预设");
    requireColor(ui.textStrong, ImVec4(0.960F, 0.980F, 1.000F, 1.0F), "高对比主文字应匹配预设");
    requireColor(ui.textMuted, ImVec4(0.680F, 0.760F, 0.840F, 1.0F), "高对比次文字应匹配预设");
    requireColor(ui.genericPlotBackground, ImVec4(0.045F, 0.058F, 0.078F, 1.0F), "高对比普通图表背景应匹配预设");
    requireColor(wave.plotBackground, ImVec4(0.025F, 0.040F, 0.058F, 1.0F), "高对比波形背景应匹配预设");
    requireColor(wave.gridMajor, ImVec4(0.210F, 0.310F, 0.420F, 0.82F), "高对比主网格应匹配预设");
    requireColor(wave.gridMinorTick, ImVec4(0.320F, 0.490F, 0.640F, 0.86F), "高对比五等分短刻度应匹配预设");
    requireColor(wave.gridCenter, ImVec4(0.750F, 0.840F, 0.920F, 0.84F), "高对比中心十字线应匹配预设");
    protoscope::tests::require(nearlyEqual(wave.gridMajorWidth, 1.2F) && nearlyEqual(wave.gridMinorTickWidth, 1.2F) &&
                                   nearlyEqual(wave.gridCenterWidth, 1.8F) &&
                                   nearlyEqual(wave.gridMinorTickHalfLength, 2.5F),
                               "高对比网格线宽和短刻度长度应匹配预设");

    protoscope::tests::require(compositedContrast(wave.gridMajor, wave.plotBackground) >= 1.90,
                               "高对比主网格与背景对比度不得低于 1.90:1");
    protoscope::tests::require(compositedContrast(wave.gridMinorTick, wave.plotBackground) >= 3.60,
                              "高对比短刻度与背景对比度不得低于 3.60:1");
    protoscope::tests::require(compositedContrast(wave.gridCenter, wave.plotBackground) >= 9.40,
                              "高对比中心线与背景对比度不得低于 9.40:1");

    protoscope::ui::applyUiTheme(protoscope::config::GuiTheme::DebugHighContrast);
    requireColor(protoscope::ui::activeWaveStyleTokens().plotBackground,
                 wave.plotBackground,
                 "applyUiTheme 后活动波形令牌应立即切换");
    protoscope::ui::applyUiTheme(protoscope::config::GuiTheme::ProfessionalDark);
}
