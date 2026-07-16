#include "workspace_controller.hpp"

#include "../runtime/gui_runtime_detail.hpp"

#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/protocol_state_file.hpp"

#include <exception>
#include <system_error>

namespace protoscope::ui {

WorkspaceController::WorkspaceController(GuiRuntime& runtime) : runtime_(runtime) {}

void WorkspaceController::requestProtocolWorkspaceSwitch(std::string protocolDir, bool forceReload)
{
    runtime_.requestProtocolWorkspaceSwitch(std::move(protocolDir), forceReload);
}

void WorkspaceController::processPendingProtocolWorkspaceSwitch()
{
    runtime_.processPendingProtocolWorkspaceSwitch();
}

bool WorkspaceController::switchProtocolWorkspace(const std::string& protocolDir, bool forceReload)
{
    return runtime_.switchProtocolWorkspace(protocolDir, forceReload);
}

void WorkspaceController::loadCurrentProtocolWorkspace()
{
    runtime_.loadCurrentProtocolWorkspace();
}

void WorkspaceController::saveCurrentProtocolWorkspace()
{
    runtime_.saveCurrentProtocolWorkspace();
}

void WorkspaceController::resetCurrentProtocolWorkspaceLayout()
{
    runtime_.resetCurrentProtocolWorkspaceLayout();
}

void WorkspaceController::loadCurrentProtocolControlState()
{
    runtime_.loadCurrentProtocolControlState();
}

void WorkspaceController::saveCurrentProtocolControlState()
{
    runtime_.saveCurrentProtocolControlState();
}

void WorkspaceController::pruneCurrentLuaDockSettings()
{
    runtime_.pruneCurrentLuaDockSettings();
}

bool WorkspaceController::isLuaDockVisible(std::string_view stableId) const
{
    return runtime_.isLuaDockVisible(stableId);
}

bool WorkspaceController::setLuaDockVisible(std::string_view stableId, bool visible)
{
    return runtime_.setLuaDockVisible(stableId, visible);
}

void WorkspaceController::syncLuaDockVisibilityDefaults()
{
    runtime_.syncLuaDockVisibilityDefaults();
}

std::filesystem::path WorkspaceController::currentProtocolLayoutPath() const
{
    return runtime_.currentProtocolLayoutPath();
}

std::filesystem::path WorkspaceController::legacyProtocolLayoutPath() const
{
    return runtime_.legacyProtocolLayoutPath();
}

std::filesystem::path WorkspaceController::protocolControlStatePath() const
{
    return runtime_.protocolControlStatePath();
}

void GuiRuntime::requestProtocolWorkspaceSwitch(std::string protocolDir, bool forceReload)
{
    pendingProtocolDir_ = std::move(protocolDir);
    pendingProtocolForceReload_ = forceReload;
}

void GuiRuntime::processPendingProtocolWorkspaceSwitch()
{
    if (!pendingProtocolDir_.has_value()) {
        return;
    }

    const auto protocolDir = std::move(*pendingProtocolDir_);
    pendingProtocolDir_.reset();
    try {
        if (switchProtocolWorkspace(protocolDir, pendingProtocolForceReload_)) {
            return;
        }
    } catch (const std::exception& ex) {
        application_.logger().error("protocol", std::string("协议重载异常: ") + ex.what());
    } catch (...) {
        application_.logger().error("protocol", "协议重载异常: 未知异常");
    }
    // UI 帧最外层兜底：Lua 协议写错只能表现为重载失败，不能让 GUI 进程退出。
    application_.setStatusMessage("协议重载失败", true);
}

bool GuiRuntime::switchProtocolWorkspace(const std::string& protocolDir, bool forceReload)
{
    try {
        const auto& previousLua = application_.docks().luaState();
        const auto requestedDir =
            configStore_.normalizeProtocolDir(previousLua.protocolRootDir, protocolDir).generic_string();
        const bool sameProtocol = isSameProtocolWorkspace(requestedDir);

        if (shouldResetLuaDefaultDockStateOnProtocolSwitch(sameProtocol)) {
            resetLuaDefaultDockStateForProtocolSwitch();
        }

        if (!reloadProtocolWorkspace(protocolDir, forceReload, sameProtocol)) {
            return false;
        }

        loadProtocolWorkspaceAfterReload(sameProtocol);
        return true;
    } catch (const std::exception& ex) {
        application_.logger().error("protocol", std::string("协议工作区切换异常: ") + ex.what());
    } catch (...) {
        application_.logger().error("protocol", "协议工作区切换异常: 未知异常");
    }
    if (!application_.docks().luaState().loaded) {
        application_.setStatusMessage("协议重载失败", true);
    }
    return false;
}

bool GuiRuntime::isSameProtocolWorkspace(const std::string& requestedDir) const
{
    const auto& previousLua = application_.docks().luaState();
    return protocolWorkspaceLoaded_ && previousLua.loaded && previousLua.protocolDir == requestedDir &&
           activeWorkspaceProtocolKey_ ==
               luaDockLayoutKey(requestedDir, configStore_.mainLuaPath(requestedDir).generic_string());
}

void GuiRuntime::resetLuaDefaultDockStateForProtocolSwitch()
{
    saveCurrentProtocolWorkspace();
    defaultDockedLuaStableIds_.clear();
    defaultLuaDockNodes_.clear();
    protocolWorkspaceLoaded_ = false;
    workspaceLayoutMode_ = WorkspaceLayoutMode::NeedsDefaultBuild;
    pendingLuaDefaultDockLayout_ = false;
    pendingProtocolWorkspaceSave_ = false;
}

bool GuiRuntime::reloadProtocolWorkspace(const std::string& protocolDir, bool forceReload, bool sameProtocol)
{
    if (application_.reloadProtocolDirectory(protocolDir, forceReload)) {
        return true;
    }
    if (!sameProtocol) {
        loadCurrentProtocolWorkspace();
    }
    return false;
}

void GuiRuntime::loadProtocolWorkspaceAfterReload(bool sameProtocol)
{
    if (sameProtocol) {
        loadCurrentProtocolControlState();
    } else {
        loadCurrentProtocolWorkspace();
    }
}

void GuiRuntime::loadCurrentProtocolWorkspace()
{
    const auto& lua = application_.docks().luaState();
    const auto layoutPaths = resolveLuaDockLayoutPaths(executableDir_, lua.protocolDir, lua.scriptPath);
    application_.logger().trace("workspace",
                                "workspace load kind=workspace endpoint=" + layoutPaths.protocolKey +
                                    " file=" + layoutPaths.layoutPath.generic_string());
    beginProtocolWorkspaceLoad(layoutPaths);
    loadProtocolWorkspaceLayoutIni(layoutPaths);
    loadCurrentProtocolControlState();
}

void GuiRuntime::beginProtocolWorkspaceLoad(const LuaDockLayoutPaths& layoutPaths)
{
    activeWorkspaceProtocolKey_ = layoutPaths.protocolKey;
    protocolWorkspaceLoaded_ = true;
    defaultDockedLuaStableIds_.clear();
    defaultLuaDockNodes_.clear();
    workspaceLayoutMode_ = workspaceLayoutModeAfterLoad(layoutPaths);
    if (application_.docks().configState().luaDockLayoutDebug) {
        const char* modeLabel = workspaceLayoutMode_ == WorkspaceLayoutMode::NeedsDefaultBuild ? "rebuilding" : "ready";
        application_.setStatusMessage("LuaDockLayout: load protocol=" + activeWorkspaceProtocolKey_ +
                                      " schemaVersion=" + std::to_string(layoutPaths.schemaVersion) + " isLegacy=" +
                                      (layoutPaths.isLegacyLayout ? "true" : "false") + " mode=" + modeLabel);
    }
    pendingLuaDefaultDockLayout_ = false;
    pendingProtocolWorkspaceSave_ = false;
}

void GuiRuntime::loadProtocolWorkspaceLayoutIni(const LuaDockLayoutPaths& layoutPaths)
{
    ImGui::ClearIniSettings();
    if (layoutPaths.hasUserLayout && !layoutPaths.isLegacyLayout) {
        const auto savedLayoutHealth = readDockLayoutIniHealth(layoutPaths.layoutPath);
        if (savedLayoutHealth.has_value() && shouldRebuildDockLayout(*savedLayoutHealth)) {
            workspaceLayoutMode_ = WorkspaceLayoutMode::NeedsDefaultBuild;
            pendingProtocolWorkspaceSave_ = true;
            application_.setStatusMessage(
                std::string("检测到损坏的协议 Dock 布局，已回退默认布局: CentralNode=") +
                std::to_string(savedLayoutHealth->centralNodeCount) +
                (savedLayoutHealth->centralNodeInLegacyLeftPane ? " left-pane=true" : " left-pane=false"));
        } else {
            ImGui::LoadIniSettingsFromDisk(layoutPaths.layoutPath.string().c_str());
        }
    } else if (layoutPaths.hasLegacyLayout) {
        ImGui::LoadIniSettingsFromDisk(layoutPaths.legacyLayoutPath.string().c_str());
        pendingProtocolWorkspaceSave_ = true;
    } else if (layoutPaths.hasUserLayout) {
        ImGui::LoadIniSettingsFromDisk(layoutPaths.layoutPath.string().c_str());
        pendingProtocolWorkspaceSave_ = true;
    } else {
        pendingProtocolWorkspaceSave_ = true;
    }
    ImGui::GetIO().WantSaveIniSettings = false;
}

void GuiRuntime::saveCurrentProtocolWorkspace()
{
    if (!protocolWorkspaceLoaded_ || activeWorkspaceProtocolKey_.empty()) {
        return;
    }
    if (waveFullscreenActive_) {
        // 波形全屏期间可能产生临时窗口焦点或 Dock ini 变化；退出恢复快照后再允许保存。
        ImGui::GetIO().WantSaveIniSettings = false;
        return;
    }

    try {
        const auto layoutPath = currentProtocolLayoutPath();
        const auto parentPath = layoutPath.parent_path();
        if (!parentPath.empty()) {
            std::error_code directoryError;
            std::filesystem::create_directories(parentPath, directoryError);
            if (directoryError) {
                application_.setStatusMessage("保存协议 Dock 布局目录失败: " + directoryError.message(), true);
                ImGui::GetIO().WantSaveIniSettings = false;
                pendingProtocolWorkspaceSave_ = false;
                return;
            }
        }
        pruneCurrentLuaDockSettings();
        ImGui::SaveIniSettingsToDisk(layoutPath.string().c_str());
        writeLuaDockLayoutMeta(luaDockLayoutMetaPath(executableDir_, activeWorkspaceProtocolKey_), 3);
        application_.logger().trace("workspace",
                                    "workspace saved kind=workspace endpoint=" + activeWorkspaceProtocolKey_ +
                                        " file=" + layoutPath.generic_string());
    } catch (const std::exception& ex) {
        application_.setStatusMessage(std::string("保存协议 Dock 布局失败: ") + ex.what(), true);
    }
    ImGui::GetIO().WantSaveIniSettings = false;
    pendingProtocolWorkspaceSave_ = false;
    saveCurrentProtocolControlState();
}

void GuiRuntime::resetCurrentProtocolWorkspaceLayout()
{
    if (!canResetProtocolWorkspaceLayout(protocolWorkspaceLoaded_, activeWorkspaceProtocolKey_)) {
        application_.setStatusMessage("当前没有可重置的协议 Dock 布局", true);
        return;
    }

    // 核心流程：手动重置只清空当前运行时 Dock 状态，下一帧按新默认布局重建并覆盖当前协议布局文件。
    ImGui::ClearIniSettings();
    defaultDockedLuaStableIds_.clear();
    defaultLuaDockNodes_.clear();
    workspaceLayoutMode_ = WorkspaceLayoutMode::NeedsDefaultBuild;
    pendingLuaDefaultDockLayout_ = false;
    pendingProtocolWorkspaceSave_ = true;
    ImGui::GetIO().WantSaveIniSettings = false;
    application_.setStatusMessage("当前协议 Dock 布局将在下一帧重置");
}

void GuiRuntime::pruneCurrentLuaDockSettings()
{
    if (activeWorkspaceProtocolKey_.empty()) {
        return;
    }

    keepOnlyCurrentLuaDockSettings(activeWorkspaceProtocolKey_);
}

bool GuiRuntime::isLuaDockVisible(std::string_view stableId) const
{
    const auto iter = luaDockVisibility_.find(std::string(stableId));
    if (iter == luaDockVisibility_.end()) {
        return true;
    }
    return iter->second;
}

bool GuiRuntime::setLuaDockVisible(std::string_view stableId, bool visible)
{
    const auto key = std::string(stableId);
    const auto iter = luaDockVisibility_.find(key);
    if (iter != luaDockVisibility_.end() && iter->second == visible) {
        return false;
    }
    luaDockVisibility_[key] = visible;
    return true;
}

void GuiRuntime::syncLuaDockVisibilityDefaults()
{
    const auto& lua = application_.docks().luaState();
    const auto layoutKey = luaDockLayoutKey(lua.protocolDir, lua.scriptPath);
    for (const auto& dockSnapshot : lua.docks) {
        const auto stableId = luaDockStableId(dockSnapshot.descriptor, layoutKey);
        if (!luaDockVisibility_.contains(stableId)) {
            luaDockVisibility_.emplace(std::move(stableId), true);
        }
    }
}

void GuiRuntime::loadCurrentProtocolControlState()
{
    resetProtocolControlLoadDefaults();

    const auto statePath = protocolControlStatePath();
    application_.logger().trace("workspace",
                                "control state load kind=control_state endpoint=" + activeWorkspaceProtocolKey_ +
                                    " file=" + statePath.generic_string());
    std::error_code existsError;
    if (!std::filesystem::exists(statePath, existsError)) {
        useDefaultProtocolControlState();
        if (existsError) {
            application_.setStatusMessage("检查协议控件状态文件失败: " + existsError.message(), true);
        }
        return;
    }

    try {
        const ProtocolStateFileOptions options;
        std::string lockError;
        auto stateLock = ProtocolStateFileLock::acquire(statePath, options.lockWaitTimeout, lockError);
        if (!stateLock) {
            useDefaultProtocolControlState();
            application_.setStatusMessage(std::string("加载协议控件状态失败: ") + lockError, true);
            return;
        }

        const auto loadResult = loadProtocolStateRootForUpdate(statePath);
        if (!loadResult.ok) {
            useDefaultProtocolControlState();
            application_.setStatusMessage(std::string("加载协议控件状态失败: ") + loadResult.error, true);
            return;
        }
        const auto& root = loadResult.root;
        const auto protocolsNode = root["protocols"];
        if (!protocolsNode || !protocolsNode.IsMap()) {
            useDefaultProtocolControlState();
            if (loadResult.recovery.recoveredCorruptFile) {
                reportRecoveredProtocolStateBackup(loadResult.recovery, "检测到损坏的协议控件状态，已备份为: ");
            }
            return;
        }
        const auto protocolNode = protocolsNode[activeWorkspaceProtocolKey_];
        restoreProtocolWorkspaceState(root, protocolNode);
        const auto protocolState = protocolNode["controls"];
        if (!protocolState) {
            syncLuaDockVisibilityDefaults();
            if (loadResult.recovery.recoveredCorruptFile) {
                reportRecoveredProtocolStateBackup(loadResult.recovery, "检测到损坏的协议控件状态，已备份为: ");
            }
            return;
        }

        restorePersistedControlValues(protocolState);
        syncLuaDockVisibilityDefaults();
        application_.docks().waveState().statusMessage = "协议波形状态已恢复";
        application_.logger().trace("workspace",
                                    "control state loaded kind=control_state endpoint=" +
                                        activeWorkspaceProtocolKey_ + " file=" + statePath.generic_string());
    } catch (const std::exception& ex) {
        syncLuaDockVisibilityDefaults();
        application_.setStatusMessage(std::string("加载协议控件状态失败: ") + ex.what(), true);
    }
}

void GuiRuntime::resetProtocolControlLoadDefaults()
{
    showCommDock_ = true;
    showProtocolDock_ = true;
    showTransferDock_ = true;
    showRequestTraceDock_ = true;
    showOfflineReplayDock_ = true;
    showLogDock_ = true;
    showScriptDock_ = true;
    showWaveDock_ = true;
    luaDockVisibility_.clear();
}

void GuiRuntime::useDefaultProtocolControlState()
{
    syncLuaDockVisibilityDefaults();
    restoreElfStaticAddressForCurrentProtocol({});
}

void GuiRuntime::reportRecoveredProtocolStateBackup(const ProtocolStateFileRecovery& recovery,
                                                    std::string_view messagePrefix)
{
    application_.setStatusMessage(std::string(messagePrefix) + recovery.backupPath.filename().string(), true);
}

void GuiRuntime::restoreProtocolWorkspaceState(const YAML::Node& root, const YAML::Node& protocolNode)
{
    restoreElfStaticAddressForCurrentProtocol(restoreElfStaticAddressPath(root, activeWorkspaceProtocolKey_));
    // 核心流程：Dock 可见性按协议工作区存储，旧文件缺字段时自动回退默认可见。
    ProtocolDockVisibilityState visibilityState;
    restoreDockVisibilityState(root, activeWorkspaceProtocolKey_, visibilityState);
    showCommDock_ = visibilityState.showCommDock;
    showProtocolDock_ = visibilityState.showProtocolDock;
    showTransferDock_ = visibilityState.showTransferDock;
    showRequestTraceDock_ = visibilityState.showRequestTraceDock;
    showOfflineReplayDock_ = visibilityState.showOfflineReplayDock;
    showLogDock_ = visibilityState.showLogDock;
    showScriptDock_ = visibilityState.showScriptDock;
    showWaveDock_ = visibilityState.showWaveDock;
    luaDockVisibility_ = std::move(visibilityState.luaDockVisibility);

    restoreWaveProtocolState(root, activeWorkspaceProtocolKey_, application_.docks().waveState());
    auto& sendState = application_.docks().sendState();
    const auto historyLimit = configuredSendHistoryLimit(application_.captureConfig());
    restoreSendHistoryFromNode(protocolNode["send"], sendState, historyLimit);
}

void GuiRuntime::restorePersistedControlValues(const YAML::Node& controlsNode)
{
    const auto controls = application_.docks().luaState().controlStates;
    for (const auto& control : controls) {
        const auto& descriptor = control.descriptor;
        if (!isPersistedControlType(descriptor.type)) {
            continue;
        }
        const auto saved = controlsNode[descriptor.id];
        if (!saved || saved["type"].as<std::string>("") != controlTypeName(descriptor.type)) {
            continue;
        }
        if (descriptor.type == scripting::ControlType::TxSequence) {
            if (const auto value = readTxSequenceValue(saved["value"], descriptor)) {
                application_.restoreControlValue(descriptor.id, *value);
            }
            continue;
        }
        if (const auto value = readControlValue(saved["value"], descriptor.type)) {
            application_.restoreControlValue(descriptor.id, *value);
        }
    }
}

void GuiRuntime::saveCurrentProtocolControlState()
{
    const auto statePath = protocolControlStatePath();
    application_.logger().trace("workspace",
                                "control state save kind=control_state endpoint=" + activeWorkspaceProtocolKey_ +
                                    " file=" + statePath.generic_string());
    try {
        const ProtocolStateFileOptions options;
        std::string lockError;
        auto stateLock = ProtocolStateFileLock::acquire(statePath, options.lockWaitTimeout, lockError);
        if (!stateLock) {
            application_.setStatusMessage(std::string("保存协议控件状态失败: ") + lockError, true);
            return;
        }

        auto loadResult = loadProtocolStateRootForUpdate(statePath);
        if (!loadResult.ok) {
            application_.setStatusMessage(std::string("保存协议控件状态失败: ") + loadResult.error, true);
            return;
        }
        auto root = loadResult.root;
        if (!root["protocols"] || !root["protocols"].IsMap()) {
            root["protocols"] = YAML::Node(YAML::NodeType::Map);
        }

        auto protocolNode = root["protocols"][activeWorkspaceProtocolKey_];
        storeCurrentProtocolState(root, protocolNode);
        std::string writeError;
        if (!writeProtocolStateRootAtomically(statePath, root, writeError)) {
            application_.setStatusMessage(std::string("保存协议控件状态失败: ") + writeError, true);
            return;
        }
        if (loadResult.recovery.recoveredCorruptFile) {
            application_.setStatusMessage(
                "检测到损坏的协议控件状态，已备份并重建: " + loadResult.recovery.backupPath.filename().string(), true);
        }
        application_.logger().trace("workspace",
                                    "control state saved kind=control_state endpoint=" +
                                        activeWorkspaceProtocolKey_ + " file=" + statePath.generic_string());
    } catch (const std::exception& ex) {
        application_.setStatusMessage(std::string("保存协议控件状态失败: ") + ex.what(), true);
    }
}

ProtocolDockVisibilityState GuiRuntime::captureCurrentDockVisibilityState() const
{
    ProtocolDockVisibilityState visibilityState;
    visibilityState.showCommDock = showCommDock_;
    visibilityState.showProtocolDock = showProtocolDock_;
    visibilityState.showTransferDock = showTransferDock_;
    visibilityState.showRequestTraceDock = showRequestTraceDock_;
    visibilityState.showOfflineReplayDock = showOfflineReplayDock_;
    visibilityState.showLogDock = showLogDock_;
    visibilityState.showScriptDock = showScriptDock_;
    visibilityState.showWaveDock = showWaveDock_;
    visibilityState.luaDockVisibility = luaDockVisibility_;
    if (waveFullscreenActive_ && waveFullscreenActiveMode_ == config::GuiWaveFullscreenMode::Focus &&
        waveFullscreenSnapshot_) {
        // Focus 全屏的 Dock 可见性只是运行期快照，控件状态保存仍使用进入全屏前的可见性。
        visibilityState.showCommDock = waveFullscreenSnapshot_->showCommDock;
        visibilityState.showProtocolDock = waveFullscreenSnapshot_->showProtocolDock;
        visibilityState.showTransferDock = waveFullscreenSnapshot_->showTransferDock;
        visibilityState.showRequestTraceDock = waveFullscreenSnapshot_->showRequestTraceDock;
        visibilityState.showOfflineReplayDock = waveFullscreenSnapshot_->showOfflineReplayDock;
        visibilityState.showLogDock = waveFullscreenSnapshot_->showLogDock;
        visibilityState.showScriptDock = waveFullscreenSnapshot_->showScriptDock;
        visibilityState.showWaveDock = waveFullscreenSnapshot_->showWaveDock;
        visibilityState.luaDockVisibility = waveFullscreenSnapshot_->luaDockVisibility;
    }
    return visibilityState;
}

YAML::Node GuiRuntime::buildPersistedControlState() const
{
    YAML::Node controlsNode;
    const auto controls = application_.docks().luaState().controlStates;
    for (const auto& control : controls) {
        const auto& descriptor = control.descriptor;
        if (!isPersistedControlType(descriptor.type)) {
            continue;
        }

        YAML::Node controlNode;
        controlNode["type"] = controlTypeName(descriptor.type);
        writeControlValue(controlNode["value"], control);
        controlsNode[descriptor.id] = controlNode;
    }
    return controlsNode;
}

void GuiRuntime::storeCurrentProtocolState(YAML::Node& root, YAML::Node& protocolNode)
{
    // 核心流程：Dock 可见性和控件状态共用同一协议状态文件，保证切协议时行为一致。
    storeDockVisibilityState(root, activeWorkspaceProtocolKey_, captureCurrentDockVisibilityState());
    protocolNode = root["protocols"][activeWorkspaceProtocolKey_];
    protocolNode["controls"] = buildPersistedControlState();
    storeElfStaticAddressPath(root, activeWorkspaceProtocolKey_, elfStaticAddressPath_);
    storeWaveProtocolState(root, activeWorkspaceProtocolKey_, application_.docks().waveState());
    writeSendHistoryNode(
        protocolNode, application_.docks().sendState(), configuredSendHistoryLimit(application_.captureConfig()));
}

std::filesystem::path GuiRuntime::currentProtocolLayoutPath() const
{
    return luaDockLayoutPath(executableDir_, activeWorkspaceProtocolKey_);
}

std::filesystem::path GuiRuntime::legacyProtocolLayoutPath() const
{
    const auto& lua = application_.docks().luaState();
    return luaDockLayoutPath(executableDir_, legacyLuaDockLayoutKey(lua.protocolDir, lua.scriptPath));
}

std::filesystem::path GuiRuntime::protocolControlStatePath() const
{
    return std::filesystem::path("config") / "ui" / "protocol-control-state.yaml";
}

} // namespace protoscope::ui
