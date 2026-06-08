#include "../runtime/gui_runtime_detail.hpp"

#include "protoscope/ui/gui_runtime.hpp"

#include <algorithm>

namespace protoscope::ui {

namespace {

    class ScopedImGuiTable final {
    public:
        ScopedImGuiTable(const char* tableId, int columnCount, ImGuiTableFlags flags)
            : opened_(ImGui::BeginTable(tableId, columnCount, flags))
        {
        }

        ~ScopedImGuiTable()
        {
            if (opened_) {
                ImGui::EndTable();
            }
        }

        [[nodiscard]] bool opened() const { return opened_; }

    private:
        bool opened_{false};
    };

} // namespace

namespace {

    bool isLuaFlowInlineNode(const scripting::LayoutNodeDescriptor& node)
    {
        return node.kind == scripting::LayoutNodeKind::Control || node.kind == scripting::LayoutNodeKind::Text;
    }

    float applyLuaLayoutWidthConstraints(const scripting::LayoutNodeDescriptor& node, float naturalWidth)
    {
        float width = naturalWidth;
        if (node.minWidth.has_value()) {
            width = std::max(width, *node.minWidth);
        }
        if (node.maxWidth.has_value()) {
            width = std::min(width, *node.maxWidth);
        }
        return std::max(1.0F, width);
    }

    float luaLayoutControlLabelPartWidth(const scripting::ControlDescriptor& descriptor)
    {
        if (descriptor.label.empty()) {
            return 0.0F;
        }
        return ImGui::CalcTextSize(descriptor.label.c_str()).x + ImGui::GetStyle().ItemInnerSpacing.x;
    }

    float luaLayoutControlNaturalWidth(const scripting::ControlDescriptor& descriptor)
    {
        if (descriptor.type == scripting::ControlType::Button) {
            return ImGui::CalcTextSize(descriptor.label.c_str()).x + 24.0F;
        }
        if (descriptor.type == scripting::ControlType::Checkbox) {
            return ImGui::GetFrameHeight() + luaLayoutControlLabelPartWidth(descriptor);
        }
        return 140.0F + luaLayoutControlLabelPartWidth(descriptor);
    }

    float luaLayoutControlWidth(const scripting::LayoutNodeDescriptor& node,
                                const scripting::ControlDescriptor& descriptor)
    {
        const float naturalWidth = luaLayoutControlNaturalWidth(descriptor);
        if (descriptor.type == scripting::ControlType::Checkbox) {
            // Checkbox 不裁剪文字；max_width 不压缩自然宽度，只允许 min_width 扩展占位。
            return node.minWidth.has_value() ? std::max(naturalWidth, *node.minWidth) : naturalWidth;
        }
        return applyLuaLayoutWidthConstraints(node, naturalWidth);
    }

    float estimateLuaFlowNodeWidth(const scripting::LayoutNodeDescriptor& node,
                                   const std::vector<scripting::ControlSnapshot>& controls)
    {
        if (node.kind == scripting::LayoutNodeKind::Text) {
            return ImGui::CalcTextSize(node.text.c_str()).x;
        }
        if (node.kind == scripting::LayoutNodeKind::Control && node.controlIndex < controls.size()) {
            const auto& descriptor = controls[node.controlIndex].descriptor;
            return luaLayoutControlWidth(node, descriptor);
        }
        return ImGui::GetFrameHeight();
    }

} // namespace

bool GuiRuntime::drawLuaLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                   const std::vector<scripting::ControlSnapshot>& controls,
                                   std::string_view stableId,
                                   std::size_t& widgetIndex,
                                   bool earlyExit)
{
    switch (node.kind) {
        case scripting::LayoutNodeKind::Column:
            return drawLuaLayoutChildren(node.children, controls, stableId, widgetIndex, earlyExit);
        case scripting::LayoutNodeKind::Flow:
            return drawLuaFlowLayoutNode(node, controls, stableId, widgetIndex, earlyExit);
        case scripting::LayoutNodeKind::Table:
            return drawLuaTableLayoutNode(node, controls, stableId, widgetIndex, earlyExit);
        case scripting::LayoutNodeKind::Group:
            return drawLuaGroupLayoutNode(node, controls, stableId, widgetIndex, earlyExit);
        case scripting::LayoutNodeKind::Collapse:
            return drawLuaCollapseLayoutNode(node, controls, stableId, widgetIndex, earlyExit);
        case scripting::LayoutNodeKind::Control:
            if (node.controlIndex >= controls.size()) {
                return false;
            }
            return drawDynamicLayoutControl(controls[node.controlIndex],
                                            luaLayoutControlWidth(node, controls[node.controlIndex].descriptor));
        case scripting::LayoutNodeKind::Text:
            ImGui::TextWrapped("%s", node.text.c_str());
            return false;
        case scripting::LayoutNodeKind::Separator:
            ImGui::Separator();
            return false;
        case scripting::LayoutNodeKind::Spacer:
            ImGui::Dummy(ImVec2(0.0F, ImGui::GetFrameHeightWithSpacing()));
            return false;
    }
    return false;
}

bool GuiRuntime::drawLuaLayoutChildren(const std::vector<scripting::LayoutNodeDescriptor>& children,
                                       const std::vector<scripting::ControlSnapshot>& controls,
                                       std::string_view stableId,
                                       std::size_t& widgetIndex,
                                       bool earlyExit)
{
    bool updated = false;
    for (const auto& child : children) {
        updated = drawLuaLayoutNode(child, controls, stableId, widgetIndex, earlyExit) || updated;
        if (earlyExit && updated) {
            return true;
        }
    }
    return updated;
}

bool GuiRuntime::drawLuaFlowLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                       const std::vector<scripting::ControlSnapshot>& controls,
                                       std::string_view stableId,
                                       std::size_t& widgetIndex,
                                       bool earlyExit)
{
    bool updated = false;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const auto& child = node.children[index];
        if (child.kind == scripting::LayoutNodeKind::Text) {
            // flow 内文本按未换行宽度绘制，保持与换行估算一致。
            ImGui::TextUnformatted(child.text.c_str());
        } else {
            updated = drawLuaLayoutNode(child, controls, stableId, widgetIndex, earlyExit) || updated;
        }
        if (earlyExit && updated) {
            return true;
        }
        if (index + 1 < node.children.size() && isLuaFlowInlineNode(child) &&
            isLuaFlowInlineNode(node.children[index + 1])) {
            const float nextWidth = estimateLuaFlowNodeWidth(node.children[index + 1], controls);
            const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            const float nextRight = ImGui::GetItemRectMax().x + node.spacing + nextWidth;
            if (nextRight <= rightEdge) {
                ImGui::SameLine(0.0F, node.spacing);
            } else if (node.runSpacing > 0.0F) {
                ImGui::Dummy(ImVec2(0.0F, node.runSpacing));
            }
        }
    }
    return updated;
}

bool GuiRuntime::drawLuaTableLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                        const std::vector<scripting::ControlSnapshot>& controls,
                                        std::string_view stableId,
                                        std::size_t& widgetIndex,
                                        bool earlyExit)
{
    ImGuiTableFlags flags = ImGuiTableFlags_None;
    if (node.borders) {
        flags |= ImGuiTableFlags_Borders;
    }
    if (node.resizable) {
        flags |= ImGuiTableFlags_Resizable;
    }
    if (node.rowBg) {
        flags |= ImGuiTableFlags_RowBg;
    }
    if (node.sizing == "stretch") {
        flags |= ImGuiTableFlags_SizingStretchSame;
    }

    const std::string tableId = "##lua_dock_table_" + std::string(stableId) + "_" + std::to_string(widgetIndex++);
    ScopedImGuiTable tableGuard(tableId.c_str(), static_cast<int>(node.columns), flags);
    if (!tableGuard.opened()) {
        return false;
    }
    for (std::size_t columnIndex = 0; columnIndex < node.columns; ++columnIndex) {
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
    }

    bool updated = false;
    for (const auto& row : node.rows) {
        ImGui::TableNextRow();
        for (std::size_t columnIndex = 0; columnIndex < node.columns; ++columnIndex) {
            ImGui::TableSetColumnIndex(static_cast<int>(columnIndex));
            if (columnIndex < row.size()) {
                updated = drawLuaLayoutNode(row[columnIndex], controls, stableId, widgetIndex, earlyExit) || updated;
                if (earlyExit && updated) {
                    return true;
                }
            }
        }
    }
    return updated;
}

bool GuiRuntime::drawLuaGroupLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                        const std::vector<scripting::ControlSnapshot>& controls,
                                        std::string_view stableId,
                                        std::size_t& widgetIndex,
                                        bool earlyExit)
{
    ImGui::TextUnformatted(node.title.c_str());
    ImGui::Indent();
    const bool updated = drawLuaLayoutChildren(node.children, controls, stableId, widgetIndex, earlyExit);
    ImGui::Unindent();
    return updated;
}

bool GuiRuntime::drawLuaCollapseLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                           const std::vector<scripting::ControlSnapshot>& controls,
                                           std::string_view stableId,
                                           std::size_t& widgetIndex,
                                           bool earlyExit)
{
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_None;
    if (node.defaultOpen) {
        flags |= ImGuiTreeNodeFlags_DefaultOpen;
    }
    const std::string label =
        node.title + "##lua_layout_collapse_" + std::string(stableId) + "_" + std::to_string(widgetIndex++);
    if (!ImGui::CollapsingHeader(label.c_str(), flags)) {
        return false;
    }
    return drawLuaLayoutChildren(node.children, controls, stableId, widgetIndex, earlyExit);
}

void GuiRuntime::drawLuaDockWindows()
{
    auto& lua = application_.docks().luaState();
    if (lua.docks.empty()) {
        return;
    }

    // 拷贝模式（默认）：深拷贝快照，避免回调中 Lua 修改 dock 列表导致迭代器失效闪烁
    const bool copyMode = application_.docks().configState().luaDockRenderCopyMode;
    const auto layoutKey = luaDockLayoutKey(lua.protocolDir, lua.scriptPath);
    syncLuaDockVisibilityDefaults();
    if (copyMode) {
        // 拷贝模式：深拷贝快照，每帧独立遍历，忽略渲染函数返回值
        const auto dockSnapshots = lua.docks;
        for (const auto& dockSnapshot : dockSnapshots) {
            const auto stableId = luaDockStableId(dockSnapshot.descriptor, layoutKey);
            if (!isLuaDockVisible(stableId)) {
                continue;
            }
            const auto windowName = luaDockWindowName(dockSnapshot.descriptor, layoutKey);
            bool windowOpen = true;
            const bool windowVisible = ImGui::Begin(windowName.c_str(), &windowOpen);
            if (!windowOpen) {
                if (setLuaDockVisible(stableId, false)) {
                    pendingProtocolWorkspaceSave_ = true;
                }
            }
            if (windowVisible) {
                if (dockSnapshot.descriptor.layout.has_value()) {
                    std::size_t widgetIndex = 0;
                    drawLuaLayoutNode(dockSnapshot.descriptor.layout->root,
                                      dockSnapshot.controls,
                                      stableId,
                                      widgetIndex,
                                      false);
                } else {
                    drawLuaDockFlow(dockSnapshot.controls, false);
                }
            }
            ImGui::End();
        }
    } else {
        // 引用模式（旧行为）：按引用遍历，配合 bool 冒泡在控件触发更新后立刻停止本帧遍历
        const auto& dockSnapshots = lua.docks;
        for (const auto& dockSnapshot : dockSnapshots) {
            const auto stableId = luaDockStableId(dockSnapshot.descriptor, layoutKey);
            if (!isLuaDockVisible(stableId)) {
                continue;
            }
            const auto windowName = luaDockWindowName(dockSnapshot.descriptor, layoutKey);
            bool windowOpen = true;
            const bool windowVisible = ImGui::Begin(windowName.c_str(), &windowOpen);
            if (!windowOpen) {
                if (setLuaDockVisible(stableId, false)) {
                    pendingProtocolWorkspaceSave_ = true;
                }
            }
            if (windowVisible) {
                bool updated = false;
                if (dockSnapshot.descriptor.layout.has_value()) {
                    std::size_t widgetIndex = 0;
                    updated = drawLuaLayoutNode(
                        dockSnapshot.descriptor.layout->root, dockSnapshot.controls, stableId, widgetIndex);
                } else {
                    updated = drawLuaDockFlow(dockSnapshot.controls);
                }
                if (updated) {
                    ImGui::End();
                    break;
                }
            }
            ImGui::End();
        }
    }
}

void GuiRuntime::updateLuaDockDefaultLayout()
{
    if (defaultLuaDockNodes_.empty()) {
        return;
    }

    const auto& lua = application_.docks().luaState();
    const auto layoutKey = luaDockLayoutKey(lua.protocolDir, lua.scriptPath);
    const auto requests = buildLuaDockLayoutRequests(lua.docks, layoutKey);
    std::unordered_map<std::string, ImGuiID> tabGroupNodes;

    for (const auto& request : requests) {
        if (defaultDockedLuaStableIds_.contains(stableWindowId(request.windowName))) {
            continue;
        }

        const auto anchor = parseLuaDockAnchor(request.anchor);
        if (!anchor.has_value()) {
            application_.setStatusMessage("Lua Dock 默认停靠点无效: " + request.anchor, true);
            continue;
        }

        ImGuiID targetNode = 0;
        if (const auto groupIter = tabGroupNodes.find(request.tabGroup); groupIter != tabGroupNodes.end()) {
            targetNode = groupIter->second;
        } else if (const auto nodeIter = defaultLuaDockNodes_.find(*anchor); nodeIter != defaultLuaDockNodes_.end()) {
            targetNode = nodeIter->second;
            tabGroupNodes.emplace(request.tabGroup, targetNode);
        }

        if (targetNode == 0) {
            application_.setStatusMessage("Lua Dock 默认停靠节点不存在: " + visibleWindowTitle(request.windowName),
                                          true);
            continue;
        }

        // 核心流程：只给首次出现、ini 尚未创建过的 Lua Dock 提供默认停靠，不覆盖用户拖拽后的布局。
        const bool debugLayout = application_.docks().configState().luaDockLayoutDebug;
        if (debugLayout) {
            application_.setStatusMessage("LuaDockLayout: stableId=" + stableWindowId(request.windowName) +
                                          " anchor=" + request.anchor + " tabGroup=" + request.tabGroup +
                                          " targetNode=" + std::to_string(targetNode));
        }
        const bool docked = dockWindowIfMissing(request.windowName, targetNode);
        if (debugLayout) {
            application_.setStatusMessage(
                "LuaDockLayout: stableId=" + stableWindowId(request.windowName) +
                " docked=" + (docked ? "true" : "false") + " schemaRebuild=" +
                (workspaceLayoutMode_ == WorkspaceLayoutMode::NeedsDefaultBuild ? "true" : "false"));
        }
        defaultDockedLuaStableIds_.insert(stableWindowId(request.windowName));
    }
}

} // namespace protoscope::ui
