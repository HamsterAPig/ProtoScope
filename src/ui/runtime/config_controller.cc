#include "protoscope/app/application.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include <filesystem>

namespace protoscope::ui {

bool GuiRuntime::reloadConfigFromDisk()
{
    const auto path = std::filesystem::path(application_.docks().configState().loadedFromPath);
    const auto loaded = configStore_.load(path);
    if (!loaded.error.empty() || !loaded.loadedFromDisk) {
        settingsError_ = "重载失败，保持运行态：" + loaded.error;
        application_.setStatusMessage(settingsError_, false); return false;
    }
    saveCurrentProtocolWorkspace();
    if (!application_.applyConfig(loaded.config)) {
        return false;
    }
    std::string themeError;
    themeManager_.setConfigPath(loaded.resolvedPath);
    if (themeManager_.reload(themeError)) themeManager_.request(loaded.config.gui.theme, themeError);
    if (loaded.config.gui.rendererBackend != options_.rendererBackend) {
        application_.setStatusMessage("渲染后端配置已更新为 " +
                                          std::string(config::guiRendererBackendId(loaded.config.gui.rendererBackend)) +
                                          "，重启后生效",
                                      false);
    }
    loadCurrentProtocolWorkspace();
    configSnapshot_ = configStore_.snapshot(path);
    auto& configState = application_.docks().configState();
    application_.docks().clearPendingExternalReload();
    application_.docks().clearDirty("已从磁盘重载配置");
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    settingsError_.clear();
    if (!themeError.empty()) application_.setStatusMessage(themeError, false);
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
    const auto observed = configStore_.snapshot(configState.loadedFromPath);
    application_.docks().setPendingExternalReload(observed.timestampMs, "检测到外部配置更新，请明确覆盖或重载");
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
        elfStaticAddressWatch_.pendingStatusMessage = "ELF 数据文件自动重载失败，继续使用旧模型";
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
    if (settingsApplyRequested_ || settingsSaveAfterApply_ || settingsOverwriteSnapshot_ ||
        application_.settingsSaveBlocked() || application_.dataTransferStatus().active) return false;
    if (!configStore_.saveChecked(path, application_.captureConfig(), configSnapshot_, error)) {
        settingsError_ = "自动保存失败: " + error;
        if (configStore_.hasChanged(configSnapshot_))
            application_.docks().setPendingExternalReload(configStore_.snapshot(path).timestampMs, "自动保存暂停：外部配置已变化，请明确覆盖或重载");
        application_.setStatusMessage(settingsError_, true);
        return false;
    }
    configSnapshot_ = configStore_.snapshot(path);
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    application_.docks().clearDirty("自动保存成功");
    lastAutoSaveAtMs_ = currentMs;
    return true;
}

} // namespace protoscope::ui
