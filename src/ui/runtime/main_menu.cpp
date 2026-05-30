#include "protoscope/ui/gui_runtime.hpp"

#include <imgui.h>

namespace protoscope::ui {

void GuiRuntime::drawMainMenu() {
    syncLuaDockVisibilityDefaults();

    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("保存配置")) {
            std::string error;
            const auto path = std::filesystem::path(application_.docks().configState().loadedFromPath);
            if (configStore_.save(path, application_.captureConfig(), error)) {
                application_.docks().clearDirty("配置已保存");
                configSnapshot_ = configStore_.snapshot(path);
                application_.docks().configState().fileTimestampMs = configSnapshot_.timestampMs;
            } else {
                application_.setStatusMessage("保存配置失败: " + error, true);
            }
        }
        if (ImGui::MenuItem("重新加载配置")) {
            if (!reloadConfigFromDisk()) {
                application_.setStatusMessage("从磁盘重载配置失败", true);
            }
        }
        if (ImGui::MenuItem("重新加载协议")) {
            requestProtocolWorkspaceSwitch(application_.docks().luaState().protocolDir, true);
        }
        if (ImGui::MenuItem("打开 ELF/JSON...")) {
            openElfStaticAddressDialog();
        }
        if (ImGui::MenuItem("导入原始波形...")) {
            openRawCaptureImportDialog();
        }
        if (ImGui::MenuItem("导出原始波形...")) {
            openRawCaptureExportDialog();
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

        ImGui::MenuItem("通讯配置", nullptr, &showCommDock_);
        ImGui::MenuItem("协议脚本 / 动态控件", nullptr, &showProtocolDock_);
        ImGui::MenuItem("收发数据", nullptr, &showTransferDock_);
        ImGui::MenuItem("日志", nullptr, &showLogDock_);
        ImGui::MenuItem("脚本", nullptr, &showScriptDock_);
        ImGui::MenuItem("波形", nullptr, &showWaveDock_);
        if (previousShowCommDock != showCommDock_
            || previousShowProtocolDock != showProtocolDock_
            || previousShowTransferDock != showTransferDock_
            || previousShowLogDock != showLogDock_
            || previousShowScriptDock != showScriptDock_
            || previousShowWaveDock != showWaveDock_) {
            pendingProtocolWorkspaceSave_ = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(
                "重置当前协议 Dock 布局",
                nullptr,
                false,
                canResetProtocolWorkspaceLayout(protocolWorkspaceLoaded_, activeWorkspaceProtocolKey_))) {
            resetCurrentProtocolWorkspaceLayout();
        }
        ImGui::EndMenu();
    }

    drawLuaViewMenu();
    drawHelpMenu();

    ImGui::EndMainMenuBar();
}

} // namespace protoscope::ui
