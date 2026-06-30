#include "protoscope/app/application.hpp"
#include "protoscope/ui/gui_runtime.hpp"

#include <filesystem>

namespace protoscope::ui {

bool GuiRuntime::reloadConfigFromDisk()
{
    saveCurrentProtocolWorkspace();
    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    if (!application_.applyConfig(loaded.config)) {
        return false;
    }
    if (loaded.config.gui.rendererBackend != options_.rendererBackend) {
        application_.setStatusMessage(
            "渲染后端配置已更新为 " +
                std::string(config::guiRendererBackendId(loaded.config.gui.rendererBackend)) + "，重启后生效",
            false);
    }
    loadCurrentProtocolWorkspace();
    configSnapshot_ = configStore_.snapshot(configStore_.defaultConfigPath());
    auto& configState = application_.docks().configState();
    application_.docks().clearPendingExternalReload();
    application_.docks().clearDirty("已从磁盘重载配置");
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    return true;
}

bool GuiRuntime::pollConfigFileChanges()
{
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

bool GuiRuntime::pollElfStaticAddressFileChanges()
{
    std::error_code error;
    auto result = pollElfStaticAddressFileWatchState(elfStaticAddressWatch_, nowMs(), error);
    if (!result.statusMessage.empty()) {
        application_.setStatusMessage(result.statusMessage, error || result.shouldReload);
    }
    if (!result.shouldReload) {
        return result.changed;
    }

    std::string loadError;
    if (!application_.loadElfStaticAddressFile(elfStaticAddressWatch_.path, loadError)) {
        elfStaticAddressError_ = loadError;
        elfStaticAddressWatch_.pendingReload = true;
        elfStaticAddressWatch_.pendingReloadSinceMs = nowMs();
        elfStaticAddressWatch_.pendingStatusMessage = "ELF 数据文件重建后自动重载失败，继续使用旧模型";
        application_.setStatusMessage(elfStaticAddressWatch_.pendingStatusMessage + ": " + loadError, true);
        return true;
    }
    application_.refreshSelectedElfSymbolControls();
    if (result.clearComboCache) {
        elfSymbolComboStates_.clear();
    }
    elfStaticAddressError_.clear();
    return true;
}

bool GuiRuntime::maybeAutoSave()
{
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
