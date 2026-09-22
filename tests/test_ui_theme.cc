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

    requireColor(ui.appBackground, rgb8(15, 23, 34), "专业深色全局背景应匹配新预设");
    requireColor(ui.panelBackground, rgb8(22, 33, 47), "专业深色面板背景应匹配新预设");
    requireColor(wave.plotBackground, rgb8(10, 18, 28), "专业深色波形背景应匹配新预设");
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

    requireColor(ui.appBackground, rgb8(5, 8, 12), "高对比全局背景应匹配预设");
    requireColor(ui.panelBackground, rgb8(10, 15, 21), "高对比面板背景应匹配预设");
    requireColor(ui.panelBackgroundAlt, rgb8(18, 26, 35), "高对比次级面板应匹配预设");
    requireColor(ui.panelBorder, rgb8(58, 72, 87), "高对比面板边框应匹配预设");
    requireColor(ui.accent, rgb8(46, 184, 250), "高对比强调色应匹配预设");
    requireColor(ui.textStrong, rgb8(245, 250, 255), "高对比主文字应匹配预设");
    requireColor(ui.textMuted, rgb8(200, 215, 230), "高对比次文字应匹配预设");
    requireColor(ui.genericPlotBackground, rgb8(2, 5, 9), "高对比普通图表背景应匹配预设");
    requireColor(wave.plotBackground, rgb8(2, 5, 9), "高对比波形背景应匹配预设");
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
                 rgb8(15, 23, 34),
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
                 rgb8(15, 23, 34),
                 "往返切换后专业深色实际 ImGui 样式应恢复");
    requireColor(ImPlot::GetStyle().Colors[ImPlotCol_PlotBg],
                 rgb8(10, 18, 28),
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
            const auto& colors = ImGui::GetStyle().Colors;
            const bool high = theme == protoscope::config::GuiTheme::DebugHighContrast;
            for (auto slot : {ImGuiCol_FrameBg, ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive,
                             ImGuiCol_Button, ImGuiCol_ButtonHovered, ImGuiCol_ButtonActive, ImGuiCol_Header,
                             ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive, ImGuiCol_Tab, ImGuiCol_TabSelected,
                             ImGuiCol_TabHovered, ImGuiCol_TabDimmedSelected, ImGuiCol_CheckboxSelectedBg,
                             ImGuiCol_TextSelectedBg, ImGuiCol_TableHeaderBg, ImGuiCol_TableRowBgAlt,
                             ImGuiCol_MenuBarBg, ImGuiCol_PopupBg}) {
                protoscope::tests::require(compositedContrast(d.ui.textStrong, colors[slot]) >= (high ? 12 : 4.5),
                                           "全部控件状态正文阈值");
                protoscope::tests::require(compositedContrast(d.ui.textMuted, colors[slot]) >= (high ? 7 : 4.5),
                                           "全部控件状态辅助文字阈值");
                protoscope::tests::require(compositedContrast(colors[ImGuiCol_Border], colors[slot]) >= 3,
                                           "必要控件边界阈值");
            }
            for (auto slot : {ImGuiCol_InputTextCursor, ImGuiCol_CheckMark, ImGuiCol_SliderGrab,
                             ImGuiCol_NavCursor, ImGuiCol_TabSelectedOverline, ImGuiCol_TabDimmedSelectedOverline,
                             ImGuiCol_DragDropTarget, ImGuiCol_UnsavedMarker})
                for (auto bg : {colors[ImGuiCol_FrameBg], colors[ImGuiCol_FrameBgActive], colors[ImGuiCol_CheckboxSelectedBg]})
                    protoscope::tests::require(compositedContrast(colors[slot], bg) >= 3, "焦点/标记/输入光标阈值");
            requireColor(colors[ImGuiCol_InputTextCursor], d.ui.textStrong, "输入光标不得残留深色基准");
            protoscope::tests::require(colors[ImGuiCol_ModalWindowDimBg].w > 0 && colors[ImGuiCol_ModalWindowDimBg].w < 1,
                                       "模态遮罩必须半透明");
            if (theme == protoscope::config::GuiTheme::ProfessionalLight) {
                requireColor(d.ui.panelBackground, rgb8(255,255,255), "浅色内容面板应为白色");
                protoscope::tests::require(relativeLuminance(d.ui.panelBackgroundAlt) > .9, "浅色工具区不应大片灰底");
            }
            const auto lighter = protoscope::ui::displayColor(ImVec4(.8F,.8F,.8F,1),ImVec4(.5F,.5F,.5F,1));
            protoscope::tests::require(lighter.x > .8F && lighter.x < .88F &&
                                       compositedContrast(lighter,ImVec4(.5F,.5F,.5F,1)) >= 3,
                                       "近达标亮色应选小幅提亮，不能大幅反转到黑端");
            const float disabled = ImGui::GetStyle().DisabledAlpha;
            protoscope::tests::require(disabled > 0 && disabled <= (high ? .86F : .72F),
                                       "禁用态必须明确淡化，不能接近启用态换取对比度");
            protoscope::ui::beginDisabled();
            const auto disabledFill=ImGui::GetStyleColorVec4(ImGuiCol_Button);
            auto disabledText=ImGui::GetStyleColorVec4(ImGuiCol_Text);
            disabledText.w*=ImGui::GetStyle().Alpha*.9F;
            protoscope::ui::beginDisabled(false); // 嵌套不得重复淡化或丢失禁用正文策略。
            requireColor(ImGui::GetStyleColorVec4(ImGuiCol_Button),disabledFill,"嵌套禁用表面稳定");
            protoscope::ui::endDisabled();
            for (auto parent : {d.ui.appBackground,d.ui.panelBackground,d.ui.panelBackgroundAlt}) {
                const ImVec4 disabledBg(disabledFill.x*disabled+parent.x*(1-disabled),
                                        disabledFill.y*disabled+parent.y*(1-disabled),
                                        disabledFill.z*disabled+parent.z*(1-disabled),1);
                protoscope::tests::require(compositedContrast(disabledText,disabledBg)>=(high?12:4.5),
                                           "禁用90%覆盖字体核心不得降低阈值");
            }
            protoscope::ui::endDisabled();
            requireColor(ImGui::GetStyleColorVec4(ImGuiCol_Text),d.ui.textStrong,"禁用结束恢复正文");
            requireColor(ImGui::GetStyleColorVec4(ImGuiCol_Button),d.ui.panelBackgroundAlt,"禁用结束恢复填充");
            const ImVec4 saturated(.05F, .8F, .4F, .08F);
            const ImVec4 alreadyReadable(.1F,.9F,.5F,1);
            requireColor(protoscope::ui::displayColor(alreadyReadable,ImVec4(0,0,0,1)),alreadyReadable,
                         "已达标颜色不得重新校色");
            const auto vivid = protoscope::ui::displayColor(saturated, ImVec4(0,0,0,1));
            protoscope::tests::require(vivid.x == saturated.x && vivid.y == saturated.y && vivid.z == saturated.z &&
                                       vivid.w > saturated.w && compositedContrast(vivid, ImVec4(0,0,0,1)) >= 3,
                                       "低 alpha 达标原色应只提升 alpha，禁止先褪色");
            for (auto bg : {ImVec4(0,0,0,1), ImVec4(1,1,1,1), ImVec4(.5F,.5F,.5F,1)}) {
                const auto corrected = protoscope::ui::displayColor(saturated, bg);
                protoscope::tests::require(compositedContrast(corrected, bg) >= 3, "黑白中灰显示修正");
            }
            const ImVec4 translucent(.2F,.3F,.4F,.25F);
            const ImVec4 effective(translucent.x*.25F+d.ui.appBackground.x*.75F,
                                   translucent.y*.25F+d.ui.appBackground.y*.75F,
                                   translucent.z*.25F+d.ui.appBackground.z*.75F,1);
            requireColor(protoscope::ui::displayColor(saturated, translucent),
                         protoscope::ui::displayColor(saturated, effective), "透明背景必须合成父表面");
            auto custom = d;
            custom.wave.correctContrast = false;
            protoscope::ui::applyUiTheme(custom);
            requireColor(protoscope::ui::displayColor(saturated, d.wave.plotBackground), saturated, "关闭修正不得改源色");
            protoscope::ui::applyUiTheme(d);
            const auto transparent = protoscope::ui::displayColor(ImVec4(1,1,1,0), d.wave.plotBackground);
            protoscope::tests::require(transparent.w == 0, "显式全透明色必须保留");
        }
    }
    protoscope::ui::applyUiTheme(protoscope::config::GuiTheme::ProfessionalDark);
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}
