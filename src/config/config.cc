#include "protoscope/config/config.hpp"

#include "protoscope/config/embedded_protocols.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <system_error>

#include <yaml-cpp/yaml.h>

namespace protoscope::config {

namespace {

    template <typename T> T readScalar(const YAML::Node& node, const char* key, T fallback)
    {
        if (!node || !node[key]) {
            return fallback;
        }
        return node[key].as<T>();
    }

    std::vector<std::string> readStringList(const YAML::Node& node, const char* key, std::vector<std::string> fallback)
    {
        if (!node || !node[key] || !node[key].IsSequence()) {
            return fallback;
        }

        std::vector<std::string> values;
        for (const auto& item : node[key]) {
            values.push_back(item.as<std::string>());
        }
        return values;
    }

    bool hasScalar(const YAML::Node& node, const char* key)
    {
        return node && node[key];
    }

    YAML::Node childNode(const YAML::Node& node, const char* key)
    {
        if (!node) {
            return {};
        }
        return node[key];
    }

    double normalizePerformanceScale(const double scale)
    {
        return scale > 0.0 ? scale : 1.0;
    }

    std::size_t scaleIntegerBudget(const std::size_t fallback, const double scale)
    {
        if (fallback == 0U) {
            return 0U;
        }
        const auto scaled = std::round(static_cast<double>(fallback) * normalizePerformanceScale(scale));
        if (scaled < 1.0) {
            return 1U;
        }
        const auto maxValue = static_cast<double>((std::numeric_limits<std::size_t>::max)());
        if (scaled > maxValue) {
            return (std::numeric_limits<std::size_t>::max)();
        }
        return static_cast<std::size_t>(scaled);
    }

    double scaleDoubleBudget(const double fallback, const double scale)
    {
        if (fallback == 0.0) {
            return 0.0;
        }
        return fallback * normalizePerformanceScale(scale);
    }

    void applyPerformanceScale(AppConfig& config)
    {
        const auto scale = normalizePerformanceScale(config.performance.scale);
        config.performance.scale = scale;

        // 核心流程：公共性能系数只缩放缺省预算；YAML 显式单项会在后续读取中覆盖这些值。
        config.receive.transportReadBufferBytes = scaleIntegerBudget(config.receive.transportReadBufferBytes, scale);
        config.scripting.workerRxQueueLimitBytes = scaleIntegerBudget(config.scripting.workerRxQueueLimitBytes, scale);
        config.scripting.workerMemoryBudgetBytes = scaleIntegerBudget(config.scripting.workerMemoryBudgetBytes, scale);
        config.scripting.workerOutputQueueLimit = scaleIntegerBudget(config.scripting.workerOutputQueueLimit, scale);
        config.scripting.workerBatchBytes = scaleIntegerBudget(config.scripting.workerBatchBytes, scale);
        config.scripting.workerOutputFlushBudgetMs =
            scaleDoubleBudget(config.scripting.workerOutputFlushBudgetMs, scale);
        config.gui.realtimeBacklog.rxChunkBytesPerPump =
            scaleIntegerBudget(config.gui.realtimeBacklog.rxChunkBytesPerPump, scale);
        config.gui.realtimeBacklog.transferFrameRowsPerPump =
            scaleIntegerBudget(config.gui.realtimeBacklog.transferFrameRowsPerPump, scale);
        config.gui.realtimeBacklog.plotAppendsPerPump =
            scaleIntegerBudget(config.gui.realtimeBacklog.plotAppendsPerPump, scale);
        config.gui.realtimeBacklog.rawFirstBacklogWarnBytes =
            scaleIntegerBudget(config.gui.realtimeBacklog.rawFirstBacklogWarnBytes, scale);
    }

    void readPerformanceSize(const YAML::Node& node, const char* key, std::size_t& value, bool& explicitOverride)
    {
        if (!hasScalar(node, key)) {
            return;
        }
        value = node[key].as<std::size_t>();
        explicitOverride = true;
    }

    void readPerformanceDouble(const YAML::Node& node, const char* key, double& value, bool& explicitOverride)
    {
        if (!hasScalar(node, key)) {
            return;
        }
        value = node[key].as<double>();
        explicitOverride = true;
    }

    bool performanceValueChanged(const std::size_t value, const std::size_t scaledDefault)
    {
        return value != scaledDefault;
    }

    bool performanceValueChanged(const double value, const double scaledDefault)
    {
        return std::abs(value - scaledDefault) > 1e-12;
    }

    template <typename T>
    void writePerformanceScalar(
        YAML::Node node, const char* key, const T value, const T scaledDefault, const bool explicitOverride)
    {
        if (explicitOverride || performanceValueChanged(value, scaledDefault)) {
            node[key] = value;
        }
    }

    std::string normalizeTextPath(std::filesystem::path path)
    {
        path.make_preferred();
        return path.generic_string();
    }

    LogLevel parseLogLevel(const std::string& value)
    {
        if (value == "debug") {
            return LogLevel::Debug;
        }
        if (value == "warn" || value == "warning") {
            return LogLevel::Warn;
        }
        if (value == "error") {
            return LogLevel::Error;
        }
        return LogLevel::Info;
    }

    std::string toLogLevelText(const LogLevel level)
    {
        switch (level) {
            case LogLevel::Debug:
                return "debug";
            case LogLevel::Info:
                return "info";
            case LogLevel::Warn:
                return "warn";
            case LogLevel::Error:
                return "error";
        }
        return "info";
    }

    plot::WaveControlMode parseWaveControlMode(const std::string& value, plot::WaveControlMode fallback)
    {
        if (value == "legacy_global") {
            return plot::WaveControlMode::LegacyGlobal;
        }
        if (value == "oscilloscope") {
            return plot::WaveControlMode::Oscilloscope;
        }
        return fallback;
    }

    const char* toWaveControlModeText(const plot::WaveControlMode mode)
    {
        switch (mode) {
            case plot::WaveControlMode::Oscilloscope:
                return "oscilloscope";
            case plot::WaveControlMode::LegacyGlobal:
                return "legacy_global";
        }
        return "oscilloscope";
    }

    plot::WaveDisplayFormula parseWaveDisplayFormula(const std::string& value, plot::WaveDisplayFormula fallback)
    {
        if (value == "scale_then_offset") {
            return plot::WaveDisplayFormula::ScaleThenOffset;
        }
        if (value == "offset_then_scale") {
            return plot::WaveDisplayFormula::OffsetThenScale;
        }
        return fallback;
    }

    const char* toWaveDisplayFormulaText(const plot::WaveDisplayFormula formula)
    {
        switch (formula) {
            case plot::WaveDisplayFormula::OffsetThenScale:
                return "offset_then_scale";
            case plot::WaveDisplayFormula::ScaleThenOffset:
                return "scale_then_offset";
        }
        return "offset_then_scale";
    }

    plot::WaveChannelCardWidthMode parseWaveChannelCardWidthMode(const std::string& value)
    {
        if (value == "adaptive") {
            return plot::WaveChannelCardWidthMode::Adaptive;
        }
        return plot::WaveChannelCardWidthMode::Fixed;
    }

    const char* toWaveChannelCardWidthModeText(const plot::WaveChannelCardWidthMode mode)
    {
        switch (mode) {
            case plot::WaveChannelCardWidthMode::Fixed:
                return "fixed";
            case plot::WaveChannelCardWidthMode::Adaptive:
                return "adaptive";
        }
        return "fixed";
    }

    plot::WaveChannelDoubleClickAction parseWaveChannelDoubleClickAction(const std::string& value,
                                                                         plot::WaveChannelDoubleClickAction fallback)
    {
        if (value == "reset_all") {
            return plot::WaveChannelDoubleClickAction::ResetAll;
        }
        if (value == "reset_scale_offset") {
            return plot::WaveChannelDoubleClickAction::ResetScaleOffset;
        }
        if (value == "reset_scale") {
            return plot::WaveChannelDoubleClickAction::ResetScale;
        }
        if (value == "reset_offset") {
            return plot::WaveChannelDoubleClickAction::ResetOffset;
        }
        return fallback;
    }

    const char* toWaveChannelDoubleClickActionText(const plot::WaveChannelDoubleClickAction action)
    {
        switch (action) {
            case plot::WaveChannelDoubleClickAction::ResetAll:
                return "reset_all";
            case plot::WaveChannelDoubleClickAction::ResetScaleOffset:
                return "reset_scale_offset";
            case plot::WaveChannelDoubleClickAction::ResetScale:
                return "reset_scale";
            case plot::WaveChannelDoubleClickAction::ResetOffset:
                return "reset_offset";
        }
        return "reset_scale_offset";
    }

    plot::WaveXAxisDoubleClickAction parseWaveXAxisDoubleClickAction(const std::string& value,
                                                                     plot::WaveXAxisDoubleClickAction fallback)
    {
        if (value == "fit_full_history") {
            return plot::WaveXAxisDoubleClickAction::FitFullHistory;
        }
        if (value == "fit_visible_window") {
            return plot::WaveXAxisDoubleClickAction::FitVisibleWindow;
        }
        return fallback;
    }

    const char* toWaveXAxisDoubleClickActionText(const plot::WaveXAxisDoubleClickAction action)
    {
        switch (action) {
            case plot::WaveXAxisDoubleClickAction::FitFullHistory:
                return "fit_full_history";
            case plot::WaveXAxisDoubleClickAction::FitVisibleWindow:
                return "fit_visible_window";
        }
        return "fit_full_history";
    }

    plot::WaveHiddenChannelPolicy parseWaveHiddenChannelPolicy(const std::string& value,
                                                               plot::WaveHiddenChannelPolicy fallback)
    {
        if (value == "include_hidden") {
            return plot::WaveHiddenChannelPolicy::IncludeInDerivedViews;
        }
        if (value == "visible_only") {
            return plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews;
        }
        return fallback;
    }

    const char* toWaveHiddenChannelPolicyText(const plot::WaveHiddenChannelPolicy policy)
    {
        switch (policy) {
            case plot::WaveHiddenChannelPolicy::IncludeInDerivedViews:
                return "include_hidden";
            case plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews:
                return "visible_only";
        }
        return "include_hidden";
    }

    plot::WaveCursorExtremeSnapPolicy parseWaveCursorExtremeSnapPolicy(const std::string& value,
                                                                       plot::WaveCursorExtremeSnapPolicy fallback)
    {
        if (value == "viewport_zone") {
            return plot::WaveCursorExtremeSnapPolicy::ViewportZone;
        }
        if (value == "nearest_waveform") {
            return plot::WaveCursorExtremeSnapPolicy::NearestWaveform;
        }
        return fallback;
    }

    const char* toWaveCursorExtremeSnapPolicyText(const plot::WaveCursorExtremeSnapPolicy policy)
    {
        switch (policy) {
            case plot::WaveCursorExtremeSnapPolicy::NearestWaveform:
                return "nearest_waveform";
            case plot::WaveCursorExtremeSnapPolicy::ViewportZone:
                return "viewport_zone";
        }
        return "nearest_waveform";
    }

    GuiWaveFullscreenMode parseWaveFullscreenMode(const std::string& value, GuiWaveFullscreenMode fallback)
    {
        if (value == "focus") {
            return GuiWaveFullscreenMode::Focus;
        }
        if (value == "overlay") {
            return GuiWaveFullscreenMode::Overlay;
        }
        return fallback;
    }

    const char* toWaveFullscreenModeText(const GuiWaveFullscreenMode mode)
    {
        switch (mode) {
            case GuiWaveFullscreenMode::Focus:
                return "focus";
            case GuiWaveFullscreenMode::Overlay:
                return "overlay";
        }
        return "focus";
    }

    double positiveOrFallback(double value, double fallback)
    {
        return value > 0.0 ? value : fallback;
    }

    transport::TransportKind parseTransportKind(const std::string& value)
    {
        if (const auto kind = transport::transportKindFromId(value)) {
            return *kind;
        }
        return transport::TransportKind::TcpClient;
    }

    std::string toTransportKindText(transport::TransportKind kind)
    {
        return std::string(transport::transportKindId(kind));
    }

    const std::vector<std::string> kDefaultSerialPorts = {"COM1", "COM2", "COM3", "COM4"};
} // namespace

ConfigStore::ConfigStore()
    : defaultConfigPath_(embedded::executableDirectory() / "config" / "protoscope.yaml"),
      defaultProtocolRootDir_(embedded::executableDirectory() / "protocols"),
      defaultProtocolDir_(defaultProtocolRootDir_ / "templates" / "default_protocol")
{
}

AppConfig ConfigStore::withDefaults() const
{
    AppConfig config;
    config.protocol.rootDir = normalizeTextPath(defaultProtocolDir_.parent_path());
    config.protocol.selectedDir = normalizeTextPath(defaultProtocolDir_);
    config.configPath = normalizeTextPath(defaultConfigPath_);
    config.communication.serialPortOptions = kDefaultSerialPorts;
    return config;
}

ConfigLoadResult ConfigStore::load(const std::filesystem::path& path) const
{
    ConfigLoadResult result;
    result.config = withDefaults();
    result.resolvedPath = path.empty() ? defaultConfigPath_ : path;

    std::error_code existsError;
    if (!std::filesystem::exists(result.resolvedPath, existsError)) {
        result.config.configPath = normalizeTextPath(result.resolvedPath);
        if (existsError) {
            result.error = "检查配置文件失败: " + existsError.message();
        }
        return result;
    }

    try {
        const YAML::Node root = YAML::LoadFile(result.resolvedPath.string());
        result.loadedFromDisk = true;

        if (const auto performance = root["performance"]) {
            result.config.performance.scale =
                normalizePerformanceScale(readScalar<double>(performance, "scale", result.config.performance.scale));
        }
        applyPerformanceScale(result.config);

        const auto app = root["app"];
        result.config.app.language = readScalar<std::string>(app, "language", result.config.app.language);
        result.config.app.fpsLimit = readScalar<std::uint32_t>(app, "fps_limit", result.config.app.fpsLimit);
        result.config.app.idleRender = readScalar<std::string>(app, "idle_render", result.config.app.idleRender);
        if (const auto autoSave = childNode(app, "auto_save")) {
            result.config.app.autoSave.enabled =
                readScalar<bool>(autoSave, "enabled", result.config.app.autoSave.enabled);
            result.config.app.autoSave.intervalMs =
                readScalar<std::uint64_t>(autoSave, "interval_ms", result.config.app.autoSave.intervalMs);
        }
        if (const auto configHotReload = childNode(app, "config_hot_reload")) {
            result.config.app.configHotReload.enabled =
                readScalar<bool>(configHotReload, "enabled", result.config.app.configHotReload.enabled);
        }

        const auto gui = root["gui"];
        if (const auto window = childNode(gui, "window")) {
            result.config.gui.window.title = readScalar<std::string>(window, "title", result.config.gui.window.title);
            result.config.gui.window.width = readScalar<int>(window, "width", result.config.gui.window.width);
            result.config.gui.window.height = readScalar<int>(window, "height", result.config.gui.window.height);
            result.config.gui.window.maximized =
                readScalar<bool>(window, "maximized", result.config.gui.window.maximized);
        }
        if (const auto wave = childNode(gui, "wave")) {
            result.config.gui.wave.controlMode = parseWaveControlMode(
                readScalar<std::string>(
                    wave, "control_mode", toWaveControlModeText(result.config.gui.wave.controlMode)),
                result.config.gui.wave.controlMode);
            result.config.gui.wave.displayFormula = parseWaveDisplayFormula(
                readScalar<std::string>(
                    wave, "display_formula", toWaveDisplayFormulaText(result.config.gui.wave.displayFormula)),
                result.config.gui.wave.displayFormula);
            result.config.gui.wave.channelCardWidthMode =
                parseWaveChannelCardWidthMode(readScalar<std::string>(wave, "channel_card_width_mode", "fixed"));
            result.config.gui.wave.channelDoubleClickAction = parseWaveChannelDoubleClickAction(
                readScalar<std::string>(
                    wave,
                    "channel_double_click_action",
                    toWaveChannelDoubleClickActionText(result.config.gui.wave.channelDoubleClickAction)),
                result.config.gui.wave.channelDoubleClickAction);
            result.config.gui.wave.xAxisDoubleClickAction = parseWaveXAxisDoubleClickAction(
                readScalar<std::string>(
                    wave,
                    "x_axis_double_click_action",
                    toWaveXAxisDoubleClickActionText(result.config.gui.wave.xAxisDoubleClickAction)),
                result.config.gui.wave.xAxisDoubleClickAction);
            result.config.gui.wave.hiddenChannelPolicy = parseWaveHiddenChannelPolicy(
                readScalar<std::string>(wave,
                                        "hidden_channel_policy",
                                        toWaveHiddenChannelPolicyText(result.config.gui.wave.hiddenChannelPolicy)),
                result.config.gui.wave.hiddenChannelPolicy);
            result.config.gui.wave.cursorExtremeSnapPolicy = parseWaveCursorExtremeSnapPolicy(
                readScalar<std::string>(
                    wave,
                    "cursor_extreme_snap_policy",
                    toWaveCursorExtremeSnapPolicyText(result.config.gui.wave.cursorExtremeSnapPolicy)),
                result.config.gui.wave.cursorExtremeSnapPolicy);
            result.config.gui.wave.zoomSelectionAutoExit =
                readScalar<bool>(wave, "zoom_selection_auto_exit", result.config.gui.wave.zoomSelectionAutoExit);
            result.config.gui.wave.maxRenderPointsPerChannel = readScalar<std::size_t>(
                wave, "max_render_points_per_channel", result.config.gui.wave.maxRenderPointsPerChannel);
            result.config.gui.wave.maxRenderVertices =
                readScalar<std::size_t>(wave, "max_render_vertices", result.config.gui.wave.maxRenderVertices);
            result.config.gui.wave.downsampleStartMultiplier = readScalar<double>(
                wave, "downsample_start_multiplier", result.config.gui.wave.downsampleStartMultiplier);
            result.config.gui.wave.overviewMaxSamples =
                readScalar<std::size_t>(wave, "overview_max_samples", result.config.gui.wave.overviewMaxSamples);
            result.config.gui.wave.maxTotalSamples =
                readScalar<std::size_t>(wave, "max_total_samples", result.config.gui.wave.maxTotalSamples);
            result.config.gui.wave.minVisibleTimeSpan =
                readScalar<double>(wave, "min_visible_time_span", result.config.gui.wave.minVisibleTimeSpan);
            result.config.gui.wave.channelCardFixedWidth = positiveOrFallback(
                readScalar<double>(wave, "channel_card_fixed_width", result.config.gui.wave.channelCardFixedWidth),
                128.0);
            result.config.gui.wave.channelCardAdaptiveRatio = positiveOrFallback(
                readScalar<double>(
                    wave, "channel_card_adaptive_ratio", result.config.gui.wave.channelCardAdaptiveRatio),
                0.22);
            result.config.gui.wave.verticalAutoFitMultiplier = positiveOrFallback(
                readScalar<double>(
                    wave, "vertical_auto_fit_multiplier", result.config.gui.wave.verticalAutoFitMultiplier),
                1.2);
            result.config.gui.wave.resetHistoryOnTimeReset =
                readScalar<bool>(wave, "reset_history_on_time_reset", result.config.gui.wave.resetHistoryOnTimeReset);
            result.config.gui.wave.showAxisLabels =
                readScalar<bool>(wave, "show_axis_labels", result.config.gui.wave.showAxisLabels);
            result.config.gui.wave.showChannelLegend =
                readScalar<bool>(wave, "show_channel_legend", result.config.gui.wave.showChannelLegend);
            result.config.gui.wave.showFftLegend =
                readScalar<bool>(wave, "show_fft_legend", result.config.gui.wave.showFftLegend);
            result.config.gui.wave.fullscreenMode =
                parseWaveFullscreenMode(readScalar<std::string>(
                                            wave,
                                            "fullscreen_mode",
                                            toWaveFullscreenModeText(result.config.gui.wave.fullscreenMode)),
                                        result.config.gui.wave.fullscreenMode);
            if (const auto logHistory = childNode(gui, "log_history")) {
                result.config.gui.logHistory.transferRawLimit = readScalar<std::size_t>(
                    logHistory, "transfer_raw_limit", result.config.gui.logHistory.transferRawLimit);
                result.config.gui.logHistory.transferFrameLimit = readScalar<std::size_t>(
                    logHistory, "transfer_frame_limit", result.config.gui.logHistory.transferFrameLimit);
                result.config.gui.logHistory.hostLimit =
                    readScalar<std::size_t>(logHistory, "host_limit", result.config.gui.logHistory.hostLimit);
                result.config.gui.logHistory.scriptLimit =
                    readScalar<std::size_t>(logHistory, "script_limit", result.config.gui.logHistory.scriptLimit);
            }
            if (const auto rawCapture = childNode(gui, "raw_capture")) {
                result.config.gui.rawCapture.liveLimitBytes = readScalar<std::size_t>(
                    rawCapture, "live_limit_bytes", result.config.gui.rawCapture.liveLimitBytes);
                result.config.gui.rawCapture.recordingQueueLimitBytes = readScalar<std::size_t>(
                    rawCapture, "recording_queue_limit_bytes", result.config.gui.rawCapture.recordingQueueLimitBytes);
            }
            if (const auto transferLog = childNode(gui, "transfer_log")) {
                result.config.gui.replayRawHistoryOnSchemaSwitch =
                    readScalar<bool>(transferLog,
                                     "replay_raw_history_on_schema_switch",
                                     result.config.gui.replayRawHistoryOnSchemaSwitch);
            }
            if (const auto realtimeBacklog = childNode(gui, "realtime_backlog")) {
                result.config.gui.realtimeBacklog.mode =
                    readScalar<std::string>(realtimeBacklog, "mode", result.config.gui.realtimeBacklog.mode);
                readPerformanceSize(realtimeBacklog,
                                    "rx_chunk_bytes_per_pump",
                                    result.config.gui.realtimeBacklog.rxChunkBytesPerPump,
                                    result.config.performance.explicitOverrides.realtimeBacklogRxChunkBytesPerPump);
                readPerformanceSize(
                    realtimeBacklog,
                    "transfer_frame_rows_per_pump",
                    result.config.gui.realtimeBacklog.transferFrameRowsPerPump,
                    result.config.performance.explicitOverrides.realtimeBacklogTransferFrameRowsPerPump);
                readPerformanceSize(realtimeBacklog,
                                    "plot_appends_per_pump",
                                    result.config.gui.realtimeBacklog.plotAppendsPerPump,
                                    result.config.performance.explicitOverrides.realtimeBacklogPlotAppendsPerPump);
                readPerformanceSize(
                    realtimeBacklog,
                    "raw_first_backlog_warn_bytes",
                    result.config.gui.realtimeBacklog.rawFirstBacklogWarnBytes,
                    result.config.performance.explicitOverrides.realtimeBacklogRawFirstBacklogWarnBytes);
                result.config.gui.realtimeBacklog.derivedBacklogDegradeEnabled =
                    readScalar<bool>(realtimeBacklog,
                                     "derived_backlog_degrade_enabled",
                                     result.config.gui.realtimeBacklog.derivedBacklogDegradeEnabled);
                result.config.gui.realtimeBacklog.discardBacklogOnDisconnect =
                    readScalar<bool>(realtimeBacklog,
                                     "discard_backlog_on_disconnect",
                                     result.config.gui.realtimeBacklog.discardBacklogOnDisconnect);
                result.config.gui.realtimeBacklog.pumpMinIntervalMs = readScalar<double>(
                    realtimeBacklog, "pump_min_interval_ms", result.config.gui.realtimeBacklog.pumpMinIntervalMs);
            }
            result.config.gui.showAppHeader = readScalar<bool>(gui, "show_app_header", result.config.gui.showAppHeader);
            result.config.gui.luaDockLayoutDebug =
                readScalar<bool>(gui, "lua_dock_layout_debug", result.config.gui.luaDockLayoutDebug);
            result.config.gui.luaDockRenderCopyMode =
                readScalar<bool>(gui, "lua_dock_render_copy_mode", result.config.gui.luaDockRenderCopyMode);
            result.config.gui.sendHistoryLimit =
                readScalar<std::size_t>(gui, "send_history_limit", result.config.gui.sendHistoryLimit);
        }
        if (const auto elfSymbolCombo = childNode(gui, "elf_symbol_combo")) {
            const int limit =
                readScalar<int>(elfSymbolCombo, "limit", static_cast<int>(result.config.gui.elfSymbolCombo.limit));
            if (limit > 0) {
                result.config.gui.elfSymbolCombo.limit = static_cast<std::size_t>(limit);
            }
            const int debounceMs =
                readScalar<int>(elfSymbolCombo, "debounce_ms", result.config.gui.elfSymbolCombo.debounceMs);
            if (debounceMs > 0) {
                result.config.gui.elfSymbolCombo.debounceMs = debounceMs;
            }
            result.config.gui.elfSymbolCombo.autoRefreshSelectedAddress = readScalar<bool>(
                elfSymbolCombo,
                "auto_refresh_selected_address",
                result.config.gui.elfSymbolCombo.autoRefreshSelectedAddress);
            result.config.gui.elfSymbolCombo.autoRefreshEmitOnControl = readScalar<bool>(
                elfSymbolCombo,
                "auto_refresh_emit_on_control",
                result.config.gui.elfSymbolCombo.autoRefreshEmitOnControl);
        }

        const auto protocol = root["protocol"];
        result.config.protocol.rootDir = readScalar<std::string>(protocol, "root_dir", result.config.protocol.rootDir);
        result.config.protocol.selectedDir =
            readScalar<std::string>(protocol, "selected_dir", result.config.protocol.selectedDir);
        if (const auto tx = childNode(protocol, "tx")) {
            result.config.protocol.tx.sendTimeoutMs =
                readScalar<std::uint64_t>(tx, "send_timeout_ms", result.config.protocol.tx.sendTimeoutMs);
            result.config.protocol.tx.requestTimeoutMs =
                readScalar<std::uint64_t>(tx, "request_timeout_ms", result.config.protocol.tx.requestTimeoutMs);
            result.config.protocol.tx.maxPending =
                readScalar<std::size_t>(tx, "max_pending", result.config.protocol.tx.maxPending);
            result.config.protocol.tx.overflowPolicy =
                readScalar<std::string>(tx, "overflow_policy", result.config.protocol.tx.overflowPolicy);
            result.config.protocol.tx.overflowNotify =
                readScalar<std::string>(tx, "overflow_notify", result.config.protocol.tx.overflowNotify);
        }

        if (const auto receive = root["receive"]) {
            readPerformanceSize(receive,
                                "transport_read_buffer_bytes",
                                result.config.receive.transportReadBufferBytes,
                                result.config.performance.explicitOverrides.receiveTransportReadBufferBytes);
            if (const auto streamBuffer = childNode(receive, "stream_buffer")) {
                result.config.receive.streamBuffer.nearOverflowThreshold = readScalar<double>(
                    streamBuffer, "near_overflow_threshold", result.config.receive.streamBuffer.nearOverflowThreshold);
                result.config.receive.streamBuffer.popupEnabled =
                    readScalar<bool>(streamBuffer, "popup_enabled", result.config.receive.streamBuffer.popupEnabled);
            }
        }

        const auto scripting = root["scripting"];
        if (const auto pipeline = childNode(scripting, "pipeline")) {
            if (pipeline["worker_threads"]) {
                result.config.scripting.pipeline.workerThreads =
                    readScalar<std::size_t>(pipeline, "worker_threads", 1U);
            }
        }
        if (const auto worker = childNode(scripting, "worker")) {
            result.config.scripting.workerEnabled =
                readScalar<bool>(worker, "enabled", result.config.scripting.workerEnabled);
            readPerformanceSize(worker,
                                "rx_queue_limit_bytes",
                                result.config.scripting.workerRxQueueLimitBytes,
                                result.config.performance.explicitOverrides.workerRxQueueLimitBytes);
            readPerformanceSize(worker,
                                "memory_budget_bytes",
                                result.config.scripting.workerMemoryBudgetBytes,
                                result.config.performance.explicitOverrides.workerMemoryBudgetBytes);
            result.config.scripting.workerMemoryBudgetAvailableRatio = readScalar<double>(
                worker, "memory_budget_available_ratio", result.config.scripting.workerMemoryBudgetAvailableRatio);
            readPerformanceSize(worker,
                                "output_queue_limit",
                                result.config.scripting.workerOutputQueueLimit,
                                result.config.performance.explicitOverrides.workerOutputQueueLimit);
            readPerformanceSize(worker,
                                "batch_bytes",
                                result.config.scripting.workerBatchBytes,
                                result.config.performance.explicitOverrides.workerBatchBytes);
            result.config.scripting.workerBackpressureEnabled =
                readScalar<bool>(worker, "backpressure_enabled", result.config.scripting.workerBackpressureEnabled);
            result.config.scripting.workerBackpressureHighWatermark =
                readScalar<double>(worker,
                                   "backpressure_rx_queue_high_watermark",
                                   result.config.scripting.workerBackpressureHighWatermark);
            result.config.scripting.workerBackpressureLowWatermark = readScalar<double>(
                worker, "backpressure_rx_queue_low_watermark", result.config.scripting.workerBackpressureLowWatermark);
            readPerformanceDouble(worker,
                                  "output_flush_budget_ms",
                                  result.config.scripting.workerOutputFlushBudgetMs,
                                  result.config.performance.explicitOverrides.workerOutputFlushBudgetMs);
            result.config.scripting.drainRequestOutputsUnbounded = readScalar<bool>(
                worker, "drain_request_outputs_unbounded", result.config.scripting.drainRequestOutputsUnbounded);
        }
        if (const auto fileIo = childNode(scripting, "file_io")) {
            auto& config = result.config.scripting.fileIo;
            config.enabled = readScalar<bool>(fileIo, "enabled", config.enabled);
            config.allowProtocolDir = readScalar<bool>(fileIo, "allow_protocol_dir", config.allowProtocolDir);
            config.allowDialogPaths = readScalar<bool>(fileIo, "allow_dialog_paths", config.allowDialogPaths);
            config.extraAllowedRoots = readStringList(fileIo, "extra_allowed_roots", config.extraAllowedRoots);
            config.maxOpenFiles = readScalar<std::size_t>(fileIo, "max_open_files", config.maxOpenFiles);
            config.defaultChunkBytes = readScalar<std::size_t>(fileIo, "default_chunk_bytes", config.defaultChunkBytes);
            config.maxChunkBytes = readScalar<std::size_t>(fileIo, "max_chunk_bytes", config.maxChunkBytes);
            config.maxFileSizeBytes = readScalar<std::uint64_t>(fileIo, "max_file_size_bytes", config.maxFileSizeBytes);
            config.maxWriteFileSizeBytes =
                readScalar<std::uint64_t>(fileIo, "max_write_file_size_bytes", config.maxWriteFileSizeBytes);
            if (const auto dialog = childNode(fileIo, "dialog")) {
                config.dialog.enabled = readScalar<bool>(dialog, "enabled", config.dialog.enabled);
                config.dialog.rememberLastDir =
                    readScalar<bool>(dialog, "remember_last_dir", config.dialog.rememberLastDir);
            }
            if (const auto sendFile = childNode(fileIo, "send_file")) {
                config.sendFile.defaultChunkBytes =
                    readScalar<std::size_t>(sendFile, "default_chunk_bytes", config.sendFile.defaultChunkBytes);
                config.sendFile.maxInflightChunks =
                    readScalar<std::size_t>(sendFile, "max_inflight_chunks", config.sendFile.maxInflightChunks);
            }
        }

        const auto logging = root["logging"];
        result.config.logging.level =
            parseLogLevel(readScalar<std::string>(logging, "level", toLogLevelText(result.config.logging.level)));
        result.config.logging.filePath = readScalar<std::string>(logging, "file_path", result.config.logging.filePath);
        result.config.configPath = normalizeTextPath(result.resolvedPath);

        const auto communication = root["communication"];
        result.config.communication.kind = parseTransportKind(
            readScalar<std::string>(communication, "kind", toTransportKindText(result.config.communication.kind)));

        if (const auto tcpClient = childNode(communication, "tcp_client")) {
            result.config.communication.tcpClient.host =
                readScalar<std::string>(tcpClient, "host", result.config.communication.tcpClient.host);
            result.config.communication.tcpClient.port =
                readScalar<std::uint16_t>(tcpClient, "port", result.config.communication.tcpClient.port);
        }

        if (const auto tcpServer = childNode(communication, "tcp_server")) {
            result.config.communication.tcpServer.bindAddress =
                readScalar<std::string>(tcpServer, "bind_address", result.config.communication.tcpServer.bindAddress);
            result.config.communication.tcpServer.port =
                readScalar<std::uint16_t>(tcpServer, "port", result.config.communication.tcpServer.port);
            result.config.communication.tcpServer.rejectNewConnection = readScalar<bool>(
                tcpServer, "reject_new_connection", result.config.communication.tcpServer.rejectNewConnection);
        }

        if (const auto serial = childNode(communication, "serial")) {
            result.config.communication.serial.portName =
                readScalar<std::string>(serial, "port_name", result.config.communication.serial.portName);
            result.config.communication.serial.baudRate =
                readScalar<std::uint32_t>(serial, "baud_rate", result.config.communication.serial.baudRate);
            result.config.communication.serial.dataBits =
                readScalar<std::uint32_t>(serial, "data_bits", result.config.communication.serial.dataBits);
            result.config.communication.serial.parity =
                readScalar<std::string>(serial, "parity", result.config.communication.serial.parity);
            result.config.communication.serial.stopBits =
                readScalar<std::string>(serial, "stop_bits", result.config.communication.serial.stopBits);
            result.config.communication.serial.flowControl =
                readScalar<std::string>(serial, "flow_control", result.config.communication.serial.flowControl);
        }

        if (const auto udpPeer = childNode(communication, "udp_peer")) {
            result.config.communication.udpPeer.bindAddress =
                readScalar<std::string>(udpPeer, "bind_address", result.config.communication.udpPeer.bindAddress);
            result.config.communication.udpPeer.bindPort =
                readScalar<std::uint16_t>(udpPeer, "bind_port", result.config.communication.udpPeer.bindPort);
            result.config.communication.udpPeer.remoteHost =
                readScalar<std::string>(udpPeer, "remote_host", result.config.communication.udpPeer.remoteHost);
            result.config.communication.udpPeer.remotePort =
                readScalar<std::uint16_t>(udpPeer, "remote_port", result.config.communication.udpPeer.remotePort);
        }
        if (const auto receive = root["receive"]) {
            result.config.communication.serialPortOptions = kDefaultSerialPorts;
            result.config.communication.reconnectRequired = false;
            result.config.communication.lastError.clear();
            result.config.communication.txCount = 0;
            result.config.communication.rxCount = 0;
            (void) receive;
        }
    } catch (const std::exception& ex) {
        result.error = std::string("读取 YAML 失败: ") + ex.what();
        result.loadedFromDisk = false;
    }

    return result;
}

bool ConfigStore::save(const std::filesystem::path& path, const AppConfig& config, std::string& error) const
{
    YAML::Node root;
    auto scaledDefaults = withDefaults();
    scaledDefaults.performance.scale = normalizePerformanceScale(config.performance.scale);
    applyPerformanceScale(scaledDefaults);

    const auto& explicitOverrides = config.performance.explicitOverrides;

    root["performance"]["scale"] = scaledDefaults.performance.scale;
    root["app"]["language"] = config.app.language;
    root["app"]["fps_limit"] = config.app.fpsLimit;
    root["app"]["idle_render"] = config.app.idleRender;
    root["app"]["auto_save"]["enabled"] = config.app.autoSave.enabled;
    root["app"]["auto_save"]["interval_ms"] = config.app.autoSave.intervalMs;
    root["app"]["config_hot_reload"]["enabled"] = config.app.configHotReload.enabled;

    root["gui"]["window"]["title"] = config.gui.window.title;
    root["gui"]["window"]["width"] = config.gui.window.width;
    root["gui"]["window"]["height"] = config.gui.window.height;
    root["gui"]["window"]["maximized"] = config.gui.window.maximized;
    root["gui"]["wave"]["control_mode"] = toWaveControlModeText(config.gui.wave.controlMode);
    root["gui"]["wave"]["display_formula"] = toWaveDisplayFormulaText(config.gui.wave.displayFormula);
    root["gui"]["wave"]["channel_card_width_mode"] =
        toWaveChannelCardWidthModeText(config.gui.wave.channelCardWidthMode);
    root["gui"]["wave"]["channel_double_click_action"] =
        toWaveChannelDoubleClickActionText(config.gui.wave.channelDoubleClickAction);
    root["gui"]["wave"]["x_axis_double_click_action"] =
        toWaveXAxisDoubleClickActionText(config.gui.wave.xAxisDoubleClickAction);
    root["gui"]["wave"]["hidden_channel_policy"] = toWaveHiddenChannelPolicyText(config.gui.wave.hiddenChannelPolicy);
    root["gui"]["wave"]["cursor_extreme_snap_policy"] =
        toWaveCursorExtremeSnapPolicyText(config.gui.wave.cursorExtremeSnapPolicy);
    root["gui"]["wave"]["zoom_selection_auto_exit"] = config.gui.wave.zoomSelectionAutoExit;
    root["gui"]["wave"]["channel_card_fixed_width"] = config.gui.wave.channelCardFixedWidth;
    root["gui"]["wave"]["channel_card_adaptive_ratio"] = config.gui.wave.channelCardAdaptiveRatio;
    root["gui"]["wave"]["vertical_auto_fit_multiplier"] = config.gui.wave.verticalAutoFitMultiplier;
    root["gui"]["wave"]["max_render_points_per_channel"] = config.gui.wave.maxRenderPointsPerChannel;
    root["gui"]["wave"]["max_render_vertices"] = config.gui.wave.maxRenderVertices;
    root["gui"]["wave"]["downsample_start_multiplier"] = config.gui.wave.downsampleStartMultiplier;
    root["gui"]["wave"]["overview_max_samples"] = config.gui.wave.overviewMaxSamples;
    root["gui"]["wave"]["max_total_samples"] = config.gui.wave.maxTotalSamples;
    root["gui"]["wave"]["min_visible_time_span"] = config.gui.wave.minVisibleTimeSpan;
    root["gui"]["wave"]["reset_history_on_time_reset"] = config.gui.wave.resetHistoryOnTimeReset;
    root["gui"]["wave"]["show_axis_labels"] = config.gui.wave.showAxisLabels;
    root["gui"]["wave"]["show_channel_legend"] = config.gui.wave.showChannelLegend;
    root["gui"]["wave"]["show_fft_legend"] = config.gui.wave.showFftLegend;
    root["gui"]["wave"]["fullscreen_mode"] = toWaveFullscreenModeText(config.gui.wave.fullscreenMode);
    root["gui"]["log_history"]["transfer_raw_limit"] = config.gui.logHistory.transferRawLimit;
    root["gui"]["log_history"]["transfer_frame_limit"] = config.gui.logHistory.transferFrameLimit;
    root["gui"]["log_history"]["host_limit"] = config.gui.logHistory.hostLimit;
    root["gui"]["log_history"]["script_limit"] = config.gui.logHistory.scriptLimit;
    root["gui"]["raw_capture"]["live_limit_bytes"] = config.gui.rawCapture.liveLimitBytes;
    root["gui"]["raw_capture"]["recording_queue_limit_bytes"] = config.gui.rawCapture.recordingQueueLimitBytes;
    root["gui"]["transfer_log"]["replay_raw_history_on_schema_switch"] = config.gui.replayRawHistoryOnSchemaSwitch;
    root["gui"]["realtime_backlog"]["mode"] = config.gui.realtimeBacklog.mode;
    writePerformanceScalar(root["gui"]["realtime_backlog"],
                           "rx_chunk_bytes_per_pump",
                           config.gui.realtimeBacklog.rxChunkBytesPerPump,
                           scaledDefaults.gui.realtimeBacklog.rxChunkBytesPerPump,
                           explicitOverrides.realtimeBacklogRxChunkBytesPerPump);
    writePerformanceScalar(root["gui"]["realtime_backlog"],
                           "transfer_frame_rows_per_pump",
                           config.gui.realtimeBacklog.transferFrameRowsPerPump,
                           scaledDefaults.gui.realtimeBacklog.transferFrameRowsPerPump,
                           explicitOverrides.realtimeBacklogTransferFrameRowsPerPump);
    writePerformanceScalar(root["gui"]["realtime_backlog"],
                           "plot_appends_per_pump",
                           config.gui.realtimeBacklog.plotAppendsPerPump,
                           scaledDefaults.gui.realtimeBacklog.plotAppendsPerPump,
                           explicitOverrides.realtimeBacklogPlotAppendsPerPump);
    writePerformanceScalar(root["gui"]["realtime_backlog"],
                           "raw_first_backlog_warn_bytes",
                           config.gui.realtimeBacklog.rawFirstBacklogWarnBytes,
                           scaledDefaults.gui.realtimeBacklog.rawFirstBacklogWarnBytes,
                           explicitOverrides.realtimeBacklogRawFirstBacklogWarnBytes);
    root["gui"]["realtime_backlog"]["derived_backlog_degrade_enabled"] =
        config.gui.realtimeBacklog.derivedBacklogDegradeEnabled;
    root["gui"]["realtime_backlog"]["discard_backlog_on_disconnect"] =
        config.gui.realtimeBacklog.discardBacklogOnDisconnect;
    root["gui"]["realtime_backlog"]["pump_min_interval_ms"] = config.gui.realtimeBacklog.pumpMinIntervalMs;
    root["gui"]["show_app_header"] = config.gui.showAppHeader;
    root["gui"]["send_history_limit"] = config.gui.sendHistoryLimit;
    root["gui"]["lua_dock_layout_debug"] = config.gui.luaDockLayoutDebug;
    root["gui"]["lua_dock_render_copy_mode"] = config.gui.luaDockRenderCopyMode;
    root["gui"]["elf_symbol_combo"]["limit"] = config.gui.elfSymbolCombo.limit;
    root["gui"]["elf_symbol_combo"]["debounce_ms"] = config.gui.elfSymbolCombo.debounceMs;
    root["gui"]["elf_symbol_combo"]["auto_refresh_selected_address"] =
        config.gui.elfSymbolCombo.autoRefreshSelectedAddress;
    root["gui"]["elf_symbol_combo"]["auto_refresh_emit_on_control"] =
        config.gui.elfSymbolCombo.autoRefreshEmitOnControl;

    root["protocol"]["root_dir"] = config.protocol.rootDir;
    root["protocol"]["selected_dir"] = config.protocol.selectedDir;
    root["protocol"]["tx"]["send_timeout_ms"] = config.protocol.tx.sendTimeoutMs;
    root["protocol"]["tx"]["request_timeout_ms"] = config.protocol.tx.requestTimeoutMs;
    root["protocol"]["tx"]["max_pending"] = config.protocol.tx.maxPending;
    root["protocol"]["tx"]["overflow_policy"] = config.protocol.tx.overflowPolicy;
    root["protocol"]["tx"]["overflow_notify"] = config.protocol.tx.overflowNotify;
    root["receive"]["stream_buffer"]["near_overflow_threshold"] = config.receive.streamBuffer.nearOverflowThreshold;
    root["receive"]["stream_buffer"]["popup_enabled"] = config.receive.streamBuffer.popupEnabled;
    writePerformanceScalar(root["receive"],
                           "transport_read_buffer_bytes",
                           config.receive.transportReadBufferBytes,
                           scaledDefaults.receive.transportReadBufferBytes,
                           explicitOverrides.receiveTransportReadBufferBytes);

    if (config.scripting.pipeline.workerThreads.has_value()) {
        root["scripting"]["pipeline"]["worker_threads"] = *config.scripting.pipeline.workerThreads;
    }
    root["scripting"]["worker"]["enabled"] = config.scripting.workerEnabled;
    writePerformanceScalar(root["scripting"]["worker"],
                           "rx_queue_limit_bytes",
                           config.scripting.workerRxQueueLimitBytes,
                           scaledDefaults.scripting.workerRxQueueLimitBytes,
                           explicitOverrides.workerRxQueueLimitBytes);
    writePerformanceScalar(root["scripting"]["worker"],
                           "memory_budget_bytes",
                           config.scripting.workerMemoryBudgetBytes,
                           scaledDefaults.scripting.workerMemoryBudgetBytes,
                           explicitOverrides.workerMemoryBudgetBytes);
    root["scripting"]["worker"]["memory_budget_available_ratio"] = config.scripting.workerMemoryBudgetAvailableRatio;
    writePerformanceScalar(root["scripting"]["worker"],
                           "output_queue_limit",
                           config.scripting.workerOutputQueueLimit,
                           scaledDefaults.scripting.workerOutputQueueLimit,
                           explicitOverrides.workerOutputQueueLimit);
    writePerformanceScalar(root["scripting"]["worker"],
                           "batch_bytes",
                           config.scripting.workerBatchBytes,
                           scaledDefaults.scripting.workerBatchBytes,
                           explicitOverrides.workerBatchBytes);
    root["scripting"]["worker"]["backpressure_enabled"] = config.scripting.workerBackpressureEnabled;
    root["scripting"]["worker"]["backpressure_rx_queue_high_watermark"] =
        config.scripting.workerBackpressureHighWatermark;
    root["scripting"]["worker"]["backpressure_rx_queue_low_watermark"] =
        config.scripting.workerBackpressureLowWatermark;
    writePerformanceScalar(root["scripting"]["worker"],
                           "output_flush_budget_ms",
                           config.scripting.workerOutputFlushBudgetMs,
                           scaledDefaults.scripting.workerOutputFlushBudgetMs,
                           explicitOverrides.workerOutputFlushBudgetMs);
    root["scripting"]["file_io"]["enabled"] = config.scripting.fileIo.enabled;
    root["scripting"]["file_io"]["allow_protocol_dir"] = config.scripting.fileIo.allowProtocolDir;
    root["scripting"]["file_io"]["allow_dialog_paths"] = config.scripting.fileIo.allowDialogPaths;
    for (const auto& rootPath : config.scripting.fileIo.extraAllowedRoots) {
        root["scripting"]["file_io"]["extra_allowed_roots"].push_back(rootPath);
    }
    if (config.scripting.fileIo.extraAllowedRoots.empty()) {
        root["scripting"]["file_io"]["extra_allowed_roots"] = YAML::Node(YAML::NodeType::Sequence);
    }
    root["scripting"]["file_io"]["max_open_files"] = config.scripting.fileIo.maxOpenFiles;
    root["scripting"]["file_io"]["default_chunk_bytes"] = config.scripting.fileIo.defaultChunkBytes;
    root["scripting"]["file_io"]["max_chunk_bytes"] = config.scripting.fileIo.maxChunkBytes;
    root["scripting"]["file_io"]["max_file_size_bytes"] = config.scripting.fileIo.maxFileSizeBytes;
    root["scripting"]["file_io"]["max_write_file_size_bytes"] = config.scripting.fileIo.maxWriteFileSizeBytes;
    root["scripting"]["file_io"]["dialog"]["enabled"] = config.scripting.fileIo.dialog.enabled;
    root["scripting"]["file_io"]["dialog"]["remember_last_dir"] = config.scripting.fileIo.dialog.rememberLastDir;
    root["scripting"]["file_io"]["send_file"]["default_chunk_bytes"] =
        config.scripting.fileIo.sendFile.defaultChunkBytes;
    root["scripting"]["file_io"]["send_file"]["max_inflight_chunks"] =
        config.scripting.fileIo.sendFile.maxInflightChunks;

    root["logging"]["level"] = toLogLevelText(config.logging.level);
    if (!config.logging.filePath.empty()) {
        root["logging"]["file_path"] = config.logging.filePath;
    }

    root["communication"]["kind"] = toTransportKindText(config.communication.kind);
    root["communication"]["tcp_client"]["host"] = config.communication.tcpClient.host;
    root["communication"]["tcp_client"]["port"] = config.communication.tcpClient.port;
    root["communication"]["tcp_server"]["bind_address"] = config.communication.tcpServer.bindAddress;
    root["communication"]["tcp_server"]["port"] = config.communication.tcpServer.port;
    root["communication"]["tcp_server"]["reject_new_connection"] = config.communication.tcpServer.rejectNewConnection;
    root["communication"]["serial"]["port_name"] = config.communication.serial.portName;
    root["communication"]["serial"]["baud_rate"] = config.communication.serial.baudRate;
    root["communication"]["serial"]["data_bits"] = config.communication.serial.dataBits;
    root["communication"]["serial"]["parity"] = config.communication.serial.parity;
    root["communication"]["serial"]["stop_bits"] = config.communication.serial.stopBits;
    root["communication"]["serial"]["flow_control"] = config.communication.serial.flowControl;
    root["communication"]["udp_peer"]["bind_address"] = config.communication.udpPeer.bindAddress;
    root["communication"]["udp_peer"]["bind_port"] = config.communication.udpPeer.bindPort;
    root["communication"]["udp_peer"]["remote_host"] = config.communication.udpPeer.remoteHost;
    root["communication"]["udp_peer"]["remote_port"] = config.communication.udpPeer.remotePort;

    try {
        if (!path.parent_path().empty()) {
            std::error_code directoryError;
            std::filesystem::create_directories(path.parent_path(), directoryError);
            if (directoryError) {
                error = "创建配置目录失败: " + directoryError.message();
                return false;
            }
        }
        std::ofstream out(path);
        if (!out.good()) {
            error = "无法写入配置文件";
            return false;
        }
        out << root;
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& dir) const
{
    return normalizeProtocolDir(defaultProtocolDir_.parent_path(), dir);
}

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& rootDir,
                                                        const std::filesystem::path& dir) const
{
    std::filesystem::path candidate = dir.empty() ? defaultProtocolDir_ : dir;
    if (protocolEntryExists(candidate)) {
        return candidate;
    }
    if (!dir.empty() && dir.is_relative() && !rootDir.empty()) {
        // 核心流程：配置里 selected_dir 常保存为 root_dir 下的相对路径；
        // 先按当前协议根目录解析，避免错误回退到扫描结果里的第一个协议。
        const auto byProtocolName = rootDir / dir.filename();
        if (protocolEntryExists(byProtocolName)) {
            return byProtocolName;
        }
        const auto byRootRelativePath = rootDir / dir;
        if (protocolEntryExists(byRootRelativePath)) {
            return byRootRelativePath;
        }
    }

    const auto scanned = scanProtocolDirectories(rootDir);
    if (!scanned.empty()) {
        return std::filesystem::path(scanned.front());
    }
    return defaultProtocolDir_;
}

std::filesystem::path ConfigStore::mainLuaPath(const std::filesystem::path& protocolDir) const
{
    return protocolDir / "main.lua";
}

std::string ConfigStore::protocolName(const std::filesystem::path& protocolDir) const
{
    const auto filename = protocolDir.filename().string();
    return filename.empty() ? std::string("default_protocol") : filename;
}

bool ConfigStore::protocolEntryExists(const std::filesystem::path& protocolDir) const
{
    std::error_code error;
    return std::filesystem::exists(mainLuaPath(protocolDir), error) && !error;
}

std::vector<std::string> ConfigStore::scanProtocolDirectories(const std::filesystem::path& rootDir) const
{
    std::vector<std::string> results;
    std::error_code ec;
    if (!std::filesystem::exists(rootDir, ec) || ec) {
        return results;
    }

    for (const auto& entry : std::filesystem::directory_iterator(rootDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_directory()) {
            continue;
        }
        if (!protocolEntryExists(entry.path())) {
            continue;
        }
        results.push_back(normalizeTextPath(entry.path()));
    }

    std::sort(results.begin(), results.end());
    return results;
}

bool ConfigStore::ensureDefaultProtocolScript(const std::filesystem::path& protocolDir, std::string& error) const
{
    return embedded::ensureDefaultProtocolScript(protocolDir, error);
}

bool ConfigStore::ensureDefaultProtocolWorkspace(std::string& error) const
{
    return embedded::ensureProtocolWorkspace(defaultProtocolRootDir_, error);
}

FileSnapshot ConfigStore::snapshot(const std::filesystem::path& path) const
{
    FileSnapshot result;
    result.path = path;
    std::error_code error;
    result.exists = std::filesystem::exists(path, error) && !error;
    if (result.exists) {
        const auto lastWriteTime = std::filesystem::last_write_time(path, error);
        if (!error) {
            result.timestampMs = toTimestampMs(lastWriteTime);
        }
    }
    return result;
}

bool ConfigStore::hasChanged(const FileSnapshot& previous) const
{
    const auto current = snapshot(previous.path);
    return current.exists != previous.exists || current.timestampMs != previous.timestampMs;
}

void ConfigStore::applyToDock(const AppConfig& config, dock::DockStore& dockStore) const
{
    auto& comm = dockStore.commState();
    comm.kind = config.communication.kind;
    comm.tcpClient = config.communication.tcpClient;
    comm.tcpServer = config.communication.tcpServer;
    comm.serial = config.communication.serial;
    if (comm.serialPortOptions.empty()) {
        comm.serialPortOptions = kDefaultSerialPorts;
    }

    const auto protocolDir = normalizeProtocolDir(config.protocol.rootDir, config.protocol.selectedDir);
    auto& lua = dockStore.luaState();
    lua.protocolRootDir = config.protocol.rootDir;
    lua.protocolDirOptions = scanProtocolDirectories(lua.protocolRootDir);
    lua.protocolDir = normalizeTextPath(protocolDir);
    lua.protocolName = protocolName(protocolDir);
    lua.scriptPath = normalizeTextPath(mainLuaPath(protocolDir));

    auto& configState = dockStore.configState();
    configState.autoSaveEnabled = config.app.autoSave.enabled;
    configState.autoSaveIntervalMs = config.app.autoSave.intervalMs;
    configState.configHotReloadEnabled = config.app.configHotReload.enabled;
    configState.fpsLimit = config.app.fpsLimit;
    configState.idleRender = config.app.idleRender;
    configState.luaDockLayoutDebug = config.gui.luaDockLayoutDebug;
    configState.luaDockRenderCopyMode = config.gui.luaDockRenderCopyMode;
    configState.loadedFromPath = config.configPath.empty() ? normalizeTextPath(defaultConfigPath_) : config.configPath;

    auto& waveState = dockStore.waveState();
    auto& wave = waveState.view;
    wave.controlMode = config.gui.wave.controlMode;
    wave.displayFormula = config.gui.wave.displayFormula;
    wave.channelCardWidthMode = config.gui.wave.channelCardWidthMode;
    wave.channelDoubleClickAction = config.gui.wave.channelDoubleClickAction;
    wave.xAxisDoubleClickAction = config.gui.wave.xAxisDoubleClickAction;
    wave.zoomSelectionAutoExit = config.gui.wave.zoomSelectionAutoExit;
    wave.maxRenderPointsPerChannel = config.gui.wave.maxRenderPointsPerChannel;
    wave.maxRenderVertices = config.gui.wave.maxRenderVertices;
    wave.downsampleStartMultiplier = (std::max)(config.gui.wave.downsampleStartMultiplier, 1.0);
    wave.overviewMaxSamples = config.gui.wave.overviewMaxSamples;
    wave.minVisibleTimeSpan = config.gui.wave.minVisibleTimeSpan;
    wave.channelCardFixedWidth = positiveOrFallback(config.gui.wave.channelCardFixedWidth, 128.0);
    wave.channelCardAdaptiveRatio = positiveOrFallback(config.gui.wave.channelCardAdaptiveRatio, 0.22);
    wave.verticalAutoFitMultiplier = positiveOrFallback(config.gui.wave.verticalAutoFitMultiplier, 1.2);
    wave.hiddenChannelPolicy = config.gui.wave.hiddenChannelPolicy;
    wave.cursorExtremeSnapPolicy = config.gui.wave.cursorExtremeSnapPolicy;
    wave.showAxisLabels = config.gui.wave.showAxisLabels;
    wave.showChannelLegend = config.gui.wave.showChannelLegend;
    wave.showFftLegend = config.gui.wave.showFftLegend;
    auto viewConfig = waveState.buffer.viewConfig();
    viewConfig.displayFormula = config.gui.wave.displayFormula;
    waveState.buffer.setViewConfig(viewConfig);
}

AppConfig ConfigStore::captureFromDock(const dock::DockStore& dockStore) const
{
    AppConfig config = withDefaults();

    config.communication = dockStore.commState();
    config.protocol.rootDir = dockStore.luaState().protocolRootDir;
    config.protocol.selectedDir = dockStore.luaState().protocolDir;
    config.app.autoSave.enabled = dockStore.configState().autoSaveEnabled;
    config.app.autoSave.intervalMs = dockStore.configState().autoSaveIntervalMs;
    config.app.configHotReload.enabled = dockStore.configState().configHotReloadEnabled;
    config.app.fpsLimit = dockStore.configState().fpsLimit;
    config.app.idleRender = dockStore.configState().idleRender;
    config.gui.luaDockLayoutDebug = dockStore.configState().luaDockLayoutDebug;
    config.gui.luaDockRenderCopyMode = dockStore.configState().luaDockRenderCopyMode;
    config.gui.wave.controlMode = dockStore.waveState().view.controlMode;
    config.gui.wave.displayFormula = dockStore.waveState().view.displayFormula;
    config.gui.wave.channelCardWidthMode = dockStore.waveState().view.channelCardWidthMode;
    config.gui.wave.channelDoubleClickAction = dockStore.waveState().view.channelDoubleClickAction;
    config.gui.wave.xAxisDoubleClickAction = dockStore.waveState().view.xAxisDoubleClickAction;
    config.gui.wave.zoomSelectionAutoExit = dockStore.waveState().view.zoomSelectionAutoExit;
    config.gui.wave.maxRenderPointsPerChannel = dockStore.waveState().view.maxRenderPointsPerChannel;
    config.gui.wave.maxRenderVertices = dockStore.waveState().view.maxRenderVertices;
    config.gui.wave.downsampleStartMultiplier = dockStore.waveState().view.downsampleStartMultiplier;
    config.gui.wave.overviewMaxSamples = dockStore.waveState().view.overviewMaxSamples;
    config.gui.wave.minVisibleTimeSpan = dockStore.waveState().view.minVisibleTimeSpan;
    config.gui.wave.channelCardFixedWidth = dockStore.waveState().view.channelCardFixedWidth;
    config.gui.wave.channelCardAdaptiveRatio = dockStore.waveState().view.channelCardAdaptiveRatio;
    config.gui.wave.verticalAutoFitMultiplier = dockStore.waveState().view.verticalAutoFitMultiplier;
    config.gui.wave.hiddenChannelPolicy = dockStore.waveState().view.hiddenChannelPolicy;
    config.gui.wave.cursorExtremeSnapPolicy = dockStore.waveState().view.cursorExtremeSnapPolicy;
    config.gui.wave.showAxisLabels = dockStore.waveState().view.showAxisLabels;
    config.gui.wave.showChannelLegend = dockStore.waveState().view.showChannelLegend;
    config.gui.wave.showFftLegend = dockStore.waveState().view.showFftLegend;
    config.configPath = dockStore.configState().loadedFromPath;

    return config;
}

const std::filesystem::path& ConfigStore::defaultConfigPath() const
{
    return defaultConfigPath_;
}

const std::filesystem::path& ConfigStore::defaultProtocolDir() const
{
    return defaultProtocolDir_;
}

std::uint64_t ConfigStore::toTimestampMs(const std::filesystem::file_time_type& fileTime)
{
    const auto normalized = fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(normalized.time_since_epoch()).count());
}

} // namespace protoscope::config
