#include "protoscope/app/application.hpp"
#include "protoscope/config/embedded_protocols.hpp"
#include "protoscope/plot/data_file_output.hpp"

#include "protoscope/plot/wave_math.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/scripting/pipeline_threading.hpp"
#include "protoscope/session/session_package.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>

#include <yaml-cpp/yaml.h>

namespace protoscope::app {

std::uint64_t Application::nowMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::uint64_t Application::nowUs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

Application::Application() = default;

bool Application::initialize()
{
    loggingFacade_.bindDockStore(&dockStore_);

    std::error_code configDirectoryError;
    std::filesystem::create_directories(configStore_.defaultConfigPath().parent_path(), configDirectoryError);
    if (configDirectoryError) {
        const std::string message = "初始化 config 工作目录失败: " + configDirectoryError.message();
        dockStore_.markDirty(message);
        loggingFacade_.warn("config", message);
    }

    std::string workspaceError;
    if (!configStore_.ensureDefaultProtocolWorkspace(workspaceError)) {
        dockStore_.markDirty("初始化 protocols 工作目录失败: " + workspaceError);
        loggingFacade_.warn("config", "初始化 protocols 工作目录失败: " + workspaceError);
    }

    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    runtimeConfig_ = loaded.config;
    loadedConfigFromDisk_ = loaded.loadedFromDisk;
    loggingFacade_.applyConfig(loaded.config.logging);
    const bool configApplied = applyConfig(loaded.config);

    auto& configState = dockStore_.configState();
    configState.loadedFromPath = loaded.resolvedPath.generic_string();
    configState.fileTimestampMs = configStore_.snapshot(loaded.resolvedPath).timestampMs;
    if (!configApplied) {
        const auto& lua = dockStore_.luaState();
        const auto message = lua.lastError.empty() ? std::string("协议加载失败") : "协议加载失败: " + lua.lastError;
        dockStore_.markDirty(message);
    } else if (loaded.loadedFromDisk) {
        dockStore_.clearDirty("已从 YAML 加载配置");
    } else if (!loaded.error.empty()) {
        dockStore_.markDirty(loaded.error);
    } else if (workspaceError.empty() && !configDirectoryError) {
        dockStore_.markDirty("未找到配置文件，已使用默认配置");
    }

    syncDockState();
    return true;
}

bool Application::applyConfig(const config::AppConfig& config, const bool reloadProtocol)
{
    if (pendingSettingsConfig_ || (dataTransferStatus().active && !applyingImportContext_)) return false;
    captureProtocolConfigOverride_.reset();
    runtimeConfig_ = config;
    adaptivePerformance_.configure(config.performance.adaptive);
    loggedAdaptivePerformanceStatus_.reset();
    resetStreamBufferAlertState();
    configureScriptWorker(config);
    applyHistoryLimits(config.gui.logHistory);
    dockStore_.waveState().buffer.setMaxTotalSamples(config.gui.wave.maxTotalSamples);
    dockStore_.waveState().buffer.setResetHistoryOnTimeReset(config.gui.wave.resetHistoryOnTimeReset);
    configStore_.applyToDock(config, dockStore_);
    applyAdaptiveWaveBudget();
    loggingFacade_.applyConfig(config.logging);
    // 配置重载后立即刷新状态，避免通讯面板在下一次 pump 前显示上一次的模式。
    syncAdaptivePerformanceStatus();
    return !reloadProtocol || reloadProtocolDirectory(dockStore_.luaState().protocolDir, false);
}

void Application::configureScriptWorker(const config::AppConfig& config)
{
    const auto postprocessWorkerThreads = scripting::resolvePipelineWorkerThreads(
        config.scripting.pipeline.workerThreads, std::thread::hardware_concurrency());
    scriptWorker_.configure(scripting::ScriptRuntimeWorkerConfig{
        .execution = config.scripting.execution,
        .storageRoot = config.scripting.storageRootDir.empty()
            ? config::embedded::executableDirectory() / "data"
            : std::filesystem::absolute(config.scripting.storageRootDir),
        .storageConfig = config.scripting.storage,
        .enabled = config.scripting.workerEnabled,
        .postprocessWorkerThreads = postprocessWorkerThreads,
        .rxQueueLimitBytes = config.scripting.workerRxQueueLimitBytes,
        .memoryBudgetBytes = config.scripting.workerMemoryBudgetBytes,
        .memoryBudgetAvailableRatio = config.scripting.workerMemoryBudgetAvailableRatio,
        .outputQueueLimit = config.scripting.workerOutputQueueLimit,
        .batchBytes = config.scripting.workerBatchBytes,
        .backpressureEnabled = config.scripting.workerBackpressureEnabled,
        .backpressureHighWatermark = config.scripting.workerBackpressureHighWatermark,
        .backpressureLowWatermark = config.scripting.workerBackpressureLowWatermark,
    });
    scriptWorker_.setFileIoConfig(config.scripting.fileIo);
}

void Application::setLogLevel(const config::LogLevel level)
{
    if (runtimeConfig_.logging.level == level) return;
    loggingFacade_.setLevel(level);
    runtimeConfig_.logging.level = level;
    dockStore_.markDirty("日志等级已修改");
}

void Application::setGuiTheme(const config::GuiTheme theme)
{
    setGuiTheme(config::guiThemeId(theme));
}

void Application::setGuiTheme(const std::string_view theme)
{
    // 主题是纯 GUI 全局偏好，只更新运行态配置，避免无关协议重载。
    runtimeConfig_.gui.theme = theme;
    dockStore_.markDirty("主题已更改");
}

config::AppConfig Application::captureConfig() const
{
    auto captured = configStore_.captureFromDock(dockStore_, runtimeConfig_);
    if (captureProtocolConfigOverride_.has_value()) {
        captured.protocol.rootDir = captureProtocolConfigOverride_->rootDir;
        captured.protocol.selectedDir = captureProtocolConfigOverride_->selectedDir;
    }
    captured.performance = runtimeConfig_.performance;
    captured.gui.theme = runtimeConfig_.gui.theme;
    captured.gui.window = runtimeConfig_.gui.window;
    captured.gui.rendererBackend = runtimeConfig_.gui.rendererBackend;
    captured.gui.logHistory = runtimeConfig_.gui.logHistory;
    captured.gui.rawCapture = runtimeConfig_.gui.rawCapture;
    captured.gui.lastDataExport = runtimeConfig_.gui.lastDataExport;
    captured.gui.fileDialogs = runtimeConfig_.gui.fileDialogs;
    captured.gui.importProtocolSource = runtimeConfig_.gui.importProtocolSource;
    captured.gui.realtimeBacklog = runtimeConfig_.gui.realtimeBacklog;
    captured.gui.elfSymbolCombo = runtimeConfig_.gui.elfSymbolCombo;
    captured.gui.interactionFeedback = runtimeConfig_.gui.interactionFeedback;
    captured.gui.wave.resetHistoryOnTimeReset = runtimeConfig_.gui.wave.resetHistoryOnTimeReset;
    captured.gui.wave.resetViewport.applyOnPlotSetupReset = runtimeConfig_.gui.wave.resetViewport.applyOnPlotSetupReset;
    captured.gui.wave.resetViewport.applyOnManualClear = runtimeConfig_.gui.wave.resetViewport.applyOnManualClear;
    captured.gui.wave.resetViewport.applyOnRawImport = runtimeConfig_.gui.wave.resetViewport.applyOnRawImport;
    captured.gui.wave.resetViewport.autoFollow = runtimeConfig_.gui.wave.resetViewport.autoFollow;
    captured.gui.showAppHeader = runtimeConfig_.gui.showAppHeader;
    captured.gui.sendHistoryLimit = runtimeConfig_.gui.sendHistoryLimit;
    captured.gui.luaDockLayoutDebug = runtimeConfig_.gui.luaDockLayoutDebug;
    captured.gui.replayRawHistoryOnSchemaSwitch = runtimeConfig_.gui.replayRawHistoryOnSchemaSwitch;
    captured.gui.historyOnParsedSwitch = runtimeConfig_.gui.historyOnParsedSwitch;
    captured.gui.historyScope = runtimeConfig_.gui.historyScope;
    captured.protocol.tx = runtimeConfig_.protocol.tx;
    captured.app.language = runtimeConfig_.app.language;
    captured.receive = runtimeConfig_.receive;
    captured.scripting = runtimeConfig_.scripting;
    captured.logging = loggingFacade_.currentConfig();
    return captured;
}

const config::AppConfig& Application::runtimeConfig() const
{
    return runtimeConfig_;
}

std::uint32_t Application::effectiveFpsLimit() const
{
    if (adaptivePerformance_.enabled()) {
        return adaptivePerformance_.budget().fpsLimit;
    }
    return runtimeConfig_.app.fpsLimit;
}

bool Application::loadedConfigFromDisk() const
{
    return loadedConfigFromDisk_;
}

bool Application::reloadProtocolDirectory(const std::string& protocolDir, bool forceReload)
{
    if (dataTransferStatus().active && !applyingImportContext_) return false;
    auto& lua = dockStore_.luaState();
    try {
        const auto resolvedDir = configStore_.normalizeProtocolDir(lua.protocolRootDir, protocolDir);
        const auto resolvedDirText = resolvedDir.generic_string();
        const auto protocolName = configStore_.protocolName(resolvedDir);
        const auto scriptPath = configStore_.mainLuaPath(resolvedDir).generic_string();
        const bool unchanged = lua.loaded && lua.protocolDir == resolvedDirText && lua.scriptPath == scriptPath;

        // 核心流程：配置热加载只在协议目录真正变化时重载脚本，避免窗口刷新阶段重复刷加载日志。
        if (!forceReload && unchanged) {
            loggingFacade_.trace("protocol", "protocol reload skipped kind=protocol endpoint=" + resolvedDirText);
            lua.lastError.clear();
            applyLuaScriptSnapshot(scriptWorker_.snapshot());
            return true;
        }

        loggingFacade_.info("protocol", "protocol reload requested kind=protocol endpoint=" + resolvedDirText);
        if (!probeProtocolDirectory(resolvedDirText)) {
            return false;
        }

        prepareProtocolRuntimeReload();
        scriptWorker_.setFileIoConfig(runtimeConfig_.scripting.fileIo);
        const auto loadResult = scriptWorker_.loadProtocolDirectory(resolvedDirText);
        if (!applyProtocolLoadResult(resolvedDirText, protocolName, scriptPath, loadResult)) {
            return false;
        }
        captureProtocolConfigOverride_.reset();
        return true;
    } catch (const std::exception& ex) {
        lua.lastError = std::string("协议重载异常: ") + ex.what();
    } catch (...) {
        lua.lastError = "协议重载异常: 未知异常";
    }

    loggingFacade_.error("protocol", lua.lastError);
    applyLuaScriptSnapshot(scriptWorker_.snapshot());
    return false;
}

void Application::applyLuaScriptSnapshot(const scripting::ScriptRuntimeSnapshot& snapshot)
{
    auto& lua = dockStore_.luaState();
    lua.docks = snapshot.docks;
    lua.controls = snapshot.controls;
    lua.controlStates = snapshot.controlStates;
    lua.businessUi = snapshot.businessUi;
}

bool Application::probeProtocolDirectory(const std::string& resolvedDirText)
{
    scripting::ScriptHost probeHost;
    probeHost.setFileIoConfig(runtimeConfig_.scripting.fileIo);
    probeHost.setExecutionConfig(runtimeConfig_.scripting.execution);
    if (probeHost.loadProtocolDirectory(resolvedDirText)) {
        return true;
    }

    auto& lua = dockStore_.luaState();
    lua.lastError = probeHost.lastError();
    loggingFacade_.error("protocol", "协议加载探测失败: " + lua.lastError);
    applyLuaScriptSnapshot(scriptWorker_.snapshot());
    return false;
}

void Application::prepareProtocolRuntimeReload()
{
    try {
        cancelAllTxRequests("协议已重新加载");
    } catch (const std::exception& ex) {
        loggingFacade_.warn("protocol", std::string("协议重载前取消旧请求失败: ") + ex.what());
    } catch (...) {
        loggingFacade_.warn("protocol", "协议重载前取消旧请求失败: 未知异常");
    }
    try {
        // 核心流程：取消旧 request 可能触发旧脚本 on_tx；替换宿主前丢弃旧输出，
        // 避免旧回调追加的新请求、状态或弹窗污染新协议运行态；清理期间主动drain，
        // 以解除输出满时worker发布者与reload控制命令的互等。
        static_cast<void>(scriptWorker_.clearPendingRealtimeOutputs());
    } catch (const std::exception& ex) {
        loggingFacade_.warn("protocol", std::string("协议重载前清理旧脚本输出失败: ") + ex.what());
    } catch (...) {
        loggingFacade_.warn("protocol", "协议重载前清理旧脚本输出失败: 未知异常");
    }
}

bool Application::applyProtocolLoadResult(const std::string& resolvedDirText,
                                          const std::string& protocolName,
                                          const std::string& scriptPath,
                                          const scripting::ScriptRuntimeLoadResult& loadResult)
{
    auto& lua = dockStore_.luaState();
    if (!loadResult.ok) {
        lua.lastError = loadResult.lastError;
        loggingFacade_.error("protocol", "协议加载失败: " + lua.lastError);
        applyLuaScriptSnapshot(loadResult.snapshot);
        return false;
    }

    // 辅助游标只属于当前数据会话，协议成功切换后立即清理；加载失败保留旧会话。
    dockStore_.waveState().view.auxiliaryCursors.clear();
    lua.protocolDir = resolvedDirText;
    lua.protocolName = protocolName;
    lua.scriptPath = scriptPath;
    lua.loaded = true;
    applyLuaScriptSnapshot(loadResult.snapshot);
    lua.lastError.clear();
    refreshTransferFrameDisplayAfterProtocolReload();
    loggingFacade_.info("protocol", "protocol loaded kind=protocol endpoint=" + resolvedDirText);
    flushScriptOutputs();
    syncDockState();
    return true;
}

void Application::refreshTransferFrameDisplayAfterProtocolReload()
{
    // 协议变更使历史切换任务/旧确认失效，不能把过期结果提交到新 schema。
    invalidateHistorySwitchWork("协议变更");
    if (dockStore_.receiveState().displayMode == dock::TransferLogDisplayMode::ParsedFrames) {
        rebuildTransferFrameRows();
    } else {
        resetTransferFrameDisplayState();
    }
}

bool Application::pumpOnce()
{
    if (dataImportActive_) return pumpDataImport();
    if (retiringImportProtocolLoad_.valid()) {
        static_cast<void>(scriptWorker_.drainOutputs());
        if (retiringImportProtocolLoad_.wait_for(std::chrono::seconds(0)) != std::future_status::ready ||
            !scriptWorker_.inputIdle()) return true;
        const auto result = retiringImportProtocolLoad_.get();
        retiringImportProtocolLoad_ = {};
        static_cast<void>(scriptWorker_.drainOutputs());
        if (result.ok) applyLuaScriptSnapshot(result.snapshot);
        else dockStore_.luaState().lastError = "取消导入后的协议恢复失败: " + result.lastError;
        return true;
    }
    bool changed = false;
    const bool hadPendingScriptPlotAppends = !pendingScriptPlotAppends_.empty();
    // 先让transport pump非阻塞投递RX；若worker达到高水位则保留输入，
    // 随后flush输出腾出worker槽位，下一轮再投递保留字节。
    changed = handleTransportEvents() || changed;
    changed = flushScriptOutputs() || changed;
    std::string replayError;
    const bool replayChanged = pumpRawCaptureReplay(replayError);
    changed = replayChanged || changed;
    if (!replayChanged && !replayError.empty()) {
        loggingFacade_.error("wave", replayError);
        dockStore_.waveState().statusMessage = replayError;
    }
    if (!rawCaptureReplay_.loaded && !dataImportActive_ && !replayReceiveHistory_) {
        scriptWorker_.postTick(nowMs());
    }
    changed = flushScriptOutputs() || changed;
    changed = processRequestTimeouts() || changed;
    changed = flushScriptOutputs() || changed;
    if (hadPendingScriptPlotAppends && !pendingScriptPlotAppends_.empty()) {
        changed = applyScriptOutputBatch(scripting::ScriptRuntimeOutputBatch{}) || changed;
    }
    changed = flushPendingTransferFrameRows(transferFrameRowsPerPump()) || changed;
    if (historyRebuild_.has_value()) {
        // 历史重建任务与实时数据共用主循环：新事件/字节先经 appendTransferRow 入 pending，再与重建结果整体替换。
        pumpHistoryRebuild();
    }
    syncDockState();
    updateAdaptivePerformance();
    return changed;
}

void Application::shutdown()
{
    dataTransfer_.cancel();
    if (rawCaptureReplay_.loaded) {
        cancelRawCaptureImportReplay();
        cleanupOfflineReplayResources();
        rawCaptureReplay_ = RawCaptureReplayState{};
    }
    closeTransport();
    cleanupOfflineReplayResources();
    scriptWorker_.stop();
}

dock::DockStore& Application::docks()
{
    return dockStore_;
}

const dock::DockStore& Application::docks() const
{
    return dockStore_;
}

void Application::updateControlValue(const std::string& id, const scripting::ControlValue& value,
                                     std::optional<std::uint64_t> generation)
{
    transport::ConnectionContext context;
    if (activeConnection_.has_value()) {
        context = *activeConnection_;
    } else {
        // 核心流程：动态控件也可能只驱动 Lua 本地演示逻辑，未连接时仍允许回调脚本。
        context.endpoint = "detached";
        context.connectionId = 0;
        context.timestampMs = nowMs();
        context.readyForIo = false;
    }
    scriptWorker_.postControl(context, id, value, generation);
    if (!runtimeConfig_.scripting.workerEnabled) {
        // 同步兼容模式等待控件回调；异步 worker 不得阻塞 UI 主线程。
        while (!scriptWorker_.idle()) {
            flushScriptOutputs();
            std::this_thread::yield();
        }
        flushScriptOutputs();
    }
    syncDockState();
}

bool Application::restoreControlValue(const std::string& id, const scripting::ControlValue& value)
{
    if (!scriptWorker_.setControlValue(id, value)) {
        return false;
    }
    syncDockState();
    return true;
}

void Application::markProtocolEdited()
{
    dockStore_.markDirty("协议配置已修改");
}

void Application::setStatusMessage(std::string message, bool markDirty)
{
    if (markDirty) {
        dockStore_.markDirty(message);
        return;
    }
    dockStore_.configState().statusMessage = std::move(message);
}

void Application::setTransientStatusMessage(std::string message)
{
    auto& feedback = runtimeConfig_.gui.interactionFeedback;
    auto& configState = dockStore_.configState();
    if (!feedback.enabled || feedback.statusDurationMs == 0 || message.empty()) {
        configState.transientStatusMessage.clear();
        configState.transientStatusExpiresAtMs = 0;
        return;
    }
    configState.transientStatusMessage = std::move(message);
    configState.transientStatusExpiresAtMs = nowMs() + feedback.statusDurationMs;
}

void Application::clearExpiredTransientStatus(const std::uint64_t currentMs)
{
    auto& configState = dockStore_.configState();
    if (configState.transientStatusExpiresAtMs == 0 || currentMs < configState.transientStatusExpiresAtMs) {
        return;
    }
    configState.transientStatusMessage.clear();
    configState.transientStatusExpiresAtMs = 0;
}

void Application::activateBusinessMenu(const std::string& id, bool checked,
                                       std::uint64_t generation, std::uint64_t revision)
{
    auto context = activeConnection_.value_or(transport::ConnectionContext{});
    if (!activeConnection_) {
        context.endpoint = "detached";
        context.timestampMs = nowMs();
    }
    // 菜单只入 worker 队列，主线程不等待 Lua 回调执行完毕。
    scriptWorker_.postMenu(std::move(context),id,checked,generation,revision);
}
void Application::interactDataTable(scripting::DataTableEvent event)
{
    auto context=activeConnection_.value_or(transport::ConnectionContext{});
    if (!activeConnection_) {context.endpoint="detached";context.timestampMs=nowMs();}
    scriptWorker_.postDataTable(std::move(context),std::move(event));
}

bool Application::loadElfStaticAddressFile(const std::filesystem::path& path, std::string& error)
{
    if (!elfStaticView_.loadFile(path, error)) {
        loggingFacade_.warn("elf", "elf load failed kind=elf endpoint=" + path.generic_string() + " error=" + error);
        return false;
    }
    ++elfStaticAddressRevision_;
    setStatusMessage("ELF/ElfStaticView 数据文件已加载: " + elfStaticView_.sourcePath());
    loggingFacade_.info("elf", "elf loaded kind=elf endpoint=" + elfStaticView_.sourcePath());
    return true;
}

void Application::clearElfStaticAddressFile()
{
    const bool hadLoadedContext = elfStaticView_.loaded() || !elfStaticView_.sourcePath().empty();
    elfStaticView_.clear();
    if (hadLoadedContext) {
        ++elfStaticAddressRevision_;
        loggingFacade_.info("elf", "elf cleared kind=elf");
    }
}

std::uint64_t Application::elfStaticAddressRevision() const
{
    return elfStaticAddressRevision_;
}

std::vector<scripting::ElfSymbolValue> Application::queryElfStaticAddresses(const std::string& queryText,
                                                                            std::size_t limit) const
{
    std::vector<scripting::ElfSymbolValue> symbols;
    for (const auto& entry : elfStaticView_.query(queryText, limit)) {
        symbols.push_back(scripting::ElfSymbolValue{
            .label = entry.label,
            .value = entry.value,
            .type = entry.type,
        });
    }
    return symbols;
}

void Application::refreshSelectedElfSymbolControls()
{
    const auto comboConfig = runtimeConfig_.gui.elfSymbolCombo;
    if (!comboConfig.autoRefreshSelectedAddress) {
        return;
    }

    const auto luaSnapshot = scriptWorker_.snapshot();
    std::string missingLabels;
    for (const auto& control : luaSnapshot.controlStates) {
        if (control.descriptor.type != scripting::ControlType::ElfSymbolCombo) {
            continue;
        }
        const auto* current = std::get_if<scripting::ElfSymbolValue>(&control.value);
        if (current == nullptr || current->label.empty()) {
            continue;
        }

        const auto refreshed = elfStaticView_.findExactLabel(current->label);
        // 完整名称消失时保留原值，集中报告，避免误绑定同名或近似变量。
        if (!refreshed.has_value()) {
            if (!missingLabels.empty()) missingLabels += ", ";
            missingLabels += current->label;
            continue;
        }
        if (refreshed->value == current->value && refreshed->type == current->type) {
            continue;
        }

        const scripting::ElfSymbolValue refreshedValue{
            .label = refreshed->label,
            .value = refreshed->value,
            .type = refreshed->type,
        };
        // 核心流程：默认只同步当前控件值；显式配置后才复用 updateControlValue 触发 Lua on_control。
        if (comboConfig.autoRefreshEmitOnControl) {
            updateControlValue(control.descriptor.id, refreshedValue);
        } else {
            static_cast<void>(restoreControlValue(control.descriptor.id, refreshedValue));
        }
    }
    flushScriptOutputs();
    syncDockState();
    if (!missingLabels.empty()) {
        const auto warning = "未找到变量，旧地址未更新: " + missingLabels;
        setStatusMessage(warning, true);
        loggingFacade_.warn("elf", warning);
    }
}

std::optional<std::uint64_t> Application::nextWakeupAtMs() const
{
    if (dataTransferStatus().active) return nowMs();
    const auto scriptSnapshot = scriptWorker_.snapshot();
    const auto pendingWorkerRxBytes = scriptWorker_.pendingRxBytes();
    if (rawCaptureReplay_.loaded && rawCaptureReplay_.playing) {
        return nowMs();
    }
    if (!pendingTransportEvents_.empty() || !pendingRxByteChunks_.empty() || !pendingTransferFrameRows_.empty() ||
        scriptSnapshot.pendingPlotAppends > 0U || pendingWorkerRxBytes > 0U) {
        return nowMs();
    }
    auto nextWakeup = scriptSnapshot.nextWakeupAtMs;
    const auto& configState = dockStore_.configState();
    if (configState.transientStatusExpiresAtMs > 0 &&
        (!nextWakeup.has_value() || configState.transientStatusExpiresAtMs < *nextWakeup)) {
        nextWakeup = configState.transientStatusExpiresAtMs;
    }
    if (activeHalfDuplexRequest_.has_value()) {
        if (!nextWakeup.has_value() || activeHalfDuplexRequest_->waitDeadlineMs < *nextWakeup) {
            nextWakeup = activeHalfDuplexRequest_->waitDeadlineMs;
        }
    }
    return nextWakeup;
}

void Application::syncDockState()
{
    auto& comm = dockStore_.commState();
    if (transport_) {
        comm.state = transport_->state();
        comm.txCount = transport_->txCount();
        comm.rxCount = transport_->rxCount();
    } else {
        comm.state = transport::TransportState::Closed;
    }
    const auto scriptSnapshot = scriptWorker_.snapshot();
    const auto pendingWorkerRxBytes = scriptWorker_.pendingRxBytes();
    comm.pendingRxBytes = pendingRxByteCount() + pendingWorkerRxBytes;
    comm.pendingTransferFrameRows = pendingTransferFrameRows_.size();
    comm.pendingPlotAppends = scriptSnapshot.pendingPlotAppends + pendingScriptPlotAppends_.size();
    comm.rxInputQueueBytes = pendingRxByteCount();
    comm.parserPendingBytes = pendingWorkerRxBytes;
    comm.postprocessPendingBatches = 0U;
    comm.luaPendingItems = scriptSnapshot.inputQueueSize;
    comm.uiPendingItems =
        scriptSnapshot.outputQueueSize + pendingTransferFrameRows_.size() + pendingScriptPlotAppends_.size();
    comm.postprocessWorkerThreads = scriptSnapshot.postprocessWorkerThreads;
    comm.backlogWarning.clear();
    const auto rawFirstWarnBytes = runtimeConfig_.gui.realtimeBacklog.rawFirstBacklogWarnBytes;
    if (rawFirstWarnBytes > 0U && comm.pendingRxBytes >= rawFirstWarnBytes) {
        comm.backlogWarning = "raw-first backlog 已超过告警阈值；原始数据继续保留，派生 UI 可能延后或降级";
    } else if (runtimeConfig_.gui.realtimeBacklog.derivedBacklogDegradeEnabled &&
               (comm.pendingTransferFrameRows > transferFrameRowsPerPump() ||
                comm.pendingPlotAppends > plotAppendsPerPump())) {
        comm.backlogWarning = "派生 UI backlog 已超过单轮预算；transfer rows / plot append 将分批追赶";
    }
    comm.lastErrorSummary = scriptSnapshot.lastTransportStats.lastErrorSummary;
    maybeLogCommPressureDebug(comm);

    auto& lua = dockStore_.luaState();
    lua.docks = scriptSnapshot.docks;
    lua.controls = scriptSnapshot.controls;
    lua.controlStates = scriptSnapshot.controlStates;
    lua.businessUi = scriptSnapshot.businessUi;

    auto& wave = dockStore_.waveState();
    const auto waveRevision = wave.buffer.dataRevision();
    if (!cachedWaveSummaryRevision_.has_value() || *cachedWaveSummaryRevision_ != waveRevision) {
        wave.channelSummaries.clear();
        const auto snapshot =
            wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity());
        for (const auto& channel : snapshot.channels) {
            wave.channelSummaries.push_back(channel.label + " samples=" + std::to_string(channel.totalSamples));
        }
        cachedWaveSummaryRevision_ = waveRevision;
    }
}

void Application::updateAdaptivePerformance()
{
    const auto& comm = dockStore_.commState();
    const auto currentMs = nowMs();
    SystemPressureSample systemPressure;
    if (adaptivePerformance_.shouldSample(currentMs)) {
        systemPressure = sampleSystemPressure();
    }
    adaptivePerformance_.update(AdaptivePerformanceInput{
        .nowMs = currentMs,
        .system = std::move(systemPressure),
        .pendingRxBytes = comm.pendingRxBytes,
        .rawBacklogWarnBytes = runtimeConfig_.gui.realtimeBacklog.rawFirstBacklogWarnBytes,
        .pendingTransferFrameRows = comm.pendingTransferFrameRows,
        .pendingPlotAppends = comm.pendingPlotAppends,
        .pendingWorkerRxBytes = comm.parserPendingBytes,
        .workerInputQueueSize = comm.luaPendingItems,
        .workerOutputQueueSize = comm.uiPendingItems,
        .scriptPumpMs = comm.lastPumpScriptMs,
    });
    applyAdaptiveWaveBudget();
    syncAdaptivePerformanceStatus();
}

void Application::applyAdaptiveWaveBudget()
{
    auto& view = dockStore_.waveState().view;
    if (!adaptivePerformance_.enabled()) {
        view.adaptiveMaxRenderPointsPerChannel.reset();
        view.adaptiveMaxRenderVertices.reset();
        view.adaptiveOverviewMaxSamples.reset();
        return;
    }

    const auto& budget = adaptivePerformance_.budget();
    view.adaptiveMaxRenderPointsPerChannel = budget.maxRenderPointsPerChannel;
    view.adaptiveMaxRenderVertices = budget.maxRenderVertices;
    view.adaptiveOverviewMaxSamples = budget.overviewMaxSamples;
}

void Application::syncAdaptivePerformanceStatus()
{
    const auto& status = adaptivePerformance_.status();
    auto& comm = dockStore_.commState();
    comm.adaptivePerformanceEnabled = status.enabled;
    comm.adaptivePerformanceMaxMultiplier = status.maxMultiplier;
    comm.adaptivePerformanceEffectiveMultiplier = status.effectiveMultiplier;
    comm.adaptivePerformanceCatchUpMultiplier = status.catchUpMultiplier;
    comm.adaptivePerformanceLevel = adaptivePressureLevelId(status.pressureLevel);
    comm.adaptivePerformanceReason = status.reason;
    comm.adaptivePerformanceSystemMetricsAvailable = status.systemMetricsAvailable;

    if (!status.enabled) {
        loggedAdaptivePerformanceStatus_.reset();
        return;
    }
    const bool changed = !loggedAdaptivePerformanceStatus_.has_value() ||
                         loggedAdaptivePerformanceStatus_->pressureLevel != status.pressureLevel ||
                         loggedAdaptivePerformanceStatus_->reason != status.reason ||
                         loggedAdaptivePerformanceStatus_->systemMetricsAvailable != status.systemMetricsAvailable;
    if (!changed) {
        return;
    }
    loggingFacade_.info("adaptive_performance",
                        "adaptive performance level=" + comm.adaptivePerformanceLevel +
                            " render_multiplier=" + std::to_string(status.effectiveMultiplier) +
                            " catchup_multiplier=" + std::to_string(status.catchUpMultiplier) +
                            " reason=" + status.reason);
    loggedAdaptivePerformanceStatus_ = status;
}

bool Application::responsiveBacklogMode() const
{
    return runtimeConfig_.gui.realtimeBacklog.mode != "complete";
}

std::size_t Application::rxBytesPerPump() const
{
    if (adaptivePerformance_.enabled()) {
        return adaptivePerformance_.budget().rxChunkBytesPerPump;
    }
    return (std::max<std::size_t>) (runtimeConfig_.gui.realtimeBacklog.rxChunkBytesPerPump, 1U);
}

std::size_t Application::transferFrameRowsPerPump() const
{
    if (adaptivePerformance_.enabled()) {
        return adaptivePerformance_.budget().transferFrameRowsPerPump;
    }
    return (std::max<std::size_t>) (runtimeConfig_.gui.realtimeBacklog.transferFrameRowsPerPump, 1U);
}

std::size_t Application::plotAppendsPerPump() const
{
    if (adaptivePerformance_.enabled()) {
        return adaptivePerformance_.budget().plotAppendsPerPump;
    }
    return (std::max<std::size_t>) (runtimeConfig_.gui.realtimeBacklog.plotAppendsPerPump, 1U);
}

double Application::outputFlushBudgetMs() const
{
    if (adaptivePerformance_.enabled()) {
        return adaptivePerformance_.budget().workerOutputFlushBudgetMs;
    }
    return runtimeConfig_.scripting.workerOutputFlushBudgetMs;
}

bool Application::applyScriptUiAndLogOutputs(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    for (const auto& update : batch.statusUpdates) {
        setStatusMessage(update.clear ? std::string{} : update.text, false);
        changed = true;
    }
    for (const auto& request : batch.dialogRequests) {
        enqueueDialogRequest(request);
        changed = true;
    }
    for (const auto& request : batch.fileDialogRequests) {
        pendingFileDialogs_.push_back(request);
        openFileDialogs_[request.id] = request;
        changed = true;
    }

    for (const auto& event : batch.events) {
        dockStore_.appendLuaEvent(event);
        changed = true;
    }
    for (const auto& log : batch.logs) {
        loggingFacade_.script(log.level, log.message, log.timestampMs);
        changed = true;
    }
    return changed;
}

bool Application::applyScriptOutputBatch(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    changed = applyScriptTransportStats(batch) || changed;
    changed = applyScriptTxOutputs(batch) || changed;
    changed = applyScriptRuntimeProfileEvents(batch) || changed;
    changed = applyScriptUiAndLogOutputs(batch) || changed;
    changed = applyScriptOscilloscopeOutputs(batch) || changed;
    changed = applyScriptPlotOutputs(batch) || changed;
    changed = driveTxScheduler() || changed;
    return changed;
}

bool Application::flushScriptOutputs()
{
    bool changed = false;
    const auto flushBudgetMs = outputFlushBudgetMs();
    std::uint64_t flushStartedAt = 0;
    if (flushBudgetMs > 0.0) {
        flushStartedAt = nowUs();
    }
    // 核心流程：逐批 drain，每次处理后检查时间预算，超时则停止
    // 剩余批次留在 worker 输出队列中，下次 pump 继续处理
    auto batch = scriptWorker_.drainOneOutput();
    while (batch.has_value()) {
        changed = applyScriptOutputBatch(*batch) || changed;
        if (flushBudgetMs > 0.0 && (nowUs() - flushStartedAt) >= static_cast<std::uint64_t>(flushBudgetMs * 1000.0)) {
            break;
        }
        batch = scriptWorker_.drainOneOutput();
    }
    return changed;
}

bool Application::flushScriptOutputsUnbounded()
{
    bool changed = false;
    auto batch = scriptWorker_.drainOneOutput();
    while (batch.has_value()) {
        changed = applyScriptOutputBatch(*batch) || changed;
        batch = scriptWorker_.drainOneOutput();
    }
    return changed;
}

bool Application::flushScriptStatusAndDialogs()
{
    return false;
}

void Application::enqueueDialogRequest(const scripting::DialogRequest& request)
{
    if (!request.dedupeKey.empty() && dialogDedupeKeys_.contains(request.dedupeKey)) {
        return;
    }
    pendingDialogs_.push_back(request);
    openDialogs_[request.id] = request;
    if (!request.dedupeKey.empty()) {
        dialogDedupeKeys_[request.dedupeKey] = request.id;
    }
}

bool Application::flushScriptLogs()
{
    return false;
}

logging::LoggingFacade& Application::logger()
{
    return loggingFacade_;
}

const logging::LoggingFacade& Application::logger() const
{
    return loggingFacade_;
}

std::vector<scripting::DialogRequest> Application::drainDialogRequests()
{
    std::vector<scripting::DialogRequest> drained;
    drained.reserve(pendingDialogs_.size());
    while (!pendingDialogs_.empty()) {
        drained.push_back(std::move(pendingDialogs_.front()));
        pendingDialogs_.pop_front();
    }
    return drained;
}

void Application::respondDialog(const scripting::DialogEvent& event)
{
    const auto iter = openDialogs_.find(event.id);
    if (iter == openDialogs_.end()) {
        return;
    }
    const auto request = iter->second;
    if (request.dedupeKey == "receive.stream_buffer.near_overflow") {
        streamBufferAlertState_.popupOpen = false;
        if (event.state == "mute_until_disconnect") {
            streamBufferAlertState_.popupMuted = true;
            streamBufferAlertState_.connectionId = request.connection.connectionId;
        }
    }
    if (!request.dedupeKey.empty()) {
        dialogDedupeKeys_.erase(request.dedupeKey);
    }
    openDialogs_.erase(iter);
    auto response = event;
    response.runtimeGeneration = request.runtimeGeneration;
    scriptWorker_.postDialogEvent(request.connection, std::move(response));
}

std::vector<scripting::FileDialogRequest> Application::drainFileDialogRequests()
{
    std::vector<scripting::FileDialogRequest> drained;
    drained.reserve(pendingFileDialogs_.size());
    while (!pendingFileDialogs_.empty()) {
        drained.push_back(std::move(pendingFileDialogs_.front()));
        pendingFileDialogs_.pop_front();
    }
    return drained;
}

void Application::respondFileDialog(const scripting::FileDialogEvent& event)
{
    const auto iter = openFileDialogs_.find(event.id);
    if (iter == openFileDialogs_.end()) {
        return;
    }
    const auto request = iter->second;
    openFileDialogs_.erase(iter);
    auto response = event;
    response.runtimeGeneration = request.runtimeGeneration;
    scriptWorker_.postFileDialogEvent(request.connection, std::move(response));
}

} // namespace protoscope::app
