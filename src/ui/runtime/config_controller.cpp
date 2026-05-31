// 本文件由 gui_runtime_core.cpp 以组件实现方式包含，承接对应 Runtime 业务逻辑。

#if !defined(PROTOSCOPE_GUI_RUNTIME_COMPONENT_INCLUDE)
#error "This runtime component implementation is included by gui_runtime_core.cpp"
#endif

namespace protoscope::ui {

bool GuiRuntime::reloadConfigFromDisk() {
    saveCurrentProtocolWorkspace();
    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    if (!application_.applyConfig(loaded.config)) {
        return false;
    }
    loadCurrentProtocolWorkspace();
    configSnapshot_ = configStore_.snapshot(configStore_.defaultConfigPath());
    auto& configState = application_.docks().configState();
    application_.docks().clearPendingExternalReload();
    application_.docks().clearDirty("已从磁盘重载配置");
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    return true;
}

bool GuiRuntime::pollConfigFileChanges() {
    auto& configState = application_.docks().configState();
    if (!configState.configHotReloadEnabled) {
        return false;
    }
    if (!configSnapshot_.path.empty() && !configStore_.hasChanged(configSnapshot_)) {
        return false;
    }
    configSnapshot_ = configStore_.snapshot(configStore_.defaultConfigPath());
    application_.docks().setPendingExternalReload(configSnapshot_.timestampMs, "检测到外部配置更新，请手动保存或重载");
    return false;
}

bool GuiRuntime::maybeAutoSave() {
    auto& configState = application_.docks().configState();
    if (!configState.autoSaveEnabled || !configState.dirty) {
        return false;
    }
    if (configState.pendingExternalReload) {
        return false;
    }

    const auto currentMs = nowMs();
    if (lastAutoSaveAtMs_ != 0 && currentMs - lastAutoSaveAtMs_ < configState.autoSaveIntervalMs) {
        return false;
    }

    std::string error;
    const auto path = std::filesystem::path(configState.loadedFromPath);
    if (!configStore_.save(path, application_.captureConfig(), error)) {
        application_.setStatusMessage("自动保存失败: " + error, true);
        return false;
    }
    configSnapshot_ = configStore_.snapshot(path);
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    application_.docks().clearDirty("自动保存成功");
    lastAutoSaveAtMs_ = currentMs;
    return true;
}

} // namespace protoscope::ui
