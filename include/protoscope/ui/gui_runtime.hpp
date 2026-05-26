#pragma once

#include "protoscope/app/application.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/ui/dock_layout.hpp"
#include "protoscope/ui/wave_dock_renderer.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct GLFWwindow;

namespace protoscope::ui {

class GuiRuntime {
public:
    GuiRuntime(app::Application& application, const config::ConfigStore& configStore);
    ~GuiRuntime();

    bool initialize();
    int run();
    void shutdown();

private:
    bool initializeWindow();
    bool initializeImGui();
    bool initializePlotContext();
    void shutdownImGui();
    void shutdownPlotContext();
    void shutdownWindow();

    void ensureChineseFont();
    void renderFrame();
    void drawStatusBar();
    void drawMainMenu();
    void drawCommDock();
    void drawProtocolDock();
    void drawSendDock();
    void drawReceiveDock();
    void drawLogDock();
    void drawScriptDock();
    void drawDynamicControl(const scripting::ControlSnapshot& control);
    void updateLuaDockDefaultLayout();
    void requestProtocolWorkspaceSwitch(std::string protocolDir, bool forceReload);
    void processPendingProtocolWorkspaceSwitch();
    bool switchProtocolWorkspace(const std::string& protocolDir, bool forceReload);
    void loadCurrentProtocolWorkspace();
    void saveCurrentProtocolWorkspace();
    void loadCurrentProtocolControlState();
    void saveCurrentProtocolControlState();
    void pruneCurrentLuaDockSettings();
    std::filesystem::path currentProtocolLayoutPath() const;
    std::filesystem::path legacyProtocolLayoutPath() const;
    std::filesystem::path protocolControlStatePath() const;

    bool reloadConfigFromDisk();
    bool pollConfigFileChanges();
    bool maybeAutoSave();
    void sleepUntilNextFrame(std::uint64_t frameStartMs) const;

    static std::uint64_t nowMs();
    static std::string formatTimestamp(std::uint64_t timestampMs);

private:
    app::Application& application_;
    const config::ConfigStore& configStore_;
    GLFWwindow* window_{nullptr};
    std::uint64_t lastRenderAtMs_{0};
    std::uint64_t lastAutoSaveAtMs_{0};
    config::FileSnapshot configSnapshot_{};
    std::string activeWorkspaceProtocolKey_;
    std::optional<std::string> pendingProtocolDir_;
    bool pendingProtocolForceReload_{false};
    bool protocolWorkspaceLoaded_{false};
    WorkspaceLayoutMode workspaceLayoutMode_{WorkspaceLayoutMode::NeedsDefaultBuild};
    std::filesystem::path executableDir_;
    bool pendingLuaDefaultDockLayout_{false};
    bool pendingProtocolWorkspaceSave_{false};
    unsigned int mainDockspaceId_{0};
    std::unordered_map<LuaDockAnchor, unsigned int> defaultLuaDockNodes_;
    std::unordered_set<std::string> defaultDockedLuaWindows_;
    bool running_{false};
    bool showCommDock_{true};
    bool showProtocolDock_{true};
    bool showSendDock_{true};
    bool showReceiveDock_{true};
    bool showLogDock_{true};
    bool showScriptDock_{true};
    bool showWaveDock_{true};
    std::string serialPortDraft_;
    std::string serialPortDraftModel_;
    std::string commonBaudRateDraft_;
    std::string commonBaudRateDraftModel_;
    std::string customBaudRateDraft_;
    std::string customBaudRateDraftModel_;
    std::string protocolDirDraft_;
    std::string protocolDirDraftModel_;
    WaveDockRenderer waveDockRenderer_;
};

} // namespace protoscope::ui
