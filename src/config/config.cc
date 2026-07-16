#include "protoscope/config/config.hpp"

#include "protoscope/config/embedded_protocols.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <string_view>
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

    std::array<float, 4> readFloat4(const YAML::Node& node, const char* key, std::array<float, 4> fallback)
    {
        if (!node || !node[key] || !node[key].IsSequence() || node[key].size() != fallback.size()) {
            return fallback;
        }
        std::array<float, 4> values{};
        for (std::size_t index = 0; index < values.size(); ++index) {
            values[index] = node[key][index].as<float>(fallback[index]);
        }
        return values;
    }

    YAML::Node makeFloat4Node(const std::array<float, 4>& values)
    {
        YAML::Node node;
        for (const float value : values) {
            node.push_back(value);
        }
        return node;
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

    template <typename T> struct EnumNamePair {
        T value;
        std::string_view name;
    };

    template <typename T, std::size_t N>
    T lookupEnum(std::string_view text, const std::array<EnumNamePair<T>, N>& pairs, T fallback)
    {
        for (const auto& pair : pairs) {
            if (pair.name == text) {
                return pair.value;
            }
        }
        return fallback;
    }

    template <typename T, std::size_t N>
    const char* enumToText(T value, const std::array<EnumNamePair<T>, N>& pairs, const char* fallback)
    {
        for (const auto& pair : pairs) {
            if (pair.value == value) {
                return pair.name.data();
            }
        }
        return fallback;
    }

    std::string normalizeRendererBackendText(std::string_view text)
    {
        std::string normalized;
        normalized.reserve(text.size());
        for (const char ch : text) {
            if (ch == '-') {
                normalized.push_back('_');
                continue;
            }
            normalized.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
        }
        return normalized;
    }

    double normalizePerformanceScale(const double scale)
    {
        return scale > 0.0 ? scale : 1.0;
    }

    double normalizeAdaptiveMaxMultiplier(const double multiplier)
    {
        if (!std::isfinite(multiplier) || multiplier <= 0.0) {
            return 1.0;
        }
        return (std::clamp)(multiplier, 0.25, 4.0);
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

    constexpr std::array<EnumNamePair<LogLevel>, 5> kLogLevelNames{{
        {LogLevel::Trace, "trace"},
        {LogLevel::Debug, "debug"},
        {LogLevel::Info, "info"},
        {LogLevel::Warn, "warn"},
        {LogLevel::Error, "error"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveControlMode>, 2> kWaveControlModeNames{{
        {plot::WaveControlMode::LegacyGlobal, "legacy_global"},
        {plot::WaveControlMode::Oscilloscope, "oscilloscope"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveDisplayFormula>, 2> kWaveDisplayFormulaNames{{
        {plot::WaveDisplayFormula::ScaleThenOffset, "scale_then_offset"},
        {plot::WaveDisplayFormula::OffsetThenScale, "offset_then_scale"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveGridDivisionReadoutMode>, 3> kWaveGridDivisionReadoutModeNames{{
        {plot::WaveGridDivisionReadoutMode::DisplayValue, "display_value"},
        {plot::WaveGridDivisionReadoutMode::ActualValue, "actual_value"},
        {plot::WaveGridDivisionReadoutMode::RawValue, "raw_value"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveChannelScaleWheelAcceleration>, 3>
        kWaveChannelScaleWheelAccelerationNames{{
            {plot::WaveChannelScaleWheelAcceleration::None, "none"},
            {plot::WaveChannelScaleWheelAcceleration::Linear, "linear"},
            {plot::WaveChannelScaleWheelAcceleration::Log, "log"},
        }};

    constexpr std::array<EnumNamePair<plot::WaveChannelCardWidthMode>, 2> kWaveChannelCardWidthModeNames{{
        {plot::WaveChannelCardWidthMode::Fixed, "fixed"},
        {plot::WaveChannelCardWidthMode::Adaptive, "adaptive"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveChannelDoubleClickAction>, 4> kWaveChannelDoubleClickActionNames{{
        {plot::WaveChannelDoubleClickAction::ResetAll, "reset_all"},
        {plot::WaveChannelDoubleClickAction::ResetScaleOffset, "reset_scale_offset"},
        {plot::WaveChannelDoubleClickAction::ResetScale, "reset_scale"},
        {plot::WaveChannelDoubleClickAction::ResetOffset, "reset_offset"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveXAxisDoubleClickAction>, 2> kWaveXAxisDoubleClickActionNames{{
        {plot::WaveXAxisDoubleClickAction::FitFullHistory, "fit_full_history"},
        {plot::WaveXAxisDoubleClickAction::FitVisibleWindow, "fit_visible_window"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveYAxisDoubleClickAction>, 2> kWaveYAxisDoubleClickActionNames{{
        {plot::WaveYAxisDoubleClickAction::FitVisibleChannels, "fit_visible_channels"},
        {plot::WaveYAxisDoubleClickAction::FitActiveChannel, "fit_active_channel"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveHiddenChannelPolicy>, 2> kWaveHiddenChannelPolicyNames{{
        {plot::WaveHiddenChannelPolicy::IncludeInDerivedViews, "include_hidden"},
        {plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews, "visible_only"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveCursorExtremeSnapPolicy>, 2> kWaveCursorExtremeSnapPolicyNames{{
        {plot::WaveCursorExtremeSnapPolicy::NearestWaveform, "nearest_waveform"},
        {plot::WaveCursorExtremeSnapPolicy::ViewportZone, "viewport_zone"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveMouseYOffsetDragMode>, 3> kWaveMouseYOffsetDragModeNames{{
        {plot::WaveMouseYOffsetDragMode::Direct, "direct"},
        {plot::WaveMouseYOffsetDragMode::Shift, "shift"},
        {plot::WaveMouseYOffsetDragMode::Disabled, "disabled"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveResetViewportScaleMode>, 2> kWaveResetViewportScaleModeNames{{
        {plot::WaveResetViewportScaleMode::Preserve, "preserve"},
        {plot::WaveResetViewportScaleMode::ProtocolDefault, "protocol_default"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveResetViewportAnchor>, 2> kWaveResetViewportAnchorNames{{
        {plot::WaveResetViewportAnchor::WaveStart, "wave_start"},
        {plot::WaveResetViewportAnchor::Latest, "latest"},
    }};

    constexpr std::array<EnumNamePair<plot::WaveResetViewportAutoFollowMode>, 3>
        kWaveResetViewportAutoFollowModeNames{{
            {plot::WaveResetViewportAutoFollowMode::Existing, "existing"},
            {plot::WaveResetViewportAutoFollowMode::Enable, "enable"},
            {plot::WaveResetViewportAutoFollowMode::Disable, "disable"},
        }};

    constexpr std::array<EnumNamePair<plot::WaveLegendOverlayOpenMode>, 3> kWaveLegendOverlayOpenModeNames{{
        {plot::WaveLegendOverlayOpenMode::Hover, "hover"},
        {plot::WaveLegendOverlayOpenMode::DoubleClick, "double_click"},
        {plot::WaveLegendOverlayOpenMode::Disabled, "disabled"},
    }};

    constexpr std::array<EnumNamePair<GuiWaveFullscreenMode>, 2> kWaveFullscreenModeNames{{
        {GuiWaveFullscreenMode::Focus, "focus"},
        {GuiWaveFullscreenMode::Overlay, "overlay"},
    }};

    constexpr std::array<EnumNamePair<GuiFontChineseGlyphRange>, 2> kFontChineseGlyphRangeNames{{
        {GuiFontChineseGlyphRange::SimplifiedCommon, "simplified_common"},
        {GuiFontChineseGlyphRange::Full, "full"},
    }};

    constexpr std::array<EnumNamePair<GuiRendererBackend>, 3> kRendererBackendNames{{
        {GuiRendererBackend::OpenGL, "opengl"},
        {GuiRendererBackend::D3D11, "d3d11"},
        {GuiRendererBackend::D3D11Warp, "d3d11_warp"},
    }};

    LogLevel parseLogLevel(const std::string& value)
    {
        if (value == "warn" || value == "warning") {
            return LogLevel::Warn;
        }
        return lookupEnum(std::string_view{value}, kLogLevelNames, LogLevel::Info);
    }

    std::string toLogLevelText(const LogLevel level)
    {
        return enumToText(level, kLogLevelNames, "info");
    }

    plot::WaveControlMode parseWaveControlMode(const std::string& value, plot::WaveControlMode fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveControlModeNames, fallback);
    }

    const char* toWaveControlModeText(const plot::WaveControlMode mode)
    {
        return enumToText(mode, kWaveControlModeNames, "oscilloscope");
    }

    plot::WaveDisplayFormula parseWaveDisplayFormula(const std::string& value, plot::WaveDisplayFormula fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveDisplayFormulaNames, fallback);
    }

    const char* toWaveDisplayFormulaText(const plot::WaveDisplayFormula formula)
    {
        return enumToText(formula, kWaveDisplayFormulaNames, "offset_then_scale");
    }

    plot::WaveGridDivisionReadoutMode parseWaveGridDivisionReadoutMode(const std::string& value,
                                                                       plot::WaveGridDivisionReadoutMode fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveGridDivisionReadoutModeNames, fallback);
    }

    const char* toWaveGridDivisionReadoutModeText(const plot::WaveGridDivisionReadoutMode mode)
    {
        return enumToText(mode, kWaveGridDivisionReadoutModeNames, "display_value");
    }

    plot::WaveChannelScaleWheelAcceleration parseWaveChannelScaleWheelAcceleration(
        const std::string& value,
        plot::WaveChannelScaleWheelAcceleration fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveChannelScaleWheelAccelerationNames, fallback);
    }

    const char* toWaveChannelScaleWheelAccelerationText(const plot::WaveChannelScaleWheelAcceleration acceleration)
    {
        return enumToText(acceleration, kWaveChannelScaleWheelAccelerationNames, "log");
    }

    plot::WaveChannelCardWidthMode parseWaveChannelCardWidthMode(const std::string& value)
    {
        return lookupEnum(
            std::string_view{value}, kWaveChannelCardWidthModeNames, plot::WaveChannelCardWidthMode::Fixed);
    }

    const char* toWaveChannelCardWidthModeText(const plot::WaveChannelCardWidthMode mode)
    {
        return enumToText(mode, kWaveChannelCardWidthModeNames, "fixed");
    }

    plot::WaveChannelDoubleClickAction parseWaveChannelDoubleClickAction(const std::string& value,
                                                                         plot::WaveChannelDoubleClickAction fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveChannelDoubleClickActionNames, fallback);
    }

    const char* toWaveChannelDoubleClickActionText(const plot::WaveChannelDoubleClickAction action)
    {
        return enumToText(action, kWaveChannelDoubleClickActionNames, "reset_scale_offset");
    }

    plot::WaveXAxisDoubleClickAction parseWaveXAxisDoubleClickAction(const std::string& value,
                                                                     plot::WaveXAxisDoubleClickAction fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveXAxisDoubleClickActionNames, fallback);
    }

    const char* toWaveXAxisDoubleClickActionText(const plot::WaveXAxisDoubleClickAction action)
    {
        return enumToText(action, kWaveXAxisDoubleClickActionNames, "fit_full_history");
    }

    plot::WaveYAxisDoubleClickAction parseWaveYAxisDoubleClickAction(const std::string& value,
                                                                     plot::WaveYAxisDoubleClickAction fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveYAxisDoubleClickActionNames, fallback);
    }

    const char* toWaveYAxisDoubleClickActionText(const plot::WaveYAxisDoubleClickAction action)
    {
        return enumToText(action, kWaveYAxisDoubleClickActionNames, "fit_visible_channels");
    }

    plot::WaveHiddenChannelPolicy parseWaveHiddenChannelPolicy(const std::string& value,
                                                               plot::WaveHiddenChannelPolicy fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveHiddenChannelPolicyNames, fallback);
    }

    const char* toWaveHiddenChannelPolicyText(const plot::WaveHiddenChannelPolicy policy)
    {
        return enumToText(policy, kWaveHiddenChannelPolicyNames, "include_hidden");
    }

    plot::WaveCursorExtremeSnapPolicy parseWaveCursorExtremeSnapPolicy(const std::string& value,
                                                                       plot::WaveCursorExtremeSnapPolicy fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveCursorExtremeSnapPolicyNames, fallback);
    }

    const char* toWaveCursorExtremeSnapPolicyText(const plot::WaveCursorExtremeSnapPolicy policy)
    {
        return enumToText(policy, kWaveCursorExtremeSnapPolicyNames, "nearest_waveform");
    }

    plot::WaveMouseYOffsetDragMode parseWaveMouseYOffsetDragMode(const std::string& value,
                                                                 plot::WaveMouseYOffsetDragMode fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveMouseYOffsetDragModeNames, fallback);
    }

    const char* toWaveMouseYOffsetDragModeText(const plot::WaveMouseYOffsetDragMode mode)
    {
        return enumToText(mode, kWaveMouseYOffsetDragModeNames, "direct");
    }

    plot::WaveResetViewportScaleMode parseWaveResetViewportScaleMode(
        const std::string& value, plot::WaveResetViewportScaleMode fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveResetViewportScaleModeNames, fallback);
    }

    const char* toWaveResetViewportScaleModeText(const plot::WaveResetViewportScaleMode mode)
    {
        return enumToText(mode, kWaveResetViewportScaleModeNames, "preserve");
    }

    plot::WaveResetViewportAnchor parseWaveResetViewportAnchor(const std::string& value,
                                                               plot::WaveResetViewportAnchor fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveResetViewportAnchorNames, fallback);
    }

    const char* toWaveResetViewportAnchorText(const plot::WaveResetViewportAnchor anchor)
    {
        return enumToText(anchor, kWaveResetViewportAnchorNames, "wave_start");
    }

    plot::WaveResetViewportAutoFollowMode parseWaveResetViewportAutoFollowMode(
        const std::string& value, plot::WaveResetViewportAutoFollowMode fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveResetViewportAutoFollowModeNames, fallback);
    }

    const char* toWaveResetViewportAutoFollowModeText(const plot::WaveResetViewportAutoFollowMode mode)
    {
        return enumToText(mode, kWaveResetViewportAutoFollowModeNames, "existing");
    }

    plot::WaveLegendOverlayOpenMode parseWaveLegendOverlayOpenMode(const std::string& value,
                                                                   plot::WaveLegendOverlayOpenMode fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveLegendOverlayOpenModeNames, fallback);
    }

    const char* toWaveLegendOverlayOpenModeText(const plot::WaveLegendOverlayOpenMode mode)
    {
        return enumToText(mode, kWaveLegendOverlayOpenModeNames, "hover");
    }

    GuiWaveFullscreenMode parseWaveFullscreenMode(const std::string& value, GuiWaveFullscreenMode fallback)
    {
        return lookupEnum(std::string_view{value}, kWaveFullscreenModeNames, fallback);
    }

    const char* toWaveFullscreenModeText(const GuiWaveFullscreenMode mode)
    {
        return enumToText(mode, kWaveFullscreenModeNames, "overlay");
    }

    GuiFontChineseGlyphRange parseFontChineseGlyphRange(const std::string& value, GuiFontChineseGlyphRange fallback)
    {
        return lookupEnum(std::string_view{value}, kFontChineseGlyphRangeNames, fallback);
    }

    const char* toFontChineseGlyphRangeText(const GuiFontChineseGlyphRange range)
    {
        return enumToText(range, kFontChineseGlyphRangeNames, "simplified_common");
    }

    const char* toRendererBackendText(const GuiRendererBackend backend)
    {
        return enumToText(backend, kRendererBackendNames, "opengl");
    }

    double positiveOrFallback(double value, double fallback)
    {
        return value > 0.0 ? value : fallback;
    }

    double positiveOrZero(double value)
    {
        return std::isfinite(value) && value > 0.0 ? value : 0.0;
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

    void loadPerformanceConfig(const YAML::Node& root, AppConfig& config)
    {
        if (const auto performance = root["performance"]) {
            config.performance.scale =
                normalizePerformanceScale(readScalar<double>(performance, "scale", config.performance.scale));
            if (const auto adaptive = childNode(performance, "adaptive")) {
                config.performance.adaptive.enabled =
                    readScalar<bool>(adaptive, "enabled", config.performance.adaptive.enabled);
                config.performance.adaptive.maxMultiplier = normalizeAdaptiveMaxMultiplier(
                    readScalar<double>(adaptive, "max_multiplier", config.performance.adaptive.maxMultiplier));
            }
        }
        if (!config.performance.adaptive.enabled) {
            applyPerformanceScale(config);
        }
    }

    void loadAppConfig(const YAML::Node& root, AppConfig& config)
    {
        const auto app = root["app"];
        config.app.language = readScalar<std::string>(app, "language", config.app.language);
        config.app.fpsLimit = readScalar<std::uint32_t>(app, "fps_limit", config.app.fpsLimit);
        config.app.idleRender = readScalar<std::string>(app, "idle_render", config.app.idleRender);
        if (const auto autoSave = childNode(app, "auto_save")) {
            config.app.autoSave.enabled = readScalar<bool>(autoSave, "enabled", config.app.autoSave.enabled);
            config.app.autoSave.intervalMs =
                readScalar<std::uint64_t>(autoSave, "interval_ms", config.app.autoSave.intervalMs);
        }
        if (const auto configHotReload = childNode(app, "config_hot_reload")) {
            config.app.configHotReload.enabled =
                readScalar<bool>(configHotReload, "enabled", config.app.configHotReload.enabled);
        }
    }

    void loadGuiWindowConfig(const YAML::Node& gui, AppConfig& config)
    {
        if (const auto window = childNode(gui, "window")) {
            config.gui.window.title = readScalar<std::string>(window, "title", config.gui.window.title);
            config.gui.window.width = readScalar<int>(window, "width", config.gui.window.width);
            config.gui.window.height = readScalar<int>(window, "height", config.gui.window.height);
            config.gui.window.maximized = readScalar<bool>(window, "maximized", config.gui.window.maximized);
        }
    }

    void loadGuiFontConfig(const YAML::Node& gui, AppConfig& config)
    {
        if (const auto font = childNode(gui, "font")) {
            config.gui.font.chineseGlyphRange = parseFontChineseGlyphRange(
                readScalar<std::string>(
                    font, "chinese_glyph_range", toFontChineseGlyphRangeText(config.gui.font.chineseGlyphRange)),
                config.gui.font.chineseGlyphRange);
        }
    }

    void loadGuiInteractionFeedbackConfig(const YAML::Node& gui, AppConfig& config)
    {
        if (const auto feedback = childNode(gui, "interaction_feedback")) {
            config.gui.interactionFeedback.enabled =
                readScalar<bool>(feedback, "enabled", config.gui.interactionFeedback.enabled);
            config.gui.interactionFeedback.statusDurationMs = readScalar<std::uint64_t>(
                feedback, "status_duration_ms", config.gui.interactionFeedback.statusDurationMs);
        }
    }

    void loadGuiWaveConfig(const YAML::Node& wave, AppConfig& config)
    {
        config.gui.wave.controlMode = parseWaveControlMode(
            readScalar<std::string>(wave, "control_mode", toWaveControlModeText(config.gui.wave.controlMode)),
            config.gui.wave.controlMode);
        config.gui.wave.displayFormula = parseWaveDisplayFormula(
            readScalar<std::string>(wave, "display_formula", toWaveDisplayFormulaText(config.gui.wave.displayFormula)),
            config.gui.wave.displayFormula);
        config.gui.wave.gridDivisionReadoutMode = parseWaveGridDivisionReadoutMode(
            readScalar<std::string>(wave,
                                    "grid_division_readout_mode",
                                    toWaveGridDivisionReadoutModeText(config.gui.wave.gridDivisionReadoutMode)),
            config.gui.wave.gridDivisionReadoutMode);
        if (const auto channelScaleWheel = childNode(wave, "channel_scale_wheel")) {
            config.gui.wave.channelScaleWheelEnabled =
                readScalar<bool>(channelScaleWheel, "enabled", config.gui.wave.channelScaleWheelEnabled);
            config.gui.wave.channelScaleWheelAcceleration = parseWaveChannelScaleWheelAcceleration(
                readScalar<std::string>(
                    channelScaleWheel,
                    "acceleration",
                    toWaveChannelScaleWheelAccelerationText(config.gui.wave.channelScaleWheelAcceleration)),
                plot::WaveChannelScaleWheelAcceleration::Log);
        }
        config.gui.wave.channelCardWidthMode =
            parseWaveChannelCardWidthMode(readScalar<std::string>(wave, "channel_card_width_mode", "fixed"));
        config.gui.wave.channelDoubleClickAction = parseWaveChannelDoubleClickAction(
            readScalar<std::string>(wave,
                                    "channel_double_click_action",
                                    toWaveChannelDoubleClickActionText(config.gui.wave.channelDoubleClickAction)),
            config.gui.wave.channelDoubleClickAction);
        config.gui.wave.xAxisDoubleClickAction = parseWaveXAxisDoubleClickAction(
            readScalar<std::string>(wave,
                                    "x_axis_double_click_action",
                                    toWaveXAxisDoubleClickActionText(config.gui.wave.xAxisDoubleClickAction)),
            config.gui.wave.xAxisDoubleClickAction);
        config.gui.wave.yAxisDoubleClickAction = parseWaveYAxisDoubleClickAction(
            readScalar<std::string>(wave,
                                    "y_axis_double_click_action",
                                    toWaveYAxisDoubleClickActionText(config.gui.wave.yAxisDoubleClickAction)),
            config.gui.wave.yAxisDoubleClickAction);
        config.gui.wave.yAxisDoubleClickAdjustOffset = readScalar<bool>(
            wave, "y_axis_double_click_adjust_offset", config.gui.wave.yAxisDoubleClickAdjustOffset);
        config.gui.wave.hiddenChannelPolicy = parseWaveHiddenChannelPolicy(
            readScalar<std::string>(
                wave, "hidden_channel_policy", toWaveHiddenChannelPolicyText(config.gui.wave.hiddenChannelPolicy)),
            config.gui.wave.hiddenChannelPolicy);
        config.gui.wave.cursorExtremeSnapPolicy = parseWaveCursorExtremeSnapPolicy(
            readScalar<std::string>(wave,
                                    "cursor_extreme_snap_policy",
                                    toWaveCursorExtremeSnapPolicyText(config.gui.wave.cursorExtremeSnapPolicy)),
            config.gui.wave.cursorExtremeSnapPolicy);
        config.gui.wave.mouseYOffsetDragMode = parseWaveMouseYOffsetDragMode(
            readScalar<std::string>(
                wave, "mouse_y_offset_drag_mode", toWaveMouseYOffsetDragModeText(config.gui.wave.mouseYOffsetDragMode)),
            config.gui.wave.mouseYOffsetDragMode);
        config.gui.wave.legendOverlayOpenMode = parseWaveLegendOverlayOpenMode(
            readScalar<std::string>(wave,
                                    "legend_overlay_open_mode",
                                    toWaveLegendOverlayOpenModeText(config.gui.wave.legendOverlayOpenMode)),
            config.gui.wave.legendOverlayOpenMode);
        config.gui.wave.legendOverlayDoubleClickAutoCollapse = readScalar<bool>(
            wave, "legend_overlay_double_click_auto_collapse", config.gui.wave.legendOverlayDoubleClickAutoCollapse);
        config.gui.wave.interactionAnimationEnabled =
            readScalar<bool>(wave, "interaction_animation_enabled", config.gui.wave.interactionAnimationEnabled);
        config.gui.wave.zoomSelectionAutoExit =
            readScalar<bool>(wave, "zoom_selection_auto_exit", config.gui.wave.zoomSelectionAutoExit);
        config.gui.wave.peakDetectDownsample =
            readScalar<bool>(wave, "peak_detect_downsample", config.gui.wave.peakDetectDownsample);
        config.gui.wave.maxRenderPointsPerChannel =
            readScalar<std::size_t>(wave, "max_render_points_per_channel", config.gui.wave.maxRenderPointsPerChannel);
        config.gui.wave.maxRenderVertices =
            readScalar<std::size_t>(wave, "max_render_vertices", config.gui.wave.maxRenderVertices);
        config.gui.wave.downsampleStartMultiplier =
            readScalar<double>(wave, "downsample_start_multiplier", config.gui.wave.downsampleStartMultiplier);
        config.gui.wave.overviewMaxSamples =
            readScalar<std::size_t>(wave, "overview_max_samples", config.gui.wave.overviewMaxSamples);
        config.gui.wave.maxTotalSamples =
            readScalar<std::size_t>(wave, "max_total_samples", config.gui.wave.maxTotalSamples);
        config.gui.wave.minVisibleTimeSpan =
            readScalar<double>(wave, "min_visible_time_span", config.gui.wave.minVisibleTimeSpan);
        config.gui.wave.channelCardFixedWidth = positiveOrFallback(
            readScalar<double>(wave, "channel_card_fixed_width", config.gui.wave.channelCardFixedWidth), 128.0);
        config.gui.wave.channelCardAdaptiveRatio = positiveOrFallback(
            readScalar<double>(wave, "channel_card_adaptive_ratio", config.gui.wave.channelCardAdaptiveRatio), 0.22);
        config.gui.wave.legendChannelNameMaxWidth = positiveOrZero(
            readScalar<double>(wave, "legend_channel_name_max_width", config.gui.wave.legendChannelNameMaxWidth));
        config.gui.wave.verticalAutoFitMultiplier = positiveOrFallback(
            readScalar<double>(wave, "vertical_auto_fit_multiplier", config.gui.wave.verticalAutoFitMultiplier), 1.25);
        config.gui.wave.resetHistoryOnTimeReset =
            readScalar<bool>(wave, "reset_history_on_time_reset", config.gui.wave.resetHistoryOnTimeReset);
        config.gui.wave.showAxisLabels = readScalar<bool>(wave, "show_axis_labels", config.gui.wave.showAxisLabels);
        config.gui.wave.showChannelLegend =
            readScalar<bool>(wave, "show_channel_legend", config.gui.wave.showChannelLegend);
        config.gui.wave.showFftLegend = readScalar<bool>(wave, "show_fft_legend", config.gui.wave.showFftLegend);
        config.gui.wave.followMeasurementCursorsOnScroll = readScalar<bool>(
            wave, "follow_measurement_cursors_on_scroll", config.gui.wave.followMeasurementCursorsOnScroll);
        config.gui.wave.cursorFftHighlightRgba =
            readFloat4(wave, "cursor_fft_highlight_rgba", config.gui.wave.cursorFftHighlightRgba);
        config.gui.wave.fullscreenMode = parseWaveFullscreenMode(
            readScalar<std::string>(wave, "fullscreen_mode", toWaveFullscreenModeText(config.gui.wave.fullscreenMode)),
            config.gui.wave.fullscreenMode);
        if (const auto resetViewport = childNode(wave, "reset_viewport")) {
            auto& policy = config.gui.wave.resetViewport;
            policy.applyOnPlotSetupReset = readScalar<bool>(
                resetViewport, "apply_on_plot_setup_reset", policy.applyOnPlotSetupReset);
            policy.applyOnManualClear =
                readScalar<bool>(resetViewport, "apply_on_manual_clear", policy.applyOnManualClear);
            policy.applyOnRawImport = readScalar<bool>(resetViewport, "apply_on_raw_import", policy.applyOnRawImport);
            policy.xScale = parseWaveResetViewportScaleMode(
                readScalar<std::string>(resetViewport, "x_scale", toWaveResetViewportScaleModeText(policy.xScale)),
                policy.xScale);
            policy.yScale = parseWaveResetViewportScaleMode(
                readScalar<std::string>(resetViewport, "y_scale", toWaveResetViewportScaleModeText(policy.yScale)),
                policy.yScale);
            policy.xAnchor = parseWaveResetViewportAnchor(
                readScalar<std::string>(resetViewport, "x_anchor", toWaveResetViewportAnchorText(policy.xAnchor)),
                policy.xAnchor);
            policy.autoFollow = parseWaveResetViewportAutoFollowMode(
                readScalar<std::string>(
                    resetViewport, "auto_follow", toWaveResetViewportAutoFollowModeText(policy.autoFollow)),
                policy.autoFollow);
        }
    }

    void loadGuiWaveScopedRuntimeConfig(const YAML::Node& gui, AppConfig& config)
    {
        // 保持旧版读取语义：这些 GUI 字段当前只在 gui.wave 节点存在时才会从配置覆盖。
        if (const auto logHistory = childNode(gui, "log_history")) {
            config.gui.logHistory.transferRawLimit =
                readScalar<std::size_t>(logHistory, "transfer_raw_limit", config.gui.logHistory.transferRawLimit);
            config.gui.logHistory.transferFrameLimit =
                readScalar<std::size_t>(logHistory, "transfer_frame_limit", config.gui.logHistory.transferFrameLimit);
            config.gui.logHistory.hostLimit =
                readScalar<std::size_t>(logHistory, "host_limit", config.gui.logHistory.hostLimit);
            config.gui.logHistory.scriptLimit =
                readScalar<std::size_t>(logHistory, "script_limit", config.gui.logHistory.scriptLimit);
            config.gui.logHistory.requestTraceLimit =
                readScalar<std::size_t>(logHistory, "request_trace_limit", config.gui.logHistory.requestTraceLimit);
        }
        if (const auto rawCapture = childNode(gui, "raw_capture")) {
            config.gui.rawCapture.liveLimitBytes =
                readScalar<std::size_t>(rawCapture, "live_limit_bytes", config.gui.rawCapture.liveLimitBytes);
            config.gui.rawCapture.recordingQueueLimitBytes = readScalar<std::size_t>(
                rawCapture, "recording_queue_limit_bytes", config.gui.rawCapture.recordingQueueLimitBytes);
        }
        if (const auto transferLog = childNode(gui, "transfer_log")) {
            config.gui.replayRawHistoryOnSchemaSwitch = readScalar<bool>(
                transferLog, "replay_raw_history_on_schema_switch", config.gui.replayRawHistoryOnSchemaSwitch);
        }
        if (const auto realtimeBacklog = childNode(gui, "realtime_backlog")) {
            config.gui.realtimeBacklog.mode =
                readScalar<std::string>(realtimeBacklog, "mode", config.gui.realtimeBacklog.mode);
            readPerformanceSize(realtimeBacklog,
                                "rx_chunk_bytes_per_pump",
                                config.gui.realtimeBacklog.rxChunkBytesPerPump,
                                config.performance.explicitOverrides.realtimeBacklogRxChunkBytesPerPump);
            readPerformanceSize(realtimeBacklog,
                                "transfer_frame_rows_per_pump",
                                config.gui.realtimeBacklog.transferFrameRowsPerPump,
                                config.performance.explicitOverrides.realtimeBacklogTransferFrameRowsPerPump);
            readPerformanceSize(realtimeBacklog,
                                "plot_appends_per_pump",
                                config.gui.realtimeBacklog.plotAppendsPerPump,
                                config.performance.explicitOverrides.realtimeBacklogPlotAppendsPerPump);
            readPerformanceSize(realtimeBacklog,
                                "raw_first_backlog_warn_bytes",
                                config.gui.realtimeBacklog.rawFirstBacklogWarnBytes,
                                config.performance.explicitOverrides.realtimeBacklogRawFirstBacklogWarnBytes);
            config.gui.realtimeBacklog.derivedBacklogDegradeEnabled =
                readScalar<bool>(realtimeBacklog,
                                 "derived_backlog_degrade_enabled",
                                 config.gui.realtimeBacklog.derivedBacklogDegradeEnabled);
            config.gui.realtimeBacklog.discardBacklogOnDisconnect =
                readScalar<bool>(realtimeBacklog,
                                 "discard_backlog_on_disconnect",
                                 config.gui.realtimeBacklog.discardBacklogOnDisconnect);
            config.gui.realtimeBacklog.pumpMinIntervalMs = readScalar<double>(
                realtimeBacklog, "pump_min_interval_ms", config.gui.realtimeBacklog.pumpMinIntervalMs);
        }
        config.gui.showAppHeader = readScalar<bool>(gui, "show_app_header", config.gui.showAppHeader);
        config.gui.luaDockLayoutDebug = readScalar<bool>(gui, "lua_dock_layout_debug", config.gui.luaDockLayoutDebug);
        config.gui.luaDockRenderCopyMode =
            readScalar<bool>(gui, "lua_dock_render_copy_mode", config.gui.luaDockRenderCopyMode);
        config.gui.sendHistoryLimit = readScalar<std::size_t>(gui, "send_history_limit", config.gui.sendHistoryLimit);
    }

    void loadGuiElfSymbolComboConfig(const YAML::Node& gui, AppConfig& config)
    {
        if (const auto elfSymbolCombo = childNode(gui, "elf_symbol_combo")) {
            const int limit =
                readScalar<int>(elfSymbolCombo, "limit", static_cast<int>(config.gui.elfSymbolCombo.limit));
            if (limit > 0) {
                config.gui.elfSymbolCombo.limit = static_cast<std::size_t>(limit);
            }
            const int debounceMs = readScalar<int>(elfSymbolCombo, "debounce_ms", config.gui.elfSymbolCombo.debounceMs);
            if (debounceMs > 0) {
                config.gui.elfSymbolCombo.debounceMs = debounceMs;
            }
            config.gui.elfSymbolCombo.autoRefreshSelectedAddress = readScalar<bool>(
                elfSymbolCombo, "auto_refresh_selected_address", config.gui.elfSymbolCombo.autoRefreshSelectedAddress);
            config.gui.elfSymbolCombo.autoRefreshEmitOnControl = readScalar<bool>(
                elfSymbolCombo, "auto_refresh_emit_on_control", config.gui.elfSymbolCombo.autoRefreshEmitOnControl);
        }
    }

    void loadGuiConfig(const YAML::Node& root, AppConfig& config)
    {
        const auto gui = root["gui"];
        const auto rendererBackendText =
            readScalar<std::string>(gui, "renderer_backend", toRendererBackendText(config.gui.rendererBackend));
        config.gui.rendererBackend =
            parseGuiRendererBackend(rendererBackendText).value_or(GuiRendererBackend::OpenGL);
        loadGuiWindowConfig(gui, config);
        loadGuiFontConfig(gui, config);
        loadGuiInteractionFeedbackConfig(gui, config);
        if (const auto wave = childNode(gui, "wave")) {
            loadGuiWaveConfig(wave, config);
            loadGuiWaveScopedRuntimeConfig(gui, config);
        }
        loadGuiElfSymbolComboConfig(gui, config);
    }

    void loadProtocolConfig(const YAML::Node& root, AppConfig& config)
    {
        const auto protocol = root["protocol"];
        config.protocol.rootDir = readScalar<std::string>(protocol, "root_dir", config.protocol.rootDir);
        config.protocol.selectedDir = readScalar<std::string>(protocol, "selected_dir", config.protocol.selectedDir);
        if (const auto tx = childNode(protocol, "tx")) {
            config.protocol.tx.sendTimeoutMs =
                readScalar<std::uint64_t>(tx, "send_timeout_ms", config.protocol.tx.sendTimeoutMs);
            config.protocol.tx.requestTimeoutMs =
                readScalar<std::uint64_t>(tx, "request_timeout_ms", config.protocol.tx.requestTimeoutMs);
            config.protocol.tx.maxPending = readScalar<std::size_t>(tx, "max_pending", config.protocol.tx.maxPending);
            config.protocol.tx.overflowPolicy =
                readScalar<std::string>(tx, "overflow_policy", config.protocol.tx.overflowPolicy);
            config.protocol.tx.overflowNotify =
                readScalar<std::string>(tx, "overflow_notify", config.protocol.tx.overflowNotify);
        }
    }

    void loadReceiveConfig(const YAML::Node& root, AppConfig& config)
    {
        if (const auto receive = root["receive"]) {
            readPerformanceSize(receive,
                                "transport_read_buffer_bytes",
                                config.receive.transportReadBufferBytes,
                                config.performance.explicitOverrides.receiveTransportReadBufferBytes);
            if (const auto streamBuffer = childNode(receive, "stream_buffer")) {
                config.receive.streamBuffer.nearOverflowThreshold = readScalar<double>(
                    streamBuffer, "near_overflow_threshold", config.receive.streamBuffer.nearOverflowThreshold);
                config.receive.streamBuffer.popupEnabled =
                    readScalar<bool>(streamBuffer, "popup_enabled", config.receive.streamBuffer.popupEnabled);
            }
        }
    }

    void loadScriptingWorkerConfig(const YAML::Node& scripting, AppConfig& config)
    {
        if (const auto pipeline = childNode(scripting, "pipeline")) {
            if (pipeline["worker_threads"]) {
                config.scripting.pipeline.workerThreads = readScalar<std::size_t>(pipeline, "worker_threads", 1U);
            }
        }
        if (const auto worker = childNode(scripting, "worker")) {
            config.scripting.workerEnabled = readScalar<bool>(worker, "enabled", config.scripting.workerEnabled);
            readPerformanceSize(worker,
                                "rx_queue_limit_bytes",
                                config.scripting.workerRxQueueLimitBytes,
                                config.performance.explicitOverrides.workerRxQueueLimitBytes);
            readPerformanceSize(worker,
                                "memory_budget_bytes",
                                config.scripting.workerMemoryBudgetBytes,
                                config.performance.explicitOverrides.workerMemoryBudgetBytes);
            config.scripting.workerMemoryBudgetAvailableRatio = readScalar<double>(
                worker, "memory_budget_available_ratio", config.scripting.workerMemoryBudgetAvailableRatio);
            readPerformanceSize(worker,
                                "output_queue_limit",
                                config.scripting.workerOutputQueueLimit,
                                config.performance.explicitOverrides.workerOutputQueueLimit);
            readPerformanceSize(worker,
                                "batch_bytes",
                                config.scripting.workerBatchBytes,
                                config.performance.explicitOverrides.workerBatchBytes);
            config.scripting.workerBackpressureEnabled =
                readScalar<bool>(worker, "backpressure_enabled", config.scripting.workerBackpressureEnabled);
            config.scripting.workerBackpressureHighWatermark = readScalar<double>(
                worker, "backpressure_rx_queue_high_watermark", config.scripting.workerBackpressureHighWatermark);
            config.scripting.workerBackpressureLowWatermark = readScalar<double>(
                worker, "backpressure_rx_queue_low_watermark", config.scripting.workerBackpressureLowWatermark);
            readPerformanceDouble(worker,
                                  "output_flush_budget_ms",
                                  config.scripting.workerOutputFlushBudgetMs,
                                  config.performance.explicitOverrides.workerOutputFlushBudgetMs);
            config.scripting.drainRequestOutputsUnbounded = readScalar<bool>(
                worker, "drain_request_outputs_unbounded", config.scripting.drainRequestOutputsUnbounded);
        }
    }

    void loadScriptingFileIoConfig(const YAML::Node& scripting, AppConfig& appConfig)
    {
        if (const auto fileIo = childNode(scripting, "file_io")) {
            auto& config = appConfig.scripting.fileIo;
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
    }

    void loadScriptingConfig(const YAML::Node& root, AppConfig& config)
    {
        const auto scripting = root["scripting"];
        loadScriptingWorkerConfig(scripting, config);
        loadScriptingFileIoConfig(scripting, config);
    }

    void loadLoggingConfig(const YAML::Node& root, AppConfig& config)
    {
        const auto logging = root["logging"];
        config.logging.level =
            parseLogLevel(readScalar<std::string>(logging, "level", toLogLevelText(config.logging.level)));
        config.logging.filePath = readScalar<std::string>(logging, "file_path", config.logging.filePath);
        config.logging.maxFileSizeBytes =
            readScalar<std::size_t>(logging, "max_file_size_bytes", config.logging.maxFileSizeBytes);
        config.logging.maxFiles = readScalar<std::size_t>(logging, "max_files", config.logging.maxFiles);
        if (const auto payloadPreview = childNode(logging, "payload_preview")) {
            config.logging.payloadPreview.enabled =
                readScalar<bool>(payloadPreview, "enabled", config.logging.payloadPreview.enabled);
            config.logging.payloadPreview.maxBytes =
                readScalar<std::size_t>(payloadPreview, "max_bytes", config.logging.payloadPreview.maxBytes);
        }
    }

    void loadCommunicationConfig(const YAML::Node& root, AppConfig& config)
    {
        const auto communication = root["communication"];
        config.communication.kind = parseTransportKind(
            readScalar<std::string>(communication, "kind", toTransportKindText(config.communication.kind)));

        if (const auto tcpClient = childNode(communication, "tcp_client")) {
            config.communication.tcpClient.host =
                readScalar<std::string>(tcpClient, "host", config.communication.tcpClient.host);
            config.communication.tcpClient.port =
                readScalar<std::uint16_t>(tcpClient, "port", config.communication.tcpClient.port);
        }

        if (const auto tcpServer = childNode(communication, "tcp_server")) {
            config.communication.tcpServer.bindAddress =
                readScalar<std::string>(tcpServer, "bind_address", config.communication.tcpServer.bindAddress);
            config.communication.tcpServer.port =
                readScalar<std::uint16_t>(tcpServer, "port", config.communication.tcpServer.port);
            config.communication.tcpServer.rejectNewConnection = readScalar<bool>(
                tcpServer, "reject_new_connection", config.communication.tcpServer.rejectNewConnection);
        }

        if (const auto serial = childNode(communication, "serial")) {
            config.communication.serial.portName =
                readScalar<std::string>(serial, "port_name", config.communication.serial.portName);
            config.communication.serial.baudRate =
                readScalar<std::uint32_t>(serial, "baud_rate", config.communication.serial.baudRate);
            config.communication.serial.dataBits =
                readScalar<std::uint32_t>(serial, "data_bits", config.communication.serial.dataBits);
            config.communication.serial.parity =
                readScalar<std::string>(serial, "parity", config.communication.serial.parity);
            config.communication.serial.stopBits =
                readScalar<std::string>(serial, "stop_bits", config.communication.serial.stopBits);
            config.communication.serial.flowControl =
                readScalar<std::string>(serial, "flow_control", config.communication.serial.flowControl);
        }

        if (const auto udpPeer = childNode(communication, "udp_peer")) {
            config.communication.udpPeer.bindAddress =
                readScalar<std::string>(udpPeer, "bind_address", config.communication.udpPeer.bindAddress);
            config.communication.udpPeer.bindPort =
                readScalar<std::uint16_t>(udpPeer, "bind_port", config.communication.udpPeer.bindPort);
            config.communication.udpPeer.remoteHost =
                readScalar<std::string>(udpPeer, "remote_host", config.communication.udpPeer.remoteHost);
            config.communication.udpPeer.remotePort =
                readScalar<std::uint16_t>(udpPeer, "remote_port", config.communication.udpPeer.remotePort);
        }
        if (const auto receive = root["receive"]) {
            config.communication.serialPortOptions = kDefaultSerialPorts;
            config.communication.reconnectRequired = false;
            config.communication.lastError.clear();
            config.communication.txCount = 0;
            config.communication.rxCount = 0;
            (void) receive;
        }
    }

    void writePerformanceConfig(YAML::Node& root, const AppConfig& config)
    {
        root["performance"]["scale"] = normalizePerformanceScale(config.performance.scale);
        root["performance"]["adaptive"]["enabled"] = config.performance.adaptive.enabled;
        root["performance"]["adaptive"]["max_multiplier"] =
            normalizeAdaptiveMaxMultiplier(config.performance.adaptive.maxMultiplier);
    }

    void writeAppConfig(YAML::Node& root, const AppConfig& config)
    {
        root["app"]["language"] = config.app.language;
        root["app"]["fps_limit"] = config.app.fpsLimit;
        root["app"]["idle_render"] = config.app.idleRender;
        root["app"]["auto_save"]["enabled"] = config.app.autoSave.enabled;
        root["app"]["auto_save"]["interval_ms"] = config.app.autoSave.intervalMs;
        root["app"]["config_hot_reload"]["enabled"] = config.app.configHotReload.enabled;
    }

    void writeGuiWaveConfig(YAML::Node& gui, const AppConfig& config)
    {
        gui["wave"]["control_mode"] = toWaveControlModeText(config.gui.wave.controlMode);
        gui["wave"]["display_formula"] = toWaveDisplayFormulaText(config.gui.wave.displayFormula);
        gui["wave"]["grid_division_readout_mode"] =
            toWaveGridDivisionReadoutModeText(config.gui.wave.gridDivisionReadoutMode);
        gui["wave"]["channel_scale_wheel"]["enabled"] = config.gui.wave.channelScaleWheelEnabled;
        gui["wave"]["channel_scale_wheel"]["acceleration"] =
            toWaveChannelScaleWheelAccelerationText(config.gui.wave.channelScaleWheelAcceleration);
        gui["wave"]["channel_card_width_mode"] = toWaveChannelCardWidthModeText(config.gui.wave.channelCardWidthMode);
        gui["wave"]["channel_double_click_action"] =
            toWaveChannelDoubleClickActionText(config.gui.wave.channelDoubleClickAction);
        gui["wave"]["x_axis_double_click_action"] =
            toWaveXAxisDoubleClickActionText(config.gui.wave.xAxisDoubleClickAction);
        gui["wave"]["y_axis_double_click_action"] =
            toWaveYAxisDoubleClickActionText(config.gui.wave.yAxisDoubleClickAction);
        gui["wave"]["y_axis_double_click_adjust_offset"] = config.gui.wave.yAxisDoubleClickAdjustOffset;
        gui["wave"]["hidden_channel_policy"] = toWaveHiddenChannelPolicyText(config.gui.wave.hiddenChannelPolicy);
        gui["wave"]["cursor_extreme_snap_policy"] =
            toWaveCursorExtremeSnapPolicyText(config.gui.wave.cursorExtremeSnapPolicy);
        gui["wave"]["mouse_y_offset_drag_mode"] = toWaveMouseYOffsetDragModeText(config.gui.wave.mouseYOffsetDragMode);
        gui["wave"]["legend_overlay_open_mode"] =
            toWaveLegendOverlayOpenModeText(config.gui.wave.legendOverlayOpenMode);
        gui["wave"]["legend_overlay_double_click_auto_collapse"] = config.gui.wave.legendOverlayDoubleClickAutoCollapse;
        gui["wave"]["interaction_animation_enabled"] = config.gui.wave.interactionAnimationEnabled;
        gui["wave"]["zoom_selection_auto_exit"] = config.gui.wave.zoomSelectionAutoExit;
        gui["wave"]["peak_detect_downsample"] = config.gui.wave.peakDetectDownsample;
        gui["wave"]["channel_card_fixed_width"] = config.gui.wave.channelCardFixedWidth;
        gui["wave"]["channel_card_adaptive_ratio"] = config.gui.wave.channelCardAdaptiveRatio;
        gui["wave"]["legend_channel_name_max_width"] = config.gui.wave.legendChannelNameMaxWidth;
        gui["wave"]["vertical_auto_fit_multiplier"] = config.gui.wave.verticalAutoFitMultiplier;
        gui["wave"]["max_render_points_per_channel"] = config.gui.wave.maxRenderPointsPerChannel;
        gui["wave"]["max_render_vertices"] = config.gui.wave.maxRenderVertices;
        gui["wave"]["downsample_start_multiplier"] = config.gui.wave.downsampleStartMultiplier;
        gui["wave"]["overview_max_samples"] = config.gui.wave.overviewMaxSamples;
        gui["wave"]["max_total_samples"] = config.gui.wave.maxTotalSamples;
        gui["wave"]["min_visible_time_span"] = config.gui.wave.minVisibleTimeSpan;
        gui["wave"]["reset_history_on_time_reset"] = config.gui.wave.resetHistoryOnTimeReset;
        gui["wave"]["show_axis_labels"] = config.gui.wave.showAxisLabels;
        gui["wave"]["show_channel_legend"] = config.gui.wave.showChannelLegend;
        gui["wave"]["show_fft_legend"] = config.gui.wave.showFftLegend;
        gui["wave"]["follow_measurement_cursors_on_scroll"] = config.gui.wave.followMeasurementCursorsOnScroll;
        gui["wave"]["cursor_fft_highlight_rgba"] = makeFloat4Node(config.gui.wave.cursorFftHighlightRgba);
        gui["wave"]["fullscreen_mode"] = toWaveFullscreenModeText(config.gui.wave.fullscreenMode);
        gui["wave"]["reset_viewport"]["apply_on_plot_setup_reset"] =
            config.gui.wave.resetViewport.applyOnPlotSetupReset;
        gui["wave"]["reset_viewport"]["apply_on_manual_clear"] = config.gui.wave.resetViewport.applyOnManualClear;
        gui["wave"]["reset_viewport"]["apply_on_raw_import"] = config.gui.wave.resetViewport.applyOnRawImport;
        gui["wave"]["reset_viewport"]["x_scale"] =
            toWaveResetViewportScaleModeText(config.gui.wave.resetViewport.xScale);
        gui["wave"]["reset_viewport"]["y_scale"] =
            toWaveResetViewportScaleModeText(config.gui.wave.resetViewport.yScale);
        gui["wave"]["reset_viewport"]["x_anchor"] =
            toWaveResetViewportAnchorText(config.gui.wave.resetViewport.xAnchor);
        gui["wave"]["reset_viewport"]["auto_follow"] =
            toWaveResetViewportAutoFollowModeText(config.gui.wave.resetViewport.autoFollow);
    }

    void writeGuiRuntimeConfig(YAML::Node& gui, const AppConfig& config, const AppConfig& scaledDefaults)
    {
        const auto& explicitOverrides = config.performance.explicitOverrides;

        gui["interaction_feedback"]["enabled"] = config.gui.interactionFeedback.enabled;
        gui["interaction_feedback"]["status_duration_ms"] = config.gui.interactionFeedback.statusDurationMs;
        gui["log_history"]["transfer_raw_limit"] = config.gui.logHistory.transferRawLimit;
        gui["log_history"]["transfer_frame_limit"] = config.gui.logHistory.transferFrameLimit;
        gui["log_history"]["host_limit"] = config.gui.logHistory.hostLimit;
        gui["log_history"]["script_limit"] = config.gui.logHistory.scriptLimit;
        gui["log_history"]["request_trace_limit"] = config.gui.logHistory.requestTraceLimit;
        gui["raw_capture"]["live_limit_bytes"] = config.gui.rawCapture.liveLimitBytes;
        gui["raw_capture"]["recording_queue_limit_bytes"] = config.gui.rawCapture.recordingQueueLimitBytes;
        gui["transfer_log"]["replay_raw_history_on_schema_switch"] = config.gui.replayRawHistoryOnSchemaSwitch;
        gui["realtime_backlog"]["mode"] = config.gui.realtimeBacklog.mode;
        writePerformanceScalar(gui["realtime_backlog"],
                               "rx_chunk_bytes_per_pump",
                               config.gui.realtimeBacklog.rxChunkBytesPerPump,
                               scaledDefaults.gui.realtimeBacklog.rxChunkBytesPerPump,
                               explicitOverrides.realtimeBacklogRxChunkBytesPerPump);
        writePerformanceScalar(gui["realtime_backlog"],
                               "transfer_frame_rows_per_pump",
                               config.gui.realtimeBacklog.transferFrameRowsPerPump,
                               scaledDefaults.gui.realtimeBacklog.transferFrameRowsPerPump,
                               explicitOverrides.realtimeBacklogTransferFrameRowsPerPump);
        writePerformanceScalar(gui["realtime_backlog"],
                               "plot_appends_per_pump",
                               config.gui.realtimeBacklog.plotAppendsPerPump,
                               scaledDefaults.gui.realtimeBacklog.plotAppendsPerPump,
                               explicitOverrides.realtimeBacklogPlotAppendsPerPump);
        writePerformanceScalar(gui["realtime_backlog"],
                               "raw_first_backlog_warn_bytes",
                               config.gui.realtimeBacklog.rawFirstBacklogWarnBytes,
                               scaledDefaults.gui.realtimeBacklog.rawFirstBacklogWarnBytes,
                               explicitOverrides.realtimeBacklogRawFirstBacklogWarnBytes);
        gui["realtime_backlog"]["derived_backlog_degrade_enabled"] =
            config.gui.realtimeBacklog.derivedBacklogDegradeEnabled;
        gui["realtime_backlog"]["discard_backlog_on_disconnect"] =
            config.gui.realtimeBacklog.discardBacklogOnDisconnect;
        gui["realtime_backlog"]["pump_min_interval_ms"] = config.gui.realtimeBacklog.pumpMinIntervalMs;
        gui["show_app_header"] = config.gui.showAppHeader;
        gui["send_history_limit"] = config.gui.sendHistoryLimit;
        gui["lua_dock_layout_debug"] = config.gui.luaDockLayoutDebug;
        gui["lua_dock_render_copy_mode"] = config.gui.luaDockRenderCopyMode;
    }

    void writeGuiElfSymbolComboConfig(YAML::Node& gui, const AppConfig& config)
    {
        gui["elf_symbol_combo"]["limit"] = config.gui.elfSymbolCombo.limit;
        gui["elf_symbol_combo"]["debounce_ms"] = config.gui.elfSymbolCombo.debounceMs;
        gui["elf_symbol_combo"]["auto_refresh_selected_address"] = config.gui.elfSymbolCombo.autoRefreshSelectedAddress;
        gui["elf_symbol_combo"]["auto_refresh_emit_on_control"] = config.gui.elfSymbolCombo.autoRefreshEmitOnControl;
    }

    void writeGuiFontConfig(YAML::Node& gui, const AppConfig& config)
    {
        gui["font"]["chinese_glyph_range"] = toFontChineseGlyphRangeText(config.gui.font.chineseGlyphRange);
    }

    void writeGuiConfig(YAML::Node& root, const AppConfig& config, const AppConfig& scaledDefaults)
    {
        auto gui = root["gui"];
        gui["renderer_backend"] = toRendererBackendText(config.gui.rendererBackend);
        gui["window"]["title"] = config.gui.window.title;
        gui["window"]["width"] = config.gui.window.width;
        gui["window"]["height"] = config.gui.window.height;
        gui["window"]["maximized"] = config.gui.window.maximized;
        writeGuiFontConfig(gui, config);
        writeGuiWaveConfig(gui, config);
        writeGuiRuntimeConfig(gui, config, scaledDefaults);
        writeGuiElfSymbolComboConfig(gui, config);
    }

    void writeProtocolConfig(YAML::Node& root, const AppConfig& config)
    {
        root["protocol"]["root_dir"] = config.protocol.rootDir;
        root["protocol"]["selected_dir"] = config.protocol.selectedDir;
        root["protocol"]["tx"]["send_timeout_ms"] = config.protocol.tx.sendTimeoutMs;
        root["protocol"]["tx"]["request_timeout_ms"] = config.protocol.tx.requestTimeoutMs;
        root["protocol"]["tx"]["max_pending"] = config.protocol.tx.maxPending;
        root["protocol"]["tx"]["overflow_policy"] = config.protocol.tx.overflowPolicy;
        root["protocol"]["tx"]["overflow_notify"] = config.protocol.tx.overflowNotify;
    }

    void writeReceiveConfig(YAML::Node& root, const AppConfig& config, const AppConfig& scaledDefaults)
    {
        const auto& explicitOverrides = config.performance.explicitOverrides;

        root["receive"]["stream_buffer"]["near_overflow_threshold"] = config.receive.streamBuffer.nearOverflowThreshold;
        root["receive"]["stream_buffer"]["popup_enabled"] = config.receive.streamBuffer.popupEnabled;
        writePerformanceScalar(root["receive"],
                               "transport_read_buffer_bytes",
                               config.receive.transportReadBufferBytes,
                               scaledDefaults.receive.transportReadBufferBytes,
                               explicitOverrides.receiveTransportReadBufferBytes);
    }

    void writeScriptingWorkerConfig(YAML::Node& scripting, const AppConfig& config, const AppConfig& scaledDefaults)
    {
        const auto& explicitOverrides = config.performance.explicitOverrides;

        if (config.scripting.pipeline.workerThreads.has_value()) {
            scripting["pipeline"]["worker_threads"] = *config.scripting.pipeline.workerThreads;
        }
        scripting["worker"]["enabled"] = config.scripting.workerEnabled;
        writePerformanceScalar(scripting["worker"],
                               "rx_queue_limit_bytes",
                               config.scripting.workerRxQueueLimitBytes,
                               scaledDefaults.scripting.workerRxQueueLimitBytes,
                               explicitOverrides.workerRxQueueLimitBytes);
        writePerformanceScalar(scripting["worker"],
                               "memory_budget_bytes",
                               config.scripting.workerMemoryBudgetBytes,
                               scaledDefaults.scripting.workerMemoryBudgetBytes,
                               explicitOverrides.workerMemoryBudgetBytes);
        scripting["worker"]["memory_budget_available_ratio"] = config.scripting.workerMemoryBudgetAvailableRatio;
        writePerformanceScalar(scripting["worker"],
                               "output_queue_limit",
                               config.scripting.workerOutputQueueLimit,
                               scaledDefaults.scripting.workerOutputQueueLimit,
                               explicitOverrides.workerOutputQueueLimit);
        writePerformanceScalar(scripting["worker"],
                               "batch_bytes",
                               config.scripting.workerBatchBytes,
                               scaledDefaults.scripting.workerBatchBytes,
                               explicitOverrides.workerBatchBytes);
        scripting["worker"]["backpressure_enabled"] = config.scripting.workerBackpressureEnabled;
        scripting["worker"]["backpressure_rx_queue_high_watermark"] = config.scripting.workerBackpressureHighWatermark;
        scripting["worker"]["backpressure_rx_queue_low_watermark"] = config.scripting.workerBackpressureLowWatermark;
        writePerformanceScalar(scripting["worker"],
                               "output_flush_budget_ms",
                               config.scripting.workerOutputFlushBudgetMs,
                               scaledDefaults.scripting.workerOutputFlushBudgetMs,
                               explicitOverrides.workerOutputFlushBudgetMs);
    }

    void writeScriptingFileIoConfig(YAML::Node& scripting, const AppConfig& config)
    {
        scripting["file_io"]["enabled"] = config.scripting.fileIo.enabled;
        scripting["file_io"]["allow_protocol_dir"] = config.scripting.fileIo.allowProtocolDir;
        scripting["file_io"]["allow_dialog_paths"] = config.scripting.fileIo.allowDialogPaths;
        for (const auto& rootPath : config.scripting.fileIo.extraAllowedRoots) {
            scripting["file_io"]["extra_allowed_roots"].push_back(rootPath);
        }
        if (config.scripting.fileIo.extraAllowedRoots.empty()) {
            scripting["file_io"]["extra_allowed_roots"] = YAML::Node(YAML::NodeType::Sequence);
        }
        scripting["file_io"]["max_open_files"] = config.scripting.fileIo.maxOpenFiles;
        scripting["file_io"]["default_chunk_bytes"] = config.scripting.fileIo.defaultChunkBytes;
        scripting["file_io"]["max_chunk_bytes"] = config.scripting.fileIo.maxChunkBytes;
        scripting["file_io"]["max_file_size_bytes"] = config.scripting.fileIo.maxFileSizeBytes;
        scripting["file_io"]["max_write_file_size_bytes"] = config.scripting.fileIo.maxWriteFileSizeBytes;
        scripting["file_io"]["dialog"]["enabled"] = config.scripting.fileIo.dialog.enabled;
        scripting["file_io"]["dialog"]["remember_last_dir"] = config.scripting.fileIo.dialog.rememberLastDir;
        scripting["file_io"]["send_file"]["default_chunk_bytes"] = config.scripting.fileIo.sendFile.defaultChunkBytes;
        scripting["file_io"]["send_file"]["max_inflight_chunks"] = config.scripting.fileIo.sendFile.maxInflightChunks;
    }

    void writeScriptingConfig(YAML::Node& root, const AppConfig& config, const AppConfig& scaledDefaults)
    {
        auto scripting = root["scripting"];
        writeScriptingWorkerConfig(scripting, config, scaledDefaults);
        writeScriptingFileIoConfig(scripting, config);
    }

    void writeLoggingConfig(YAML::Node& root, const AppConfig& config)
    {
        root["logging"]["level"] = toLogLevelText(config.logging.level);
        if (!config.logging.filePath.empty()) {
            root["logging"]["file_path"] = config.logging.filePath;
        }
        root["logging"]["payload_preview"]["enabled"] = config.logging.payloadPreview.enabled;
        root["logging"]["payload_preview"]["max_bytes"] = config.logging.payloadPreview.maxBytes;
        root["logging"]["max_file_size_bytes"] = config.logging.maxFileSizeBytes;
        root["logging"]["max_files"] = config.logging.maxFiles;
    }

    void writeCommunicationConfig(YAML::Node& root, const AppConfig& config)
    {
        root["communication"]["kind"] = toTransportKindText(config.communication.kind);
        root["communication"]["tcp_client"]["host"] = config.communication.tcpClient.host;
        root["communication"]["tcp_client"]["port"] = config.communication.tcpClient.port;
        root["communication"]["tcp_server"]["bind_address"] = config.communication.tcpServer.bindAddress;
        root["communication"]["tcp_server"]["port"] = config.communication.tcpServer.port;
        root["communication"]["tcp_server"]["reject_new_connection"] =
            config.communication.tcpServer.rejectNewConnection;
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
    }

    YAML::Node buildConfigYamlRoot(const AppConfig& config, AppConfig scaledDefaults)
    {
        YAML::Node root;
        scaledDefaults.performance.scale = normalizePerformanceScale(config.performance.scale);
        scaledDefaults.performance.adaptive = config.performance.adaptive;
        if (!scaledDefaults.performance.adaptive.enabled) {
            applyPerformanceScale(scaledDefaults);
        }

        // 核心流程：文件保存和现场包内存保存必须走同一套字段写入逻辑，避免配置格式分叉。
        writePerformanceConfig(root, config);
        writeAppConfig(root, config);
        writeGuiConfig(root, config, scaledDefaults);
        writeProtocolConfig(root, config);
        writeReceiveConfig(root, config, scaledDefaults);
        writeScriptingConfig(root, config, scaledDefaults);
        writeLoggingConfig(root, config);
        writeCommunicationConfig(root, config);
        return root;
    }

    void loadConfigYamlRoot(const YAML::Node& root, AppConfig& config)
    {
        loadPerformanceConfig(root, config);
        loadAppConfig(root, config);
        loadGuiConfig(root, config);
        loadProtocolConfig(root, config);
        loadReceiveConfig(root, config);
        loadScriptingConfig(root, config);
        loadLoggingConfig(root, config);
        loadCommunicationConfig(root, config);
    }

    bool writeConfigYamlFile(const std::filesystem::path& path, const YAML::Node& root, std::string& error)
    {
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
} // namespace

std::optional<GuiRendererBackend> parseGuiRendererBackend(std::string_view value)
{
    const auto normalized = normalizeRendererBackendText(value);
    for (const auto& pair : kRendererBackendNames) {
        if (pair.name == normalized) {
            return pair.value;
        }
    }
    return std::nullopt;
}

std::string_view guiRendererBackendId(const GuiRendererBackend backend)
{
    return toRendererBackendText(backend);
}

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

        loadConfigYamlRoot(root, result.config);
        result.config.configPath = normalizeTextPath(result.resolvedPath);
    } catch (const std::exception& ex) {
        result.error = std::string("读取 YAML 失败: ") + ex.what();
        result.loadedFromDisk = false;
    }

    return result;
}

ConfigLoadResult ConfigStore::loadText(std::string_view yamlText, const std::filesystem::path& sourcePath) const
{
    ConfigLoadResult result;
    result.config = withDefaults();
    result.resolvedPath = sourcePath.empty() ? defaultConfigPath_ : sourcePath;

    try {
        const YAML::Node root = YAML::Load(std::string(yamlText));
        result.loadedFromDisk = true;
        loadConfigYamlRoot(root, result.config);
        result.config.configPath = normalizeTextPath(result.resolvedPath);
    } catch (const std::exception& ex) {
        result.error = std::string("读取 YAML 文本失败: ") + ex.what();
        result.loadedFromDisk = false;
    }
    return result;
}

bool ConfigStore::save(const std::filesystem::path& path, const AppConfig& config, std::string& error) const
{
    const auto root = buildConfigYamlRoot(config, withDefaults());
    return writeConfigYamlFile(path, root, error);
}

bool ConfigStore::saveText(const AppConfig& config, std::string& yamlText, std::string& error) const
{
    try {
        const auto root = buildConfigYamlRoot(config, withDefaults());
        yamlText = YAML::Dump(root);
        return true;
    } catch (const std::exception& ex) {
        error = std::string("生成 YAML 文本失败: ") + ex.what();
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

    for (const auto& entry :
         std::filesystem::directory_iterator(rootDir, std::filesystem::directory_options::skip_permission_denied, ec)) {
        if (ec) {
            break;
        }
        std::error_code entryError;
        if (!entry.is_directory(entryError) || entryError) {
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
    wave.gridDivisionReadoutMode = config.gui.wave.gridDivisionReadoutMode;
    wave.channelScaleWheelEnabled = config.gui.wave.channelScaleWheelEnabled;
    wave.channelScaleWheelAcceleration = config.gui.wave.channelScaleWheelAcceleration;
    wave.channelScaleWheelState = {};
    wave.channelCardWidthMode = config.gui.wave.channelCardWidthMode;
    wave.channelDoubleClickAction = config.gui.wave.channelDoubleClickAction;
    wave.xAxisDoubleClickAction = config.gui.wave.xAxisDoubleClickAction;
    wave.yAxisDoubleClickAction = config.gui.wave.yAxisDoubleClickAction;
    wave.yAxisDoubleClickAdjustOffset = config.gui.wave.yAxisDoubleClickAdjustOffset;
    wave.mouseYOffsetDragMode = config.gui.wave.mouseYOffsetDragMode;
    wave.zoomSelectionAutoExit = config.gui.wave.zoomSelectionAutoExit;
    wave.peakDetectDownsample = config.gui.wave.peakDetectDownsample;
    wave.maxRenderPointsPerChannel = config.gui.wave.maxRenderPointsPerChannel;
    wave.maxRenderVertices = config.gui.wave.maxRenderVertices;
    wave.downsampleStartMultiplier = (std::max)(config.gui.wave.downsampleStartMultiplier, 1.0);
    wave.overviewMaxSamples = config.gui.wave.overviewMaxSamples;
    wave.minVisibleTimeSpan = config.gui.wave.minVisibleTimeSpan;
    wave.channelCardFixedWidth = positiveOrFallback(config.gui.wave.channelCardFixedWidth, 128.0);
    wave.channelCardAdaptiveRatio = positiveOrFallback(config.gui.wave.channelCardAdaptiveRatio, 0.22);
    wave.legendChannelNameMaxWidth = positiveOrZero(config.gui.wave.legendChannelNameMaxWidth);
    wave.verticalAutoFitMultiplier = positiveOrFallback(config.gui.wave.verticalAutoFitMultiplier, 1.25);
    wave.hiddenChannelPolicy = config.gui.wave.hiddenChannelPolicy;
    wave.cursorExtremeSnapPolicy = config.gui.wave.cursorExtremeSnapPolicy;
    wave.interactionAnimationEnabled = config.gui.wave.interactionAnimationEnabled;
    wave.effectiveInteractionAnimationEnabled =
        config.gui.interactionFeedback.enabled && config.gui.wave.interactionAnimationEnabled;
    wave.showAxisLabels = config.gui.wave.showAxisLabels;
    wave.showChannelLegend = config.gui.wave.showChannelLegend;
    wave.showFftLegend = config.gui.wave.showFftLegend;
    wave.followMeasurementCursorsOnScroll = config.gui.wave.followMeasurementCursorsOnScroll;
    wave.cursorFftHighlightRgba = config.gui.wave.cursorFftHighlightRgba;
    wave.resetViewportApplyOnPlotSetupReset = config.gui.wave.resetViewport.applyOnPlotSetupReset;
    wave.resetViewportApplyOnManualClear = config.gui.wave.resetViewport.applyOnManualClear;
    wave.resetViewportApplyOnRawImport = config.gui.wave.resetViewport.applyOnRawImport;
    wave.defaultViewportXScale = config.gui.wave.resetViewport.xScale;
    wave.defaultViewportYScale = config.gui.wave.resetViewport.yScale;
    wave.defaultViewportXAnchor = config.gui.wave.resetViewport.xAnchor;
    wave.defaultViewportAutoFollow = config.gui.wave.resetViewport.autoFollow;
    waveState.legendOverlay.openMode = config.gui.wave.legendOverlayOpenMode;
    waveState.legendOverlay.doubleClickAutoCollapse = config.gui.wave.legendOverlayDoubleClickAutoCollapse;
    if (waveState.legendOverlay.openMode != plot::WaveLegendOverlayOpenMode::DoubleClick) {
        waveState.legendOverlay.expanded = false;
    }
    waveState.legendOverlay.hoverFloating = false;
    waveState.legendOverlay.hoverInteractionLocked = false;
    waveState.legendOverlay.hoverCloseRemainingSec = 0.0F;
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
    config.gui.wave.gridDivisionReadoutMode = dockStore.waveState().view.gridDivisionReadoutMode;
    config.gui.wave.channelScaleWheelEnabled = dockStore.waveState().view.channelScaleWheelEnabled;
    config.gui.wave.channelScaleWheelAcceleration = dockStore.waveState().view.channelScaleWheelAcceleration;
    config.gui.wave.channelCardWidthMode = dockStore.waveState().view.channelCardWidthMode;
    config.gui.wave.channelDoubleClickAction = dockStore.waveState().view.channelDoubleClickAction;
    config.gui.wave.xAxisDoubleClickAction = dockStore.waveState().view.xAxisDoubleClickAction;
    config.gui.wave.yAxisDoubleClickAction = dockStore.waveState().view.yAxisDoubleClickAction;
    config.gui.wave.yAxisDoubleClickAdjustOffset = dockStore.waveState().view.yAxisDoubleClickAdjustOffset;
    config.gui.wave.mouseYOffsetDragMode = dockStore.waveState().view.mouseYOffsetDragMode;
    config.gui.wave.zoomSelectionAutoExit = dockStore.waveState().view.zoomSelectionAutoExit;
    config.gui.wave.peakDetectDownsample = dockStore.waveState().view.peakDetectDownsample;
    config.gui.wave.maxRenderPointsPerChannel = dockStore.waveState().view.maxRenderPointsPerChannel;
    config.gui.wave.maxRenderVertices = dockStore.waveState().view.maxRenderVertices;
    config.gui.wave.downsampleStartMultiplier = dockStore.waveState().view.downsampleStartMultiplier;
    config.gui.wave.overviewMaxSamples = dockStore.waveState().view.overviewMaxSamples;
    config.gui.wave.minVisibleTimeSpan = dockStore.waveState().view.minVisibleTimeSpan;
    config.gui.wave.channelCardFixedWidth = dockStore.waveState().view.channelCardFixedWidth;
    config.gui.wave.channelCardAdaptiveRatio = dockStore.waveState().view.channelCardAdaptiveRatio;
    config.gui.wave.legendChannelNameMaxWidth = positiveOrZero(dockStore.waveState().view.legendChannelNameMaxWidth);
    config.gui.wave.verticalAutoFitMultiplier = dockStore.waveState().view.verticalAutoFitMultiplier;
    config.gui.wave.hiddenChannelPolicy = dockStore.waveState().view.hiddenChannelPolicy;
    config.gui.wave.cursorExtremeSnapPolicy = dockStore.waveState().view.cursorExtremeSnapPolicy;
    config.gui.wave.interactionAnimationEnabled = dockStore.waveState().view.interactionAnimationEnabled;
    config.gui.wave.showAxisLabels = dockStore.waveState().view.showAxisLabels;
    config.gui.wave.showChannelLegend = dockStore.waveState().view.showChannelLegend;
    config.gui.wave.showFftLegend = dockStore.waveState().view.showFftLegend;
    config.gui.wave.followMeasurementCursorsOnScroll = dockStore.waveState().view.followMeasurementCursorsOnScroll;
    config.gui.wave.cursorFftHighlightRgba = dockStore.waveState().view.cursorFftHighlightRgba;
    config.gui.wave.resetViewport.applyOnPlotSetupReset =
        dockStore.waveState().view.resetViewportApplyOnPlotSetupReset;
    config.gui.wave.resetViewport.applyOnManualClear = dockStore.waveState().view.resetViewportApplyOnManualClear;
    config.gui.wave.resetViewport.applyOnRawImport = dockStore.waveState().view.resetViewportApplyOnRawImport;
    config.gui.wave.resetViewport.xScale = dockStore.waveState().view.defaultViewportXScale;
    config.gui.wave.resetViewport.yScale = dockStore.waveState().view.defaultViewportYScale;
    config.gui.wave.resetViewport.xAnchor = dockStore.waveState().view.defaultViewportXAnchor;
    config.gui.wave.resetViewport.autoFollow = dockStore.waveState().view.defaultViewportAutoFollow;
    config.gui.wave.legendOverlayOpenMode = dockStore.waveState().legendOverlay.openMode;
    config.gui.wave.legendOverlayDoubleClickAutoCollapse = dockStore.waveState().legendOverlay.doubleClickAutoCollapse;
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
