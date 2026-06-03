#pragma once

#include "protoscope/dock/docks.hpp"
#include "protoscope/scripting/file_io_config.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace protoscope::config {

struct AppAutoSaveConfig {
    bool enabled{false};
    std::uint64_t intervalMs{5000};
};

struct AppConfigHotReloadConfig {
    bool enabled{false};
};

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

struct AppLoggingConfig {
    LogLevel level{LogLevel::Info};
    std::string filePath;
};

struct AppRuntimeConfig {
    std::string language{"zh-CN"};
    std::uint32_t fpsLimit{60};
    std::string idleRender{"dirty_only"};
    AppAutoSaveConfig autoSave{};
    AppConfigHotReloadConfig configHotReload{};
};

struct GuiWindowConfig {
    std::string title{"ProtoScope"};
    int width{1600};
    int height{900};
    bool maximized{false};
};

struct GuiWaveConfig {
    plot::WaveControlMode controlMode{plot::WaveControlMode::Oscilloscope};
    plot::WaveDisplayFormula displayFormula{plot::WaveDisplayFormula::OffsetThenScale};
    plot::WaveChannelCardWidthMode channelCardWidthMode{plot::WaveChannelCardWidthMode::Fixed};
    plot::WaveChannelDoubleClickAction channelDoubleClickAction{plot::WaveChannelDoubleClickAction::ResetScaleOffset};
    plot::WaveHiddenChannelPolicy hiddenChannelPolicy{plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews};
    bool zoomSelectionAutoExit{false};
    std::size_t maxRenderPointsPerChannel{1200};
    std::size_t maxRenderVertices{60000};
    double downsampleStartMultiplier{2.0};
    std::size_t overviewMaxSamples{20000};
    double minVisibleTimeSpan{0.001};
    std::size_t maxTotalSamples{0};
    double channelCardFixedWidth{128.0};
    double channelCardAdaptiveRatio{0.22};
    double verticalAutoFitMultiplier{1.2};
    bool showAxisLabels{false};
    bool showChannelLegend{true};
    bool showFftLegend{true};
};

struct GuiLogHistoryConfig {
    std::size_t transferRawLimit{10000};
    std::size_t transferFrameLimit{120000};
    std::size_t hostLimit{5000};
    std::size_t scriptLimit{5000};
};

struct GuiRawCaptureConfig {
    std::size_t liveLimitBytes{64U * 1024U * 1024U};
    std::size_t recordingQueueLimitBytes{256U * 1024U * 1024U};
};

struct GuiRealtimeBacklogConfig {
    std::string mode{"responsive"};
    std::size_t rxChunkBytesPerPump{64U * 1024U};
    std::size_t transferFrameRowsPerPump{2000};
    std::size_t plotAppendsPerPump{4096};
    std::size_t rawFirstBacklogWarnBytes{32U * 1024U * 1024U};
    bool derivedBacklogDegradeEnabled{true};
    bool discardBacklogOnDisconnect{false};
    double pumpMinIntervalMs{2.0};
};

struct GuiElfSymbolComboConfig {
    std::size_t limit{10};
    int debounceMs{300};
};

struct GuiConfig {
    GuiWindowConfig window{};
    GuiWaveConfig wave{};
    GuiLogHistoryConfig logHistory{};
    GuiRawCaptureConfig rawCapture{};
    GuiRealtimeBacklogConfig realtimeBacklog{};
    GuiElfSymbolComboConfig elfSymbolCombo{};
    bool showAppHeader{false};
    bool luaDockLayoutDebug{false};
    std::size_t sendHistoryLimit{20};
    bool replayRawHistoryOnSchemaSwitch{false};
};

struct ProtocolTxConfig {
    std::uint64_t sendTimeoutMs{1000};
    std::uint64_t requestTimeoutMs{1000};
    std::size_t maxPending{64};
    std::string overflowPolicy{"reject_new"};
    std::string overflowNotify{"popup_once"};
};

struct ProtocolConfig {
    std::string rootDir{"protocols/templates"};
    std::string selectedDir{"protocols/templates/default_protocol"};
    ProtocolTxConfig tx{};
};

struct ReceiveStreamBufferConfig {
    double nearOverflowThreshold{0.8};
    bool popupEnabled{true};
};

struct ReceiveConfig {
    ReceiveStreamBufferConfig streamBuffer{};
    std::size_t transportReadBufferBytes{64U * 1024U};
};

struct ScriptingPipelineConfig {
    std::optional<std::size_t> workerThreads{};
};

struct ScriptingConfig {
    scripting::FileIoConfig fileIo{};
    ScriptingPipelineConfig pipeline{};
    bool workerEnabled{true};
    std::size_t workerRxQueueLimitBytes{64U * 1024U * 1024U};
    std::size_t workerMemoryBudgetBytes{256U * 1024U * 1024U};
    double workerMemoryBudgetAvailableRatio{0.0};
    std::size_t workerOutputQueueLimit{65536U};
    std::size_t workerBatchBytes{256U * 1024U};
    bool workerBackpressureEnabled{true};
    double workerBackpressureHighWatermark{0.5};
    double workerBackpressureLowWatermark{0.3};
    double workerOutputFlushBudgetMs{4.0};
};

struct AppConfig {
    AppRuntimeConfig app{};
    GuiConfig gui{};
    ProtocolConfig protocol{};
    ReceiveConfig receive{};
    ScriptingConfig scripting{};
    AppLoggingConfig logging{};
    dock::CommDockState communication{};
    std::string configPath{"config/protoscope.yaml"};
};

struct ConfigLoadResult {
    AppConfig config{};
    std::filesystem::path resolvedPath;
    bool loadedFromDisk{false};
    std::string error;
};

struct FileSnapshot {
    std::filesystem::path path;
    std::uint64_t timestampMs{0};
    bool exists{false};
};

class ConfigStore {
public:
    ConfigStore();

    ConfigLoadResult load(const std::filesystem::path& path) const;
    bool save(const std::filesystem::path& path, const AppConfig& config, std::string& error) const;

    std::filesystem::path normalizeProtocolDir(const std::filesystem::path& dir) const;
    std::filesystem::path normalizeProtocolDir(const std::filesystem::path& rootDir, const std::filesystem::path& dir) const;
    std::filesystem::path mainLuaPath(const std::filesystem::path& protocolDir) const;
    std::string protocolName(const std::filesystem::path& protocolDir) const;
    bool protocolEntryExists(const std::filesystem::path& protocolDir) const;
    std::vector<std::string> scanProtocolDirectories(const std::filesystem::path& rootDir) const;
    bool ensureDefaultProtocolScript(const std::filesystem::path& protocolDir, std::string& error) const;
    bool ensureDefaultProtocolWorkspace(std::string& error) const;

    FileSnapshot snapshot(const std::filesystem::path& path) const;
    bool hasChanged(const FileSnapshot& previous) const;

    void applyToDock(const AppConfig& config, dock::DockStore& dockStore) const;
    AppConfig captureFromDock(const dock::DockStore& dockStore) const;

    const std::filesystem::path& defaultConfigPath() const;
    const std::filesystem::path& defaultProtocolDir() const;

private:
    AppConfig withDefaults() const;
    static std::uint64_t toTimestampMs(const std::filesystem::file_time_type& fileTime);

private:
    std::filesystem::path defaultConfigPath_;
    std::filesystem::path defaultProtocolRootDir_;
    std::filesystem::path defaultProtocolDir_;
};

} // namespace protoscope::config
