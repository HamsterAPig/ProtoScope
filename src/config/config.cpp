#include "protoscope/config/config.hpp"
#include "protoscope/config/embedded_protocols.hpp"

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <system_error>

namespace protoscope::config {

namespace {

template <typename T>
T readScalar(const YAML::Node& node, const char* key, T fallback) {
    if (!node || !node[key]) {
        return fallback;
    }
    return node[key].as<T>();
}

std::vector<std::string> readStringList(const YAML::Node& node, const char* key, std::vector<std::string> fallback) {
    if (!node || !node[key] || !node[key].IsSequence()) {
        return fallback;
    }

    std::vector<std::string> values;
    for (const auto& item : node[key]) {
        values.push_back(item.as<std::string>());
    }
    return values;
}

std::string normalizeTextPath(std::filesystem::path path) {
    path.make_preferred();
    return path.generic_string();
}

LogLevel parseLogLevel(const std::string& value) {
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

std::string toLogLevelText(const LogLevel level) {
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

plot::WaveControlMode parseWaveControlMode(const std::string& value, plot::WaveControlMode fallback) {
    if (value == "legacy_global") {
        return plot::WaveControlMode::LegacyGlobal;
    }
    if (value == "oscilloscope") {
        return plot::WaveControlMode::Oscilloscope;
    }
    return fallback;
}

const char* toWaveControlModeText(const plot::WaveControlMode mode) {
    switch (mode) {
    case plot::WaveControlMode::Oscilloscope:
        return "oscilloscope";
    case plot::WaveControlMode::LegacyGlobal:
        return "legacy_global";
    }
    return "oscilloscope";
}

plot::WaveDisplayFormula parseWaveDisplayFormula(const std::string& value, plot::WaveDisplayFormula fallback) {
    if (value == "scale_then_offset") {
        return plot::WaveDisplayFormula::ScaleThenOffset;
    }
    if (value == "offset_then_scale") {
        return plot::WaveDisplayFormula::OffsetThenScale;
    }
    return fallback;
}

const char* toWaveDisplayFormulaText(const plot::WaveDisplayFormula formula) {
    switch (formula) {
    case plot::WaveDisplayFormula::OffsetThenScale:
        return "offset_then_scale";
    case plot::WaveDisplayFormula::ScaleThenOffset:
        return "scale_then_offset";
    }
    return "offset_then_scale";
}

plot::WaveChannelCardWidthMode parseWaveChannelCardWidthMode(const std::string& value) {
    if (value == "adaptive") {
        return plot::WaveChannelCardWidthMode::Adaptive;
    }
    return plot::WaveChannelCardWidthMode::Fixed;
}

const char* toWaveChannelCardWidthModeText(const plot::WaveChannelCardWidthMode mode) {
    switch (mode) {
    case plot::WaveChannelCardWidthMode::Fixed:
        return "fixed";
    case plot::WaveChannelCardWidthMode::Adaptive:
        return "adaptive";
    }
    return "fixed";
}

plot::WaveChannelDoubleClickAction parseWaveChannelDoubleClickAction(
    const std::string& value,
    plot::WaveChannelDoubleClickAction fallback) {
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

const char* toWaveChannelDoubleClickActionText(const plot::WaveChannelDoubleClickAction action) {
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

plot::WaveHiddenChannelPolicy parseWaveHiddenChannelPolicy(const std::string& value,
                                                           plot::WaveHiddenChannelPolicy fallback) {
    if (value == "include_hidden") {
        return plot::WaveHiddenChannelPolicy::IncludeInDerivedViews;
    }
    if (value == "visible_only") {
        return plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews;
    }
    return fallback;
}

const char* toWaveHiddenChannelPolicyText(const plot::WaveHiddenChannelPolicy policy) {
    switch (policy) {
    case plot::WaveHiddenChannelPolicy::IncludeInDerivedViews:
        return "include_hidden";
    case plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews:
        return "visible_only";
    }
    return "include_hidden";
}

double positiveOrFallback(double value, double fallback) {
    return value > 0.0 ? value : fallback;
}

transport::TransportKind parseTransportKind(const std::string& value) {
    if (const auto kind = transport::transportKindFromId(value)) {
        return *kind;
    }
    return transport::TransportKind::TcpClient;
}

std::string toTransportKindText(transport::TransportKind kind) {
    return std::string(transport::transportKindId(kind));
}

const std::vector<std::string> kDefaultSerialPorts = {"COM1", "COM2", "COM3", "COM4"};
} // namespace

ConfigStore::ConfigStore()
    : defaultConfigPath_(embedded::executableDirectory() / "config" / "protoscope.yaml"),
      defaultProtocolRootDir_(embedded::executableDirectory() / "protocols"),
      defaultProtocolDir_(defaultProtocolRootDir_ / "templates" / "default_protocol") {}

AppConfig ConfigStore::withDefaults() const {
    AppConfig config;
    config.protocol.rootDir = normalizeTextPath(defaultProtocolDir_.parent_path());
    config.protocol.selectedDir = normalizeTextPath(defaultProtocolDir_);
    config.configPath = normalizeTextPath(defaultConfigPath_);
    config.communication.serialPortOptions = kDefaultSerialPorts;
    return config;
}

ConfigLoadResult ConfigStore::load(const std::filesystem::path& path) const {
    ConfigLoadResult result;
    result.config = withDefaults();
    result.resolvedPath = path.empty() ? defaultConfigPath_ : path;

    if (!std::filesystem::exists(result.resolvedPath)) {
        result.config.configPath = normalizeTextPath(result.resolvedPath);
        return result;
    }

    try {
        const YAML::Node root = YAML::LoadFile(result.resolvedPath.string());
        result.loadedFromDisk = true;

        const auto app = root["app"];
        result.config.app.language = readScalar<std::string>(app, "language", result.config.app.language);
        result.config.app.fpsLimit = readScalar<std::uint32_t>(app, "fps_limit", result.config.app.fpsLimit);
        result.config.app.idleRender = readScalar<std::string>(app, "idle_render", result.config.app.idleRender);
        if (const auto autoSave = app["auto_save"]) {
            result.config.app.autoSave.enabled = readScalar<bool>(autoSave, "enabled", result.config.app.autoSave.enabled);
            result.config.app.autoSave.intervalMs = readScalar<std::uint64_t>(autoSave, "interval_ms", result.config.app.autoSave.intervalMs);
        }
        if (const auto configHotReload = app["config_hot_reload"]) {
            result.config.app.configHotReload.enabled =
                readScalar<bool>(configHotReload, "enabled", result.config.app.configHotReload.enabled);
        }

        const auto gui = root["gui"];
        if (const auto window = gui["window"]) {
            result.config.gui.window.title = readScalar<std::string>(window, "title", result.config.gui.window.title);
            result.config.gui.window.width = readScalar<int>(window, "width", result.config.gui.window.width);
            result.config.gui.window.height = readScalar<int>(window, "height", result.config.gui.window.height);
            result.config.gui.window.maximized = readScalar<bool>(window, "maximized", result.config.gui.window.maximized);
        }
        if (const auto wave = gui["wave"]) {
            result.config.gui.wave.controlMode =
                parseWaveControlMode(readScalar<std::string>(wave,
                                                             "control_mode",
                                                             toWaveControlModeText(result.config.gui.wave.controlMode)),
                                     result.config.gui.wave.controlMode);
            result.config.gui.wave.displayFormula =
                parseWaveDisplayFormula(readScalar<std::string>(wave,
                                                                "display_formula",
                                                                toWaveDisplayFormulaText(result.config.gui.wave.displayFormula)),
                                        result.config.gui.wave.displayFormula);
            result.config.gui.wave.channelCardWidthMode =
                parseWaveChannelCardWidthMode(readScalar<std::string>(wave, "channel_card_width_mode", "fixed"));
            result.config.gui.wave.channelDoubleClickAction =
                parseWaveChannelDoubleClickAction(
                    readScalar<std::string>(wave,
                                            "channel_double_click_action",
                                            toWaveChannelDoubleClickActionText(result.config.gui.wave.channelDoubleClickAction)),
                    result.config.gui.wave.channelDoubleClickAction);
            result.config.gui.wave.hiddenChannelPolicy =
                parseWaveHiddenChannelPolicy(
                    readScalar<std::string>(wave,
                                            "hidden_channel_policy",
                                            toWaveHiddenChannelPolicyText(result.config.gui.wave.hiddenChannelPolicy)),
                    result.config.gui.wave.hiddenChannelPolicy);
            result.config.gui.wave.zoomSelectionAutoExit =
                readScalar<bool>(wave, "zoom_selection_auto_exit", result.config.gui.wave.zoomSelectionAutoExit);
            result.config.gui.wave.maxRenderPointsPerChannel =
                readScalar<std::size_t>(wave, "max_render_points_per_channel", result.config.gui.wave.maxRenderPointsPerChannel);
            result.config.gui.wave.maxRenderVertices =
                readScalar<std::size_t>(wave, "max_render_vertices", result.config.gui.wave.maxRenderVertices);
            result.config.gui.wave.downsampleStartMultiplier =
                readScalar<double>(wave, "downsample_start_multiplier", result.config.gui.wave.downsampleStartMultiplier);
            result.config.gui.wave.overviewMaxSamples =
                readScalar<std::size_t>(wave, "overview_max_samples", result.config.gui.wave.overviewMaxSamples);
            result.config.gui.wave.minVisibleTimeSpan =
                readScalar<double>(wave, "min_visible_time_span", result.config.gui.wave.minVisibleTimeSpan);
            result.config.gui.wave.channelCardFixedWidth =
                positiveOrFallback(readScalar<double>(wave, "channel_card_fixed_width", result.config.gui.wave.channelCardFixedWidth), 128.0);
            result.config.gui.wave.channelCardAdaptiveRatio =
                positiveOrFallback(readScalar<double>(wave, "channel_card_adaptive_ratio", result.config.gui.wave.channelCardAdaptiveRatio),
                                   0.22);
            result.config.gui.wave.verticalAutoFitMultiplier =
                positiveOrFallback(readScalar<double>(wave, "vertical_auto_fit_multiplier", result.config.gui.wave.verticalAutoFitMultiplier),
                                   1.2);
            result.config.gui.wave.showAxisLabels =
                readScalar<bool>(wave, "show_axis_labels", result.config.gui.wave.showAxisLabels);
            result.config.gui.wave.showChannelLegend =
                readScalar<bool>(wave, "show_channel_legend", result.config.gui.wave.showChannelLegend);
            result.config.gui.wave.showFftLegend =
                readScalar<bool>(wave, "show_fft_legend", result.config.gui.wave.showFftLegend);
            if (const auto logHistory = gui["log_history"]) {
                result.config.gui.logHistory.transferRawLimit =
                    readScalar<std::size_t>(logHistory, "transfer_raw_limit", result.config.gui.logHistory.transferRawLimit);
                result.config.gui.logHistory.transferFrameLimit =
                    readScalar<std::size_t>(logHistory, "transfer_frame_limit", result.config.gui.logHistory.transferFrameLimit);
                result.config.gui.logHistory.hostLimit =
                    readScalar<std::size_t>(logHistory, "host_limit", result.config.gui.logHistory.hostLimit);
                result.config.gui.logHistory.scriptLimit =
                    readScalar<std::size_t>(logHistory, "script_limit", result.config.gui.logHistory.scriptLimit);
            }
            if (const auto rawCapture = gui["raw_capture"]) {
                result.config.gui.rawCapture.liveLimitBytes =
                    readScalar<std::size_t>(rawCapture, "live_limit_bytes", result.config.gui.rawCapture.liveLimitBytes);
            }
            if (const auto transferLog = gui["transfer_log"]) {
                result.config.gui.replayRawHistoryOnSchemaSwitch =
                    readScalar<bool>(transferLog,
                                     "replay_raw_history_on_schema_switch",
                                     result.config.gui.replayRawHistoryOnSchemaSwitch);
            }
            if (const auto realtimeBacklog = gui["realtime_backlog"]) {
                result.config.gui.realtimeBacklog.mode =
                    readScalar<std::string>(realtimeBacklog, "mode", result.config.gui.realtimeBacklog.mode);
                result.config.gui.realtimeBacklog.rxChunkBytesPerPump =
                    readScalar<std::size_t>(realtimeBacklog,
                                            "rx_chunk_bytes_per_pump",
                                            result.config.gui.realtimeBacklog.rxChunkBytesPerPump);
                result.config.gui.realtimeBacklog.transferFrameRowsPerPump =
                    readScalar<std::size_t>(realtimeBacklog,
                                            "transfer_frame_rows_per_pump",
                                            result.config.gui.realtimeBacklog.transferFrameRowsPerPump);
                result.config.gui.realtimeBacklog.plotAppendsPerPump =
                    readScalar<std::size_t>(realtimeBacklog,
                                            "plot_appends_per_pump",
                                            result.config.gui.realtimeBacklog.plotAppendsPerPump);
                result.config.gui.realtimeBacklog.discardBacklogOnDisconnect =
                    readScalar<bool>(realtimeBacklog,
                                     "discard_backlog_on_disconnect",
                                     result.config.gui.realtimeBacklog.discardBacklogOnDisconnect);
            }
            result.config.gui.showAppHeader = readScalar<bool>(gui, "show_app_header", result.config.gui.showAppHeader);
            result.config.gui.luaDockLayoutDebug = readScalar<bool>(gui, "lua_dock_layout_debug", result.config.gui.luaDockLayoutDebug);
            result.config.gui.sendHistoryLimit = readScalar<std::size_t>(gui, "send_history_limit", result.config.gui.sendHistoryLimit);
        }
        if (const auto elfSymbolCombo = gui["elf_symbol_combo"]) {
            const int limit = readScalar<int>(elfSymbolCombo,
                                              "limit",
                                              static_cast<int>(result.config.gui.elfSymbolCombo.limit));
            if (limit > 0) {
                result.config.gui.elfSymbolCombo.limit = static_cast<std::size_t>(limit);
            }
            const int debounceMs =
                readScalar<int>(elfSymbolCombo, "debounce_ms", result.config.gui.elfSymbolCombo.debounceMs);
            if (debounceMs > 0) {
                result.config.gui.elfSymbolCombo.debounceMs = debounceMs;
            }
        }

        const auto protocol = root["protocol"];
        result.config.protocol.rootDir = readScalar<std::string>(protocol, "root_dir", result.config.protocol.rootDir);
        result.config.protocol.selectedDir = readScalar<std::string>(protocol, "selected_dir", result.config.protocol.selectedDir);
        if (const auto tx = protocol["tx"]) {
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
            if (const auto streamBuffer = receive["stream_buffer"]) {
                result.config.receive.streamBuffer.nearOverflowThreshold =
                    readScalar<double>(streamBuffer,
                                       "near_overflow_threshold",
                                       result.config.receive.streamBuffer.nearOverflowThreshold);
                result.config.receive.streamBuffer.popupEnabled =
                    readScalar<bool>(streamBuffer,
                                     "popup_enabled",
                                     result.config.receive.streamBuffer.popupEnabled);
            }
        }

        const auto scripting = root["scripting"];
        if (const auto worker = scripting["worker"]) {
            result.config.scripting.workerEnabled =
                readScalar<bool>(worker, "enabled", result.config.scripting.workerEnabled);
            result.config.scripting.workerRxQueueLimitBytes =
                readScalar<std::size_t>(worker,
                                        "rx_queue_limit_bytes",
                                        result.config.scripting.workerRxQueueLimitBytes);
            result.config.scripting.workerOutputQueueLimit =
                readScalar<std::size_t>(worker, "output_queue_limit", result.config.scripting.workerOutputQueueLimit);
            result.config.scripting.workerBatchBytes =
                readScalar<std::size_t>(worker, "batch_bytes", result.config.scripting.workerBatchBytes);
        }
        if (const auto fileIo = scripting["file_io"]) {
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
            if (const auto dialog = fileIo["dialog"]) {
                config.dialog.enabled = readScalar<bool>(dialog, "enabled", config.dialog.enabled);
                config.dialog.rememberLastDir = readScalar<bool>(dialog, "remember_last_dir", config.dialog.rememberLastDir);
            }
            if (const auto sendFile = fileIo["send_file"]) {
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
        result.config.communication.kind =
            parseTransportKind(readScalar<std::string>(communication, "kind", toTransportKindText(result.config.communication.kind)));

        if (const auto tcpClient = communication["tcp_client"]) {
            result.config.communication.tcpClient.host =
                readScalar<std::string>(tcpClient, "host", result.config.communication.tcpClient.host);
            result.config.communication.tcpClient.port =
                readScalar<std::uint16_t>(tcpClient, "port", result.config.communication.tcpClient.port);
        }

        if (const auto tcpServer = communication["tcp_server"]) {
            result.config.communication.tcpServer.bindAddress =
                readScalar<std::string>(tcpServer, "bind_address", result.config.communication.tcpServer.bindAddress);
            result.config.communication.tcpServer.port =
                readScalar<std::uint16_t>(tcpServer, "port", result.config.communication.tcpServer.port);
            result.config.communication.tcpServer.rejectNewConnection =
                readScalar<bool>(tcpServer, "reject_new_connection", result.config.communication.tcpServer.rejectNewConnection);
        }

        if (const auto serial = communication["serial"]) {
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

        if (const auto udpPeer = communication["udp_peer"]) {
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
            (void)receive;
        }
    } catch (const std::exception& ex) {
        result.error = std::string("读取 YAML 失败: ") + ex.what();
        result.loadedFromDisk = false;
    }

    return result;
}

bool ConfigStore::save(const std::filesystem::path& path, const AppConfig& config, std::string& error) const {
    YAML::Node root;

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
    root["gui"]["wave"]["channel_card_width_mode"] = toWaveChannelCardWidthModeText(config.gui.wave.channelCardWidthMode);
    root["gui"]["wave"]["channel_double_click_action"] =
        toWaveChannelDoubleClickActionText(config.gui.wave.channelDoubleClickAction);
    root["gui"]["wave"]["hidden_channel_policy"] = toWaveHiddenChannelPolicyText(config.gui.wave.hiddenChannelPolicy);
    root["gui"]["wave"]["zoom_selection_auto_exit"] = config.gui.wave.zoomSelectionAutoExit;
    root["gui"]["wave"]["channel_card_fixed_width"] = config.gui.wave.channelCardFixedWidth;
    root["gui"]["wave"]["channel_card_adaptive_ratio"] = config.gui.wave.channelCardAdaptiveRatio;
    root["gui"]["wave"]["vertical_auto_fit_multiplier"] = config.gui.wave.verticalAutoFitMultiplier;
    root["gui"]["wave"]["max_render_points_per_channel"] = config.gui.wave.maxRenderPointsPerChannel;
    root["gui"]["wave"]["max_render_vertices"] = config.gui.wave.maxRenderVertices;
    root["gui"]["wave"]["downsample_start_multiplier"] = config.gui.wave.downsampleStartMultiplier;
    root["gui"]["wave"]["overview_max_samples"] = config.gui.wave.overviewMaxSamples;
    root["gui"]["wave"]["min_visible_time_span"] = config.gui.wave.minVisibleTimeSpan;
    root["gui"]["wave"]["show_axis_labels"] = config.gui.wave.showAxisLabels;
    root["gui"]["wave"]["show_channel_legend"] = config.gui.wave.showChannelLegend;
    root["gui"]["wave"]["show_fft_legend"] = config.gui.wave.showFftLegend;
    root["gui"]["log_history"]["transfer_raw_limit"] = config.gui.logHistory.transferRawLimit;
    root["gui"]["log_history"]["transfer_frame_limit"] = config.gui.logHistory.transferFrameLimit;
    root["gui"]["log_history"]["host_limit"] = config.gui.logHistory.hostLimit;
    root["gui"]["log_history"]["script_limit"] = config.gui.logHistory.scriptLimit;
    root["gui"]["raw_capture"]["live_limit_bytes"] = config.gui.rawCapture.liveLimitBytes;
    root["gui"]["transfer_log"]["replay_raw_history_on_schema_switch"] = config.gui.replayRawHistoryOnSchemaSwitch;
    root["gui"]["realtime_backlog"]["mode"] = config.gui.realtimeBacklog.mode;
    root["gui"]["realtime_backlog"]["rx_chunk_bytes_per_pump"] = config.gui.realtimeBacklog.rxChunkBytesPerPump;
    root["gui"]["realtime_backlog"]["transfer_frame_rows_per_pump"] = config.gui.realtimeBacklog.transferFrameRowsPerPump;
    root["gui"]["realtime_backlog"]["plot_appends_per_pump"] = config.gui.realtimeBacklog.plotAppendsPerPump;
    root["gui"]["realtime_backlog"]["discard_backlog_on_disconnect"] =
        config.gui.realtimeBacklog.discardBacklogOnDisconnect;
    root["gui"]["show_app_header"] = config.gui.showAppHeader;
    root["gui"]["send_history_limit"] = config.gui.sendHistoryLimit;
    root["gui"]["lua_dock_layout_debug"] = config.gui.luaDockLayoutDebug;
    root["gui"]["elf_symbol_combo"]["limit"] = config.gui.elfSymbolCombo.limit;
    root["gui"]["elf_symbol_combo"]["debounce_ms"] = config.gui.elfSymbolCombo.debounceMs;

    root["protocol"]["root_dir"] = config.protocol.rootDir;
    root["protocol"]["selected_dir"] = config.protocol.selectedDir;
    root["protocol"]["tx"]["send_timeout_ms"] = config.protocol.tx.sendTimeoutMs;
    root["protocol"]["tx"]["request_timeout_ms"] = config.protocol.tx.requestTimeoutMs;
    root["protocol"]["tx"]["max_pending"] = config.protocol.tx.maxPending;
    root["protocol"]["tx"]["overflow_policy"] = config.protocol.tx.overflowPolicy;
    root["protocol"]["tx"]["overflow_notify"] = config.protocol.tx.overflowNotify;
    root["receive"]["stream_buffer"]["near_overflow_threshold"] = config.receive.streamBuffer.nearOverflowThreshold;
    root["receive"]["stream_buffer"]["popup_enabled"] = config.receive.streamBuffer.popupEnabled;

    root["scripting"]["worker"]["enabled"] = config.scripting.workerEnabled;
    root["scripting"]["worker"]["rx_queue_limit_bytes"] = config.scripting.workerRxQueueLimitBytes;
    root["scripting"]["worker"]["output_queue_limit"] = config.scripting.workerOutputQueueLimit;
    root["scripting"]["worker"]["batch_bytes"] = config.scripting.workerBatchBytes;
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
    root["scripting"]["file_io"]["send_file"]["default_chunk_bytes"] = config.scripting.fileIo.sendFile.defaultChunkBytes;
    root["scripting"]["file_io"]["send_file"]["max_inflight_chunks"] = config.scripting.fileIo.sendFile.maxInflightChunks;

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
            std::filesystem::create_directories(path.parent_path());
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

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& dir) const {
    return normalizeProtocolDir(defaultProtocolDir_.parent_path(), dir);
}

std::filesystem::path ConfigStore::normalizeProtocolDir(const std::filesystem::path& rootDir, const std::filesystem::path& dir) const {
    std::filesystem::path candidate = dir.empty() ? defaultProtocolDir_ : dir;
    if (protocolEntryExists(candidate)) {
        return candidate;
    }

    const auto scanned = scanProtocolDirectories(rootDir);
    if (!scanned.empty()) {
        return std::filesystem::path(scanned.front());
    }
    return defaultProtocolDir_;
}

std::filesystem::path ConfigStore::mainLuaPath(const std::filesystem::path& protocolDir) const {
    return protocolDir / "main.lua";
}

std::string ConfigStore::protocolName(const std::filesystem::path& protocolDir) const {
    const auto filename = protocolDir.filename().string();
    return filename.empty() ? std::string("default_protocol") : filename;
}

bool ConfigStore::protocolEntryExists(const std::filesystem::path& protocolDir) const {
    return std::filesystem::exists(mainLuaPath(protocolDir));
}

std::vector<std::string> ConfigStore::scanProtocolDirectories(const std::filesystem::path& rootDir) const {
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

bool ConfigStore::ensureDefaultProtocolScript(const std::filesystem::path& protocolDir, std::string& error) const {
    return embedded::ensureDefaultProtocolScript(protocolDir, error);
}

bool ConfigStore::ensureDefaultProtocolWorkspace(std::string& error) const {
    return embedded::ensureProtocolWorkspace(defaultProtocolRootDir_, error);
}

FileSnapshot ConfigStore::snapshot(const std::filesystem::path& path) const {
    FileSnapshot result;
    result.path = path;
    result.exists = std::filesystem::exists(path);
    if (result.exists) {
        result.timestampMs = toTimestampMs(std::filesystem::last_write_time(path));
    }
    return result;
}

bool ConfigStore::hasChanged(const FileSnapshot& previous) const {
    const auto current = snapshot(previous.path);
    return current.exists != previous.exists || current.timestampMs != previous.timestampMs;
}

void ConfigStore::applyToDock(const AppConfig& config, dock::DockStore& dockStore) const {
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
    configState.loadedFromPath = config.configPath.empty() ? normalizeTextPath(defaultConfigPath_) : config.configPath;

    auto& waveState = dockStore.waveState();
    auto& wave = waveState.view;
    wave.controlMode = config.gui.wave.controlMode;
    wave.displayFormula = config.gui.wave.displayFormula;
    wave.channelCardWidthMode = config.gui.wave.channelCardWidthMode;
    wave.channelDoubleClickAction = config.gui.wave.channelDoubleClickAction;
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
    wave.showAxisLabels = config.gui.wave.showAxisLabels;
    wave.showChannelLegend = config.gui.wave.showChannelLegend;
    wave.showFftLegend = config.gui.wave.showFftLegend;
    auto viewConfig = waveState.buffer.viewConfig();
    viewConfig.displayFormula = config.gui.wave.displayFormula;
    waveState.buffer.setViewConfig(viewConfig);
}

AppConfig ConfigStore::captureFromDock(const dock::DockStore& dockStore) const {
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
    config.gui.wave.controlMode = dockStore.waveState().view.controlMode;
    config.gui.wave.displayFormula = dockStore.waveState().view.displayFormula;
    config.gui.wave.channelCardWidthMode = dockStore.waveState().view.channelCardWidthMode;
    config.gui.wave.channelDoubleClickAction = dockStore.waveState().view.channelDoubleClickAction;
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
    config.gui.wave.showAxisLabels = dockStore.waveState().view.showAxisLabels;
    config.gui.wave.showChannelLegend = dockStore.waveState().view.showChannelLegend;
    config.gui.wave.showFftLegend = dockStore.waveState().view.showFftLegend;
    config.configPath = dockStore.configState().loadedFromPath;

    return config;
}

const std::filesystem::path& ConfigStore::defaultConfigPath() const {
    return defaultConfigPath_;
}

const std::filesystem::path& ConfigStore::defaultProtocolDir() const {
    return defaultProtocolDir_;
}

std::uint64_t ConfigStore::toTimestampMs(const std::filesystem::file_time_type& fileTime) {
    const auto normalized = fileTime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(normalized.time_since_epoch()).count());
}

} // namespace protoscope::config
