#pragma once

#include "protoscope/app/application.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/ui/dock_layout.hpp"
#include "protoscope/ui/update_check.hpp"
#include "protoscope/ui/wave_dock_renderer.hpp"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <optional>
#include <span>
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
    enum class LogExportTarget {
        Transfer,
        Host,
        Script,
    };

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
    void drawRawCaptureFileDialogs();
    void drawMainMenu();
    void drawLuaViewMenu();
    void drawCommDock();
    void drawProtocolDock();
    void drawLuaDockWindows();
    void drawTransferDock();
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
    void resetCurrentProtocolWorkspaceLayout();
    void loadCurrentProtocolControlState();
    void saveCurrentProtocolControlState();
    void pruneCurrentLuaDockSettings();
    bool isLuaDockVisible(std::string_view stableId) const;
    bool setLuaDockVisible(std::string_view stableId, bool visible);
    void syncLuaDockVisibilityDefaults();
    void openRawCaptureImportDialog();
    void openRawCaptureExportDialog();
    void openTransferLogExportDialog();
    void openHostLogExportDialog();
    void openScriptLogExportDialog();
    void openLogExportDialog(LogExportTarget target);
    void openElfStaticAddressDialog();
    void importRawCaptureFromPath(const std::filesystem::path& path);
    void exportRawCaptureToPath(const std::filesystem::path& path);
    void drawLogExportFileDialog();
    std::vector<dock::ReceiveRow> logExportRows(LogExportTarget target);
    bool exportLogTargetToPath(LogExportTarget target, const std::filesystem::path& path);
    bool exportLogRowsToPath(const std::filesystem::path& path,
                             std::span<const dock::ReceiveRow> rows,
                             bool showTimestamps,
                             bool showHex,
                             std::string_view title);
    void loadElfStaticAddressFromPath(const std::filesystem::path& path);
    void drawElfStaticAddressDialog();
    void refreshWindowTitle();
    void requestAboutDialog();
    void drawAboutDialog();
    void startUpdateCheck();
    void drawUpdateCheckDialog();
    std::filesystem::path currentProtocolLayoutPath() const;
    std::filesystem::path legacyProtocolLayoutPath() const;
    std::filesystem::path protocolControlStatePath() const;

    bool reloadConfigFromDisk();
    bool pollConfigFileChanges();
    bool maybeAutoSave();
    void sleepUntilNextFrame(std::uint64_t frameStartMs) const;

    static std::uint64_t nowMs();
    static std::string formatTimestamp(std::uint64_t timestampMs);

    app::Application& application_;
    const config::ConfigStore& configStore_;
    GLFWwindow* window_{nullptr};
    std::uint64_t lastRenderAtMs_{0};
    std::uint64_t lastAutoSaveAtMs_{0};
    config::FileSnapshot configSnapshot_{};
    std::string lastWindowTitle_;
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
    bool showTransferDock_{true};
    bool showLogDock_{true};
    bool showScriptDock_{true};
    bool showWaveDock_{true};
    std::unordered_map<std::string, bool> luaDockVisibility_;
    float transferSendSectionHeight_{210.0F};
    bool aboutDialogRequested_{false};
    bool updateCheckDialogRequested_{false};
    bool updateCheckInProgress_{false};
    std::optional<UpdateCheckResult> updateCheckResult_;
    std::future<UpdateCheckResult> updateCheckFuture_;
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
    bool rawCaptureImportDialogOpen_{false};
    bool rawCaptureImportDialogOpened_{false};
    std::string rawCaptureImportPath_;
    std::string rawCaptureImportError_;
    bool rawCaptureExportDialogOpen_{false};
    bool rawCaptureExportDialogOpened_{false};
    std::string rawCaptureExportPath_;
    std::string rawCaptureExportError_;
    bool logExportDialogOpen_{false};
    bool logExportDialogOpened_{false};
    LogExportTarget logExportTarget_{LogExportTarget::Transfer};
    std::string logExportPath_;
    std::string logExportError_;
    std::string logExportDialogTitle_;
    bool elfStaticAddressDialogOpen_{false};
    bool elfStaticAddressDialogOpened_{false};
    std::string elfStaticAddressPath_;
    std::string elfStaticAddressError_;
    struct ElfSymbolComboUiState {
        std::string draft;
        std::string queriedDraft;
        std::uint64_t editedAtMs{0};
        std::vector<scripting::ElfSymbolValue> options;
    };
    std::unordered_map<std::string, ElfSymbolComboUiState> elfSymbolComboStates_;
    WaveDockRenderer waveDockRenderer_;
};

} // namespace protoscope::ui
