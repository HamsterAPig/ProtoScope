#include "protoscope/config/config.hpp"
#include "protoscope/ui/ui_theme.hpp"
#include "../src/ui/wave/wave_render_service.hpp"
#include "test_helpers.hpp"
#include "theme_test_contract.hpp"

#include <algorithm>
#include <cmath>
#include <implot.h>
#include <imgui_internal.h>

namespace {
using namespace protoscope;
using tests::require;
using tests::theme::color;

double linearSrgb(double value)
{
    return value<=.04045 ? value/12.92 : std::pow((value+.055)/1.055,2.4);
}
double luminance(ImVec4 c)
{
    return .2126*linearSrgb(c.x)+.7152*linearSrgb(c.y)+.0722*linearSrgb(c.z);
}
double compositedContrast(ImVec4 foreground,ImVec4 background)
{
    const double a=luminance(tests::theme::over(foreground,background)),b=luminance(background);
    return ((std::max)(a,b)+.05)/((std::min)(a,b)+.05);
}
void verifyWaveCorrection(const ui::UiThemeDefinition& d)
{
    // 这些是信号/游标显示算法契约，不是 UI 审美比例。
    if(d.base=="professional_dark") {
        const auto readable=ui::displayColor(d.wave.cursorPalette.front(),d.wave.plotBackground);
        color(ImGui::ColorConvertU32ToFloat4(ui::cursorGuardColor(readable)),d.wave.plotBackground,"可读身份色使用背景护线");
        const auto darkGuard=ImGui::ColorConvertU32ToFloat4(ui::cursorGuardColor({.09F,.09F,.09F,1}));
        require(compositedContrast(darkGuard,d.wave.plotBackground)>=3 && darkGuard.x<.8F,"暗手动游标保留非白可读护线");
    } else {
        require(ui::cursorGuardColor({.09F,.09F,.09F,1})==IM_COL32(255,255,255,255),"其他基底保留暗身份色白护线");
    }
    for (auto c:d.wave.channelPalette)
        require(compositedContrast(ui::displayColor(c,d.wave.plotBackground,.65F),d.wave.plotBackground)>=3,
                "通道显示校色不得回退");
    const auto lighter=ui::displayColor({.8F,.8F,.8F,1},{.5F,.5F,.5F,1});
    require(lighter.x>.8F && lighter.x<.88F && compositedContrast(lighter,{.5F,.5F,.5F,1})>=3,
            "近达标信号应选择最小 RGB 调整");
    const ImVec4 saturated(.05F,.8F,.4F,.08F),readable(.1F,.9F,.5F,1);
    color(ui::displayColor(readable,{0,0,0,1}),readable,"已达标信号不改色");
    const auto vivid=ui::displayColor(saturated,{0,0,0,1});
    require(vivid.x==saturated.x && vivid.y==saturated.y && vivid.z==saturated.z &&
            vivid.w>saturated.w && compositedContrast(vivid,{0,0,0,1})>=3,"先增 alpha 而非褪色");
    for (auto bg:{ImVec4(0,0,0,1),ImVec4(1,1,1,1),ImVec4(.5F,.5F,.5F,1)})
        require(compositedContrast(ui::displayColor(saturated,bg),bg)>=3,"黑白中灰信号校色");
    const ImVec4 translucent(.2F,.3F,.4F,.25F);
    color(ui::displayColor(saturated,translucent),
          ui::displayColor(saturated,tests::theme::over(translucent,d.ui.appBackground)),"透明波形背景合成");
    auto custom=d;
    custom.wave.correctContrast=false;
    ui::applyUiTheme(custom);
    color(ui::displayColor(saturated,d.wave.plotBackground),saturated,"关闭校色保留源色");
    auto faded=saturated; faded.w*=.4F;
    color(ui::displayColor(saturated,d.wave.plotBackground,.4F),faded,"关闭校色仍处理请求 opacity");
    ui::applyUiTheme(d);
    require(ui::displayColor({1,1,1,0},d.wave.plotBackground).w==0,"零 alpha 不校色");
}
}

void test_ui_theme_professional_dark_preset()
{
    using namespace protoscope;
    const auto& d=ui::uiThemeDefinition(config::GuiTheme::ProfessionalDark);
    tests::require(d.id=="professional_dark" && d.base==d.id && d.name=="石墨青绿","深色身份及显示名称");
    tests::require(d.wave.channelPalette.size()>=2 && !d.wave.cursorPalette.empty(),"保留信号与游标色板");
    tests::require(d.wave.gridMajorWidth>0 && d.wave.gridMinorTickWidth>0 && d.wave.gridCenterWidth>0 &&
                   d.wave.gridMinorTickHalfLength>0,"网格几何必须有效");
}

// 保留既有注册函数名；debug_high_contrast 现在是兼容 ID，不再代表 UI 比例门槛。
void test_ui_theme_high_contrast_tokens_and_grid_contrast()
{
    using namespace protoscope;
    ImGui::CreateContext(); ImPlot::CreateContext();
    auto& io=ImGui::GetIO();
    io.DisplaySize={1280,720}; io.DeltaTime=1.F/60; io.IniFilename=nullptr;
    unsigned char* pixels; int w,h;
    io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    ImGui::NewFrame();
    for (int cycle=0;cycle<3;++cycle)
        for (auto theme:{config::GuiTheme::ProfessionalLight,config::GuiTheme::ProfessionalDark,config::GuiTheme::DebugHighContrast}) {
            const auto& d=ui::uiThemeDefinition(theme);
            ui::applyUiTheme(d);
            tests::theme::mapped(d);
            require(ImGui::GetStyle().WindowPadding.x==d.ui.windowPaddingX,"切换不能累积缩放");
            require(ImGui::GetStyle().TabBarOverlineSize==1 && ImGui::GetStyle().TabBorderSize==0 &&
                    ImGui::GetStyle().PopupBorderSize==1,"原生标签上沿和 Popup 边框尺寸");
            const auto original=ImGui::GetStyle();
            const auto plotOriginal=ImPlot::GetStyle();
            const auto guard=ui::cursorGuardColor({.08F,.09F,.10F,1});
            auto renamed=d; renamed.id="custom_copy";
            ui::applyUiTheme(renamed);
            tests::theme::sameStyles(original,ImGui::GetStyle());
            require(ui::cursorGuardColor({.08F,.09F,.10F,1})==guard,"改自定义 ID 不应改变波形护线");
            for(int slot=0;slot<ImPlotCol_COUNT;++slot)
                color(ImPlot::GetStyle().Colors[slot],plotOriginal.Colors[slot],"改 ID 不改变 ImPlot");
            renamed.wave.correctContrast=false;
            ui::applyUiTheme(renamed);
            tests::theme::sameStyles(original,ImGui::GetStyle());
            ui::applyUiTheme(d);
            ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({600,650});
            ImGui::Begin("按钮角色恢复检查");
            ImGui::SetCursorPos({12,32});
            const int colors=GImGui->ColorStack.Size,vars=GImGui->StyleVarStack.Size;
            int roleIndex=0;
            for(auto role:{ui::UiButtonRole::Secondary,ui::UiButtonRole::Primary,ui::UiButtonRole::Toolbar,ui::UiButtonRole::Danger}) {
                ImGui::PushID(roleIndex++); ui::drawUiButton("角色按钮",{80,30},role); ImGui::PopID();
                tests::theme::sameStyles(original,ImGui::GetStyle());
            }
            ui::drawToolbarSectionButton("开关",nullptr,true,{80,30});
            ui::drawGhostIconButton("选项",nullptr);
            const auto id=ImGui::GetCurrentWindow()->GetID("键盘按钮");
            GImGui->NavActivateId=GImGui->NavActivateDownId=id;
            require(ui::drawUiButton("键盘按钮",{80,30},ui::UiButtonRole::Toolbar),"键盘导航必须激活角色按钮");
            GImGui->NavActivateId=GImGui->NavActivateDownId=0; ImGui::ClearActiveID();
            tests::theme::sameStyles(original,ImGui::GetStyle());
            char input[8]{}; ImGui::InputText("按钮后的输入",input,sizeof(input));
            bool checked=true; ImGui::Checkbox("按钮后的复选框",&checked);
            ui::beginToolbarGroup("group","容器",80);
            color(ImGui::GetStyleColorVec4(ImGuiCol_Border),d.ui.controlBorder,"容器装饰不污染内部输入");
            ImGui::InputText("组内输入",input,sizeof(input)); ui::endToolbarGroup();
            require(ImGui::GetStyle().DisabledAlpha==.72F,"三款统一原生禁用 alpha");
            ui::beginDisabled();
            color(ImGui::GetStyleColorVec4(ImGuiCol_Button),d.ui.panelBackgroundAlt,"禁用不置空黑底");
            color(ImGui::GetStyleColorVec4(ImGuiCol_Text),d.ui.textStrong,"禁用不补偿正文 alpha");
            const float alpha=ImGui::GetStyle().Alpha;
            require(std::abs(alpha-original.Alpha*.72F)<1e-6F,"禁用 alpha 仅乘一次");
            ui::beginDisabled(false);
            require(ImGui::GetStyle().Alpha==alpha && (GImGui->CurrentItemFlags&ImGuiItemFlags_Disabled),"嵌套仍禁用且不重复淡化");
            const auto disabledId=ImGui::GetCurrentWindow()->GetID("禁用键盘按钮");
            GImGui->NavActivateId=GImGui->NavActivateDownId=disabledId;
            require(!ui::drawUiButton("禁用键盘按钮",{80,30}),"禁用按钮不可导航激活");
            GImGui->NavActivateId=GImGui->NavActivateDownId=0;
            ui::endDisabled(); ui::endDisabled();
            tests::theme::sameStyles(original,ImGui::GetStyle());
            require(colors==GImGui->ColorStack.Size && vars==GImGui->StyleVarStack.Size,"角色/容器/禁用样式栈平衡");
            ImGui::End();
            verifyWaveCorrection(d);
            plot::WaveViewState view;
            view.cursorColors.key.automatic=true;
            view.cursorColors.labelText={1,1,1,1};
            const plot::OverviewColor identity{.6,.7,.2,.4};
            view.cursorColors.entries.push_back({17,{identity,3.5,5,true,true}});
            color(ui::cursorLabelText(view,17,0),ui::cursorImVec(identity),"textPass 通过不得再有 debug 12:1 例外");
        }
    // 合法的同色/低对比/全透明输入不要求状态相异，只要求直接应用。
    auto custom=ui::uiThemeDefinition(config::GuiTheme::DebugHighContrast);
    for (float alpha:{0.F,.25F,1.F}) {
        for(const auto& field:tests::theme::fields) custom.ui.*field.member={.4F,.4F,.4F,alpha};
        ui::applyUiTheme(custom); tests::theme::mapped(custom);
        const auto before=ImGui::GetStyle();
        custom.wave.correctContrast=!custom.wave.correctContrast;
        ui::applyUiTheme(custom); tests::theme::sameStyles(before,ImGui::GetStyle());
        const auto base=tests::theme::over(custom.ui.panelBackgroundAlt,custom.ui.panelBackground);
        auto accent=custom.ui.accent; accent.w*=.10F;
        color(ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered),tests::theme::over(accent,base),"悬停仅一次混色且尊重源 alpha");
    }
    ui::applyUiTheme(config::GuiTheme::ProfessionalDark);
    ImGui::EndFrame(); ImPlot::DestroyContext(); ImGui::DestroyContext();
}
