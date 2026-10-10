// 被现有 test_theme_render.cc 包含，复用其真实 GL 帧缓冲、截图和错误处理，不新增测试 EXE。
namespace theme_scenarios {
using Frame = std::function<void()>;

void settle(const Frame& frame,int count=4) { for(int i=0;i<count;++i) frame(); }
void move(const Frame& frame,ImVec2 p)
{
    ImGui::GetIO().AddMousePosEvent(p.x,p.y); settle(frame,2);
}
void click(const Frame& frame,ImVec2 p,int button=0)
{
    move(frame,p); ImGui::GetIO().AddMouseButtonEvent(button,true); frame();
    ImGui::GetIO().AddMouseButtonEvent(button,false); settle(frame,2);
}
void key(const Frame& frame,ImGuiKey keyCode)
{
    ImGui::GetIO().AddKeyEvent(keyCode,true); frame();
    ImGui::GetIO().AddKeyEvent(keyCode,false); settle(frame,2);
}
void focusId(const Frame& frame,ImGuiID id)
{
    // 只用真实 Tab 导航查找生产 ID；不写 NavId/ActiveId，不误触途经开关。
    for(int i=0;i<100;++i) {
        if(GImGui->NavId==id) return;
        key(frame,ImGuiKey_Tab);
    }
    throw std::runtime_error("真实键盘导航找不到控件 id="+std::to_string(id));
}
void finishFrame()
{
    ImGui::Render(); glViewport(0,0,width,height);
    const auto bg=ui::activeUiStyleTokens().appBackground;
    glClearColor(bg.x,bg.y,bg.z,1); glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData()); glFinish();
}
void coverage(const std::string& theme,const std::string& state)
{
    std::cout<<"COVERAGE theme="<<theme<<" state="<<state<<" result=passed\n";
}
ImGuiWindow* activeWindowContaining(const char* part)
{
    for(auto* window:GImGui->Windows)
        if(window->Active && !window->Hidden && std::string_view(window->Name).find(part)!=std::string_view::npos) return window;
    throw std::runtime_error(std::string("缺少真实可见窗口: ")+part);
}
ImRect bounds(const ImGuiWindow* window) { return {window->Pos,{window->Pos.x+window->Size.x,window->Pos.y+window->Size.y}}; }
void fourBorders(const std::vector<unsigned char>& pixels,ImRect r,ImVec4 color,const std::string& label)
{
    // 四边各自检测且避开圆角；左边 splitter 即使存在也不能代替其余三边。
    const ImRect sides[]{
        {{r.Min.x-2,r.Min.y+12},{r.Min.x+3,r.Max.y-12}},
        {{r.Max.x-3,r.Min.y+12},{r.Max.x+2,r.Max.y-12}},
        {{r.Min.x+12,r.Min.y-2},{r.Max.x-12,r.Min.y+3}},
        {{r.Min.x+12,r.Max.y-3},{r.Max.x-12,r.Max.y+2}}
    };
    for(int side=0;side<4;++side) {
        const auto& band=sides[side];
        const float length=side<2 ? band.GetHeight() : band.GetWidth();
        if(matchingPixels(pixels,color,band.Min,band.Max)<std::size_t((std::max)(3.F,length*.25F)))
            throw std::runtime_error(label+" 四边像素缺失 side="+std::to_string(side));
    }
}
void popup(const std::filesystem::path& directory,const std::string& name)
{
    check(!GImGui->OpenPopupStack.empty() && GImGui->OpenPopupStack.back().Window,"真实 Popup 未打开");
    const auto* p=GImGui->OpenPopupStack.back().Window;
    check(p->Active && !p->Hidden && (p->Flags&ImGuiWindowFlags_Popup) && p->WindowBorderSize==1,"Popup 未缓存原生边框尺寸");
    const auto pixels=capture(directory,name);
    const auto& t=ui::activeUiStyleTokens();
    fourBorders(pixels,bounds(p),t.controlBorder,name);
    check(matchingPixels(pixels,t.floatingBackground,p->InnerRect.Min,p->InnerRect.Max)>20,"Popup 浮层背景未应用");
}

ImVec4 pixelAt(const std::vector<unsigned char>& pixels,int x,int y)
{
    check(x>=0 && x<width && y>=0 && y<height,"像素坐标越界");
    const auto offset=((height-1-y)*width+x)*3;
    return {pixels[offset]/255.F,pixels[offset+1]/255.F,pixels[offset+2]/255.F,1};
}
void expectPixel(const std::vector<unsigned char>& pixels,ImVec2 point,ImVec4 expected,const char* label)
{
    expected.w=1;
    if(!tests::theme::same(pixelAt(pixels,int(point.x),int(point.y)),expected,2.1F/255))
        throw std::runtime_error(std::string(label)+" 实际合成 RGB 不符");
}
void transparentSurfaces(const std::filesystem::path& directory)
{
    for(int sample=0;sample<3;++sample) {
        auto d=ui::uiThemeDefinition(config::GuiTheme::DebugHighContrast);
        d.id="custom-surface-"+std::to_string(sample);
        d.ui.appBackground={.2F,.22F,.24F,1};
        d.ui.panelBackgroundAlt=sample==0 ? ImVec4(.22F,.24F,.26F,1) : ImVec4(.6F,.2F,.4F,sample==1?.4F:0.F);
        d.ui.controlBorder=sample==0 ? d.ui.panelBackgroundAlt : ImVec4(.2F,.7F,.5F,sample==1?.5F:0.F);
        d.ui.floatingBorder={.8F,.1F,.6F,.6F};
        d.ui.textStrong=d.ui.panelBackgroundAlt;
        ui::applyUiTheme(d); tests::theme::mapped(d);
        ImRect input,button;
        const Frame frame=[&] {
            ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
            ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({float(width),float(height)});
            ImGui::Begin("RGBA surfaces",nullptr,ImGuiWindowFlags_NoSavedSettings);
            char value[8]{};
            ImGui::SetNextItemWidth(250); ImGui::InputText("##faithful_input",value,sizeof(value));
            input={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
            ui::drawUiButton("##faithful_button",{250,40}); button={ImGui::GetItemRectMin(),ImGui::GetItemRectMax()};
            ImGui::End(); finishFrame();
        };
        move(frame,{-100,-100}); settle(frame,4);
        const auto pixels=capture(directory,d.id);
        // 期望采用实际8位顶点量化，再按标准 alpha 合成；不调用生产 surfaceTint。
        const auto quantized=[](ImVec4 c) { return ImGui::ColorConvertU32ToFloat4(ImGui::ColorConvertFloat4ToU32(c)); };
        const auto fill=tests::theme::over(quantized(d.ui.panelBackgroundAlt),d.ui.appBackground);
        expectPixel(pixels,input.GetCenter(),fill,"输入填充");
        expectPixel(pixels,button.GetCenter(),fill,"按钮填充");
        const auto border=tests::theme::over(quantized(d.ui.controlBorder),fill);
        expectPixel(pixels,{input.GetCenter().x,input.Min.y},border,"输入上边框");
        auto missingBorder=pixels;
        const ImVec2 borderPoint(input.GetCenter().x,input.Min.y);
        if(sample==1) {
            const auto offset=((height-1-int(borderPoint.y))*width+int(borderPoint.x))*3;
            missingBorder[offset]=static_cast<unsigned char>(std::lround(fill.x*255));
            missingBorder[offset+1]=static_cast<unsigned char>(std::lround(fill.y*255));
            missingBorder[offset+2]=static_cast<unsigned char>(std::lround(fill.z*255));
            bool rejected=false;
            try { expectPixel(missingBorder,borderPoint,border,"反向边框夹具"); }
            catch(const std::runtime_error&) { rejected=true; }
            check(rejected,"实际 alpha 边框采样接受了擦除边框");
        }
        const auto before=ImGui::GetStyle(); d.id="renamed"; d.wave.correctContrast=false;
        ui::applyUiTheme(d); tests::theme::sameStyles(before,ImGui::GetStyle()); settle(frame,4);
        const auto unchanged=capture(directory,"custom-surface-"+std::to_string(sample)+"-renamed-wave-off");
        check(pixels==unchanged,"仅改 ID/波形开关改变了实际 UI 像素");
        coverage("custom","rgba-surface-"+std::to_string(sample));
    }
}

void docking(const std::filesystem::path& directory)
{
    auto& io=ImGui::GetIO(); const auto oldFlags=io.ConfigFlags;
    io.ConfigFlags|=ImGuiConfigFlags_DockingEnable|ImGuiConfigFlags_NavEnableKeyboard;
    const ImGuiID dockId=ImHashStr("theme-contract-native-dock");
    bool build=true;
    float value=1;
    // 上一套控件用例可能留下合成鼠标状态；显式释放后再进入真实 Dock 场景。
    io.AddMouseButtonEvent(0,false); io.AddMouseButtonEvent(1,false); io.AddMousePosEvent(-100,-100);
    const Frame frame=[&] {
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({740,700});
        ImGui::Begin("Theme Dock Host",nullptr,ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoDocking);
        if(build) {
            ImGui::DockBuilderRemoveNode(dockId);
            ImGui::DockBuilderAddNode(dockId,ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockId,{710,660});
            ImGui::DockBuilderDockWindow("Native page A",dockId);
            ImGui::DockBuilderDockWindow("Native page B",dockId);
            ImGui::DockBuilderFinish(dockId); build=false;
        }
        ImGui::DockSpace(dockId,{0,0}); ImGui::End();
        for(auto name:{"Native page A","Native page B"}) {
            // IniFilename 已为 nullptr；仍允许内存 settings 接收 DockBuilder 的首次布局。
            ImGui::Begin(name);
            ImGui::TextUnformatted(name); ImGui::InputFloat("Value",&value);
            ImGui::End();
        }
        ImGui::SetNextWindowPos({760,20}); ImGui::SetNextWindowSize({220,220});
        ImGui::Begin("Independent focus",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoDocking);
        ImGui::Button("Focus here",{170,60}); ImGui::End();
        finishFrame();
    };
    const auto tabRect=[&](int index) {
        auto* n=ImGui::DockBuilderGetNode(dockId);
        check(n && n->TabBar && n->TabBar->Tabs.Size==2 && n->Windows.Size==2,"必须是同一真实 Dock 节点的两页");
        const auto& bar=*n->TabBar; const auto& tab=bar.Tabs[index];
        const float x=bar.BarRect.Min.x+tab.Offset-bar.ScrollingAnim;
        return ImRect({x,bar.BarRect.Min.y},{x+tab.Width,bar.BarRect.Max.y});
    };
    for(auto theme:{config::GuiTheme::ProfessionalDark,config::GuiTheme::DebugHighContrast,config::GuiTheme::ProfessionalLight}) {
        ui::applyUiTheme(theme); settle(frame,8);
        const std::string id(config::guiThemeId(theme)); const auto& t=ui::activeUiStyleTokens();
        check(!tests::theme::same(t.tabSelected,t.tabBackground) && !tests::theme::same(t.tabDimmedSelected,t.tabDimmed),
              "内置已选/未选状态应可辨；不将此规则用于自定义主题");
        for(int selected=0;selected<2;++selected) {
            click(frame,tabRect(selected).GetCenter());
            for(bool focused:{true,false}) {
                click(frame,focused ? ImVec2(300,350) : ImVec2(850,110));
                for(int hover:{-1,0,1}) {
                    move(frame,hover<0 ? ImVec2(990,710) : tabRect(hover).GetCenter());
                    auto* n=ImGui::DockBuilderGetNode(dockId);
                    check(n->IsFocused==focused,"真实 Dock 焦点与场景不符");
                    check(n->TabBar->SelectedTabId==n->TabBar->Tabs[selected].ID,"真实点击未切换选中 Dock 页");
                    const std::string state="dock-selected-"+std::to_string(selected)+(focused?"-focused":"-dimmed")+"-hover-"+std::to_string(hover);
                    const auto pixels=capture(directory,id+"-"+state);
                    for(int index=0;index<2;++index) {
                        const auto r=tabRect(index);
                        const auto fill=hover==index ? t.tabHovered : focused ?
                            (selected==index?t.tabSelected:t.tabBackground) : (selected==index?t.tabDimmedSelected:t.tabDimmed);
                        check(matchingPixels(pixels,fill,{r.Min.x+6,r.Min.y+4},{r.Max.x-6,r.Max.y-2})>15,"原生 Dock 填充不忠实");
                    }
                    const auto selectedRect=tabRect(selected);
                    const auto overline=focused?t.tabSelectedOverline:t.tabDimmedSelectedOverline;
                    const float overlineSize=ImGui::GetStyle().TabBarOverlineSize;
                    const ImVec2 overlineMin(selectedRect.Min.x+8,selectedRect.Min.y+1);
                    const ImVec2 overlineMax(selectedRect.Max.x-8,selectedRect.Min.y+1+overlineSize);
                    const auto underOverline=hover==selected?t.tabHovered:
                        (focused?t.tabSelected:t.tabDimmedSelected);
                    const ImVec4 antialiasedOverline((overline.x+underOverline.x)*.5F,
                                                      (overline.y+underOverline.y)*.5F,
                                                      (overline.z+underOverline.z)*.5F,1);
                    const auto overlinePixels=(std::max)(matchingPixels(pixels,overline,overlineMin,overlineMax,3),
                        matchingPixels(pixels,antialiasedOverline,overlineMin,overlineMax,3));
                    if(overlinePixels<8) {
                        std::map<unsigned,std::size_t> observed;
                        for(int y=int(overlineMin.y);y<(std::min)(height,int(overlineMax.y));++y)
                            for(int x=int(overlineMin.x);x<(std::min)(width,int(overlineMax.x));++x) {
                                const auto pixel=pixelAt(pixels,x,y);
                                const auto rgb=(unsigned(std::lround(pixel.x*255))<<16)|
                                               (unsigned(std::lround(pixel.y*255))<<8)|unsigned(std::lround(pixel.z*255));
                                ++observed[rgb];
                            }
                        std::ostringstream diagnostic;
                        diagnostic<<"原生 Dock 选中上沿像素缺失; theme="<<id<<"; selected="<<selected
                                  <<"; focused="<<focused<<"; hover="<<hover<<"; rect=("
                                  <<selectedRect.Min.x<<','<<selectedRect.Min.y<<")-("
                                  <<selectedRect.Max.x<<','<<selectedRect.Max.y<<"); expectedRGBA=("
                                  <<overline.x<<','<<overline.y<<','<<overline.z<<','<<overline.w
                                  <<"); matchingPixels="<<overlinePixels<<"; actualRGB=";
                        for(auto it=observed.rbegin();it!=observed.rend();++it)
                            diagnostic<<std::hex<<it->first<<std::dec<<':'<<it->second<<',';
                        throw std::runtime_error(diagnostic.str());
                    }
                    coverage(id,state);
                }
            }
        }
    }
    ImGui::DockBuilderRemoveNode(dockId);
    io.ConfigFlags=oldFlags; io.AddMousePosEvent(-100,-100);
}

void drawers(const std::filesystem::path& directory)
{
    app::Application application; ui::WaveDockRenderer renderer(application);
    auto& wave=application.docks().waveState();
    ImGui::GetIO().ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;
    wave.view.interactionAnimationEnabled=true; wave.view.showChannelLegend=false;
    wave.buffer.append(0,{{},{{0,0},{1,1}}});
    bool visible=true; float hostWidth=float(width);
    const Frame frame=[&] {
        const auto before=ImGui::GetStyle();
        const int colors=GImGui->ColorStack.Size,vars=GImGui->StyleVarStack.Size;
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({hostWidth,float(height)});
        renderer.draw(visible);
        tests::theme::sameStyles(before,ImGui::GetStyle());
        check(colors==GImGui->ColorStack.Size && vars==GImGui->StyleVarStack.Size,"抽屉样式栈泄漏");
        finishFrame();
    };
    const auto railPoint=[&](int index) {
        const auto* rail=activeWindowContaining("##wave_tools_rail");
        // 从实际 rail 内容起点取固定原生按钮中心，按生产 28x30、8px gap 定位。
        return ImVec2(rail->Pos.x+rail->Size.x*.5F,rail->DC.CursorStartPos.y+8+8+15+index*38);
    };
    const std::array drawers{plot::WaveToolsDrawer::Main,plot::WaveToolsDrawer::FFT,plot::WaveToolsDrawer::Renderer,
                             plot::WaveToolsDrawer::Cursor,plot::WaveToolsDrawer::Measure,plot::WaveToolsDrawer::View};
    const std::array names{"main","fft","renderer","cursor","measure","view"};
    for(auto theme:{config::GuiTheme::ProfessionalDark,config::GuiTheme::DebugHighContrast,config::GuiTheme::ProfessionalLight}) {
        ui::applyUiTheme(theme); const auto& t=ui::activeUiStyleTokens();
        const std::string id(config::guiThemeId(theme));
        for(float size:{1000.F,560.F}) {
            hostWidth=size; settle(frame,6);
            for(int index=0;index<6;++index) {
                const auto* rail=activeWindowContaining("##wave_tools_rail");
                const int enumIndex=static_cast<int>(drawers[index]);
                const ImGuiID seed=ImHashData(&enumIndex,sizeof(enumIndex),rail->ID);
                move(frame,railPoint(index));
                check(GImGui->HoveredId==ImHashStr("##rail_toggle_button",0,seed),"rail 坐标未命中目标生产按钮 ID");
                click(frame,railPoint(index));
                check(!wave.toolsCollapsed && wave.activeToolsDrawer==drawers[index],"rail 真实点击未打开目标抽屉");
                check(wave.toolsDrawerProgress>0 && wave.toolsDrawerProgress<1,"抽屉缺少动画中间帧");
                capture(directory,id+"-drawer-"+names[index]+"-opening-"+std::to_string(int(size)));
                settle(frame,24); move(frame,{-100,-100});
                auto* drawer=activeWindowContaining("##wave_tools_drawer");
                check(!(drawer->Flags&(ImGuiWindowFlags_Popup|ImGuiWindowFlags_ChildWindow)) &&
                      (drawer->Flags&ImGuiWindowFlags_NoDocking) && drawer->WindowBorderSize==1,
                      "抽屉必须是有边框的普通 NoDocking 窗口");
                check(drawer->Pos.x>=0 && drawer->Pos.x+drawer->Size.x<=hostWidth && drawer->Size.y>100,"窄窗口抽屉越界");
                const auto state="drawer-"+std::string(names[index])+"-open-"+std::to_string(int(size));
                const auto pixels=capture(directory,id+"-"+state);
                fourBorders(pixels,bounds(drawer),t.floatingBorder,state);
                check(matchingPixels(pixels,t.floatingBackground,drawer->InnerRect.Min,drawer->InnerRect.Max)>50,"抽屉未用浮层背景");
                coverage(id,state);
                // 拖动左侧命中区，既验证几何也验证释放后关闭行为。
                if(index==0 && size==1000) {
                    const float old=wave.toolsExpandedWidth;
                    const ImVec2 handle(drawer->Pos.x-3,drawer->Pos.y+8);
                    move(frame,handle);
                    auto* hoveredWindowBeforePtr=GImGui->HoveredWindow;
                    check(hoveredWindowBeforePtr!=nullptr,"splitter 位置没有真实 HoveredWindow");
                    auto* splitterWindow=hoveredWindowBeforePtr->RootWindow;
                    const ImGuiID splitterId=splitterWindow->GetID("##wave_tools_drawer_splitter");
                    const ImGuiID hoveredBefore=GImGui->HoveredId;
                    const char* hoveredWindowBefore=hoveredWindowBeforePtr->Name;
                    ImGui::GetIO().AddMouseButtonEvent(0,true); frame();
                    const ImGuiID activeAfterPress=GImGui->ActiveId;
                    auto* activeWindow=GImGui->ActiveIdWindow;
                    const char* activeWindowAfterPress=activeWindow?activeWindow->Name:"<none>";
                    auto* hoveredWindow=GImGui->HoveredWindow;
                    const bool splitterActivated=activeAfterPress==splitterId && activeWindow==splitterWindow;
                    if(!splitterActivated || wave.toolsCollapsed) {
                        std::ostringstream diagnostic;
                        diagnostic<<"拖宽按下未激活真实 splitter; theme="<<id<<"; splitter="<<splitterId
                                  <<"; hoveredBefore="<<hoveredBefore<<"; hoveredWindow="<<hoveredWindowBefore
                                  <<"; activeAfterPress="<<activeAfterPress<<"; activeWindow="<<activeWindowAfterPress
                                  <<"; drawerWindow="<<drawer->Name<<"; handle=("<<handle.x<<','<<handle.y
                                  <<"); splitterWindowRect=("<<(hoveredWindow?hoveredWindow->Pos.x:0)<<','
                                  <<(hoveredWindow?hoveredWindow->Pos.y:0)<<','<<(hoveredWindow?hoveredWindow->Size.x:0)<<','
                                  <<(hoveredWindow?hoveredWindow->Size.y:0)<<"); itemRect=("
                                  <<GImGui->LastItemData.Rect.Min.x<<','<<GImGui->LastItemData.Rect.Min.y<<','
                                  <<GImGui->LastItemData.Rect.Max.x<<','<<GImGui->LastItemData.Rect.Max.y
                                  <<"); clipRect=("<<(activeWindow?activeWindow->ClipRect.Min.x:0)<<','
                                  <<(activeWindow?activeWindow->ClipRect.Min.y:0)<<','
                                  <<(activeWindow?activeWindow->ClipRect.Max.x:0)<<','
                                  <<(activeWindow?activeWindow->ClipRect.Max.y:0)
                                  <<"); cursorPos=("<<(activeWindow?activeWindow->DC.CursorPos.x:0)<<','
                                  <<(activeWindow?activeWindow->DC.CursorPos.y:0)
                                  <<"); cursorStart=("<<(activeWindow?activeWindow->DC.CursorStartPos.x:0)<<','
                                  <<(activeWindow?activeWindow->DC.CursorStartPos.y:0)
                                  <<"); skipItems="<<(activeWindow?activeWindow->SkipItems:0)<<"; moveId="
                                  <<(activeWindow?activeWindow->MoveId:0)
                                  <<"; collapsed="<<wave.toolsCollapsed
                                  <<"; width="<<wave.toolsExpandedWidth;
                        throw std::runtime_error(diagnostic.str());
                    }
                    ImGui::GetIO().AddMousePosEvent(handle.x-30,handle.y); frame();
                    const float dragDelta=ImGui::GetIO().MouseDelta.x;
                    const float widthDuringDrag=wave.toolsExpandedWidth;
                    settle(frame,1);
                    ImGui::GetIO().AddMouseButtonEvent(0,false); settle(frame,3);
                    if(!(widthDuringDrag>old && wave.toolsExpandedWidth>old && !wave.toolsCollapsed)) {
                        std::ostringstream diagnostic;
                        diagnostic<<"真实 splitter 拖动未扩宽或错误关闭抽屉; theme="<<id<<"; splitter="<<splitterId
                                  <<"; activeAfterPress="<<activeAfterPress<<"; dragMouseDeltaX="<<dragDelta
                                  <<"; oldWidth="<<old<<"; widthDuringDrag="<<widthDuringDrag
                                  <<"; widthAfterRelease="<<wave.toolsExpandedWidth<<"; clamp=["
                                  <<wave.minToolsExpandedWidth<<','<<wave.maxToolsExpandedWidth<<"]"
                                  <<"; collapsed="<<wave.toolsCollapsed;
                        throw std::runtime_error(diagnostic.str());
                    }
                    coverage(id,"drawer-drag-resize");
                }
                if(index%3==0) key(frame,ImGuiKey_Escape);
                else if(index%3==1) {
                    drawer=activeWindowContaining("##wave_tools_drawer");
                    const auto& style=ImGui::GetStyle();
                    click(frame,{drawer->Pos.x+drawer->Size.x-style.FramePadding.x-ImGui::GetFontSize()*.5F,
                                 drawer->Pos.y+style.FramePadding.y+ImGui::GetFontSize()*.5F});
                } else click(frame,{35,350});
                check(wave.toolsCollapsed,"ESC/关闭按钮/点外部未关闭抽屉");
                settle(frame,24);
                check(wave.toolsDrawerProgress==0,"关闭动画未完成");
                const auto closed="drawer-"+std::string(names[index])+"-closed-"+std::to_string(int(size));
                capture(directory,id+"-"+closed); coverage(id,closed);
            }
        }
    }
    // 使用真实自定义主题的不同边框哨兵色，而非测试 PushStyleColor 替生产补画。
    auto custom=ui::uiThemeDefinition(config::GuiTheme::ProfessionalDark);
    custom.id="drawer_sentinel"; custom.ui.controlBorder={.2F,.8F,.3F,1}; custom.ui.floatingBorder={.8F,.2F,.7F,1};
    ui::applyUiTheme(custom); hostWidth=1000; settle(frame,6); click(frame,railPoint(5)); settle(frame,24);
    auto* drawer=activeWindowContaining("##wave_tools_drawer");
    const auto pixels=capture(directory,"custom-drawer-input-border");
    fourBorders(pixels,bounds(drawer),custom.ui.floatingBorder,"自定义抽屉");
    const auto inner=drawer->InnerRect;
    check(matchingPixels(pixels,custom.ui.controlBorder,inner.Min,inner.Max)>20,"窗口装饰污染抽屉内部控件边框");
    click(frame,{drawer->Pos.x+40,drawer->Pos.y+12});
    const auto durationId=drawer->GetID("##visible_duration");
    focusId(frame,durationId);
    if(GImGui->ActiveId!=durationId) key(frame,ImGuiKey_Enter);
    settle(frame,2);
    check(ImGui::GetIO().WantTextInput && GImGui->ActiveIdWindow &&
          GImGui->ActiveIdWindow->RootWindow==drawer,"抽屉内部键盘焦点未到达输入");
    check(!wave.toolsCollapsed,"输入焦点导致抽屉关闭");
    key(frame,ImGuiKey_Escape); // 首次 Escape 只取消输入。
    check(!wave.toolsCollapsed,"输入编辑 Escape 错误关闭抽屉");
    focusId(frame,drawer->GetID("固定颜色")); key(frame,ImGuiKey_Space);
    check(!wave.view.overviewSelection.automatic,"真实固定色选项未激活");
    focusId(frame,ImHashStr("##ColorButton",0,drawer->GetID("固定色")));
    key(frame,ImGuiKey_Enter); settle(frame,5);
    popup(directory,"custom-drawer-color-picker");
    const auto* picker=GImGui->OpenPopupStack.back().Window;
    const auto depth=GImGui->OpenPopupStack.Size;
    const ImVec2 nestedClick(picker->DC.CursorStartPos.x+30,picker->DC.CursorStartPos.y+30);
    click(frame,nestedClick,1); settle(frame,5);
    check(GImGui->OpenPopupStack.Size>depth,"抽屉嵌套 Popup 未打开");
    popup(directory,"custom-drawer-picker-options");
    // 首次点外部关闭 Popup 时，抽屉仍受任意层 Popup 保护。
    click(frame,{35,350});
    check(!wave.toolsCollapsed,"嵌套 Popup 的点外操作误关抽屉");
    // 不用 Escape 清 Popup，以免把抽屉的正常 Escape 关闭误当嵌套保护失败。
    for(int i=0;i<4 && !GImGui->OpenPopupStack.empty();++i) click(frame,{35,350});
    check(GImGui->OpenPopupStack.empty(),"抽屉嵌套 Popup 点外未关闭");
    key(frame,ImGuiKey_Escape); settle(frame,24);
    check(wave.toolsCollapsed,"退出编辑后 Escape 未关闭抽屉");
    coverage("custom","drawer-input-border-and-keyboard");
}

void legend(const std::filesystem::path& directory)
{
    plot::WaveDockState wave;
    wave.view.initialized=true; wave.view.defaultViewportPending=false; wave.view.autoFollowLatest=false;
    wave.view.showChannelLegend=true; wave.view.interactionAnimationEnabled=true;
    wave.legendOverlay.openMode=plot::WaveLegendOverlayOpenMode::DoubleClick;
    wave.legendOverlay.doubleClickAutoCollapse=true;
    wave.legendOverlay.hoverCloseDelaySec=.5F;
    wave.view.effectiveInteractionAnimationEnabled=true;
    wave.buffer.setChannelSpec(0,{.label="Popup signal"}); wave.buffer.append(0,{{},{{0,0},{1,1}}});
    const Frame frame=[&] {
        const auto before=ImGui::GetStyle(); const int colors=GImGui->ColorStack.Size,vars=GImGui->StyleVarStack.Size;
        ImGui_ImplOpenGL3_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos({0,0}); ImGui::SetNextWindowSize({float(width),float(height)});
        ImGui::Begin("Legend real host",nullptr,ImGuiWindowFlags_NoSavedSettings);
        const auto snapshot=wave.buffer.snapshot(0,1);
        ui::drawChannelLegendOverlay(wave,snapshot,{30,80},{930,600},ImGui::GetWindowViewport(),ui::WaveLegendOverlayLayerPolicy::ForceDisplayFront);
        ImGui::End();
        tests::theme::sameStyles(before,ImGui::GetStyle());
        check(colors==GImGui->ColorStack.Size && vars==GImGui->StyleVarStack.Size,"图例样式栈泄漏");
        finishFrame();
    };
    for(auto theme:{config::GuiTheme::ProfessionalDark,config::GuiTheme::DebugHighContrast,config::GuiTheme::ProfessionalLight}) {
        ui::applyUiTheme(theme); settle(frame,8);
        const std::string id(config::guiThemeId(theme));
        auto* host=activeWindowContaining("##wave_channel_overlay_legend");
        check(host->WindowBorderSize==0,"图例不得恢复原生第二圈边框");
        const auto row=ImVec2(host->DC.CursorStartPos.x+45,host->DC.CursorStartPos.y+12);
        click(frame,row,1); settle(frame,5);
        popup(directory,id+"-legend-context"); coverage(id,"legend-context");
        key(frame,ImGuiKey_Escape);
        // 两次真实按下/释放走 DoubleClick 展开路径。
        click(frame,row); click(frame,row); settle(frame,24);
        check(wave.legendOverlay.expanded,"图例双击未展开");
        auto* child=activeWindowContaining("##wave_legend_overlay_rows");
        auto* table=GImGui->Tables.GetByKey(child->GetID("##wave_legend_overlay_table"));
        check(table && table->ColumnsCount==8,"真实图例表格不存在");
        // 使用实际表格行几何，并验证鼠标命中了真实 ColorEdit 子控件 ID。
        const ImVec2 swatch(table->Columns[1].WorkMinX+8,table->RowPosY1+ImGui::GetStyle().CellPadding.y+10);
        move(frame,swatch);
        const int channel=0;
        const ImGuiID rowId=ImHashData(&channel,sizeof(channel),table->ID);
        const ImGuiID colorId=ImHashStr("##color",0,rowId);
        check(GImGui->HoveredId==ImHashStr("##ColorButton",0,colorId),"几何未命中真实图例 ColorEdit 色块");
        click(frame,swatch); settle(frame,6);
        popup(directory,id+"-legend-color-picker");
        const auto* picker=GImGui->OpenPopupStack.back().Window;
        const auto pickerPixels=capture({},"");
        check(matchingPixels(pickerPixels,ui::activeUiStyleTokens().controlBorder,
                             {picker->InnerRect.Min.x+4,picker->InnerRect.Min.y+4},
                             {picker->InnerRect.Max.x-4,picker->InnerRect.Max.y-4})>20,
              "拾色器内部 RGB/HSV/HEX 控件边框被宿主透明 Border 污染");
        const auto countBefore=GImGui->OpenPopupStack.Size;
        click(frame,{picker->DC.CursorStartPos.x+30,picker->DC.CursorStartPos.y+30},1); settle(frame,6);
        check(GImGui->OpenPopupStack.Size>countBefore,"拾色器嵌套选项菜单未打开");
        popup(directory,id+"-legend-picker-options");
        move(frame,{980,700}); settle(frame,60);
        check(wave.legendOverlay.expanded && !GImGui->OpenPopupStack.empty(),"嵌套 Popup 丢失图例交互保护");
        coverage(id,"legend-picker-and-nested-options");
        key(frame,ImGuiKey_Escape); key(frame,ImGuiKey_Escape);
        host=activeWindowContaining("##wave_channel_overlay_legend");
        click(frame,{host->Pos.x+20,host->Pos.y+12});
        focusId(frame,host->GetID("收起")); key(frame,ImGuiKey_Enter); settle(frame,24);
        check(!wave.legendOverlay.expanded,"图例收起按钮未生效");
    }
    auto translucent=ui::uiThemeDefinition(config::GuiTheme::ProfessionalDark);
    translucent.id="legend-alpha";
    translucent.wave.legendOverlayBackground={.2F,.3F,.4F,.4F};
    translucent.wave.legendOverlayBorder={.8F,.6F,.3F,.5F};
    ui::applyUiTheme(translucent); move(frame,{-100,-100}); settle(frame,8);
    auto* host=activeWindowContaining("##wave_channel_overlay_legend");
    check(host->WindowBorderSize==0,"半透明图例不能额外绘制原生边框");
    const auto pixels=capture(directory,"custom-legend-single-alpha-border");
    const auto quantized=[](ImVec4 c) { return ImGui::ColorConvertU32ToFloat4(ImGui::ColorConvertFloat4ToU32(c)); };
    const auto fill=tests::theme::over(quantized(translucent.wave.legendOverlayBackground),translucent.ui.appBackground);
    const auto line=tests::theme::over(quantized(translucent.wave.legendOverlayBorder),fill);
    // 顶边中央远离圆角及行内容，第二次描边会再次合成而不能通过。
    expectPixel(pixels,{host->Pos.x+host->Size.x*.5F,host->Pos.y},line,"图例唯一半透明自绘边");
    coverage("custom","legend-single-alpha-border");
}
} // namespace theme_scenarios
