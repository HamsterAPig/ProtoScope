#include "protoscope/ui/gui_runtime.hpp"

#include <imgui.h>

namespace protoscope::ui {

void GuiRuntime::drawHelpMenu() {
    if (!ImGui::BeginMenu("帮助")) {
        return;
    }

    if (ImGui::MenuItem("检查更新")) {
        startUpdateCheck();
    }
    if (ImGui::MenuItem("关于 ProtoScope")) {
        requestAboutDialog();
    }
    ImGui::EndMenu();
}

} // namespace protoscope::ui
