#include "protoscope/ui/gui_runtime.hpp"
#include "../src/ui/runtime/gui_runtime_detail.hpp"

#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <fstream>
#include <iostream>

namespace protoscope::ui {
struct GuiRuntimeTestAccess {
    static void draw(GuiRuntime& runtime, const std::vector<scripting::ControlSnapshot>& controls,
                     std::map<std::string, ImRect>* rectangles = nullptr)
    {
        for (const auto& control : controls) {
            if (control.descriptor.type==scripting::ControlType::TabSelection) continue;
            const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
            runtime.drawDynamicLayoutControl(control, ImGui::GetContentRegionAvail().x);
            if (rectangles) (*rectangles)[control.descriptor.id] = ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            if (ImGui::GetItemRectMax().x > right + 1)
                throw std::runtime_error("control overflows viewport: " + control.descriptor.id);
        }
    }
    static const ControlEditState& draft(const GuiRuntime& runtime,const std::string& id)
    {
        return runtime.luaControlDrafts_.at(id);
    }
    static bool submitted(const GuiRuntime& runtime,const std::string& id)
    {
        return runtime.luaControlFeedbackStates_.contains(id);
    }
    static void clearFeedback(GuiRuntime& runtime)
    {
        runtime.luaControlFeedbackStates_.clear();
    }
    static void drawLayout(GuiRuntime& runtime, const scripting::DockSnapshot& dock)
    {
        std::size_t index=0;
        runtime.drawLuaLayoutNode(dock.descriptor.layout->root,dock.controls,"test",index,false);
    }
    static std::string selectedTab(const GuiRuntime& runtime)
    {
        return runtime.luaTabsUiStates_.at("telemetry_pages").visibleValue;
    }
    static ImVec2 firstTabCenter(std::uint64_t generation)
    {
        auto* window=ImGui::FindWindowByName("Tabs");
        const auto id=window->GetID(("##tabs_test_telemetry_pages_"+std::to_string(generation)).c_str());
        auto* bar=ImGui::GetCurrentContext()->TabBars.GetByKey(id);
        if (!bar || bar->Tabs.empty()) throw std::runtime_error("missing tab bar geometry");
        return ImVec2(bar->BarRect.Min.x+bar->Tabs[0].Offset+bar->Tabs[0].Width/2,bar->BarRect.GetCenter().y);
    }
};
}

namespace {
void capture(const std::filesystem::path& directory, int width, int height)
{
    const int stride = (width*3+3)&~3;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(stride*height));
    glPixelStorei(GL_PACK_ALIGNMENT,4);
    glReadPixels(0,0,width,height,GL_RGB,GL_UNSIGNED_BYTE,pixels.data());
    const auto range=std::minmax_element(pixels.begin(),pixels.end());
    if (*range.first==*range.second || glGetError()!=GL_NO_ERROR) throw std::runtime_error("blank framebuffer");
    for (int y=0;y<height;++y) for (int x=0;x<width;++x)
        std::swap(pixels[y*stride+x*3],pixels[y*stride+x*3+2]);
    std::array<unsigned char,54> header{};
    header[0]='B';header[1]='M';header[26]=1;header[28]=24;
    auto put=[&](int offset,std::uint32_t value) {
        for (int i=0;i<4;++i) header[offset+i]=static_cast<unsigned char>(value>>(8*i));
    };
    put(2,static_cast<std::uint32_t>(54+pixels.size()));put(10,54);put(14,40);put(18,width);put(22,height);
    std::filesystem::create_directories(directory);
    std::ofstream file(directory/("industrial-"+std::to_string(width)+".bmp"),std::ios::binary);
    file.write(reinterpret_cast<const char*>(header.data()),header.size());
    file.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()));
    if (!file) throw std::runtime_error("screenshot write failed");
}
}

int main(int argc,char** argv)
{
    using namespace protoscope;
    if (argc<2) return 2;
    const bool withGl=argc>2;
    GLFWwindow* window=nullptr;
    if (withGl) {
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_VISIBLE,GLFW_FALSE);
        window=glfwCreateWindow(1000,900,"Industrial controls",nullptr,nullptr);
        if (!window) return 2;
        glfwMakeContextCurrent(window);
    }
    ImGui::CreateContext(); ImPlot::CreateContext();
    auto& io=ImGui::GetIO();
    io.IniFilename=nullptr; io.DeltaTime=1.0F/60;
    io.BackendFlags|=ImGuiBackendFlags_RendererHasVtxOffset;
    unsigned char* pixels;int w,h;
    if (withGl) {
        if (!ImGui_ImplOpenGL3_Init("#version 130")) return 2;
    } else io.Fonts->GetTexDataAsRGBA32(&pixels,&w,&h);
    int result=0;
    try {
        scripting::ScriptHost host;
        if (!host.loadProtocolDirectory(argv[1])) throw std::runtime_error(host.lastError());
        auto controls=host.controlStatesSnapshot();
        for (auto& control:controls) {
            if (control.descriptor.type==scripting::ControlType::Readout)
                control.value=std::string("9223372036854775807.00");
            if (control.descriptor.type==scripting::ControlType::Progress) control.descriptor.indeterminate=true;
            if (scripting::isOutputControl(control.descriptor.type) && ui::isPersistedControlType(control.descriptor.type))
                throw std::runtime_error("measurement must not be persisted");
            if (ui::isPersistedControlType(control.descriptor.type)) {
                YAML::Node node;
                ui::writeControlValue(node["value"],control);
                if (!ui::readControlValue(node["value"],control.descriptor.type))
                    throw std::runtime_error("input persistence roundtrip failed: " + control.descriptor.id);
            }
        }
        app::Application application;
        config::ConfigStore configs;
        ui::GuiRuntime runtime(application,configs);
        for (const int width:{1000,360}) {
            io.DisplaySize=ImVec2(static_cast<float>(width),900);
            for (int frame=0;frame<6;++frame) {
                if (withGl) ImGui_ImplOpenGL3_NewFrame();
                ImGui::NewFrame();
                ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
                ImGui::Begin("Telemetry",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
                ui::GuiRuntimeTestAccess::draw(runtime,controls);
                ImGui::End();ImGui::Render();
                if (ImGui::GetDrawData()->TotalVtxCount==0) throw std::runtime_error("blank draw data");
                if (withGl) {
                    glViewport(0,0,width,900);glClearColor(0.05F,0.05F,0.05F,1);glClear(GL_COLOR_BUFFER_BIT);
                    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());glFinish();
                    if (frame==5) capture(argv[2],width,900);
                }
            }
        }
        // 真实 ImGui 输入帧覆盖按下、宿主更新、释放及多行普通回车，不依赖 UI 测试插件。
        std::map<std::string,ImRect> rectangles;
        auto frame = [&] {
            if (withGl) ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Telemetry",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
            ui::GuiRuntimeTestAccess::draw(runtime,controls,&rectangles);
            ImGui::End();ImGui::Render();
        };
        auto require = [](bool condition,const char* message) {
            if (!condition) throw std::runtime_error(message);
        };
        frame();
        const auto slider=rectangles.at("target");
        io.AddMousePosEvent(slider.Min.x+slider.GetWidth()*0.8F,slider.GetCenter().y);
        io.AddMouseButtonEvent(0,true);frame();
        require(ui::GuiRuntimeTestAccess::draft(runtime,"target").editing &&
                !ui::GuiRuntimeTestAccess::submitted(runtime,"target"),"slider press must not submit");
        for (auto& control:controls) if (control.descriptor.id=="target") control.value=55;
        frame();
        require(std::get<int>(ui::GuiRuntimeTestAccess::draft(runtime,"target").value)!=55,
                "active ImGui slider must preserve draft after host update");
        io.AddMouseButtonEvent(0,false);frame();
        require(ui::GuiRuntimeTestAccess::submitted(runtime,"target"),"slider release must submit");
        const auto area=rectangles.at("notes");
        io.AddMousePosEvent(area.Min.x+15,area.Min.y+10);io.AddMouseButtonEvent(0,true);frame();
        io.AddMouseButtonEvent(0,false);frame();
        io.AddInputCharactersUTF8("draft");frame();
        for (auto& control:controls) if (control.descriptor.id=="notes") control.value=std::string("program update");
        frame();
        require(std::get<std::string>(ui::GuiRuntimeTestAccess::draft(runtime,"notes").value)=="draft",
                "active textarea must preserve draft");
        io.AddKeyEvent(ImGuiKey_Enter,true);frame();
        require(!ui::GuiRuntimeTestAccess::submitted(runtime,"notes") &&
                std::get<std::string>(ui::GuiRuntimeTestAccess::draft(runtime,"notes").value).find('\n')!=std::string::npos,
                "ordinary multiline Enter must insert newline without commit");
        io.AddKeyEvent(ImGuiKey_Enter,false);frame();
        io.AddMousePosEvent(30,850);io.AddMouseButtonEvent(0,true);frame();
        io.AddMouseButtonEvent(0,false);frame();
        require(ui::GuiRuntimeTestAccess::submitted(runtime,"notes"),"textarea focus loss must commit");
        ui::GuiRuntimeTestAccess::clearFeedback(runtime);
        io.AddMousePosEvent(area.Min.x+15,area.Min.y+10);io.AddMouseButtonEvent(0,true);frame();
        io.AddMouseButtonEvent(0,false);frame();
        io.AddInputCharactersUTF8("cancel");frame();
        io.AddKeyEvent(ImGuiKey_Escape,true);frame();
        io.AddKeyEvent(ImGuiKey_Escape,false);frame();
        require(!ui::GuiRuntimeTestAccess::submitted(runtime,"notes"),"Escape must cancel without commit");
        require(std::get<std::string>(ui::GuiRuntimeTestAccess::draft(runtime,"notes").value)=="program update",
                "Escape must restore current host value");
        auto dock=host.dockSnapshots().front();
        auto tabsFrame = [&] {
            if (withGl) ImGui_ImplOpenGL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("Tabs",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoResize);
            ui::GuiRuntimeTestAccess::drawLayout(runtime,dock);
            ImGui::End();ImGui::Render();
        };
        ui::GuiRuntimeTestAccess::clearFeedback(runtime);
        for (int i=0;i<4;++i) tabsFrame();
        require(ui::GuiRuntimeTestAccess::selectedTab(runtime)=="live", "initial tab selection");
        require(!ui::GuiRuntimeTestAccess::submitted(runtime,"telemetry_pages"),"initial tab must not emit input");
        for (auto& control:dock.controls)
            if (control.descriptor.id=="telemetry_pages") control.value=std::string("settings");
        for (int i=0;i<3;++i) tabsFrame();
        require(ui::GuiRuntimeTestAccess::selectedTab(runtime)=="settings", "host must select tab");
        require(!ui::GuiRuntimeTestAccess::submitted(runtime,"telemetry_pages"),"host selection must not echo input");
        // 页签标题使用默认 ImGui 字体，在固定窗口左上按真实鼠标事件切回 Live。
        const auto tabCenter=ui::GuiRuntimeTestAccess::firstTabCenter(host.runtimeGeneration());
        io.AddMousePosEvent(tabCenter.x,tabCenter.y);tabsFrame();
        io.AddMouseButtonEvent(0,true);tabsFrame();
        io.AddMouseButtonEvent(0,false);tabsFrame();tabsFrame();
        require(ui::GuiRuntimeTestAccess::selectedTab(runtime)=="live" &&
                ui::GuiRuntimeTestAccess::submitted(runtime,"telemetry_pages"), "tab click must submit page ID");
        for (const int width:{1000,360}) {
            io.DisplaySize=ImVec2(static_cast<float>(width),900);
            for (int i=0;i<4;++i) tabsFrame();
            if (withGl) {
                glViewport(0,0,width,900);glClear(GL_COLOR_BUFFER_BIT);
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());glFinish();
                capture(std::filesystem::path(argv[2])/"tabs",width,900);
            }
        }
        std::cout<<"industrial UI: 1000/360 px, nonblank frames, bounded widgets, input persistence passed\n";
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';result=1;}
    if (withGl) ImGui_ImplOpenGL3_Shutdown();
    ImPlot::DestroyContext();ImGui::DestroyContext();
    if (window) {glfwDestroyWindow(window);glfwTerminate();}
    return result;
}
