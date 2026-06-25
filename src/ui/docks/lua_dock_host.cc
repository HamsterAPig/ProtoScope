#include "../runtime/gui_runtime_detail.hpp"
#include "lua_control_label.hpp"

#include "protoscope/ui/gui_runtime.hpp"

#include <algorithm>
#include <optional>

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
        return node.kind == scripting::LayoutNodeKind::Control || node.kind == scripting::LayoutNodeKind::Text ||
               node.kind == scripting::LayoutNodeKind::InlineGroup;
    }

    bool isLuaFlowFillNode(const scripting::LayoutNodeDescriptor& node)
    {
        return node.fillWidth &&
               (node.kind == scripting::LayoutNodeKind::Control || node.kind == scripting::LayoutNodeKind::InlineGroup);
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

    float luaLayoutControlLabelPartWidth(std::string_view visibleLabel)
    {
        if (visibleLabel.empty()) {
            return 0.0F;
        }
        return ImGui::CalcTextSize(visibleLabel.data(), visibleLabel.data() + visibleLabel.size()).x +
               ImGui::GetStyle().ItemInnerSpacing.x;
    }

    float luaLayoutControlNaturalWidth(const scripting::ControlDescriptor& descriptor, std::string_view visibleLabel)
    {
        if (descriptor.type == scripting::ControlType::Button) {
            return ImGui::CalcTextSize(visibleLabel.data(), visibleLabel.data() + visibleLabel.size()).x + 24.0F;
        }
        if (descriptor.type == scripting::ControlType::Checkbox) {
            return ImGui::GetFrameHeight() + luaLayoutControlLabelPartWidth(visibleLabel);
        }
        return 140.0F + luaLayoutControlLabelPartWidth(visibleLabel);
    }

    float luaLayoutEditableComboItemNaturalWidth(float textWidth)
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float arrowWidth = ImGui::GetFrameHeight();
        return textWidth + style.FramePadding.x * 2.0F + arrowWidth;
    }

    float applyLuaLayoutControlWidth(const scripting::LayoutNodeDescriptor& node,
                                     const scripting::ControlDescriptor& descriptor,
                                     float naturalWidth)
    {
        if (descriptor.type == scripting::ControlType::Checkbox) {
            // Checkbox 不裁剪文字；max_width 不压缩自然宽度，只允许 min_width 扩展占位。
            return node.minWidth.has_value() ? std::max(naturalWidth, *node.minWidth) : naturalWidth;
        }
        return applyLuaLayoutWidthConstraints(node, naturalWidth);
    }

    void reserveLuaLayoutNodeWidth(float startX, float width)
    {
        const float drawnWidth = ImGui::GetItemRectMax().x - startX;
        if (width <= drawnWidth) {
            return;
        }
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::Dummy(ImVec2(width - drawnWidth, 0.0F));
    }

    float luaLayoutRemainingLineWidth()
    {
        const float rightEdge = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        return std::max(1.0F, rightEdge - ImGui::GetCursorScreenPos().x);
    }

} // namespace

float GuiRuntime::elfSymbolComboLayoutNaturalWidth(const scripting::ControlSnapshot& control,
                                                   std::string_view visibleLabel) const
{
    float maxTextWidth = 0.0F;
    const auto measureText = [&maxTextWidth](std::string_view text) {
        if (text.empty()) {
            return;
        }
        maxTextWidth = std::max(maxTextWidth, ImGui::CalcTextSize(text.data(), text.data() + text.size()).x);
    };

    if (const auto* current = std::get_if<scripting::ElfSymbolValue>(&control.value)) {
        measureText(current->label);
    }
    if (const auto stateIter = elfSymbolComboStates_.find(control.descriptor.id);
        stateIter != elfSymbolComboStates_.end()) {
        // 核心流程：按上一帧已加载候选和当前编辑草稿估算宽度，后端刷新后下一帧自然扩展。
        measureText(stateIter->second.draft);
        for (const auto& option : stateIter->second.options) {
            measureText(option.label);
        }
    }

    const float itemWidth = std::max(140.0F, luaLayoutEditableComboItemNaturalWidth(maxTextWidth));
    return itemWidth + luaLayoutControlLabelPartWidth(visibleLabel);
}

float GuiRuntime::luaLayoutControlWidth(const scripting::LayoutNodeDescriptor& node,
                                        const scripting::ControlSnapshot& control) const
{
    const auto& descriptor = control.descriptor;
    const auto naturalWidth = [this, &control, &descriptor](std::string_view visibleLabel) {
        if (descriptor.type == scripting::ControlType::ElfSymbolCombo) {
            return elfSymbolComboLayoutNaturalWidth(control, visibleLabel);
        }
        return luaLayoutControlNaturalWidth(descriptor, visibleLabel);
    };

    const float fullLabelWidth = applyLuaLayoutControlWidth(node, descriptor, naturalWidth(descriptor.label));
    const std::string visibleLabel = resolveLuaControlVisibleLabel(descriptor, fullLabelWidth);
    return applyLuaLayoutControlWidth(node, descriptor, naturalWidth(visibleLabel));
}

float GuiRuntime::luaLayoutControlFillWidth(const scripting::LayoutNodeDescriptor& node,
                                            const scripting::ControlSnapshot& control,
                                            float availableWidth) const
{
    const float naturalWidth = luaLayoutControlWidth(node, control);
    return applyLuaLayoutControlWidth(node, control.descriptor, std::max(naturalWidth, availableWidth));
}

float GuiRuntime::estimateLuaInlineGroupWidth(const scripting::LayoutNodeDescriptor& node,
                                              const std::vector<scripting::ControlSnapshot>& controls) const
{
    float width = 0.0F;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        if (index > 0) {
            width += node.spacing;
        }
        width += estimateLuaFlowNodeWidth(node.children[index], controls);
    }
    return node.minWidth.has_value() ? std::max(width, *node.minWidth) : width;
}

float GuiRuntime::estimateLuaFlowNodeWidth(const scripting::LayoutNodeDescriptor& node,
                                           const std::vector<scripting::ControlSnapshot>& controls) const
{
    if (node.kind == scripting::LayoutNodeKind::Text) {
        return ImGui::CalcTextSize(node.text.c_str()).x;
    }
    if (node.kind == scripting::LayoutNodeKind::Control && node.controlIndex < controls.size()) {
        return luaLayoutControlWidth(node, controls[node.controlIndex]);
    }
    if (node.kind == scripting::LayoutNodeKind::InlineGroup) {
        return estimateLuaInlineGroupWidth(node, controls);
    }
    return ImGui::GetFrameHeight();
}

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
        case scripting::LayoutNodeKind::InlineGroup:
            return drawLuaInlineGroupLayoutNode(
                node,
                controls,
                stableId,
                widgetIndex,
                earlyExit,
                node.fillWidth ? std::optional<float>(luaLayoutRemainingLineWidth()) : std::nullopt);
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
            return drawDynamicLayoutControl(
                controls[node.controlIndex],
                node.fillWidth
                    ? luaLayoutControlFillWidth(node, controls[node.controlIndex], luaLayoutRemainingLineWidth())
                    : luaLayoutControlWidth(node, controls[node.controlIndex]));
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

bool GuiRuntime::drawLuaInlineGroupLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                              const std::vector<scripting::ControlSnapshot>& controls,
                                              std::string_view stableId,
                                              std::size_t& widgetIndex,
                                              bool earlyExit,
                                              std::optional<float> layoutWidth)
{
    const float startX = ImGui::GetCursorScreenPos().x;
    std::optional<std::size_t> fillChildIndex;
    if (layoutWidth.has_value()) {
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            const auto& child = node.children[index];
            if (isLuaFlowFillNode(child) &&
                (child.kind != scripting::LayoutNodeKind::Control || child.controlIndex < controls.size())) {
                fillChildIndex = index;
                break;
            }
        }
    }
    float fixedChildrenWidth = 0.0F;
    if (fillChildIndex.has_value()) {
        for (std::size_t index = 0; index < node.children.size(); ++index) {
            if (index > 0) {
                fixedChildrenWidth += node.spacing;
            }
            if (index == *fillChildIndex) {
                continue;
            }
            fixedChildrenWidth += estimateLuaFlowNodeWidth(node.children[index], controls);
        }
    }
    bool updated = false;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const auto& child = node.children[index];
        if (child.kind == scripting::LayoutNodeKind::Text) {
            ImGui::TextUnformatted(child.text.c_str());
        } else if (fillChildIndex.has_value() && index == *fillChildIndex &&
                   child.kind == scripting::LayoutNodeKind::Control && child.controlIndex < controls.size()) {
            const float fillWidth =
                luaLayoutControlFillWidth(child, controls[child.controlIndex], *layoutWidth - fixedChildrenWidth);
            updated = drawDynamicLayoutControl(controls[child.controlIndex], fillWidth) || updated;
        } else {
            updated = drawLuaLayoutNode(child, controls, stableId, widgetIndex, earlyExit) || updated;
        }
        if (earlyExit && updated) {
            return true;
        }
        if (index + 1 < node.children.size()) {
            ImGui::SameLine(0.0F, node.spacing);
        }
    }
    if (layoutWidth.has_value()) {
        // inline_group 自身 fill_width 只扩展组占位；组内 fill 子控件才吃掉剩余宽度。
        reserveLuaLayoutNodeWidth(startX, *layoutWidth);
    } else if (node.minWidth.has_value()) {
        // inline_group 作为 flow 原子项时，min_width 只扩展占位，不压缩组内控件。
        reserveLuaLayoutNodeWidth(startX, *node.minWidth);
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
        std::optional<float> fillWidth;
        if (isLuaFlowFillNode(child)) {
            if (child.kind == scripting::LayoutNodeKind::Control && child.controlIndex < controls.size()) {
                fillWidth =
                    luaLayoutControlFillWidth(child, controls[child.controlIndex], luaLayoutRemainingLineWidth());
            } else {
                fillWidth = std::max(estimateLuaFlowNodeWidth(child, controls), luaLayoutRemainingLineWidth());
            }
        }
        if (child.kind == scripting::LayoutNodeKind::Text) {
            // flow 内文本按未换行宽度绘制，保持与换行估算一致。
            ImGui::TextUnformatted(child.text.c_str());
        } else if (fillWidth.has_value() && child.kind == scripting::LayoutNodeKind::Control &&
                   child.controlIndex < controls.size()) {
            updated = drawDynamicLayoutControl(controls[child.controlIndex], *fillWidth) || updated;
        } else if (fillWidth.has_value() && child.kind == scripting::LayoutNodeKind::InlineGroup) {
            updated =
                drawLuaInlineGroupLayoutNode(child, controls, stableId, widgetIndex, earlyExit, fillWidth) || updated;
        } else {
            updated = drawLuaLayoutNode(child, controls, stableId, widgetIndex, earlyExit) || updated;
        }
        if (earlyExit && updated) {
            return true;
        }
        if (fillWidth.has_value()) {
            if (index + 1 < node.children.size() && node.runSpacing > 0.0F) {
                ImGui::Dummy(ImVec2(0.0F, node.runSpacing));
            }
            continue;
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
                    drawLuaLayoutNode(
                        dockSnapshot.descriptor.layout->root, dockSnapshot.controls, stableId, widgetIndex, false);
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
