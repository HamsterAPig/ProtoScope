#pragma once

#include "protoscope/app/application.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/ui/dock_layout.hpp"
#include "protoscope/ui/elf_static_address_file_watch.hpp"
#include "protoscope/ui/protocol_state_file.hpp"
#include "protoscope/ui/protocol_ui_state.hpp"
#include "protoscope/ui/ui_component.hpp"
#include "protoscope/ui/ui_host_context.hpp"
#include "protoscope/ui/update_check.hpp"
#include "protoscope/ui/wave_dock_renderer.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct GLFWwindow;

namespace protoscope::ui {

class IDialogComponent;
class IDockComponent;
class IMenuContributor;
class IUiComponent;
class WorkspaceController;

class GuiRuntime {
public:
    GuiRuntime(app::Application& application, const config::ConfigStore& configStore);
    ~GuiRuntime();

    bool initialize();
    int run();
    void shutdown();

private:
    friend class RuntimeMenuComponent;
    friend class RuntimeDialogComponent;
    friend class CommDockComponent;
    friend class ProtocolDockComponent;
    friend class LuaDockComponent;
    friend class RequestTraceDockComponent;
    friend class OfflineReplayDockComponent;
    friend class LogDockComponent;
    friend class WorkspaceController;

    enum class LogExportTarget {
        Transfer,
        Host,
        Script,
    };

    struct FilteredLogRowsCache {
        const void* source{nullptr};
        std::uint64_t version{0};
        dock::LogFilterState filter{};
        bool includeBytePreview{false};
        std::size_t rowCount{0};
        const dock::ReceiveRow* firstRow{nullptr};
        float endpointWidth{120.0F};
        std::vector<const dock::ReceiveRow*> rows;
    };

    struct WaveFullscreenDockSnapshot {
        bool showCommDock{true};
        bool showProtocolDock{true};
        bool showTransferDock{true};
        bool showRequestTraceDock{true};
        bool showOfflineReplayDock{true};
        bool showLogDock{true};
        bool showScriptDock{true};
        bool showWaveDock{true};
        std::unordered_map<std::string, bool> luaDockVisibility;
        std::string dockIniSnapshot;
    };

    bool initializeWindow();
    bool initializeImGui();
    bool initializePlotContext();
    void shutdownImGui();
    void shutdownPlotContext();
    void shutdownWindow();

    void ensureChineseFont();
    void registerUiComponents();
    void attachUiComponents();
    void detachUiComponents();
    void syncRuntimeState();
    RuntimeUiContext makeUiContext();
    void drawRegisteredMenus();
    void syncRegisteredDialogs();
    void drawRegisteredDialogs();
    void drawRegisteredDocks();
    void drawAppShell();
    void buildModernDefaultLayout(ImGuiID dockspaceId);
    void renderFrame();
    void drawAppHeader(float menuBarHeight);
    void drawStatusBar();
    void processWaveFullscreenInput();
    void enterWaveFullscreen();
    void exitWaveFullscreen();
    void captureWaveFullscreenDockSnapshot();
    void restoreWaveFullscreenDockIniSnapshot();
    void applyWaveFocusFullscreen();
    void restoreWaveFocusFullscreen();
    bool saveCurrentConfigToDisk();
    bool stopRawCaptureRecordingWithStatus();
    void syncDialogQueue();
    void drawDialogs();
    void drawRawCaptureFileDialogs();
    void handleGlobalShortcuts();
    void drawMainMenu();
    void drawHelpMenu();
    void drawLuaViewMenu();
    void drawCommDock();
    void drawCommTransportModeSelector(dock::CommDockState& comm);
    void drawCommTransportConfig(dock::CommDockState& comm);
    void drawTcpClientCommConfig(dock::CommDockState& comm);
    void drawTcpServerCommConfig(dock::CommDockState& comm);
    void drawSerialCommConfig(dock::CommDockState& comm);
    void drawUdpPeerCommConfig(dock::CommDockState& comm);
    void drawCommStatus(const dock::CommDockState& comm);
    void drawCommActions(dock::ConfigDockState& configState);
    void drawCommParserStatus(const dock::CommDockState& comm);
    void drawProtocolDock();
    void drawLuaDockWindows();
    void drawTransferDock();
    void drawTransferLogSection(float logHeight);
    void drawTransferSendSection(float minPayloadHeight, const ImGuiStyle& style);
    void drawRequestTraceDock();
    void drawOfflineReplayDock();
    void drawLogDock();
    void drawScriptDock();
    bool drawLuaDockFlow(const std::vector<scripting::ControlSnapshot>& controls, bool earlyExit = true);
    bool drawLuaLayoutNode(const scripting::LayoutNodeDescriptor& node,
                           const std::vector<scripting::ControlSnapshot>& controls,
                           std::string_view stableId,
                           std::size_t& widgetIndex,
                           bool earlyExit = true);
    bool drawLuaLayoutChildren(const std::vector<scripting::LayoutNodeDescriptor>& children,
                               const std::vector<scripting::ControlSnapshot>& controls,
                               std::string_view stableId,
                               std::size_t& widgetIndex,
                               bool earlyExit);
    bool drawLuaFlowLayoutNode(const scripting::LayoutNodeDescriptor& node,
                               const std::vector<scripting::ControlSnapshot>& controls,
                               std::string_view stableId,
                               std::size_t& widgetIndex,
                               bool earlyExit);
    bool drawLuaTableLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                const std::vector<scripting::ControlSnapshot>& controls,
                                std::string_view stableId,
                                std::size_t& widgetIndex,
                                bool earlyExit);
    bool drawLuaGroupLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                const std::vector<scripting::ControlSnapshot>& controls,
                                std::string_view stableId,
                                std::size_t& widgetIndex,
                                bool earlyExit);
    bool drawLuaCollapseLayoutNode(const scripting::LayoutNodeDescriptor& node,
                                   const std::vector<scripting::ControlSnapshot>& controls,
                                   std::string_view stableId,
                                   std::size_t& widgetIndex,
                                   bool earlyExit);
    bool drawDynamicControl(const scripting::ControlSnapshot& control);
    bool drawDynamicLayoutControl(const scripting::ControlSnapshot& control, float layoutWidth);
    bool drawDynamicControl(const scripting::ControlSnapshot& control, std::optional<float> layoutWidth);
    bool drawDynamicButtonControl(const scripting::ControlSnapshot& control,
                                  const std::string& imguiLabel,
                                  std::optional<float> layoutWidth = std::nullopt);
    bool drawDynamicCheckboxControl(const scripting::ControlSnapshot& control, const std::string& inputLabel);
    bool drawDynamicTextControl(const scripting::ControlSnapshot& control, const std::string& inputLabel);
    bool drawDynamicComboControl(const scripting::ControlSnapshot& control, const std::string& inputLabel);
    bool drawDynamicElfSymbolComboControl(const scripting::ControlSnapshot& control, const std::string& inputLabel);
    bool drawDynamicIntControl(const scripting::ControlSnapshot& control, const std::string& inputLabel);
    bool drawDynamicFloatControl(const scripting::ControlSnapshot& control, const std::string& inputLabel);
    bool drawValueTableControl(const scripting::ControlSnapshot& control);
    void updateLuaDockDefaultLayout();
    void requestProtocolWorkspaceSwitch(std::string protocolDir, bool forceReload);
    void processPendingProtocolWorkspaceSwitch();
    bool switchProtocolWorkspace(const std::string& protocolDir, bool forceReload);
    [[nodiscard]] bool isSameProtocolWorkspace(const std::string& requestedDir) const;
    void resetLuaDefaultDockStateForProtocolSwitch();
    bool reloadProtocolWorkspace(const std::string& protocolDir, bool forceReload, bool sameProtocol);
    void loadProtocolWorkspaceAfterReload(bool sameProtocol);
    void loadCurrentProtocolWorkspace();
    void beginProtocolWorkspaceLoad(const LuaDockLayoutPaths& layoutPaths);
    void loadProtocolWorkspaceLayoutIni(const LuaDockLayoutPaths& layoutPaths);
    void saveCurrentProtocolWorkspace();
    void resetCurrentProtocolWorkspaceLayout();
    void loadCurrentProtocolControlState();
    void resetProtocolControlLoadDefaults();
    void useDefaultProtocolControlState();
    void reportRecoveredProtocolStateBackup(const ProtocolStateFileRecovery& recovery, std::string_view messagePrefix);
    void restoreProtocolWorkspaceState(const YAML::Node& root, const YAML::Node& protocolNode);
    void restorePersistedControlValues(const YAML::Node& controlsNode);
    void saveCurrentProtocolControlState();
    ProtocolDockVisibilityState captureCurrentDockVisibilityState() const;
    YAML::Node buildPersistedControlState() const;
    void storeCurrentProtocolState(YAML::Node& root, YAML::Node& protocolNode);
    void pruneCurrentLuaDockSettings();
    bool isLuaDockVisible(std::string_view stableId) const;
    bool setLuaDockVisible(std::string_view stableId, bool visible);
    void syncLuaDockVisibilityDefaults();
    void openRawCaptureImportDialog();
    void openRawCaptureExportDialog();
    void openRawCaptureRecordingDialog();
    void openRawCaptureReplayTimelineDialog();
    void openSessionPackageImportDialog();
    void openSessionPackageExportDialog();
    void openWaveAnalysisExportDialog();
    void openTransferLogExportDialog();
    void openHostLogExportDialog();
    void openScriptLogExportDialog();
    void openRequestTraceExportDialog();
    void openLogExportDialog(LogExportTarget target);
    void openElfStaticAddressDialog();
    void importRawCaptureFromPath(const std::filesystem::path& path);
    void exportRawCaptureToPath(const std::filesystem::path& path);
    void loadRawCaptureReplayTimelineFromPath(const std::filesystem::path& path);
    void startRawCaptureRecordingToPath(const std::filesystem::path& path);
    void importSessionPackageFromPath(const std::filesystem::path& path);
    void exportSessionPackageToPath(const std::filesystem::path& path);
    void exportWaveAnalysisReportToPath(const std::filesystem::path& path);
    void drawLogExportFileDialog();
    void drawRequestTraceExportFileDialog();
    std::vector<dock::ReceiveRow> logExportRows(LogExportTarget target);
    bool exportLogTargetToPath(LogExportTarget target, const std::filesystem::path& path);
    bool exportLogRowsToPath(const std::filesystem::path& path,
                             std::span<const dock::ReceiveRow> rows,
                             bool showTimestamps,
                             bool showHex,
                             std::string_view title);
    std::vector<dock::RequestTraceRow> requestTraceExportRows();
    bool exportRequestTraceToPath(const std::filesystem::path& path);
    bool exportRequestTraceRowsToPath(const std::filesystem::path& path,
                                      std::span<const dock::RequestTraceRow> rows,
                                      bool showTimestamps);
    const FilteredLogRowsCache& filteredLogRowsCached(FilteredLogRowsCache& cache,
                                                      const std::deque<dock::ReceiveRow>& rows,
                                                      std::uint64_t version,
                                                      const dock::LogFilterState& filter,
                                                      bool includeBytePreview);
    void loadElfStaticAddressFromPath(const std::filesystem::path& path);
    bool loadElfStaticAddressFromPath(const std::filesystem::path& path,
                                      bool clearLoadedContextOnFailure,
                                      bool saveProtocolStateOnSuccess);
    void clearElfStaticAddressContext(bool clearDialogPath);
    void restoreElfStaticAddressForCurrentProtocol(const std::string& savedPath);
    void drawElfStaticAddressDialog();
    void refreshWindowTitle();
    void requestAboutDialog();
    void drawAboutDialog();
    void requestShortcutHelpDialog();
    void drawShortcutHelpDialog();
    void startUpdateCheck();
    void drawUpdateCheckDialog();
    std::filesystem::path currentProtocolLayoutPath() const;
    std::filesystem::path legacyProtocolLayoutPath() const;
    std::filesystem::path protocolControlStatePath() const;
    struct ElfSymbolComboUiState;
    void seedElfSymbolComboDraft(ElfSymbolComboUiState& state, const scripting::ElfSymbolValue& current) const;
    void refreshElfSymbolComboOptionsIfNeeded(const scripting::ControlDescriptor& descriptor,
                                              ElfSymbolComboUiState& state,
                                              std::uint64_t currentMs);
    std::vector<std::string> elfSymbolComboLabels(const ElfSymbolComboUiState& state) const;
    bool commitElfSymbolComboSelection(const scripting::ControlDescriptor& descriptor,
                                       const ElfSymbolComboUiState& state,
                                       const std::string& selectedLabel);

    bool reloadConfigFromDisk();
    bool pollConfigFileChanges();
    bool pollElfStaticAddressFileChanges();
    bool maybeAutoSave();
    void sleepUntilNextFrame(std::uint64_t frameStartMs) const;
    void sleepUntil(std::uint64_t targetMs) const;

    static std::uint64_t nowMs();
    static std::string formatTimestamp(std::uint64_t timestampMs);

    app::Application& application_;
    const config::ConfigStore& configStore_;
    GLFWwindow* window_{nullptr};
    std::unique_ptr<WorkspaceController> workspaceController_;
    GuiRuntimeState runtimeState_{};
    std::vector<std::unique_ptr<IUiComponent>> uiComponents_;
    std::vector<IMenuContributor*> menuContributors_;
    std::vector<IDialogComponent*> dialogComponents_;
    std::vector<IDockComponent*> dockComponents_;
    std::uint64_t lastRenderAtMs_{0};
    std::uint64_t lastPumpMs_{0};
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
    bool showRequestTraceDock_{true};
    bool showOfflineReplayDock_{true};
    bool showLogDock_{true};
    bool showScriptDock_{true};
    bool showWaveDock_{true};
    bool waveFullscreenActive_{false};
    bool waveFullscreenToggleRequested_{false};
    config::GuiWaveFullscreenMode waveFullscreenActiveMode_{config::GuiWaveFullscreenMode::Overlay};
    std::optional<WaveFullscreenDockSnapshot> waveFullscreenSnapshot_;
    FilteredLogRowsCache transferLogRowsCache_;
    FilteredLogRowsCache hostLogRowsCache_;
    FilteredLogRowsCache scriptLogRowsCache_;
    std::unordered_map<std::string, bool> luaDockVisibility_;
    float transferSendSectionHeight_{210.0F};
    bool aboutDialogRequested_{false};
    bool shortcutHelpDialogRequested_{false};
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
    bool rawCaptureReplayTimelineDialogOpen_{false};
    bool rawCaptureReplayTimelineDialogOpened_{false};
    std::string rawCaptureReplayTimelinePath_;
    std::string rawCaptureReplayTimelineError_;
    bool rawCaptureExportDialogOpen_{false};
    bool rawCaptureExportDialogOpened_{false};
    std::string rawCaptureExportPath_;
    std::string rawCaptureExportError_;
    bool rawCaptureRecordingDialogOpen_{false};
    bool rawCaptureRecordingDialogOpened_{false};
    std::string rawCaptureRecordingPath_;
    std::string rawCaptureRecordingError_;
    bool sessionPackageImportDialogOpen_{false};
    bool sessionPackageImportDialogOpened_{false};
    std::string sessionPackageImportPath_;
    std::string sessionPackageImportError_;
    bool sessionPackageExportDialogOpen_{false};
    bool sessionPackageExportDialogOpened_{false};
    std::string sessionPackageExportPath_;
    std::string sessionPackageExportError_;
    bool logExportDialogOpen_{false};
    bool logExportDialogOpened_{false};
    LogExportTarget logExportTarget_{LogExportTarget::Transfer};
    std::string logExportPath_;
    std::string logExportError_;
    std::string logExportDialogTitle_;
    bool requestTraceExportDialogOpen_{false};
    bool requestTraceExportDialogOpened_{false};
    std::string requestTraceExportPath_;
    std::string requestTraceExportError_;
    bool elfStaticAddressDialogOpen_{false};
    bool elfStaticAddressDialogOpened_{false};
    std::string elfStaticAddressPath_;
    std::string elfStaticAddressError_;
    ElfStaticAddressFileWatchState elfStaticAddressWatch_;

    struct ElfSymbolComboUiState {
        std::string draft;
        std::string queriedDraft;
        std::uint64_t editedAtMs{0};
        std::uint64_t loadedRevision{0};
        std::size_t queriedLimit{0};
        std::vector<scripting::ElfSymbolValue> options;
    };

    std::unordered_map<std::string, ElfSymbolComboUiState> elfSymbolComboStates_;
    WaveDockRenderer waveDockRenderer_;
};

} // namespace protoscope::ui
