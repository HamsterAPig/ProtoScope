#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/keyboard_shortcuts.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include <imgui.h>

namespace protoscope::ui {

namespace {

    struct LogLevelMenuItem {
        const char* label;
        config::LogLevel level;
    };

    constexpr LogLevelMenuItem kLogLevelMenuItems[] = {
        {.label = "调试", .level = config::LogLevel::Debug},
        {.label = "信息", .level = config::LogLevel::Info},
        {.label = "警告", .level = config::LogLevel::Warn},
        {.label = "错误", .level = config::LogLevel::Error},
    };

} // namespace

void GuiRuntime::drawMainMenu()
{
    syncLuaDockVisibilityDefaults();

    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("保存配置", shortcutLabel(ShortcutAction::SaveConfig).data())) {
            saveCurrentConfigToDisk();
        }
        if (ImGui::MenuItem("重新加载配置", shortcutLabel(ShortcutAction::ReloadConfig).data())) {
            if (!reloadConfigFromDisk()) {
                application_.setStatusMessage("从磁盘重载配置失败", true);
            }
        }
        if (ImGui::MenuItem("重新加载协议", shortcutLabel(ShortcutAction::ReloadProtocol).data())) {
            requestProtocolWorkspaceSwitch(application_.docks().luaState().protocolDir, true);
        }
        if (ImGui::MenuItem("重置当前协议 Dock 布局",
                            nullptr,
                            false,
                            canResetProtocolWorkspaceLayout(protocolWorkspaceLoaded_, activeWorkspaceProtocolKey_))) {
            resetCurrentProtocolWorkspaceLayout();
        }
        if (ImGui::MenuItem("打开 ELF/ElfStaticView 数据文件...",
                            shortcutLabel(ShortcutAction::OpenElfDataFile).data())) {
            openElfStaticAddressDialog();
        }
        if (ImGui::MenuItem("导入原始波形...", shortcutLabel(ShortcutAction::ImportRawWave).data())) {
            openRawCaptureImportDialog();
        }
        if (ImGui::MenuItem("导出原始波形...", shortcutLabel(ShortcutAction::ExportRawWave).data())) {
            openRawCaptureExportDialog();
        }
        ImGui::Separator();
        const bool recording = application_.isRawCaptureRecording();
        if (ImGui::MenuItem("开始完整原始数据录制...",
                            shortcutLabel(ShortcutAction::ToggleRawRecording).data(),
                            false,
                            !recording)) {
            openRawCaptureRecordingDialog();
        }
        if (ImGui::MenuItem("停止完整原始数据录制",
                            shortcutLabel(ShortcutAction::ToggleRawRecording).data(),
                            false,
                            recording)) {
            stopRawCaptureRecordingWithStatus();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("视图")) {
        const bool previousShowCommDock = showCommDock_;
        const bool previousShowProtocolDock = showProtocolDock_;
        const bool previousShowTransferDock = showTransferDock_;
        const bool previousShowLogDock = showLogDock_;
        const bool previousShowScriptDock = showScriptDock_;
        const bool previousShowWaveDock = showWaveDock_;

        ImGui::MenuItem("通讯配置", shortcutLabel(ShortcutAction::ToggleCommDock).data(), &showCommDock_);
        ImGui::MenuItem("协议脚本 / 动态控件",
                        shortcutLabel(ShortcutAction::ToggleProtocolDock).data(),
                        &showProtocolDock_);
        ImGui::MenuItem("收发数据", shortcutLabel(ShortcutAction::ToggleTransferDock).data(), &showTransferDock_);
        ImGui::MenuItem("日志", shortcutLabel(ShortcutAction::ToggleLogDock).data(), &showLogDock_);
        ImGui::MenuItem("脚本", shortcutLabel(ShortcutAction::ToggleScriptDock).data(), &showScriptDock_);
        ImGui::MenuItem("波形", shortcutLabel(ShortcutAction::ToggleWaveDock).data(), &showWaveDock_);
        ImGui::Separator();
        drawHeaderBadge("布局", defaultUiStyleTokens().accent, false);
        ImGui::SameLine();
        ImGui::TextDisabled("中心波形 / 左配置 / 右分析 / 底部事件流");
        if (previousShowCommDock != showCommDock_ || previousShowProtocolDock != showProtocolDock_ ||
            previousShowTransferDock != showTransferDock_ || previousShowLogDock != showLogDock_ ||
            previousShowScriptDock != showScriptDock_ || previousShowWaveDock != showWaveDock_) {
            pendingProtocolWorkspaceSave_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("重置当前协议 Dock 布局",
                            nullptr,
                            false,
                            canResetProtocolWorkspaceLayout(protocolWorkspaceLoaded_, activeWorkspaceProtocolKey_))) {
            resetCurrentProtocolWorkspaceLayout();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("设置")) {
        if (ImGui::BeginMenu("日志等级")) {
            const auto currentLevel = application_.logger().currentConfig().level;
            for (const auto& item : kLogLevelMenuItems) {
                const bool selected = currentLevel == item.level;
                if (ImGui::MenuItem(item.label, nullptr, selected) && !selected) {
                    application_.setLogLevel(item.level);
                    application_.setStatusMessage(std::string("日志等级已切换为：") + item.label, true);
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    drawLuaViewMenu();
    drawHelpMenu();

    ImGui::EndMainMenuBar();
}

} // namespace protoscope::ui
