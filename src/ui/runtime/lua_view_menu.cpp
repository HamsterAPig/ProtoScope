#include "protoscope/ui/gui_runtime.hpp"

#include "gui_runtime_detail.hpp"

#include <imgui.h>

namespace protoscope::ui {

void GuiRuntime::drawLuaViewMenu() {
    if (!ImGui::BeginMenu("Lua视图")) {
        return;
    }

    const auto& lua = application_.docks().luaState();
    if (lua.docks.empty()) {
        ImGui::TextDisabled("当前协议没有 Lua Dock");
        ImGui::EndMenu();
        return;
    }

    const auto layoutKey = luaDockLayoutKey(lua.protocolDir, lua.scriptPath);
    for (const auto& dockSnapshot : lua.docks) {
        const auto stableId = luaDockStableId(dockSnapshot.descriptor, layoutKey);
        bool visible = isLuaDockVisible(stableId);
        if (ImGui::MenuItem(dockSnapshot.descriptor.title.c_str(), nullptr, &visible)) {
            if (setLuaDockVisible(stableId, visible)) {
                pendingProtocolWorkspaceSave_ = true;
            }
        }
    }
    ImGui::EndMenu();
}

} // namespace protoscope::ui
