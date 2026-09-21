#include "protoscope/app/application.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include <imgui_internal.h>
#include <implot.h>
#include <iostream>
#include <stdexcept>

namespace protoscope::ui {
struct GuiRuntimeTestAccess {
    static void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    static void verify(app::Application& application, GuiRuntime& runtime)
    {
        for (const auto mode : {config::GuiWaveFullscreenMode::Overlay, config::GuiWaveFullscreenMode::Focus}) {
            auto config = application.captureConfig();
            config.gui.wave.fullscreenMode = mode;
            application.applyConfig(config);
            runtime.showCommDock_ = true;
            runtime.showLogDock_ = false;
            runtime.luaDockVisibility_["test"] = true;
            runtime.enterWaveFullscreen();
            require(runtime.waveFullscreenActive_, "enter fullscreen");
            runtime.openUnifiedDataExport(-1, true);
            require(runtime.waveFullscreenActive_ && !runtime.pendingBuiltinFileOperation_, "rejected operation stays fullscreen");
            bool called = false;
            runtime.deferBuiltinFileOperation([&] {
                require(!runtime.waveFullscreenActive_, "operation requires normal layout");
                require(runtime.showCommDock_ && !runtime.showLogDock_ && runtime.luaDockVisibility_["test"],
                        "restore visibility");
                called = true;
            });
            runtime.dispatchBuiltinFileOperation();
            require(!called && runtime.waveFullscreenActive_, "not dispatched in request frame");
            runtime.prepareBuiltinFileOperation();
            require(!called && !runtime.waveFullscreenActive_, "restore before normal frame");
            ImGui::NewFrame();
            ImGui::Begin("normal layout");
            ImGui::TextUnformatted("normal");
            ImGui::End();
            ImGui::Render();
            runtime.dispatchBuiltinFileOperation();
            require(called && !runtime.waveFullscreenActive_, "dispatch after normal frame");
            runtime.dispatchBuiltinFileOperation();
            require(!runtime.pendingBuiltinFileOperation_, "operation consumed");

            runtime.enterWaveFullscreen();
            runtime.openUnifiedDataExport();
            require(runtime.pendingBuiltinFileOperation_ && runtime.waveFullscreenActive_, "unified entry is deferred");
            runtime.prepareBuiltinFileOperation();
            ImGui::NewFrame();
            ImGui::Render();
            runtime.dispatchBuiltinFileOperation();
            require(runtime.unifiedDataDialogOpen_ && runtime.focusUnifiedDataDialog_, "open and focus once");
            ImGui::NewFrame();
            runtime.drawUnifiedDataDialog();
            const auto* dataWindow = ImGui::FindWindowByName("数据导入导出");
            require(dataWindow && GImGui->NavWindow == dataWindow, "focus operation window");
            require(!runtime.focusUnifiedDataDialog_, "focus request consumed");
            ImGui::Render();
            runtime.unifiedDataDialogOpen_ = false;
            require(!runtime.waveFullscreenActive_, "closing stays normal");
        }
    }
};
}

int main()
{
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1000, 700);
    io.DeltaTime = 1.0F / 60.0F;
    unsigned char* pixels;
    int width, height;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    int result = 0;
    try {
        protoscope::app::Application app;
        protoscope::config::ConfigStore store;
        protoscope::ui::GuiRuntime runtime(app, store);
        protoscope::ui::GuiRuntimeTestAccess::verify(app, runtime);
        std::cout << "file operation frame boundary and focus passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return result;
}
