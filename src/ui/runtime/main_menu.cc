#include "protoscope/app/application.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/keyboard_shortcuts.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include <cmath>
#include <string>

#include <imgui.h>
#if defined(_WIN32)
#include <windows.h>
#include <shellapi.h>
#endif

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

    bool menuItemWithHelp(
        const char* label, const char* shortcut, const char* help, bool selected = false, bool enabled = true)
    {
        const bool clicked = ImGui::MenuItem(label, shortcut, selected, enabled);
        if (help != nullptr && help[0] != '\0' &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", help);
        }
        return clicked;
    }

} // namespace

void GuiRuntime::drawFileMenu()
{
    if (!ImGui::BeginMenu("文件")) {
        return;
    }

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
    ImGui::Separator();
    if (ImGui::MenuItem("打开 ELF/ElfStaticView 数据文件...", shortcutLabel(ShortcutAction::OpenElfDataFile).data())) {
        openElfStaticAddressDialog();
    }
    ImGui::BeginDisabled(application_.dataTransferStatus().active);
    if (ImGui::MenuItem("导入数据...")) openUnifiedDataImport();
    if (ImGui::MenuItem("导出数据...")) openUnifiedDataExport();
    if (ImGui::MenuItem("使用上次配置导出", nullptr, false,
                        application_.runtimeConfig().gui.lastDataExport.valid)) openUnifiedDataExport(-1, true);
    ImGui::EndDisabled();
    if (ImGui::MenuItem("导出波形分析报告...")) {
        openWaveAnalysisExportDialog();
    }
    ImGui::Separator();
    const bool recording = application_.isRawCaptureRecording();
    if (menuItemWithHelp("开始完整原始数据录制...",
                         shortcutLabel(ShortcutAction::ToggleRawRecording).data(),
                         "选择 .psraw 文件后开始连续写入完整原始事件流。",
                         false,
                         !recording)) {
        openRawCaptureRecordingDialog();
    }
    if (menuItemWithHelp("停止完整原始数据录制",
                         shortcutLabel(ShortcutAction::ToggleRawRecording).data(),
                         "停止写入当前完整录制文件，并保留已经采集的事件流。",
                         false,
                         recording)) {
        stopRawCaptureRecordingWithStatus();
    }
    ImGui::EndMenu();
}

void GuiRuntime::drawReplayMenu()
{
    if (!ImGui::BeginMenu("回放")) {
        return;
    }

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
    if (menuItemWithHelp("继续/暂停回放",
                         shortcutLabel(ShortcutAction::PlaybackTogglePlayPause).data(),
                         "从当前位置继续按原始时间轴播放事件，或暂停正在播放的时间轴。",
                         false,
                         (canAdvance && !status.playing) || (status.loaded && status.playing))) {
        if (status.playing) {
            application_.pauseRawCaptureReplay();
        } else if (!application_.playRawCaptureReplay(error)) {
            application_.setStatusMessage("原始回放继续失败: " + error);
        }
    }
    if (menuItemWithHelp("单步推进",
                         shortcutLabel(ShortcutAction::PlaybackStepForward).data(),
                         "只执行下一个原始事件，便于逐帧排查。",
                         false,
                         canAdvance)) {
        if (!application_.stepRawCaptureReplay(error)) {
            application_.setStatusMessage("原始回放单步失败: " + error);
        }
    }
    if (menuItemWithHelp("停止并卸载时间轴",
                         shortcutLabel(ShortcutAction::PlaybackUnloadTimeline).data(),
                         "停止回放并释放当前载入的 .psraw 时间轴。",
                         false,
                         status.loaded)) {
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

void GuiRuntime::drawViewMenu()
{
    if (!ImGui::BeginMenu("视图")) {
        return;
    }

    const bool previousShowCommDock = showCommDock_;
    const bool previousShowProtocolDock = showProtocolDock_;
    const bool previousShowTransferDock = showTransferDock_;
    const bool previousShowRequestTraceDock = showRequestTraceDock_;
    const bool previousShowOfflineReplayDock = showOfflineReplayDock_;
    const bool previousShowLogDock = showLogDock_;
    const bool previousShowScriptDock = showScriptDock_;
    const bool previousShowWaveDock = showWaveDock_;

    ImGui::MenuItem("通讯配置", shortcutLabel(ShortcutAction::ToggleCommDock).data(), &showCommDock_);
    ImGui::MenuItem(
        "协议脚本 / 动态控件", shortcutLabel(ShortcutAction::ToggleProtocolDock).data(), &showProtocolDock_);
    ImGui::MenuItem("收发数据", shortcutLabel(ShortcutAction::ToggleTransferDock).data(), &showTransferDock_);
    ImGui::MenuItem("请求追踪", shortcutLabel(ShortcutAction::ToggleRequestTraceDock).data(), &showRequestTraceDock_);
    ImGui::MenuItem("离线复现", shortcutLabel(ShortcutAction::ToggleOfflineReplayDock).data(), &showOfflineReplayDock_);
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

void GuiRuntime::drawSettingsMenu()
{
    if (!ImGui::BeginMenu("设置")) {
        return;
    }

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
    if (ImGui::BeginMenu("主题")) {
        const auto currentTheme = application_.runtimeConfig().gui.theme;
        std::string error;
        for (const auto& item : themeManager_.themes()) {
            const bool selected = currentTheme == item.id;
            ImGui::PushID(item.id.c_str());
            if (ImGui::MenuItem(item.name.c_str(), nullptr, selected) && !selected) {
                if (themeManager_.request(item.id, error)) application_.setGuiTheme(item.id);
            }
            ImGui::PopID();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("重载主题")) {
            themeManager_.setConfigPath(application_.docks().configState().loadedFromPath);
            if (themeManager_.reload(error)) themeManager_.request(currentTheme, error);
        }
        if (ImGui::MenuItem("导出当前主题")) {
            const auto path = themeManager_.directory() / ("exported_" + std::to_string(nowMs()) + ".yaml");
            if (themeManager_.exportCurrent(path, error))
                application_.setStatusMessage("主题已导出: " + path.string(), false);
        }
        if (ImGui::MenuItem("打开主题目录")) {
            std::error_code ec;
            std::filesystem::create_directories(themeManager_.directory(), ec);
            if (ec) error = ec.message();
#if defined(_WIN32)
            else if (reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open",
                         std::filesystem::absolute(themeManager_.directory()).c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
                error = "无法打开主题目录";
#else
            else error = "主题目录: " + std::filesystem::absolute(themeManager_.directory()).string();
#endif
        }
        if (!error.empty()) application_.setStatusMessage(error, false);
        ImGui::EndMenu();
    }
    ImGui::EndMenu();
}

void GuiRuntime::drawMainMenu()
{
    syncLuaDockVisibilityDefaults();

    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    drawFileMenu();
    drawReplayMenu();
    drawViewMenu();
    drawSettingsMenu();

    drawLuaViewMenu();
    drawBusinessMenu();
    drawHelpMenu();

    ImGui::EndMainMenuBar();
}

} // namespace protoscope::ui
