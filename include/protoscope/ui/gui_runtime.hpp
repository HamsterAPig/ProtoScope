#pragma once

#include "protoscope/app/application.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/ui/dock_layout.hpp"
#include "protoscope/ui/wave_dock_renderer.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    void syncDialogQueue();
    void drawDialogs();
    void drawMainMenu();
    void drawCommDock();
    void drawProtocolDock();
    void drawLuaDockWindows();
    void drawSendDock();
    void drawReceiveDock();
    void drawLogDock();
    void drawScriptDock();
    void drawLuaDockFlow(const std::vector<scripting::ControlSnapshot>& controls);
    void drawLuaDockTable(const scripting::DockSnapshot& dockSnapshot,
                          const scripting::TableLayoutDescriptor& layout,
                          std::string_view stableId);
    void drawLuaDockForm(const scripting::DockSnapshot& dockSnapshot,
                         const scripting::FormLayoutDescriptor& layout,
                         std::string_view stableId);
    void drawLuaDockFormItems(const std::vector<scripting::FormLayoutItemDescriptor>& items,
                              const std::unordered_map<std::string, const scripting::ControlSnapshot*>& controlsById,
                              std::string_view stableId,
                              std::size_t& widgetIndex);
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
    std::unordered_map<LuaDockAnchor, unsigned int> defaultLuaDockNodes_;
    std::unordered_set<std::string> defaultDockedLuaStableIds_;
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
    bool serialPortsScanned_{false};
    std::string commonBaudRateDraft_;
    std::string commonBaudRateDraftModel_;
    std::string protocolDirDraft_;
    std::string protocolDirDraftModel_;
    std::deque<scripting::DialogRequest> dialogQueue_;
    std::optional<scripting::DialogRequest> activeDialog_;
    bool activeDialogOpened_{false};
    WaveDockRenderer waveDockRenderer_;
};

} // namespace protoscope::ui
