#include "protoscope/ui/ui_theme.hpp"
#include "../runtime/gui_runtime_detail.hpp"
#include "protoscope/ui/gui_runtime.hpp"

namespace protoscope::ui {
bool GuiRuntime::drawLuaTabsLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                     const std::vector<scripting::ControlSnapshot>& controls,
                                     std::string_view stableId,
                                     std::size_t& widgetIndex,
                                     bool earlyExit)
{
    if (node.controlIndex >= controls.size()) return false;
    const auto selector = controls[node.controlIndex];
    const auto& descriptor = selector.descriptor;
    if (luaTabsGeneration_ != descriptor.runtimeGeneration) {
        luaTabsUiStates_.clear();
        luaTabsGeneration_ = descriptor.runtimeGeneration;
    }
    if (!descriptor.visible) {
        luaTabsUiStates_.erase(descriptor.id);
        return false;
    }
    auto& state = luaTabsUiStates_[descriptor.id];
    const auto selected = std::get<std::string>(selector.value);
    const int frame = ImGui::GetFrameCount();
    if (state.hostValue != selected || state.lastFrame != frame - 1) {
        state.hostValue = selected;
        state.visibleValue = selected;
        state.syncing = true;
    }
    state.lastFrame = frame;
    const std::string barId = "##tabs_" + std::string(stableId) + "_" + descriptor.id +
                              "_" + std::to_string(descriptor.runtimeGeneration);
    if (!ImGui::BeginTabBar(barId.c_str(), ImGuiTabBarFlags_FittingPolicyScroll)) return false;
    bool updated = false;
    std::string shown;
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        const auto& page = node.children[i];
        const auto& id = node.tabIds[i];
        const auto label = page.title + "###page_" + id;
        const auto flags = state.syncing && id == selected ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        // 只禁止切页，不将选择器属性冒充页内控件属性；输入仍由各控件和 worker 校验。
        protoscope::ui::beginDisabled(descriptor.disabled || descriptor.readOnly);
        const bool open = ImGui::BeginTabItem(label.c_str(), nullptr, flags);
        protoscope::ui::endDisabled();
        if (!descriptor.tooltip.empty()) ImGui::SetItemTooltip("%s", descriptor.tooltip.c_str());
        if (open) {
            shown = id;
            updated = drawLuaLayoutChildren(page.children, controls, stableId, widgetIndex, earlyExit);
            ImGui::EndTabItem();
            if (updated && earlyExit) break;
        }
    }
    ImGui::EndTabBar();
    // ImGui 的 SetSelected 延后一帧生效；同步期间不把旧显示页回写给 worker。
    if (state.syncing) {
        if (shown == selected) state.syncing = false;
    } else if (!shown.empty() && shown != state.visibleValue && !descriptor.disabled && !descriptor.readOnly) {
        state.visibleValue = shown;
        updateDynamicControlValueWithFeedback(descriptor, shown);
        updated = true;
    }
    return updated;
}
} // namespace protoscope::ui
