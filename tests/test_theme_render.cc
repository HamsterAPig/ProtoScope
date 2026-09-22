#include "../src/ui/wave/wave_render_service.hpp"

#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <sstream>
#include <thread>

using namespace protoscope;
namespace {
constexpr int width = 1000, height = 720;
void check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }

std::vector<unsigned char> capture(const std::filesystem::path& directory, const std::string& name)
{
    std::vector<unsigned char> pixels(width * height * 3);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    check(glGetError() == GL_NO_ERROR, "framebuffer read failed");
    if (!directory.empty()) {
        std::filesystem::create_directories(directory);
        auto bgr = pixels;
        for (std::size_t i = 0; i < bgr.size(); i += 3) std::swap(bgr[i], bgr[i+2]);
        std::array<unsigned char, 54> header{};
        header[0] = 'B'; header[1] = 'M'; header[26] = 1; header[28] = 24;
        const auto put = [&](std::size_t offset, std::uint32_t value) {
            for (unsigned i = 0; i < 4; ++i) header[offset+i] = static_cast<unsigned char>(value >> (8*i));
        };
        put(2, static_cast<std::uint32_t>(54 + bgr.size())); put(10,54); put(14,40); put(18,width); put(22,height);
        std::ofstream output(directory / (name + ".bmp"), std::ios::binary);
        output.write(reinterpret_cast<const char*>(header.data()), header.size());
        output.write(reinterpret_cast<const char*>(bgr.data()), static_cast<std::streamsize>(bgr.size()));
        check(bool(output), "screenshot write failed");
    }
    return pixels;
}

std::size_t matchingPixels(const std::vector<unsigned char>& pixels, ImVec4 color, ImVec2 min, ImVec2 max)
{
    std::size_t count = 0;
    for (int y = (std::max)(0, int(min.y)); y < (std::min)(height, int(max.y)); ++y)
        for (int x = (std::max)(0, int(min.x)); x < (std::min)(width, int(max.x)); ++x) {
            const auto i = ((height-1-y)*width+x)*3;
            if (std::abs(pixels[i] - color.x*255) <= 2 &&
                std::abs(pixels[i+1] - color.y*255) <= 2 &&
                std::abs(pixels[i+2] - color.z*255) <= 2) ++count;
        }
    return count;
}

std::size_t readableTextPixels(const std::vector<unsigned char>& pixels, ImVec2 min, ImVec2 max, double target)
{
    const auto read = [&](int x, int y) {
        const auto i = ((height-1-y)*width+x)*3;
        return plot::OverviewColor{pixels[i]/255.,pixels[i+1]/255.,pixels[i+2]/255.,1};
    };
    // 字体抗锯齿会改变字形像素 RGB，按实测背景统计满足对比度的核心像素。
    const auto background = read((std::max)(0,int(min.x)-2), int(min.y));
    std::size_t count = 0;
    for (int y = int(min.y); y < int(max.y); ++y)
        for (int x = int(min.x); x < int(max.x); ++x)
            count += plot::overviewContrast(read(x,y),background) >= target;
    return count;
}

// 新场景的参考图使用旧 UI 令牌 + 当前渲染器，绝不冒充旧版本截图。
ui::UiThemeDefinition baselineTokens(config::GuiTheme theme)
{
    auto d = ui::uiThemeDefinition(theme);
    const auto rgb = [](int r,int g,int b) { return ImVec4(r/255.F,g/255.F,b/255.F,1); };
    if (theme == config::GuiTheme::ProfessionalLight) {
        d.ui.appBackground=rgb(240,242,244); d.ui.panelBackground=rgb(247,248,250);
        d.ui.panelBackgroundAlt=rgb(229,233,239); d.ui.panelBorder=rgb(120,128,139);
        d.ui.textStrong=rgb(36,41,47); d.ui.textMuted=rgb(86,97,111); d.ui.accent=rgb(20,91,170);
    } else if (theme == config::GuiTheme::DebugHighContrast) {
        d.ui.appBackground=rgb(12,13,15); d.ui.panelBackground=rgb(19,21,24);
        d.ui.panelBackgroundAlt=rgb(28,30,34); d.ui.panelBorder=rgb(125,137,151);
        d.ui.textStrong=rgb(245,250,255); d.ui.textMuted=rgb(196,203,213); d.ui.accent=rgb(46,184,250);
    } else {
        d.ui.appBackground=rgb(27,29,33); d.ui.panelBackground=rgb(36,39,44);
        d.ui.panelBackgroundAlt=rgb(46,49,55); d.ui.panelBorder=rgb(113,123,135);
        d.ui.textStrong=rgb(237,245,252); d.ui.textMuted=rgb(180,188,200); d.ui.accent=rgb(46,148,224);
    }
    return d;
}

void verifyControls(const std::filesystem::path& directory, bool baseline)
{
    struct Region { ImRect bounds; ImVec4 text; double threshold; std::string control; ImU32 ink; };
    const auto actualContrast = [&](const std::vector<unsigned char>& pixels, const Region& region, const std::string& scene,
                                    ImVec2* corePosition = nullptr) {
        const auto r=region.bounds;
        const auto* data=ImGui::GetDrawData();
        check(data->DisplayPos.x==0 && data->DisplayPos.y==0 && data->FramebufferScale.x==1 && data->FramebufferScale.y==1,
              "control sampler requires 1:1 framebuffer coordinates");
        check(r.Min.x>=0 && r.Min.y>=0 && r.Max.x<=width && r.Max.y<=height,"control region outside framebuffer");
        // 控件内部最常见色是实测填充；原始 RGB 匹配仅作诊断，字形核心由下方 UV 覆盖确定。
        std::map<unsigned,std::size_t> frequency;
        for (int y=int(r.Min.y)+2;y<int(r.Max.y)-2;++y) for (int x=int(r.Min.x)+2;x<int(r.Max.x)-2;++x) {
            const auto i=((height-1-y)*width+x)*3;
            ++frequency[(unsigned(pixels[i])<<16)|(unsigned(pixels[i+1])<<8)|pixels[i+2]];
        }
        check(!frequency.empty(),"empty control region");
        const auto background=std::max_element(frequency.begin(),frequency.end(),
            [](const auto& a,const auto& b){return a.second<b.second;});
        const auto bg=plot::overviewRgb(background->first);
        std::size_t core=0;
        double minimumContrast=21;
        for (int y=int(r.Min.y)+2;y<int(r.Max.y)-2;++y) for (int x=int(r.Min.x)+2;x<int(r.Max.x)-2;++x) {
            const auto i=((height-1-y)*width+x)*3;
            if (std::abs(pixels[i]-region.text.x*255)<=2 && std::abs(pixels[i+1]-region.text.y*255)<=2 &&
                std::abs(pixels[i+2]-region.text.z*255)<=2) {
                const plot::OverviewColor pixel{pixels[i]/255.,pixels[i+1]/255.,pixels[i+2]/255.,1};
                minimumContrast=(std::min)(minimumContrast,plot::overviewContrast(pixel,bg));
                ++core;
            }
        }
        // 实测 msyh 字形顶点存在 .25/.5 等分数坐标，双线性过滤后并无足量原始 RGB。
        // 按实际索引、裁剪与字体 UV 采样覆盖率，限定真正字形内部，不能以边框冒充文字。
        unsigned char* atlas; int atlasWidth,atlasHeight;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&atlas,&atlasWidth,&atlasHeight);
        std::size_t readableCore=0, sampledCore=0, renderedCore=0;
        double maxCoverage=0;
        const auto texel=[&](int x,int y) { return atlas[((std::clamp)(y,0,atlasHeight-1)*atlasWidth+
            (std::clamp)(x,0,atlasWidth-1))*4+3]/255.; };
        for (const auto* list : ImGui::GetDrawData()->CmdLists) for (const auto& cmd : list->CmdBuffer)
            for (unsigned j=cmd.IdxOffset;j+2<cmd.IdxOffset+cmd.ElemCount;j+=3) {
                const auto& a=list->VtxBuffer[cmd.VtxOffset+list->IdxBuffer[j]];
                const auto& b=list->VtxBuffer[cmd.VtxOffset+list->IdxBuffer[j+2]];
                if (cmd.UserCallback || !r.Contains(a.pos) || !r.Contains(b.pos) || a.col!=region.ink || b.col!=region.ink ||
                    b.pos.x<=a.pos.x || b.pos.y<=a.pos.y ||
                    b.uv.x<=a.uv.x || b.uv.y<=a.uv.y) continue;
                for(int y=int(std::ceil(a.pos.y-.5F));y+.5F<b.pos.y;++y)
                    for(int x=int(std::ceil(a.pos.x-.5F));x+.5F<b.pos.x;++x) {
                        if(x<0 || x>=width || y<0 || y>=height || x+.5F<cmd.ClipRect.x || x+.5F>=cmd.ClipRect.z ||
                            y+.5F<cmd.ClipRect.y || y+.5F>=cmd.ClipRect.w) continue;
                        const double u=(a.uv.x+(b.uv.x-a.uv.x)*(x+.5F-a.pos.x)/(b.pos.x-a.pos.x))*atlasWidth-.5;
                        const double v=(a.uv.y+(b.uv.y-a.uv.y)*(y+.5F-a.pos.y)/(b.pos.y-a.pos.y))*atlasHeight-.5;
                        const int ix=int(std::floor(u)), iy=int(std::floor(v));
                        const double fx=u-ix, fy=v-iy;
                        const double alpha=(texel(ix,iy)*(1-fx)+texel(ix+1,iy)*fx)*(1-fy)+
                            (texel(ix,iy+1)*(1-fx)+texel(ix+1,iy+1)*fx)*fy;
                        maxCoverage=(std::max)(maxCoverage,alpha);
                        if(alpha<.9) continue; // 仅字体内部，不用抗锯齿边缘验收正文。
                        ++sampledCore;
                        if(corePosition && sampledCore==1) *corePosition=ImVec2(float(x),float(y));
                        const auto expected=plot::compositeOverviewColor({region.text.x,region.text.y,region.text.z,alpha},bg);
                        const auto i=((height-1-y)*width+x)*3;
                        const plot::OverviewColor pixel{pixels[i]/255.,pixels[i+1]/255.,pixels[i+2]/255.,1};
                        const double contrast=plot::overviewContrast(pixel,bg);
                        minimumContrast=(std::min)(minimumContrast,contrast);
                        readableCore+=contrast>=region.threshold;
                        if(std::abs(pixel.r-expected.r)<=2./255 && std::abs(pixel.g-expected.g)<=2./255 &&
                            std::abs(pixel.b-expected.b)<=2./255) ++renderedCore;
                    }
            }
        // 同时验证设计正文与实际可读核心；4.5/12 均不因抗锯齿而放宽。
        if (sampledCore<=3 || readableCore!=sampledCore || renderedCore!=sampledCore || minimumContrast<region.threshold ||
            plot::overviewContrast(ui::cursorOverviewColor(region.text),bg)<region.threshold) {
            std::ostringstream message;
            message<<(core<=3 ? "control text core missing" : "control actual core pixel contrast")
                   <<"; scene="<<scene<<"; control="<<region.control
                   <<"; rect=("<<r.Min.x<<','<<r.Min.y<<")-("<<r.Max.x<<','<<r.Max.y<<')'
                   <<"; expectedRGB=("<<region.text.x*255<<','<<region.text.y*255<<','<<region.text.z*255<<')'
                   <<"; matches="<<core<<"; backgroundRGB="<<background->first
                   <<"; minimumContrast="<<minimumContrast<<"; required="<<region.threshold
                   <<"; sampledCore="<<sampledCore<<"; renderedCore="<<renderedCore
                   <<"; readableCore="<<readableCore<<"; maxCoverage="<<maxCoverage;
            throw std::runtime_error(message.str());
        }
    };
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigInputTextCursorBlink = false;
    for (auto theme : {config::GuiTheme::ProfessionalDark, config::GuiTheme::ProfessionalLight,
                       config::GuiTheme::DebugHighContrast}) {
        ui::applyUiTheme(baseline ? baselineTokens(theme) : ui::uiThemeDefinition(theme));
        const auto& t = ui::activeUiStyleTokens();
        const double threshold = theme == config::GuiTheme::DebugHighContrast ? 12 : 4.5;
        ImRect button, danger, input, enabledButton, disabledButton;
        for (int scene=0; scene<8; ++scene) {
            // 截图前 CloseCurrentPopup 会恢复宿主焦点并改变同帧窗口层序，菜单实际被宿主覆盖。
            // 保持被验收弹窗展开，下一场景开始时再清理。
            if (!GImGui->OpenPopupStack.empty()) ImGui::ClosePopupToLevel(0,true);
            std::vector<Region> regions;
            bool activeInput=false, activeButton=false;
            for (int frame=0; frame<12; ++frame) {
                if (frame==0) io.AddMouseButtonEvent(0, false);
                io.AddMousePosEvent(-100,-100);
                if (scene==0 && frame>0) {
                    const auto p=disabledButton.GetCenter(); io.AddMousePosEvent(p.x,p.y);
                    io.AddMouseButtonEvent(0,true);
                }
                if (scene==1 || scene==2) {
                    const auto p=button.GetCenter(); io.AddMousePosEvent(p.x,p.y);
                    if (scene==2 && frame>0) io.AddMouseButtonEvent(0,true);
                }
                if (scene==6 || scene==7) {
                    const auto p=danger.GetCenter(); io.AddMousePosEvent(p.x,p.y);
                    if (scene==7 && frame>0) io.AddMouseButtonEvent(0,true);
                }
                ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
                regions.clear();
                ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({width,height});
                ImGui::Begin("Control state verification", nullptr, ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_MenuBar);
                const auto record = [&](const std::string& control) {
                    regions.push_back({ImRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax()),t.textStrong,threshold,control,
                        ImGui::GetColorU32(ImGuiCol_Text)});
                };
                if (ImGui::BeginMenuBar()) {
                    if (ImGui::BeginMenu("File")) { ImGui::MenuItem("Export"); ImGui::EndMenu(); }
                    ImGui::EndMenuBar();
                }
                ImGui::Button("Acquire signal", {220,36}); button=ImRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
                activeButton=ImGui::IsItemActive(); record("Acquire signal");
                ImGui::SameLine(); ui::drawDangerIconButton("Delete capture", nullptr);
                danger=ImRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax()); record("Delete capture");
                ImGui::SameLine(); ui::drawGhostIconButton("Options", nullptr); record("Options");
                ui::drawHeaderBadge("Connected",t.success,true); record("Connected");
                ImGui::SameLine(); ui::drawHeaderBadge("Warning",t.warning,false); record("Warning");
                char value[64]="Signal 123";
                if (scene==3) value[0]='\0'; // 空输入仅保留光标，避免把文字像素冒充插入光标。
                ImGui::SetNextItemWidth(300);
                if (scene==3 && frame==0) ImGui::SetKeyboardFocusHere();
                ImGui::InputText("Input",value,sizeof(value)); input=ImRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
                activeInput=ImGui::IsItemActive();
                bool enabled=true; ImGui::Checkbox("Selected channel", &enabled); record("Selected channel");
                float amount=.5F; ImGui::SliderFloat("Gain", &amount,0,1);
                if (ImGui::BeginTabBar("tabs")) {
                    if (ImGui::BeginTabItem("Waveform",nullptr,ImGuiTabItemFlags_UnsavedDocument)) {
                        record("Waveform");
                        ImGui::TextUnformatted("Waveform content"); ImGui::EndTabItem();
                    }
                    if (ImGui::BeginTabItem("Measurements")) ImGui::EndTabItem();
                    ImGui::EndTabBar();
                }
                if (ImGui::BeginTable("table",2,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)) {
                    ImGui::TableSetupColumn("Channel"); ImGui::TableSetupColumn("Value"); ImGui::TableHeadersRow();
                    for (int row=0;row<3;++row) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::Selectable("Selected row",row==1); record("Selected row "+std::to_string(row));
                        ImGui::TableNextColumn(); ImGui::TextUnformatted("1.250 V");
                    }
                    ImGui::EndTable();
                }
                ImGui::Button("Unavailable##enabled",{220,32});
                enabledButton=ImRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax()); record("Unavailable enabled");
                ImGui::SameLine();
                ui::beginDisabled(); ImGui::Button("Unavailable##disabled",{220,32});
                disabledButton=ImRect(ImGui::GetItemRectMin(),ImGui::GetItemRectMax());
                const auto alpha=ImGui::GetStyle().Alpha;
                auto fill=ui::cursorOverviewColor(ImGui::GetStyleColorVec4(ImGuiCol_Button)); fill.a*=alpha;
                const auto disabledBg=plot::compositeOverviewColor(fill,ui::cursorOverviewColor(t.appBackground));
                auto text=ui::cursorOverviewColor(ImGui::GetStyleColorVec4(ImGuiCol_Text)); text.a*=alpha;
                regions.push_back({disabledButton,ui::cursorImVec(plot::compositeOverviewColor(text,disabledBg)),threshold,
                    "Unavailable disabled",ImGui::GetColorU32(ImGuiCol_Text)});
                check(!ImGui::IsItemActive(),"disabled button became active");
                ui::endDisabled();
                if (scene==4) ImGui::OpenPopup("Actions");
                ImGui::SetNextWindowPos({500,250});
                if (ImGui::BeginPopup("Actions")) {
                    ImGui::MenuItem("Save capture",nullptr,true); record("Save capture");
                    ImGui::MenuItem("Close capture"); record("Close capture");
                    ImGui::EndPopup();
                }
                if (scene==5) ImGui::OpenPopup("Confirm");
                ImGui::SetNextWindowPos({430,390});
                if (ImGui::BeginPopupModal("Confirm",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextUnformatted("Keep this capture?");
                    ImGui::Button("Keep",{160,32});
                    // 模态遮罩改变背后对比，只有模态自身是可交互验收目标。
                    regions.clear(); record("Keep");
                    ImGui::EndPopup();
                }
                ImGui::End(); ImGui::Render();
                glViewport(0,0,width,height); glClearColor(t.appBackground.x,t.appBackground.y,t.appBackground.z,1);
                glClear(GL_COLOR_BUFFER_BIT); ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
            }
            const auto name=std::string(config::guiThemeId(theme))+"-controls-"+std::to_string(scene)+
                (baseline ? "-baseline-tokens-current-renderer" : "-current");
            const auto pixels=capture(directory,name);
            if (baseline) continue; // 参考令牌图只作比较，不伪装成通过新标准。
            for (const auto& region : regions) actualContrast(pixels,region,name);
            if (scene==0) {
                // 反向回归：抹去按钮内部文字但保留边框，采样器必须拒绝，不能靠放宽RGB或边框通过。
                auto missingText=pixels;
                const auto r=regions.front().bounds;
                const auto sample=((height-1-int(r.Min.y)-4)*width+int(r.Min.x)+12)*3;
                for(int y=int(r.Min.y)+2;y<int(r.Max.y)-2;++y) for(int x=int(r.Min.x)+2;x<int(r.Max.x)-2;++x) {
                    const auto i=((height-1-y)*width+x)*3;
                    for(int channel=0;channel<3;++channel) missingText[i+channel]=pixels[sample+channel];
                }
                bool rejected=false;
                try { actualContrast(missingText,regions.front(),name); }
                catch(const std::runtime_error& error) {
                    rejected=std::string(error.what()).find("control=Acquire signal")!=std::string::npos;
                }
                check(rejected,"control sampler accepted missing text");
                // 单一已确认核心不达标也必须失败，其他大量达标核心不能掩盖它。
                ImVec2 corePosition;
                actualContrast(pixels,regions.front(),name,&corePosition);
                auto oneBadCore=pixels;
                const auto bad=((height-1-int(corePosition.y))*width+int(corePosition.x))*3;
                for(int channel=0;channel<3;++channel) oneBadCore[bad+channel]=pixels[sample+channel];
                rejected=false;
                try { actualContrast(oneBadCore,regions.front(),name); }
                catch(const std::runtime_error&) { rejected=true; }
                check(rejected,"control sampler accepted one unreadable core among readable cores");
            }
            if (scene==2) check(activeButton,"pressed button state missing");
            if (scene==3) {
                check(activeInput,"keyboard focused input state missing");
                check(matchingPixels(pixels,ImGui::GetStyleColorVec4(ImGuiCol_InputTextCursor),input.Min,
                    ImVec2(input.Min.x+290,input.Max.y))>5,
                      "input cursor/text pixels missing");
            }
            if (scene!=5) {
                std::size_t different=0;
                for (int y=3;y<29;++y) for (int x=3;x<217;++x) {
                    const auto a=((height-1-int(enabledButton.Min.y)-y)*width+int(enabledButton.Min.x)+x)*3;
                    const auto b=((height-1-int(disabledButton.Min.y)-y)*width+int(disabledButton.Min.x)+x)*3;
                    different += std::abs(int(pixels[a])-int(pixels[b]))>=20 ||
                                 std::abs(int(pixels[a+1])-int(pixels[b+1]))>=20 ||
                                 std::abs(int(pixels[a+2])-int(pixels[b+2]))>=20;
                }
                check(different>30,"enabled/disabled buttons visually identical");
                const auto border=ImGui::GetStyleColorVec4(ImGuiCol_Border);
                check(matchingPixels(pixels,border,button.Min,button.Max)>10,"control boundary pixels missing");
                const int x=int(button.Min.x+12), y=int(button.Min.y+4);
                const auto i=((height-1-y)*width+x)*3;
                const plot::OverviewColor bg{pixels[i]/255.,pixels[i+1]/255.,pixels[i+2]/255.,1};
                check(plot::overviewContrast(ui::cursorOverviewColor(border),bg)>=3,"control border/background contrast");
            }
            std::cout<<name<<" per-control pixels verified\n";
        }
    }
    io.AddMouseButtonEvent(0,false); io.AddMousePosEvent(-100,-100);
}

void verifyReadoutLabels(const std::filesystem::path& directory)
{
    for (auto theme : {config::GuiTheme::ProfessionalDark,config::GuiTheme::ProfessionalLight,
                       config::GuiTheme::DebugHighContrast}) {
        ui::applyUiTheme(theme);
        plot::WaveDockState wave;
        wave.view.initialized=true; wave.view.defaultViewportPending=false; wave.view.autoFollowLatest=false;
        wave.buffer.append(0,{{},{{0,0},{1,1}}});
        std::vector<ImRect> labels;
        for (int frame=0;frame<3;++frame) {
            ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
            ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({width,height});
            ImGui::Begin("Readout label regression",nullptr,ImGuiWindowFlags_NoSavedSettings);
            ui::prepareWaveFrame(wave,width);
            labels.clear();
            if (ImPlot::BeginPlot("labels",{-1,-1},ImPlotFlags_NoLegend)) {
                ImPlot::SetupAxisLimits(ImAxis_X1,0,4,ImPlotCond_Always);
                ImPlot::SetupAxisLimits(ImAxis_Y1,0,4,ImPlotCond_Always);
                // 复核给出的两种高对比失败色及两种普通正文失败色。
                const std::array<unsigned,4> colors{0xCCBB44,0xBBBBBB,0x009E73,0xD55E00};
                for (std::size_t i=0;i<colors.size();++i)
                    labels.push_back(ui::drawCursorReadoutLabel(wave.view,ui::cursorImVec(plot::overviewRgb(colors[i])),
                        .5+(i%2)*1.8,.8+(i/2)*1.6,{0,0},"%c Signal 123\n1.250 V",i%2?'B':'A'));
                ImPlot::EndPlot();
            }
            ImGui::End(); ImGui::Render();
            glViewport(0,0,width,height); glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
        }
        const auto pixels=capture(directory,std::string(config::guiThemeId(theme))+"-readout-labels");
        const auto expected=wave.view.cursorColors.labelText;
        const double threshold=theme==config::GuiTheme::DebugHighContrast?12:4.5;
        check(labels.size()==4,"readout label geometry missing");
        std::size_t labelIndex=0;
        const std::array<unsigned,4> identityColors{0xCCBB44,0xBBBBBB,0x009E73,0xD55E00};
        for (auto bounds:labels) {
            // 捕获厚度/flags参数误用：非零身份边框必须真实绘制，不只验证正文。
            check(matchingPixels(pixels,ui::cursorImVec(plot::overviewRgb(identityColors[labelIndex++])),
                                 bounds.Min,bounds.Max)>20,"readout identity border pixels missing");
            const auto sample=[&](int x,int y) {
                const auto i=((height-1-y)*width+x)*3;
                return plot::OverviewColor{pixels[i]/255.,pixels[i+1]/255.,pixels[i+2]/255.,1};
            };
            const auto bg=sample(int(bounds.Min.x+4),int(bounds.Min.y+4));
            std::size_t core=0;
            for (int y=int(bounds.Min.y+4);y<int(bounds.Max.y-4);++y)
                for (int x=int(bounds.Min.x+6);x<int(bounds.Max.x-6);++x) {
                    const auto p=sample(x,y);
                    if (std::abs(p.r-expected.r)<2.1/255 && std::abs(p.g-expected.g)<2.1/255 && std::abs(p.b-expected.b)<2.1/255) {
                        check(plot::overviewContrast(p,bg)>=threshold,"readout label actual text contrast"); ++core;
                    }
                }
            check(core>10,"readout label expected text pixels missing");
        }
    }
}

void verify(const std::filesystem::path& directory)
{
    plot::WaveDockState wave;
    auto& view = wave.view;
    view.initialized = true;
    view.defaultViewportPending = false;
    view.autoFollowLatest = false;
    view.viewMinTime = .2; view.viewMaxTime = .8; view.visibleDuration = .6;
    view.viewMinValue = -1.5; view.viewMaxValue = 1.5;
    view.sampleFrequencyHz = 2048;
    view.showCursors = true;
    view.cursors[0].time = .3; view.cursors[1].time = .65;
    view.showChannelLegend = true;
    view.interactionAnimationEnabled = false;
    view.glowEnabled = false;
    for (std::size_t c = 0; c < 3; ++c) {
        wave.buffer.setChannelSpec(c, {.label = "Signal " + std::to_string(c + 1)});
        plot::WaveAppendRequest request;
        for (int i = 0; i < 4096; ++i) request.samples.push_back({i/2048., c == 2 ? double((i/100)%4) : std::sin(i*.03+c)});
        wave.buffer.append(c, std::move(request));
    }
    wave.buffer.setChannelSpec(0, {.label = "Custom translucent", .color = std::array{.15F,.7F,.45F,.3F}});
    wave.buffer.setChannelSpec(2, {.label = "Digital", .bitDisplay = {.enabled = true, .bitCount = 2}});
    // 首次帧准备会初始化历史/频率基准并清理旧 T，先完成它再创建固定身份。
    ui::prepareWaveFrame(wave,width);
    view.auxiliaryCursors.random.seed(20260922);
    view.auxiliaryCursors.add(.45,.45);
    const auto colorBefore = wave.buffer.channelSpec(0)->color;
    const auto cursorIdentity = view.auxiliaryCursors.items.front().id;
    const auto dataRevision = wave.buffer.analysisRevision();
    ImVec2 textMin, textMax, mutedMin, mutedMax;
    auto render = [&] {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0));
        ImGui::SetNextWindowSize(ImVec2(width,height));
        ImGui::Begin("Theme verification", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
        ImGui::TextUnformatted("Signal acquisition 0123456789");
        textMin = ImGui::GetItemRectMin(); textMax = ImGui::GetItemRectMax();
        ImGui::TextDisabled("Frequency / Cursor / Measurement");
        mutedMin = ImGui::GetItemRectMin(); mutedMax = ImGui::GetItemRectMax();
        auto frame = ui::prepareWaveFrame(wave, width-40);
        ImGui::BeginChild("overview", ImVec2(0,100));
        ui::drawOverviewWindow(wave, frame.fullSnapshot->config, *frame.fullSnapshot, *frame.overviewDisplayData,
            plot::computeDisplayBounds(*frame.overviewDisplayData, 1e-6), {0,1,2}, frame.renderBudget);
        ImGui::EndChild();
        if (view.fft.enabled) ui::drawWaveFftPlot(wave, frame, true, true);
        else ui::drawOscilloscopePlot(wave, frame, {.drawMeasurementOverlay = true, .drawLegendOverlay = true}, nullptr);
        ImGui::End();
        ImGui::Render();
        glViewport(0,0,width,height);
        const auto bg = ui::activeUiStyleTokens().appBackground;
        glClearColor(bg.x,bg.y,bg.z,1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glFinish();
    };
    int themePass=0;
    for (const auto theme : {config::GuiTheme::ProfessionalDark, config::GuiTheme::DebugHighContrast,
                             config::GuiTheme::ProfessionalLight, config::GuiTheme::ProfessionalDark}) {
        ui::applyUiTheme(theme);
        ++themePass;
        for (int scenario = 0; scenario < 6; ++scenario) {
            view.viewMode = scenario == 1 ? plot::WaveViewMode::Split : plot::WaveViewMode::Overlay;
            view.phosphorEnabled = scenario >= 4;
            view.phosphorBackend = scenario == 4 ? plot::WavePhosphorBackend::CpuTexture : plot::WavePhosphorBackend::GpuFbo;
            view.fft.enabled = scenario == 3;
            view.fft.displayMode = plot::WaveFftDisplayMode::FullSpectrum;
            view.overviewShowBitChannels = true;
            view.overviewSelection.automatic = scenario != 2;
            view.viewMinTime = .2;
            view.viewMaxTime = scenario == 2 ? .200001 : .8;
            view.forceNextMainPlotLimits = true;
            if (view.fft.enabled) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do {
                    ui::prepareWaveFrame(wave, width);
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                } while ((!wave.cachedFftFrame.valid || wave.fftRequestActive) && std::chrono::steady_clock::now() < deadline);
                check(wave.cachedFftFrame.valid, "FFT not ready");
            }
            ++view.phosphorResetGeneration;
            for (int i = 0; i < 8; ++i) render();
            const auto name = std::string(config::guiThemeId(theme)) + "-" + std::to_string(scenario) +
                (themePass==4 ? "-return" : "");
            const auto pixels = capture(directory, name);
            const auto& ui = ui::activeUiStyleTokens();
            check(readableTextPixels(pixels, textMin, textMax,
                theme == config::GuiTheme::DebugHighContrast ? 12 : 4.5) > 20, "primary text pixel contrast");
            check(readableTextPixels(pixels, mutedMin, mutedMax,
                theme == config::GuiTheme::DebugHighContrast ? 7 : 4.5) > 20, "secondary text pixel contrast");
            check(matchingPixels(pixels, ui.genericPlotBackground, {0,160}, {width,height}) > 1000, "plot background pixels missing");
            if (scenario >= 4) {
                check(!view.autoFollowLatest, "theme change unfroze viewport");
                check(view.lastRenderStats.phosphorBackendStatus.find(scenario == 4 ? "CPU Texture" : "GPU FBO") != std::string::npos,
                      "requested phosphor backend missing");
                const auto bg = ui::activeWaveStyleTokens().plotBackground;
                std::size_t readableCore = 0;
                for (int y = 370; y < height-65; ++y) for (int x = 40; x < width-40; ++x) {
                    const auto i = ((height-1-y)*width+x)*3;
                    const plot::OverviewColor pixel{pixels[i]/255., pixels[i+1]/255., pixels[i+2]/255., 1};
                    if (pixel.b-pixel.r > .12 &&
                        plot::overviewContrast(pixel, {bg.x,bg.y,bg.z,1}) >= 2.95) ++readableCore;
                }
                check(readableCore > 20, "phosphor core pixels below 3:1 contrast");
            }
            check(wave.buffer.analysisRevision() == dataRevision && wave.buffer.channelSpec(0)->color == colorBefore,
                  "theme changed acquisition or source color");
            check(view.auxiliaryCursors.items.front().id == cursorIdentity, "theme changed cursor identity");
            std::cout << name << " pixels verified; backend=" << view.lastRenderStats.phosphorBackendStatus << '\n';
        }
    }
}
}

int main(int argc, char** argv)
{
    if (!glfwInit()) return 2;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    auto* window = glfwCreateWindow(width,height,"Theme verification",nullptr,nullptr);
    if (!window) { glfwTerminate(); return 2; }
    glfwMakeContextCurrent(window);
    ImGui::CreateContext(); ImPlot::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().DisplaySize = ImVec2(width,height);
    ImGui::GetIO().DeltaTime = 1.F/60;
    if (std::filesystem::exists("C:/Windows/Fonts/msyh.ttc"))
        ImGui::GetIO().Fonts->AddFontFromFileTTF("C:/Windows/Fonts/msyh.ttc", 16, nullptr,
                                               ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon());
    ImGui_ImplOpenGL3_Init("#version 330");
    unsigned char* pixels; int w,h;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    int status = 0;
    try {
        const auto directory=argc>1 ? std::filesystem::path(argv[1]) : std::filesystem::path{};
        const bool controlsOnly=argc>1 && std::string(argv[1])=="--controls-only";
        if (!controlsOnly) verify(directory);
        if (!controlsOnly) verifyReadoutLabels(directory);
        verifyControls(controlsOnly ? std::filesystem::path{} : directory,false);
        if (!controlsOnly) verifyControls(directory,true);
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; status = 1; }
    ImGui_ImplOpenGL3_Shutdown();
    ImPlot::DestroyContext(); ImGui::DestroyContext();
    glfwDestroyWindow(window); glfwTerminate();
    return status;
}
