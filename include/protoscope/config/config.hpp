#pragma once

#include "protoscope/dock/docks.hpp"
#include "protoscope/scripting/file_io_config.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace protoscope::config {

struct AppAutoSaveConfig {
    bool enabled{false};
    std::uint64_t intervalMs{5000};
};

struct AppConfigHotReloadConfig {
    bool enabled{false};
};

struct PerformanceExplicitOverrides {
    bool receiveTransportReadBufferBytes{false};
    bool workerRxQueueLimitBytes{false};
    bool workerMemoryBudgetBytes{false};
    bool workerOutputQueueLimit{false};
    bool workerBatchBytes{false};
    bool workerOutputFlushBudgetMs{false};
    bool realtimeBacklogRxChunkBytesPerPump{false};
    bool realtimeBacklogTransferFrameRowsPerPump{false};
    bool realtimeBacklogPlotAppendsPerPump{false};
    bool realtimeBacklogRawFirstBacklogWarnBytes{false};
};

struct AdaptivePerformanceConfig {
    bool enabled{false};
    double maxMultiplier{1.0};
};

struct PerformanceConfig {
    double scale{1.0};
    AdaptivePerformanceConfig adaptive{};
    PerformanceExplicitOverrides explicitOverrides{};
};

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

struct AppLoggingPayloadPreviewConfig {
    bool enabled{false};
    std::size_t maxBytes{64};
};

struct AppLoggingConfig {
    LogLevel level{LogLevel::Info};
    std::string filePath;
    AppLoggingPayloadPreviewConfig payloadPreview{};
    std::size_t maxFileSizeBytes{5U * 1024U * 1024U};
    std::size_t maxFiles{3};
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

enum class GuiRendererBackend {
    OpenGL,
    D3D11,
    D3D11Warp,
};

enum class GuiWaveFullscreenMode {
    Focus,
    Overlay,
};

enum class GuiTheme {
    ProfessionalDark,
    DebugHighContrast,
};

struct GuiWaveResetViewportConfig {
    bool applyOnPlotSetupReset{true};
    bool applyOnManualClear{true};
    bool applyOnRawImport{true};
    plot::WaveResetViewportScaleMode xScale{plot::WaveResetViewportScaleMode::Preserve};
    plot::WaveResetViewportScaleMode yScale{plot::WaveResetViewportScaleMode::Preserve};
    plot::WaveResetViewportAnchor xAnchor{plot::WaveResetViewportAnchor::WaveStart};
    plot::WaveResetViewportAutoFollowMode autoFollow{plot::WaveResetViewportAutoFollowMode::Existing};
};

struct GuiWaveConfig {
    plot::WaveControlMode controlMode{plot::WaveControlMode::Oscilloscope};
    plot::WaveDisplayFormula displayFormula{plot::WaveDisplayFormula::OffsetThenScale};
    plot::WaveGridDivisionReadoutMode gridDivisionReadoutMode{plot::WaveGridDivisionReadoutMode::DisplayValue};
    bool channelScaleWheelEnabled{true};
    plot::WaveChannelScaleWheelAcceleration channelScaleWheelAcceleration{plot::WaveChannelScaleWheelAcceleration::Log};
    plot::WaveChannelCardWidthMode channelCardWidthMode{plot::WaveChannelCardWidthMode::Fixed};
    plot::WaveChannelDoubleClickAction channelDoubleClickAction{plot::WaveChannelDoubleClickAction::ResetScaleOffset};
    plot::WaveXAxisDoubleClickAction xAxisDoubleClickAction{plot::WaveXAxisDoubleClickAction::FitFullHistory};
    plot::WaveYAxisDoubleClickAction yAxisDoubleClickAction{plot::WaveYAxisDoubleClickAction::FitVisibleChannels};
    bool yAxisDoubleClickAdjustOffset{false};
    plot::WaveHiddenChannelPolicy hiddenChannelPolicy{plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews};
    plot::WaveCursorExtremeSnapPolicy cursorExtremeSnapPolicy{plot::WaveCursorExtremeSnapPolicy::NearestWaveform};
    plot::WaveMouseYOffsetDragMode mouseYOffsetDragMode{plot::WaveMouseYOffsetDragMode::Direct};
    plot::WaveLegendOverlayOpenMode legendOverlayOpenMode{plot::WaveLegendOverlayOpenMode::Hover};
    bool legendOverlayDoubleClickAutoCollapse{true};
    bool interactionAnimationEnabled{true};
    bool zoomSelectionAutoExit{false};
    bool peakDetectDownsample{true};
    std::size_t maxRenderPointsPerChannel{1200};
    std::size_t maxRenderVertices{60000};
    double downsampleStartMultiplier{2.0};
    std::size_t overviewMaxSamples{20000};
    double minVisibleTimeSpan{0.001};
    std::size_t maxTotalSamples{0};
    double channelCardFixedWidth{128.0};
    double channelCardAdaptiveRatio{0.22};
    double legendChannelNameMaxWidth{0.0};
    double verticalAutoFitMultiplier{1.25};
    bool resetHistoryOnTimeReset{true};
    bool showAxisLabels{false};
    bool showChannelLegend{true};
    bool showFftLegend{true};
    bool followMeasurementCursorsOnScroll{false};
    std::array<float, 4> cursorFftHighlightRgba{0.20F, 0.55F, 1.00F, 0.16F};
    GuiWaveFullscreenMode fullscreenMode{GuiWaveFullscreenMode::Overlay};
    GuiWaveResetViewportConfig resetViewport{};
};

struct GuiLogHistoryConfig {
    std::size_t transferRawLimit{10000};
    std::size_t transferFrameLimit{120000};
    std::size_t hostLimit{5000};
    std::size_t scriptLimit{5000};
    std::size_t requestTraceLimit{5000};
};

struct GuiRawCaptureConfig {
    std::size_t liveLimitBytes{64U * 1024U * 1024U};
    std::size_t recordingQueueLimitBytes{256U * 1024U * 1024U};
};

struct GuiRealtimeBacklogConfig {
    std::string mode{"responsive"};
    std::size_t rxChunkBytesPerPump{4096U};
    std::size_t transferFrameRowsPerPump{2000};
    std::size_t plotAppendsPerPump{128};
    std::size_t rawFirstBacklogWarnBytes{32U * 1024U * 1024U};
    bool derivedBacklogDegradeEnabled{true};
    bool discardBacklogOnDisconnect{false};
    double pumpMinIntervalMs{1.0};
};

struct GuiElfSymbolComboConfig {
    std::size_t limit{10};
    int debounceMs{300};
    bool autoRefreshSelectedAddress{true};
    bool autoRefreshEmitOnControl{false};
};

enum class GuiFontChineseGlyphRange {
    SimplifiedCommon,
    Full,
};

struct GuiFontConfig {
    GuiFontChineseGlyphRange chineseGlyphRange{GuiFontChineseGlyphRange::SimplifiedCommon};
};

struct GuiInteractionFeedbackConfig {
    bool enabled{true};
    std::uint64_t statusDurationMs{2000};
};

struct GuiConfig {
    GuiTheme theme{GuiTheme::ProfessionalDark};
    GuiWindowConfig window{};
    GuiRendererBackend rendererBackend{GuiRendererBackend::OpenGL};
    GuiInteractionFeedbackConfig interactionFeedback{};
    GuiWaveConfig wave{};
    GuiFontConfig font{};
    GuiLogHistoryConfig logHistory{};
    GuiRawCaptureConfig rawCapture{};
    GuiRealtimeBacklogConfig realtimeBacklog{};
    GuiElfSymbolComboConfig elfSymbolCombo{};
    bool showAppHeader{false};
    bool luaDockLayoutDebug{false};
    bool luaDockRenderCopyMode{true};
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
    std::size_t transportReadBufferBytes{4096U};
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
    std::size_t workerBatchBytes{8192U};
    bool workerBackpressureEnabled{true};
    double workerBackpressureHighWatermark{0.5};
    double workerBackpressureLowWatermark{0.3};
    double workerOutputFlushBudgetMs{2.0};
    /// 超时前是否使用无帧预算限制的 drain（默认关闭）；开启时采用 flushScriptOutputsUnbounded()
    bool drainRequestOutputsUnbounded{false};
};

struct AppConfig {
    PerformanceConfig performance{};
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
    ConfigLoadResult loadText(std::string_view yamlText, const std::filesystem::path& sourcePath = {}) const;
    bool save(const std::filesystem::path& path, const AppConfig& config, std::string& error) const;
    bool saveText(const AppConfig& config, std::string& yamlText, std::string& error) const;

    std::filesystem::path normalizeProtocolDir(const std::filesystem::path& dir) const;
    std::filesystem::path normalizeProtocolDir(const std::filesystem::path& rootDir,
                                               const std::filesystem::path& dir) const;
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

std::optional<GuiRendererBackend> parseGuiRendererBackend(std::string_view value);
std::string_view guiRendererBackendId(GuiRendererBackend backend);
std::string_view guiThemeId(GuiTheme theme);

} // namespace protoscope::config
