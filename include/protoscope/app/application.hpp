#pragma once

#include "protoscope/app/adaptive_performance.hpp"
#include "protoscope/app/data_transfer_task.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/config/settings.hpp"
#include "protoscope/dock/docks.hpp"
#include "protoscope/logging/logging.hpp"
#include "protoscope/plot/csv_data_file.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/plugin/elf_static_view_bridge.hpp"
#include "protoscope/scripting/script_runtime_worker.hpp"
#include "protoscope/transport/transport.hpp"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace protoscope::app {

enum class SettingsApplyStatus { Applied, Pending, Failed, ConfirmationRequired };
struct SettingsApplyOptions {
    bool confirmHistoryTrim{false};
    bool overwriteConflicts{false};
    std::uint64_t confirmationId{0};
    // 宿主必须通过 ThemeManager 验证并提交主题；核心不直接渲染。
    std::function<bool(std::string_view, std::string&)> applyTheme;
    // 可失败的资源校验/准备应在 worker 执行前完成；不得预览主题。
    std::function<bool(std::string_view, std::string&)> prepareTheme;
};
struct SettingsApplyResult {
    SettingsApplyStatus status{SettingsApplyStatus::Failed};
    std::string error;
    std::vector<std::string> conflicts;
    std::uint64_t confirmationId{0};
    bool restartRequired{false};
    bool nextConnectionRequired{false};
    bool partiallyApplied{false};
};

class Application {
public:
    Application();

    bool initialize();
    bool applyConfig(const config::AppConfig& config, bool reloadProtocol = true);
    config::AppConfig captureConfig() const;
    SettingsApplyResult applySettingsChanges(const config::AppConfig& baseline, const config::AppConfig& draft,
                                             SettingsApplyOptions options = {});
    SettingsApplyResult pollSettingsApply();
    [[nodiscard]] bool settingsSaveBlocked() const { return pendingSettingsConfig_.has_value() || settingsPartialSaveBlocked_; }
    bool preparePartialWorkerRecovery(config::SettingsDraft& draft) const;
    bool acceptPartialSettings(std::string& error);
    // 只保存最新运行态；导入导出或应用 Pending 时禁止落盘。
    bool saveSettings(config::SettingsPersistence& persistence, std::string& error);
    [[nodiscard]] AdaptivePerformanceBudget effectiveSettingsPerformance() const;
    [[nodiscard]] const config::AppConfig& runtimeConfig() const;
    [[nodiscard]] std::uint32_t effectiveFpsLimit() const;
    [[nodiscard]] bool loadedConfigFromDisk() const;
    // 配置/启动调用必须显式关闭手动导入重算；无变化的非强制 reload 本身不会重算。
    bool reloadProtocolDirectory(const std::string& protocolDir, bool forceReload = false,
                                 bool manualSwitch = true);
    bool pumpOnce();
    void shutdown();

    dock::DockStore& docks();
    const dock::DockStore& docks() const;

    void openTransport();
    void closeTransport();
    bool sendManualPayload(const std::string& payload, bool hexMode);
    void updateControlValue(const std::string& id, const scripting::ControlValue& value,
                            std::optional<std::uint64_t> generation = {});
    bool requestOscilloscopeToggle(bool currentRunning, bool targetRunning);
    bool restoreControlValue(const std::string& id, const scripting::ControlValue& value);
    void activateBusinessMenu(const std::string& id, bool checked, std::uint64_t generation, std::uint64_t revision);
    void interactDataTable(scripting::DataTableEvent event);
    void markCommConfigEdited(bool reconnectRequired);
    void markProtocolEdited();
    void setStatusMessage(std::string message, bool markDirty = false);
    void setTransientStatusMessage(std::string message);
    void clearExpiredTransientStatus(std::uint64_t nowMs);
    void setLogLevel(config::LogLevel level);
    void setGuiTheme(config::GuiTheme theme);
    void setGuiTheme(std::string_view theme);
    bool setSendHexMode(bool enabled);
    bool exportWaveRawCapture(const std::filesystem::path& path, std::string& error) const;
    bool exportWaveRawCapture(const std::filesystem::path& path,
                              const plot::CsvExportRange& range,
                              std::string& error) const;
    bool importWaveCsvData(const plot::WaveCsvData& data, std::string& error);
    std::optional<plot::WaveCsvData> captureWaveData(const plot::CsvExportRange& range, std::string& error) const;
    bool importRawRecords(const plot::RawCaptureFileData& data, std::string& error);
    bool startDataImport(const std::filesystem::path& path, std::string& error);
    enum class OfflineReplayProcessing {
        BrowseRaw,
        CurrentProtocol,
        PackageProtocol,
    };

    enum class OfflineReplayPacing {
        OriginalTimeline,
        FastBatchParse,
    };

    void confirmDataImport(bool parseWaveform = false) {
        confirmDataImport(parseWaveform ? OfflineReplayProcessing::CurrentProtocol : OfflineReplayProcessing::BrowseRaw,
                          OfflineReplayPacing::FastBatchParse);
    }
    void confirmDataImport(OfflineReplayProcessing processing, OfflineReplayPacing pacing);
    void cancelDataTransfer();
    DataTransferStatus dataTransferStatus() const {
        auto status = dataTransfer_.status();
        status.active = status.active || dataImportActive_;
        return status;
    }
    void rememberDataExport(const config::DataExportConfig& value) { runtimeConfig_.gui.lastDataExport = value; }
    void rememberFileDialogPreferences(const config::GuiFileDialogConfig& value) { runtimeConfig_.gui.fileDialogs = value; }
    bool startDataExport(const std::filesystem::path& path, int content, int format,
                         const plot::CsvExportRange& waveRange, int recordRange,
                         std::uint64_t recordBeginMs, std::uint64_t recordEndMs,
                         plot::WaveCsvShape shape, std::string& error);
    bool exportWaveCsv(const std::filesystem::path& path,
                       plot::WaveCsvShape shape,
                       const plot::CsvExportRange& range,
                       std::string& error) const;
    bool exportRawCaptureCsv(const std::filesystem::path& path,
                             const plot::CsvExportRange& range,
                             std::string& error) const;
    bool exportSessionPackage(const std::filesystem::path& path, std::string& error) const;
    bool importSessionPackage(const std::filesystem::path& path, std::string& error);
    bool applySessionPackage(const session::SessionPackageData& package, bool restoreCapture, std::string& error,
                             bool allowMissingProtocol = false);
    bool importWaveRawCapture(const plot::RawCaptureFileData& capture, std::string& error);

    enum class OfflineReplayPhase {
        Preparing,
        Replaying,
        Draining,
        Paused,
        Completed,
        Failed,
        Cancelled,
    };

    struct RawCaptureReplayStatus {
        bool loaded{false};
        bool playing{false};
        std::size_t eventIndex{0};
        std::size_t eventCount{0};
        double progress{0.0};
        double speed{1.0};
        OfflineReplayPhase phase{OfflineReplayPhase::Paused};
        OfflineReplayProcessing processing{OfflineReplayProcessing::BrowseRaw};
        OfflineReplayPacing pacing{OfflineReplayPacing::OriginalTimeline};
        std::string protocolSource;
        std::size_t inputBacklog{0};
        std::size_t workerBacklog{0};
        std::size_t outputBacklog{0};
        bool hasStreamSchema{false};
        bool truncated{false};
        std::string error;
    };

    bool loadRawCaptureReplayTimeline(const plot::RawCaptureFileData& capture, std::string& error);
    bool loadRawCaptureReplayTimeline(const plot::RawCaptureFileData& capture,
                                      OfflineReplayProcessing processing,
                                      OfflineReplayPacing pacing,
                                      std::string& error,
                                      const session::SessionPackageData* package = nullptr);
    void unloadRawCaptureReplayTimeline();
    bool playRawCaptureReplay(std::string& error);
    void pauseRawCaptureReplay();
    bool stepRawCaptureReplay(std::string& error);
    bool seekRawCaptureReplay(std::size_t eventIndex, std::string& error);
    void setRawCaptureReplaySpeed(double speed);
    [[nodiscard]] RawCaptureReplayStatus rawCaptureReplayStatus() const;
    bool startRawCaptureRecording(const std::filesystem::path& path, std::string& error);
    bool stopRawCaptureRecording(std::string& error);
    [[nodiscard]] bool isRawCaptureRecording() const;
    [[nodiscard]] const std::filesystem::path& rawCaptureRecordingPath() const;
    [[nodiscard]] std::uint64_t rawCaptureRecordingBytes() const;
    void resetWaveHistory();
    bool exportWaveAnalysisReport(const std::filesystem::path& path, std::string& error) const;
    bool loadElfStaticAddressFile(const std::filesystem::path& path, std::string& error);
    void clearElfStaticAddressFile();
    [[nodiscard]] std::uint64_t elfStaticAddressRevision() const;
    [[nodiscard]] std::vector<scripting::ElfSymbolValue> queryElfStaticAddresses(const std::string& queryText,
                                                                                 std::size_t limit) const;
    void refreshSelectedElfSymbolControls();
    void rebuildTransferFrameRows();
    void activateParsedTransferLogView();
    // 清空收发记录与已解析帧缓存；同时使历史切换任务/旧确认失效。
    void clearTransferLogHistory();
    // 原始->逐帧切换的三态历史策略入口：宿主确认 modal 通过 resolveHistorySwitch/cancelHistorySwitch 回写结果。
    [[nodiscard]] std::string parsedTransferUnavailableReason() const;
    void requestParsedHistorySwitch();
    void resolveHistorySwitch(config::TransferLogHistoryOnParsedSwitch decision);
    void cancelHistorySwitch();
    [[nodiscard]] bool historySwitchConfirmationPending() const { return historySwitchConfirmationOpen_; }
    [[nodiscard]] std::uint64_t historySwitchConfirmationId() const { return historySwitchConfirmationId_; }
    [[nodiscard]] config::TransferLogHistoryOnParsedSwitch historySwitchStrategy() const {
        return runtimeConfig_.gui.historyOnParsedSwitch;
    }
    [[nodiscard]] config::TransferLogHistoryScope historySwitchScope() const { return runtimeConfig_.gui.historyScope; }
    logging::LoggingFacade& logger();
    const logging::LoggingFacade& logger() const;
    std::vector<scripting::DialogRequest> drainDialogRequests();
    void respondDialog(const scripting::DialogEvent& event);
    std::vector<scripting::FileDialogRequest> drainFileDialogRequests();
    void respondFileDialog(const scripting::FileDialogEvent& event);

    std::optional<std::uint64_t> nextWakeupAtMs() const;
    void setTransportFactoryForTest(
        std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> factory);

private:
    // 仅测试协调背压观察/取消，不暴露业务侧 worker 访问入口。
    friend struct ApplicationBackpressureTestAccess;
    friend struct ApplicationSettingsTestAccess;

    // 跨职责实现共用的辅助函数，不增加对象状态或公开接口。
    static std::uint64_t nowMs();
    static std::uint64_t nowUs();
    static std::string csvEscape(std::string_view text);
    static std::string formatFrequencyInput(double valueHz);
    static bool sameAppliedPlotSetup(const plot::WaveDockState& wave, const plot::RawCapturePlotSetupEventData& setup);
    static std::string payloadPreviewSuffix(const config::AppLoggingConfig& logging,
                                           const std::vector<std::uint8_t>& payload);
    static std::string transportContextFields(std::string_view kind, const transport::ConnectionContext& context);

    enum class WaveResetViewportTrigger {
        PlotSetupReset,
        ManualClear,
        RawImport,
    };

    struct ActiveTxRequest {
        scripting::TxRequest request;
        std::uint64_t sentAtMs{0};
        std::uint64_t waitDeadlineMs{0};
    };

    struct TransferFrameParserState {
        scripting::FrameStreamParser rx;
        scripting::FrameStreamParser tx;
    };

    struct PendingRxBytes {
        transport::ConnectionContext context{};
        std::vector<std::uint8_t> bytes{};
        std::size_t offset{0};
    };

    // 历史重建范围输入：记录顺序的 RX/TX 字节 + profile 事件；不引入范围外字节。
    struct HistoryRebuildSourceEvent {
        plot::RawCaptureEventType type{plot::RawCaptureEventType::RxBytes};
        std::string direction;
        std::uint64_t timestampMs{0};
        std::string endpoint;
        plot::RawCaptureProfileEventData profile;
        std::uint64_t position{0};
        std::size_t byteSize{0};
        std::size_t rowIndex{0};
        std::uint64_t rowFrontPosition{0};
    };

    // 历史重建任务的唯一状态载体；来源键 = rawCaptureReplay_.generation + 导入代次标记。
    struct HistoryRebuildTask {
        bool hasLoadedTimeline{false};
        std::uint64_t generation{0};
        std::size_t upperBound{0};
        std::size_t eventCursor{0};
        std::size_t byteOffset{0};
        std::size_t sourceCount{0};
        std::uint64_t firstPosition{0};
        std::uint64_t rowBegin{0};
        std::uint64_t rowEnd{0};
        std::size_t rowProfileCursor{0};
        std::optional<dock::ReceiveRow> currentRow;
        std::shared_ptr<const std::vector<plot::RawCaptureEvent>> boundaryProfiles;
        std::size_t boundaryProfileCursor{0};
        bool boundaryProfilesStarted{false};
        bool rowsOnly{false};
        bool legacyPayload{false};
        std::deque<HistoryRebuildSourceEvent> following;
        std::size_t followingBytes{0};
        std::optional<TransferFrameParserState> parser;
        std::deque<dock::ReceiveRow> rebuilt;
    };

    struct StreamBufferAlertState {
        std::uint64_t connectionId{0};
        bool popupMuted{false};
        bool popupOpen{false};
    };

    struct RealtimeBacklogDiscardCounts {
        std::size_t transportEvents{0};
        std::size_t rxBytes{0};
        std::size_t transferFrameRows{0};
        std::size_t plotAppends{0};
        std::size_t scriptLogs{0};
        std::size_t scriptEvents{0};
    };

    struct CommPressureDebugSnapshot {
        std::size_t pendingRxBytes{0};
        std::size_t pendingTransferFrameRows{0};
        std::size_t pendingPlotAppends{0};
        std::size_t rxInputQueueBytes{0};
        std::size_t parserPendingBytes{0};
        std::size_t postprocessPendingBatches{0};
        std::size_t luaPendingItems{0};
        std::size_t uiPendingItems{0};
        std::size_t postprocessWorkerThreads{0};
        std::string backlogWarning;
        std::size_t lastPumpEvents{0};
        std::size_t lastPumpRxBytes{0};
        std::size_t lastPumpStreamFrames{0};
        std::size_t lastPumpStreamErrors{0};
        double lastPumpTransportMs{0.0};
        double lastPumpParserMs{0.0};
        double lastPumpCallbackMs{0.0};
        double lastPumpScriptMs{0.0};

        bool operator==(const CommPressureDebugSnapshot&) const = default;
    };

    struct CommPressureDebugLogState {
        bool hasSnapshot{false};
        CommPressureDebugSnapshot lastSnapshot{};
        std::uint64_t lastLogMs{0};
    };

    struct RawCaptureReplayState {
        bool loaded{false};
        bool playing{false};
        plot::RawCaptureFileData capture{};
        transport::ConnectionContext context{};
        std::size_t eventIndex{0};
        std::size_t legacyByteOffset{0};
        bool legacyEventStarted{false};
        std::size_t targetEventIndex{0};
        std::size_t completedLuaInputs{0};
        bool resumeAfterSeek{false};
        bool replaceWaveLabels{false};
        OfflineReplayPacing pacingAfterSeek{OfflineReplayPacing::OriginalTimeline};
        double speed{1.0};
        double accumulatedMs{0.0};
        std::uint64_t lastPumpMs{0};
        std::uint64_t generation{0};
        OfflineReplayPhase phase{OfflineReplayPhase::Paused};
        OfflineReplayProcessing processing{OfflineReplayProcessing::BrowseRaw};
        OfflineReplayPacing pacing{OfflineReplayPacing::OriginalTimeline};
        std::string protocolSource;
        std::string error;
        std::filesystem::path temporaryProtocolDir;
        std::filesystem::path temporaryStorageDir;
        std::string protocolDirectory;
        std::vector<session::SessionPackageEntry> packageProtocolEntries;
        bool hasStreamSchema{false};
        std::shared_future<scripting::ScriptRuntimeLoadResult> preparation;
        std::shared_future<std::pair<bool, std::string>> profileFence;
    };

    std::unique_ptr<transport::ITransport> createTransport(transport::TransportKind kind) const;
    transport::TransportConfig currentTransportConfig(transport::TransportKind kind) const;
    void syncDockState();
    void updateAdaptivePerformance();
    void applyAdaptiveWaveBudget();
    void syncAdaptivePerformanceStatus();
    void maybeLogCommPressureDebug(const dock::CommDockState& comm);
    bool applyScriptOutputBatch(const scripting::ScriptRuntimeOutputBatch& batch);
    bool applyOfflineScriptOutputBatch(scripting::ScriptRuntimeOutputBatch batch);
    void applyLuaScriptSnapshot(const scripting::ScriptRuntimeSnapshot& snapshot);
    bool probeProtocolDirectory(const std::string& resolvedDirText);
    void prepareProtocolRuntimeReload();
    bool applyProtocolLoadResult(const std::string& resolvedDirText,
                                 const std::string& protocolName,
                                 const std::string& scriptPath,
                                 const scripting::ScriptRuntimeLoadResult& loadResult,
                                 bool preserveImportedWave = false);
    void refreshTransferFrameDisplayAfterProtocolReload();
    bool handleTransportEvents();
    void resetTransportPumpMetrics();
    void pullTransportEventsFromTransport();
    bool drainTransportEventQueues(const std::chrono::steady_clock::time_point& startedAt,
                                   std::size_t maxRxBytes,
                                   std::size_t& processedEvents,
                                   std::size_t& processedRxBytes);
    bool processTransportEvent(const transport::TransportEvent& event);
    bool processTransportOpenEvent(const transport::TransportOpenEvent& event);
    bool processTransportCloseEvent(const transport::TransportCloseEvent& event);
    bool processTransportErrorEvent(const transport::TransportErrorEvent& event);
    bool processTransportBytesEvent(const transport::TransportBytesEvent& event);
    std::size_t tryProcessLiveRxBytes(const transport::TransportBytesEvent& event, std::size_t maxBytes);
    bool processTransportTxEvent(const transport::TransportTxEvent& event);
    bool processPendingRxBytes(std::size_t maxBytes);
    void enqueuePendingRxBytes(transport::TransportBytesEvent event);
    void detachPendingRealtimeBacklogFromConnection();
    RealtimeBacklogDiscardCounts clearPendingRealtimeBacklog();
    void logRealtimeBacklogDiscard(const RealtimeBacklogDiscardCounts& counts);
    [[nodiscard]] bool responsiveBacklogMode() const;
    [[nodiscard]] std::size_t rxBytesPerPump() const;
    [[nodiscard]] std::size_t transferFrameRowsPerPump() const;
    [[nodiscard]] std::size_t plotAppendsPerPump() const;
    [[nodiscard]] double outputFlushBudgetMs() const;
    [[nodiscard]] std::size_t pendingRxByteCount() const;
    [[nodiscard]] bool hasPendingRequestDrainWork() const;
    bool drainRequestTimeoutBacklog();
    bool flushScriptOutputs();
    bool flushScriptOutputsUnbounded();
    bool flushOfflineScriptOutputs();
    bool flushOfflineScriptOutputsUnbounded();
    bool flushScriptLogs();
    bool flushScriptPlots();
    bool flushPendingTransferFrameRows(std::size_t maxRows);
    bool flushScriptStatusAndDialogs();
    bool processScriptRequestCompletions();
    bool processRequestTimeouts();
    bool driveTxScheduler();
    bool enqueueTxRequest(scripting::TxRequest request);
    bool applyScriptTransportStats(const scripting::ScriptRuntimeOutputBatch& batch);
    bool applyScriptTxOutputs(const scripting::ScriptRuntimeOutputBatch& batch);
    bool applyScriptRuntimeProfileEvents(const scripting::ScriptRuntimeOutputBatch& batch);
    bool applyScriptUiAndLogOutputs(const scripting::ScriptRuntimeOutputBatch& batch);
    bool applyScriptOscilloscopeOutputs(const scripting::ScriptRuntimeOutputBatch& batch);
    bool applyScriptPlotOutputs(const scripting::ScriptRuntimeOutputBatch& batch, bool rebuilding = false);
    bool applyScriptPlotSetups(const std::vector<scripting::PlotSetup>& setups, bool rebuilding = false);
    void enqueueScriptPlotAppends(const std::vector<std::pair<std::size_t, plot::WaveAppendRequest>>& appends);
    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> drainScriptPlotAppendsForPump(
        std::size_t maxPlotAppends);
    bool appendScriptPlotRequestsToWave(std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> requests,
                                        bool rebuilding = false);
    void finishTxRequest(const scripting::TxRequest& request,
                         scripting::TxEventState state,
                         std::optional<std::string> error,
                         std::uint64_t finishedAtMs);
    void cancelPendingTxRequests(const std::string& reason, std::uint64_t finishedAtMs);
    void cancelAllTxRequests(const std::string& reason);
    void configureScriptWorker(const config::AppConfig& config);
    bool prepareSessionScriptConfig(const session::SessionPackageData& package, std::string& error);
    void resetDataImportGenerationState(bool cleanupReplayResources);
    bool selectSessionImportProtocol(const session::SessionPackageData& package, std::string& directory,
                                     std::filesystem::path& extractedDirectory, std::string& error);
    bool restoreSessionPackageContext(const session::SessionPackageData& package,
                                      bool allowMissingProtocol,
                                      std::vector<plot::WaveAnalysisMarker>& markers,
                                      std::string& error);
    void notifyTxOverflow(const std::string& message);
    void handleStreamBufferAlert(const transport::ConnectionContext& context,
                                 const scripting::StreamParseBatch& batch,
                                 const scripting::StreamBufferDefinition& bufferDefinition);
    void resetStreamBufferAlertState(std::uint64_t connectionId = 0);
    void enqueueDialogRequest(const scripting::DialogRequest& request);
    void appendTransferRow(dock::ReceiveRow row);
    [[nodiscard]] bool validateOfflineReplayTransport(std::string& error) const;
    void appendLiveRawCapture(const transport::TransportBytesEvent& event);
    void appendRawCaptureEvent(const plot::RawCaptureEvent& event);
    bool validateRawCaptureImport(const plot::RawCaptureFileData& capture, std::string& error) const;
    bool prepareOfflineReplayProtocol(const plot::RawCaptureFileData& capture,
                                      OfflineReplayProcessing processing,
                                      const session::SessionPackageData* package,
                                      std::string& error);
    void prepareRawCaptureImportReplay(const plot::RawCaptureFileData& capture);
    void prepareRawCaptureBrowse(const plot::RawCaptureFileData& capture);
    void cleanupOfflineReplayResources();
    bool drainRawCaptureReplay(std::string& error);
    [[nodiscard]] transport::ConnectionContext makeRawCaptureReplayContext(
        const plot::RawCaptureFileData& capture) const;
    bool replayRawCaptureEvents(const plot::RawCaptureFileData& capture, std::string& error);
    bool pumpRawCaptureReplay(std::string& error);
    bool replayRawCaptureEventAt(std::size_t eventIndex, std::string& error);
    bool replayRawCaptureEvent(const plot::RawCaptureEvent& event,
                               transport::ConnectionContext& replayContext,
                               std::string& error);
    bool applyRawCaptureRuntimeProfileEvent(const plot::RawCaptureEvent& event, bool cleared, std::string& error);
    void enqueueRawCaptureBytes(const transport::ConnectionContext& replayContext,
                                const std::vector<std::uint8_t>& bytes);
    void replayRawCaptureBytes(const transport::ConnectionContext& replayContext,
                               const std::vector<std::uint8_t>& bytes);
    void flushRawCaptureReplayBatch();
    bool applyTransferFrameRuntimeProfileEvent(const scripting::StreamRuntimeProfileEvent& event, std::string& error);
    void finishRawCaptureImportReplay();
    void cancelRawCaptureImportReplay();
    bool applyPlotSetup(const plot::RawCapturePlotSetupEventData& setup, bool rebuilding = false);
    bool offlineWaveRebuilding() const;
    void freezeImportedChannelLabels();
    void switchToLiveWaveSource();
    plot::RawCapturePlotSetupEventData normalizeImportedSetup(plot::RawCapturePlotSetupEventData setup) const;
    bool applyResetViewportPolicy(WaveResetViewportTrigger trigger, const plot::ViewConfig& protocolView);
    void applyLegacyResetViewport(const plot::ViewConfig& protocolView, bool resetVerticalRange);
    void resetWaveHistoryForTrigger(WaveResetViewportTrigger trigger);
    void recordPlotSetupSnapshot(const plot::RawCapturePlotSetupEventData& setup, std::uint64_t timestampMs);
    void resetTransferFrameParser();
    void resetTransferFrameDisplayState();
    void appendTransferFrameRows(const dock::ReceiveRow& sourceRow);
    void enqueueTransferFrameRows(std::vector<dock::ReceiveRow> rows);
    void trimPendingTransferFrameRowsToLimit();
    // 三态历史切换与分批重建实现。
    std::uint64_t historySwitchGeneration() const;
    void invalidateHistorySwitchWork(std::string_view reason);
    bool beginHistoryRebuild(config::TransferLogHistoryScope scope, std::uint64_t generation, std::size_t upperBound);
    bool pumpHistoryRebuild();
    bool historySourceAvailable() const;
    void rememberTransferProfile(const plot::RawCaptureEvent& event);
    void appendHistoryReceiveRow(dock::ReceiveRow row);
    bool queueHistoryFollowing(HistoryRebuildSourceEvent event);
    void applyHistoryLimits(const config::GuiLogHistoryConfig& config);
    [[nodiscard]] dock::ReceiveRow makeTransferFrameRow(const dock::ReceiveRow& sourceRow,
                                                        const scripting::StreamParsedFrame& frame) const;
    [[nodiscard]] std::optional<TransferFrameParserState> makeTransferFrameParserState() const;
    [[nodiscard]] std::optional<TransferFrameParserState> makeTransferFrameParserState(
        const scripting::ScriptRuntimeSnapshot& snapshot) const;

    dock::DockStore dockStore_;
    config::ConfigStore configStore_{};
    config::AppConfig runtimeConfig_{};
    AdaptivePerformanceController adaptivePerformance_{};
    std::optional<AdaptivePerformanceStatus> loggedAdaptivePerformanceStatus_{};
    bool loadedConfigFromDisk_{false};
    std::optional<config::ProtocolConfig> captureProtocolConfigOverride_;
    logging::LoggingFacade loggingFacade_{};
    scripting::ScriptRuntimeWorker scriptWorker_;
    std::future<bool> settingsBudgetFence_;
    std::optional<config::AppConfig> pendingSettingsConfig_;
    std::optional<config::AppConfig> pendingSettingsBaseline_;
    SettingsApplyOptions pendingSettingsOptions_;
    SettingsApplyResult settingsApplyResult_;
    bool settingsPartialSaveBlocked_{false};
    std::optional<config::PerformanceExplicitOverrides> settingsWorkerOriginalFlags_;
    std::uint64_t settingsConfirmationId_{0};
    std::optional<config::AppConfig> settingsConfirmationRuntime_;
    std::optional<config::AppConfig> settingsConfirmationDraft_;
    void commitSettingsChanges(const config::AppConfig& value);
    std::unique_ptr<scripting::ScriptRuntimeWorker> offlineScriptWorker_;
    // future 持有后台退场任务，未完成时不能销毁（std::async 的析构会等待）。
    std::vector<std::future<std::string>> retiringOfflineWorkers_;
    plugin::ElfStaticViewBridge elfStaticView_;
    std::uint64_t elfStaticAddressRevision_{0};
    std::unique_ptr<transport::ITransport> transport_;
    std::optional<transport::ConnectionContext> activeConnection_;
    std::function<std::unique_ptr<transport::ITransport>(transport::TransportKind)> transportFactoryForTest_;
    std::deque<scripting::TxRequest> pendingTxQueue_;
    std::optional<ActiveTxRequest> activeWrite_;
    std::optional<ActiveTxRequest> activeHalfDuplexRequest_;
    bool txRequestGuardHalted_{false};
    std::deque<scripting::DialogRequest> pendingDialogs_;
    std::unordered_map<std::uint64_t, scripting::DialogRequest> openDialogs_;
    std::deque<scripting::FileDialogRequest> pendingFileDialogs_;
    std::unordered_map<std::uint64_t, scripting::FileDialogRequest> openFileDialogs_;
    std::deque<std::pair<std::size_t, plot::WaveAppendRequest>> pendingScriptPlotAppends_;
    std::unordered_map<std::string, std::uint64_t> dialogDedupeKeys_;
    StreamBufferAlertState streamBufferAlertState_{};
    std::optional<TransferFrameParserState> transferFrameParser_;
    plot::RawCaptureStreamWriter rawCaptureRecording_;
    RawCaptureReplayState rawCaptureReplay_;
    // 按位置冻结两组名称，短前缀重建不丢失尚未出现的位置。
    std::vector<std::string> importedActualLabels_;
    std::vector<std::string> importedDefaultLabels_;
    // 历史切换来源键：导入用单调计数器；live 用稳定计数器，仅在导入/seek/协议变更时递增，
    // 避免实时事件使进行中的历史重建被误失效。
    std::optional<std::uint64_t> importedRebuildGeneration_;
    std::uint64_t importCounter_{0};
    std::uint64_t liveGeneration_{0}; // 所有显式失效入口递增；字节接纳不改变代次。
    std::uint64_t transferPosition_{0};
    std::shared_ptr<const std::vector<plot::RawCaptureEvent>> transferProfiles_;
    std::size_t rawCaptureReplayChunkBytes_{1024U};
    std::size_t rawCaptureReplayPendingBytes_{0U};
    std::vector<std::uint8_t> rawCaptureReplayBatchBytes_;
    transport::ConnectionContext rawCaptureReplayBatchContext_{};
    bool replayReceiveHistory_{false};
    // 原始->逐帧切换的三态状态机：确认 modal 打开标记 + 历史重建任务（唯一所有者）。
    bool historySwitchConfirmationOpen_{false};
    std::uint64_t historySwitchConfirmationId_{0};
    std::optional<HistoryRebuildTask> historyRebuild_;
    std::deque<transport::TransportEvent> pendingTransportEvents_;
    std::deque<PendingRxBytes> pendingRxByteChunks_;
    std::deque<dock::ReceiveRow> pendingTransferFrameRows_;
    std::optional<std::uint64_t> cachedWaveSummaryRevision_;
    CommPressureDebugLogState commPressureDebugLog_{};
    bool suppressRawCaptureProfileEvents_{false};
    bool suppressRawCapturePlotSetupEvents_{false};
    std::uint64_t rawEventSequence_{0};
    std::size_t retainedRawBytes_{0};
    DataTransferTask dataTransfer_;
    bool dataImportActive_{false};
    OfflineReplayProcessing dataImportProcessing_{OfflineReplayProcessing::BrowseRaw};
    OfflineReplayPacing dataImportPacing_{OfflineReplayPacing::FastBatchParse};
    // 已载入包协议独立于离线 timeline；卸载原始浏览不能使协议面板的目录失效。
    std::vector<std::filesystem::path> importedProtocolDirectories_;
    std::filesystem::path pendingImportProtocolDirectory_;
    std::string pendingImportProtocol_;
    std::optional<DataImportBatch> pendingImportMetadata_;
    bool importProtocolPrepared_{false};
    std::optional<config::AppConfig> pendingImportPreviousConfig_;
    std::shared_future<scripting::ScriptRuntimeLoadResult> importProtocolLoad_;
    std::shared_future<scripting::ScriptRuntimeLoadResult> retiringImportProtocolLoad_;
    std::shared_ptr<session::SessionPackageData> pendingImportSession_;
    std::vector<plot::WaveAnalysisMarker> pendingImportMarkers_;
    // 明确关联身份，波形单独导入后不得借用先前采集的 raw。
    bool importedRawAssociated_{false};
    bool protocolSwitchReplay_{false};
    bool discardProtocolInitializationOutputs_{false};
    // 初始化输出排空前暂存用户控件操作，避免与初始化一起被丢弃。
    std::vector<std::function<void()>> deferredProtocolControls_;
    bool importedWaveIncomplete_{false};
    std::string importedWaveRange_{"full"};
    bool importReplacedWave_{false};
    bool importReplacedRecords_{false};
    bool applyingImportContext_{false};
    bool parseImportedWave_{false};
    std::optional<std::future<bool>> importReset_;
    std::optional<std::future<std::pair<bool, std::string>>> importProfile_;
    plot::RawCaptureFileData pendingImportedCapture_;
    bool pumpDataImport();
};

} // namespace protoscope::app
