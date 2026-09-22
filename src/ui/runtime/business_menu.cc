#include "../runtime/gui_runtime_detail.hpp"
#include "protoscope/ui/gui_runtime.hpp"

namespace protoscope::ui {
void GuiRuntime::drawBusinessMenuItems(const std::vector<scripting::BusinessMenuItem>& items,
                                      std::uint64_t generation, std::uint64_t revision)
{
    for (const auto& item : items) {
        if (!item.visible) continue;
        if (item.separator) { ImGui::Separator(); continue; }
        const auto label = item.label + "###business_" + item.id;
        if (!item.children.empty()) {
            const bool open = ImGui::BeginMenu(label.c_str(),!item.disabled);
            if (!item.tooltip.empty()) ImGui::SetItemTooltip("%s",item.tooltip.c_str());
            if (open) {
                drawBusinessMenuItems(item.children,generation,revision);
                ImGui::EndMenu();
            }
        } else {
            if (ImGui::MenuItem(label.c_str(),nullptr,item.checkable && item.checked,!item.disabled))
                application_.activateBusinessMenu(item.id,item.checkable ? !item.checked : false,generation,revision);
            if (!item.tooltip.empty()) ImGui::SetItemTooltip("%s",item.tooltip.c_str());
        }
    }
}

void GuiRuntime::drawBusinessMenu()
{
    const auto& state = application_.docks().luaState().businessUi;
    if (state.menu.empty()) return;
    // 与内置菜单隔离；运行时代次和菜单树版本也进入 ImGui ID，重载关闭旧弹出菜单。
    const auto label = "业务###business_" + std::to_string(state.runtimeGeneration) + "_" +
                       std::to_string(state.menuRevision);
    if (ImGui::BeginMenu(label.c_str())) {
        drawBusinessMenuItems(state.menu,state.runtimeGeneration,state.menuRevision);
        ImGui::EndMenu();
    }
}

void GuiRuntime::applyBusinessDockRequests()
{
    const auto& lua = application_.docks().luaState();
    const auto& state = lua.businessUi;
    if (businessDockGeneration_ != state.runtimeGeneration) {
        businessDockGeneration_ = state.runtimeGeneration;
        businessDockRevision_ = 0;
    }
    const auto layoutKey = luaDockLayoutKey(lua.protocolDir,lua.scriptPath);
    for (const auto& request : state.dockRequests) {
        if (request.revision <= businessDockRevision_) continue;
        const auto found = std::find_if(lua.docks.begin(),lua.docks.end(),[&](const auto& dock) {
            return dock.descriptor.id == request.id;
        });
        if (found != lua.docks.end() &&
            setLuaDockVisible(luaDockStableId(found->descriptor,layoutKey),request.visible))
            pendingProtocolWorkspaceSave_ = true;
    }
    // 快照中的请求只应用一次，后续刷新不能覆盖用户通过视图菜单手动切换的状态。
    businessDockRevision_ = state.dockRevision;
}
} // namespace protoscope::ui
