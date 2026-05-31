// 本文件由 gui_runtime_core.cpp 以组件实现方式包含，承接对应 Runtime 业务逻辑。

#if !defined(PROTOSCOPE_GUI_RUNTIME_COMPONENT_INCLUDE)
#error "This runtime component implementation is included by gui_runtime_core.cpp"
#endif

namespace protoscope::ui {

void GuiRuntime::drawLuaDockTable(const scripting::DockSnapshot& dockSnapshot,
                                  const scripting::TableLayoutDescriptor& layout,
                                  std::string_view stableId) {
    std::unordered_map<std::string, const scripting::ControlSnapshot*> controlsById;
    controlsById.reserve(dockSnapshot.controls.size());
    for (const auto& control : dockSnapshot.controls) {
        controlsById.emplace(control.descriptor.id, &control);
    }

    ImGuiTableFlags flags = ImGuiTableFlags_None;
    if (layout.borders) {
        flags |= ImGuiTableFlags_Borders;
    }
    if (layout.resizable) {
        flags |= ImGuiTableFlags_Resizable;
    }
    if (layout.rowBg) {
        flags |= ImGuiTableFlags_RowBg;
    }
    if (layout.sizing == "stretch") {
        flags |= ImGuiTableFlags_SizingStretchSame;
    }

    const std::string tableId = "##lua_dock_table_" + std::string(stableId);
    if (!ImGui::BeginTable(tableId.c_str(), static_cast<int>(layout.columns), flags)) {
        return;
    }

    for (std::size_t columnIndex = 0; columnIndex < layout.columns; ++columnIndex) {
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
    }

    // 核心流程：表格布局只负责按声明式 rows/cells 摆放控件，
    // 具体控件行为仍然复用 drawDynamicControl，避免改动现有控件契约。
    for (const auto& row : layout.rows) {
        ImGui::TableNextRow();
        for (std::size_t columnIndex = 0; columnIndex < layout.columns; ++columnIndex) {
            ImGui::TableSetColumnIndex(static_cast<int>(columnIndex));
            if (columnIndex >= row.cells.size()) {
                continue;
            }

            const auto& cell = row.cells[columnIndex];
            if (cell.spacer) {
                continue;
            }

            const auto controlIter = controlsById.find(cell.controlId);
            if (controlIter == controlsById.end()) {
                continue;
            }

            ImGui::PushID(controlIter->second->descriptor.id.c_str());
            drawDynamicControl(*controlIter->second);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void GuiRuntime::drawLuaDockFormItems(
    const std::vector<scripting::FormLayoutItemDescriptor>& items,
    const std::unordered_map<std::string, const scripting::ControlSnapshot*>& controlsById,
    std::string_view stableId,
    std::size_t& widgetIndex) {
    for (const auto& item : items) {
        switch (item.kind) {
        case scripting::FormLayoutItemKind::Control: {
            const auto controlIter = controlsById.find(item.controlId);
            if (controlIter != controlsById.end()) {
                drawDynamicControl(*controlIter->second);
            }
            break;
        }
        case scripting::FormLayoutItemKind::Controls: {
            bool firstControl = true;
            for (const auto& controlId : item.controls.controlIds) {
                const auto controlIter = controlsById.find(controlId);
                if (controlIter == controlsById.end()) {
                    continue;
                }
                if (!firstControl) {
                    ImGui::SameLine();
                }
                drawDynamicControl(*controlIter->second);
                firstControl = false;
            }
            break;
        }
        case scripting::FormLayoutItemKind::Group:
            if (item.group) {
                ImGui::SeparatorText(item.group->title.c_str());
                drawLuaDockFormItems(item.group->items, controlsById, stableId, widgetIndex);
            }
            break;
        case scripting::FormLayoutItemKind::Collapse:
            if (item.collapse) {
                ImGuiTreeNodeFlags flags = item.collapse->defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
                const std::string headerId = item.collapse->title + "##lua_form_collapse_" + std::string(stableId)
                    + "_" + std::to_string(widgetIndex++);
                if (ImGui::CollapsingHeader(headerId.c_str(), flags)) {
                    drawLuaDockFormItems(item.collapse->items, controlsById, stableId, widgetIndex);
                }
            }
            break;
        case scripting::FormLayoutItemKind::Separator:
            ImGui::Separator();
            break;
        case scripting::FormLayoutItemKind::Text:
            ImGui::TextWrapped("%s", item.text.text.c_str());
            break;
        }
    }
}

void GuiRuntime::drawLuaDockForm(const scripting::DockSnapshot& dockSnapshot,
                                 const scripting::FormLayoutDescriptor& layout,
                                 std::string_view stableId) {
    std::unordered_map<std::string, const scripting::ControlSnapshot*> controlsById;
    controlsById.reserve(dockSnapshot.controls.size());
    for (const auto& control : dockSnapshot.controls) {
        controlsById.emplace(control.descriptor.id, &control);
    }

    // 核心流程：form 布局只负责分组、折叠和同排摆放，
    // 具体控件交互继续复用 drawDynamicControl，保证旧控件行为不变。
    std::size_t widgetIndex = 0;
    drawLuaDockFormItems(layout.items, controlsById, stableId, widgetIndex);
}

void GuiRuntime::drawLuaDockWindows() {
    auto& lua = application_.docks().luaState();
    if (lua.docks.empty()) {
        return;
    }

    // 这里按值复制当前帧快照，保证本帧渲染遍历期间底层容器不会被重入修改。
    const auto dockSnapshots = lua.docks;
    const auto layoutKey = luaDockLayoutKey(lua.protocolDir, lua.scriptPath);
    syncLuaDockVisibilityDefaults();
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
                if (dockSnapshot.descriptor.layout->kind == scripting::DockLayoutKind::Table) {
                    drawLuaDockTable(dockSnapshot, dockSnapshot.descriptor.layout->table, stableId);
                } else if (dockSnapshot.descriptor.layout->kind == scripting::DockLayoutKind::Form) {
                    drawLuaDockForm(dockSnapshot, dockSnapshot.descriptor.layout->form, stableId);
                } else {
                    drawLuaDockFlow(dockSnapshot.controls);
                }
            } else {
                drawLuaDockFlow(dockSnapshot.controls);
            }
        }
        ImGui::End();
    }
}

void GuiRuntime::updateLuaDockDefaultLayout() {
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
            application_.setStatusMessage("Lua Dock 默认停靠节点不存在: " + visibleWindowTitle(request.windowName), true);
            continue;
        }

        // 核心流程：只给首次出现、ini 尚未创建过的 Lua Dock 提供默认停靠，不覆盖用户拖拽后的布局。
        const bool debugLayout = application_.docks().configState().luaDockLayoutDebug;
        if (debugLayout) {
            application_.setStatusMessage("LuaDockLayout: stableId=" + stableWindowId(request.windowName) + " anchor=" + request.anchor + " tabGroup=" + request.tabGroup + " targetNode=" + std::to_string(targetNode));
        }
        const bool docked = dockWindowIfMissing(request.windowName, targetNode);
        if (debugLayout) {
            application_.setStatusMessage("LuaDockLayout: stableId=" + stableWindowId(request.windowName) + " docked=" + (docked ? "true" : "false") + " schemaRebuild=" + (workspaceLayoutMode_ == WorkspaceLayoutMode::NeedsDefaultBuild ? "true" : "false"));
        }
        defaultDockedLuaStableIds_.insert(stableWindowId(request.windowName));
    }
}

} // namespace protoscope::ui
