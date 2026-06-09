#include "protoscope/app/application.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/keyboard_shortcuts.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include <cmath>
#include <imgui.h>
#include <string>

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
        if (ImGui::MenuItem("导入现场会话包...")) {
            openSessionPackageImportDialog();
        }
        if (ImGui::MenuItem("导出现场会话包...")) {
            openSessionPackageExportDialog();
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
        if (ImGui::MenuItem("载入原始回放时间轴...")) {
            openRawCaptureReplayTimelineDialog();
        }
        if (ImGui::MenuItem("导出当前缓存快照...", shortcutLabel(ShortcutAction::ExportRawWave).data())) {
            openRawCaptureExportDialog();
        }
        if (ImGui::MenuItem("导出波形分析报告...")) {
            openWaveAnalysisExportDialog();
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

    if (ImGui::BeginMenu("回放")) {
        const auto status = application_.rawCaptureReplayStatus();
        const bool canAdvance = status.loaded && status.eventIndex < status.eventCount;
        const char* replayState = "未载入";
        if (status.loaded) {
            replayState = status.playing ? "播放中" : (status.eventIndex >= status.eventCount ? "已结束" : "已暂停");
        }
        ImGui::Text("状态 %s", replayState);
        ImGui::Text("位置 %zu / %zu (%.1f%%)", status.eventIndex, status.eventCount, status.progress * 100.0);
        ImGui::Text("倍速 %.1fx", status.speed);
        std::string error;
        if (ImGui::MenuItem("继续", nullptr, false, canAdvance && !status.playing)) {
            if (!application_.playRawCaptureReplay(error)) {
                application_.setStatusMessage("原始回放继续失败: " + error);
            }
        }
        if (ImGui::MenuItem("暂停", nullptr, false, status.loaded && status.playing)) {
            application_.pauseRawCaptureReplay();
        }
        if (ImGui::MenuItem("单步", nullptr, false, canAdvance)) {
            if (!application_.stepRawCaptureReplay(error)) {
                application_.setStatusMessage("原始回放单步失败: " + error);
            }
        }
        if (ImGui::MenuItem("停止并卸载时间轴", nullptr, false, status.loaded)) {
            application_.unloadRawCaptureReplayTimeline();
        }
        if (ImGui::BeginMenu("倍速", status.loaded)) {
            for (const double speed : {0.5, 1.0, 2.0, 4.0, 8.0}) {
                const bool selected = std::abs(status.speed - speed) < 0.001;
                const std::string label = std::to_string(speed) + "x";
                if (ImGui::MenuItem(label.c_str(), nullptr, selected)) {
                    application_.setRawCaptureReplaySpeed(speed);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("定位到开头", nullptr, false, status.loaded)) {
            if (!application_.seekRawCaptureReplay(0, error)) {
                application_.setStatusMessage("原始回放定位失败: " + error);
            }
        }
        if (ImGui::MenuItem("定位到中点", nullptr, false, status.loaded && status.eventCount > 0)) {
            if (!application_.seekRawCaptureReplay(status.eventCount / 2, error)) {
                application_.setStatusMessage("原始回放定位失败: " + error);
            }
        }
        if (ImGui::MenuItem("定位到末尾", nullptr, false, status.loaded && status.eventCount > 0)) {
            if (!application_.seekRawCaptureReplay(status.eventCount, error)) {
                application_.setStatusMessage("原始回放定位失败: " + error);
            }
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("视图")) {
        const bool previousShowCommDock = showCommDock_;
        const bool previousShowProtocolDock = showProtocolDock_;
        const bool previousShowTransferDock = showTransferDock_;
        const bool previousShowRequestTraceDock = showRequestTraceDock_;
        const bool previousShowOfflineReplayDock = showOfflineReplayDock_;
        const bool previousShowLogDock = showLogDock_;
        const bool previousShowScriptDock = showScriptDock_;
        const bool previousShowWaveDock = showWaveDock_;

        ImGui::MenuItem("通讯配置", shortcutLabel(ShortcutAction::ToggleCommDock).data(), &showCommDock_);
        ImGui::MenuItem("协议脚本 / 动态控件",
                        shortcutLabel(ShortcutAction::ToggleProtocolDock).data(),
                        &showProtocolDock_);
        ImGui::MenuItem("收发数据", shortcutLabel(ShortcutAction::ToggleTransferDock).data(), &showTransferDock_);
        ImGui::MenuItem("请求追踪",
                        shortcutLabel(ShortcutAction::ToggleRequestTraceDock).data(),
                        &showRequestTraceDock_);
        ImGui::MenuItem("离线复现",
                        shortcutLabel(ShortcutAction::ToggleOfflineReplayDock).data(),
                        &showOfflineReplayDock_);
        ImGui::MenuItem("日志", shortcutLabel(ShortcutAction::ToggleLogDock).data(), &showLogDock_);
        ImGui::MenuItem("脚本", shortcutLabel(ShortcutAction::ToggleScriptDock).data(), &showScriptDock_);
        ImGui::MenuItem("波形", shortcutLabel(ShortcutAction::ToggleWaveDock).data(), &showWaveDock_);
        ImGui::Separator();
        drawHeaderBadge("布局", defaultUiStyleTokens().accent, false);
        ImGui::SameLine();
        ImGui::TextDisabled("中心波形 / 左配置 / 右分析 / 底部事件流");
        if (previousShowCommDock != showCommDock_ || previousShowProtocolDock != showProtocolDock_ ||
            previousShowTransferDock != showTransferDock_ || previousShowRequestTraceDock != showRequestTraceDock_ ||
            previousShowOfflineReplayDock != showOfflineReplayDock_ || previousShowLogDock != showLogDock_ ||
            previousShowScriptDock != showScriptDock_ ||
            previousShowWaveDock != showWaveDock_) {
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
