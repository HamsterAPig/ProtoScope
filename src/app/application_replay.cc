#include "protoscope/app/application.hpp"
#include "protoscope/scripting/pipeline_threading.hpp"
#include "protoscope/session/session_package.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace protoscope::app {

namespace {

    constexpr std::size_t kRawCaptureReplayLargeChunkBytes = 64U * 1024U;
    constexpr std::size_t kRawCaptureReplaySyncBatchBytes = 1024U * 1024U;
    constexpr std::size_t kRawCaptureReplayEventsPerPump = 256;
    constexpr std::size_t kRawCaptureReplayBytesPerPump = 64U * 1024U;
    constexpr auto kRawCaptureReplayPumpBudget = std::chrono::milliseconds(4);
    constexpr std::string_view kSessionProtocolEntryPrefix = "protocol/";

    std::size_t resolveRawCaptureReplayChunkBytes(const std::optional<scripting::StreamBufferDefinition>& streamBuffer)
    {
        if (!streamBuffer.has_value()) {
            return kRawCaptureReplayLargeChunkBytes;
        }
        if (streamBuffer->dropOldest) {
            // drop_oldest 模式下输入块必须小于环形缓冲区容量，避免覆盖上一个块留下的半帧。
            return (std::max)(std::size_t{1U}, streamBuffer->capacity / 2U);
        }
        return kRawCaptureReplayLargeChunkBytes;
    }


    bool writeFileBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes, std::string& error)
    {
        std::error_code directoryError;
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError) {
                error = "创建目录失败: " + directoryError.message();
                return false;
            }
        }
        std::ofstream out(path, std::ios::binary);
        if (!out.good()) {
            error = "无法写入文件: " + path.generic_string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) {
            error = "写入文件失败: " + path.generic_string();
            return false;
        }
        return true;
    }


    std::vector<const session::SessionPackageEntry*> collectSessionProtocolEntries(
        const session::SessionPackageData& package)
    {
        std::vector<const session::SessionPackageEntry*> protocolEntries;
        for (const auto& entry : package.entries) {
            if (entry.name.starts_with(kSessionProtocolEntryPrefix)) {
                protocolEntries.push_back(&entry);
            }
        }
        return protocolEntries;
    }


    bool releaseSessionProtocolEntries(const std::vector<const session::SessionPackageEntry*>& protocolEntries,
                                       const std::filesystem::path& protocolDir,
                                       std::string& error)
    {
        for (const auto* entry : protocolEntries) {
            const auto relativeName = entry->name.substr(kSessionProtocolEntryPrefix.size());
            const auto outputPath = protocolDir / std::filesystem::path(relativeName);
            if (!writeFileBytes(outputPath, entry->bytes, error)) {
                const auto writeError = error;
                error = "释放现场包协议目录失败: " + writeError;
                return false;
            }
        }
        return true;
    }

} // namespace

bool Application::selectSessionImportProtocol(const session::SessionPackageData& package, std::string& directory,
                                               std::filesystem::path& extractedDirectory, std::string& error)
{
    directory = dockStore_.luaState().protocolDir;
    extractedDirectory.clear();
    const auto entries = collectSessionProtocolEntries(package);
    if (runtimeConfig_.gui.importProtocolSource == config::ImportProtocolSource::CurrentProtocol || entries.empty())
        return true;
    // 调用方先校验所有条目名称；存在但缺 main.lua 的包协议不能伪装成“缺失”回退。
    if (std::none_of(entries.begin(), entries.end(), [](const auto* entry) { return entry->name == "protocol/main.lua"; })) {
        error = "现场包协议损坏：缺少 protocol/main.lua";
        return false;
    }
    extractedDirectory = std::filesystem::temp_directory_path() /
        ("ProtoScope-import-protocol-" + std::to_string(nowUs()));
    if (!releaseSessionProtocolEntries(entries, extractedDirectory, error)) return false;
    directory = extractedDirectory.generic_string();
    return true;
}

bool Application::validateOfflineReplayTransport(std::string& error) const
{
    if (!transport_) {
        return true;
    }
    const auto state = transport_->state();
    if (state != transport::TransportState::Opening && state != transport::TransportState::Open) {
        return true;
    }
    error = "实时连接仍处于打开状态，请先关闭连接再载入离线回放";
    return false;
}


bool Application::importRawRecords(const plot::RawCaptureFileData& data, std::string& error)
{
    if (!validateOfflineReplayTransport(error)) return false;
    // 新原始导入取代旧 timeline 来源；先复制以兼容参数引用旧 timeline 成员。
    const auto imported = data;
    if (rawCaptureReplay_.loaded) unloadRawCaptureReplayTimeline();
    dockStore_.clearReceiveRows();
    dockStore_.clearTransferFrameRows();
    resetTransferFrameParser();
    pendingTransferFrameRows_.clear();
    dockStore_.waveState().rawCapture = imported;
    importedRawAssociated_ = !imported.events.empty() || !imported.payload.empty();
    transferProfiles_.reset();
    replayReceiveHistory_ = true;
    // 纯导入也建立导入代次来源键，供历史切换与失效判定；不加载 timeline、不改时间轴起点行为。
    // 使用单调递增计数器：同一事件数的再导入也必须让旧确认/旧任务失效。
    importedRebuildGeneration_ = ++importCounter_;
    // 重新导入使历史切换任务/旧确认失效，过期结果不得提交到新来源。
    invalidateHistorySwitchWork("重新导入");
    // 导入含原始字节后收发面板默认原始模式，保留字节、顺序、时间戳、端点与状态。
    dockStore_.receiveState().displayMode = dock::TransferLogDisplayMode::RawChunks;
    for (auto& event : dockStore_.waveState().rawCapture.events) {
        event.displayPosition = ++transferPosition_;
        rememberTransferProfile(event);
        if (event.type != plot::RawCaptureEventType::RxBytes && event.type != plot::RawCaptureEventType::TxBytes) continue;
        // 事件端点缺失时回退到记录来源，保证 RX/TX 行端点信息完整。
        auto endpoint = event.endpoint;
        if (endpoint.empty()) endpoint = imported.source;
        appendHistoryReceiveRow({.timestampMs = event.timestampMs,
            .direction = event.type == plot::RawCaptureEventType::TxBytes ? "TX" : "RX",
            .endpoint = std::move(endpoint), .bytes = event.bytes, .message = event.writeStatus,
            .messageIsWriteStatus = event.type == plot::RawCaptureEventType::TxBytes,
            .displayPosition = event.displayPosition});
    }
    if (imported.events.empty() && !imported.payload.empty())
        appendHistoryReceiveRow({.timestampMs = imported.capturedAtMs, .direction = "RX",
            .endpoint = imported.source, .bytes = imported.payload, .displayPosition = ++transferPosition_});
    return true;
}


bool Application::validateRawCaptureImport(const plot::RawCaptureFileData&, std::string& error) const
{
    if (dataTransferStatus().active && !dataImportActive_) {
        error = "数据任务进行中，不能载入回放";
        return false;
    }
    return validateOfflineReplayTransport(error);
}


bool Application::prepareOfflineReplayProtocol(const plot::RawCaptureFileData& capture,
                                               const OfflineReplayProcessing processing,
                                               const session::SessionPackageData* package,
                                               std::string& error)
{
    static_cast<void>(capture);
    rawCaptureReplay_.processing = processing;
    rawCaptureReplay_.protocolSource = processing == OfflineReplayProcessing::BrowseRaw
                                           ? "不执行 Lua"
                                           : processing == OfflineReplayProcessing::CurrentProtocol ? "当前磁盘协议"
                                                                                                     : "现场包内协议";
    const auto existingProtocolDirectory = rawCaptureReplay_.protocolDirectory;
    rawCaptureReplay_.protocolDirectory.clear();
    if (processing == OfflineReplayProcessing::BrowseRaw) {
        return true;
    }

    auto offlineConfig = runtimeConfig_;
    offlineConfig.scripting.fileIo = {};
    offlineConfig.scripting.fileIo.enabled = false;
    offlineConfig.scripting.fileIo.dialog.enabled = false;
    rawCaptureReplay_.temporaryStorageDir =
        std::filesystem::temp_directory_path() /
        ("ProtoScope-offline-storage-" + std::to_string(rawCaptureReplay_.generation));
    offlineConfig.scripting.storageRootDir = rawCaptureReplay_.temporaryStorageDir.generic_string();
    offlineScriptWorker_ = std::make_unique<scripting::ScriptRuntimeWorker>();
    offlineScriptWorker_->configure(scripting::ScriptRuntimeWorkerConfig{
        .execution = offlineConfig.scripting.execution,
        .storageRoot = offlineConfig.scripting.storageRootDir,
        .storageConfig = offlineConfig.scripting.storage,
        .enabled = offlineConfig.scripting.workerEnabled,
        .offlineRestricted = true,
        .postprocessWorkerThreads = scripting::resolvePipelineWorkerThreads(
            offlineConfig.scripting.pipeline.workerThreads, std::thread::hardware_concurrency()),
        .rxQueueLimitBytes = offlineConfig.scripting.workerRxQueueLimitBytes,
        .memoryBudgetBytes = offlineConfig.scripting.workerMemoryBudgetBytes,
        .memoryBudgetAvailableRatio = offlineConfig.scripting.workerMemoryBudgetAvailableRatio,
        .outputQueueLimit = offlineConfig.scripting.workerOutputQueueLimit,
        .batchBytes = offlineConfig.scripting.workerBatchBytes,
        .backpressureEnabled = offlineConfig.scripting.workerBackpressureEnabled,
        .backpressureHighWatermark = offlineConfig.scripting.workerBackpressureHighWatermark,
        .backpressureLowWatermark = offlineConfig.scripting.workerBackpressureLowWatermark,
    });
    offlineScriptWorker_->setFileIoConfig(offlineConfig.scripting.fileIo);

    std::filesystem::path protocolDir;
    if (processing == OfflineReplayProcessing::PackageProtocol && !rawCaptureReplay_.packageProtocolEntries.empty()) {
        protocolDir = std::filesystem::temp_directory_path() /
                      ("ProtoScope-session-protocol-" + std::to_string(nowUs()));
        rawCaptureReplay_.temporaryProtocolDir = protocolDir;
        std::vector<const session::SessionPackageEntry*> protocolEntries;
        protocolEntries.reserve(rawCaptureReplay_.packageProtocolEntries.size());
        for (const auto& entry : rawCaptureReplay_.packageProtocolEntries) protocolEntries.push_back(&entry);
        if (!releaseSessionProtocolEntries(protocolEntries, protocolDir, error)) {
            cleanupOfflineReplayResources();
            return false;
        }
    } else if (!existingProtocolDirectory.empty()) {
        protocolDir = existingProtocolDirectory;
    } else if (processing == OfflineReplayProcessing::CurrentProtocol) {
        protocolDir = dockStore_.luaState().protocolDir;
        if (protocolDir.empty()) {
            error = "当前磁盘协议尚未加载";
            return false;
        }
    } else {
        if (package == nullptr) {
            error = "现场包协议来源不可用";
            return false;
        }
        const auto protocolEntries = collectSessionProtocolEntries(*package);
        if (protocolEntries.empty()) {
            error = "现场包不包含协议目录";
            return false;
        }
        rawCaptureReplay_.packageProtocolEntries.clear();
        rawCaptureReplay_.packageProtocolEntries.reserve(protocolEntries.size());
        for (const auto* entry : protocolEntries) rawCaptureReplay_.packageProtocolEntries.push_back(*entry);
        protocolDir = std::filesystem::temp_directory_path() /
                      ("ProtoScope-session-protocol-" + std::to_string(nowUs()));
        rawCaptureReplay_.temporaryProtocolDir = protocolDir;
        if (!releaseSessionProtocolEntries(protocolEntries, protocolDir, error)) {
            cleanupOfflineReplayResources();
            return false;
        }
    }

    rawCaptureReplay_.protocolDirectory = protocolDir.generic_string();
    rawCaptureReplay_.phase = OfflineReplayPhase::Preparing;
    rawCaptureReplay_.preparation = offlineScriptWorker_->loadProtocolDirectoryAsync(rawCaptureReplay_.protocolDirectory).share();
    return true;
}


void Application::prepareRawCaptureBrowse(const plot::RawCaptureFileData& capture)
{
    dockStore_.clearScriptRows();
    const auto liveSnapshot = scriptWorker_.snapshot();
    static_cast<void>(scriptWorker_.drainOutputs());
    const auto discarded = clearPendingRealtimeBacklog();
    logRealtimeBacklogDiscard(discarded);
    resetWaveHistoryForTrigger(WaveResetViewportTrigger::RawImport);
    dockStore_.clearReceiveRows();
    transferProfiles_.reset();
    pendingTransferFrameRows_.clear();
    transferFrameParser_ = makeTransferFrameParserState(liveSnapshot);
    replayReceiveHistory_ = true;
    auto& wave = dockStore_.waveState();
    wave.rawCapture = capture;
    wave.view.sampleFrequencyHz = capture.sampleFrequencyHz;
    wave.view.sampleFrequencyInput = formatFrequencyInput(capture.sampleFrequencyHz);
    wave.view.sampleFrequencyError.clear();
}


void Application::cleanupOfflineReplayResources()
{
    if (dockStore_.waveState().buffer.importedLabelsReadOnly()) freezeImportedChannelLabels();
    importedRebuildGeneration_.reset();
    rawCaptureReplay_.preparation = {};
    rawCaptureReplay_.profileFence = {};
    auto worker = std::move(offlineScriptWorker_);
    auto protocolDir = std::move(rawCaptureReplay_.temporaryProtocolDir);
    auto storageDir = std::move(rawCaptureReplay_.temporaryStorageDir);
    rawCaptureReplay_.temporaryProtocolDir.clear();
    rawCaptureReplay_.temporaryStorageDir.clear();
    if (!worker && protocolDir.empty() && storageDir.empty()) return;
    if (worker) worker->requestStop();
    // UI 仅请求停止；后台 join 完成后才按原生命周期清理专属目录，不接触新代次输出。
    retiringOfflineWorkers_.push_back(std::async(std::launch::async,
        [worker = std::move(worker), protocolDir = std::move(protocolDir),
         storageDir = std::move(storageDir)]() mutable {
            if (worker) { worker->stop(); worker.reset(); }
            std::string errors;
            for (const auto& path : {protocolDir, storageDir}) {
                if (path.empty()) continue;
                std::error_code ec;
                std::filesystem::remove_all(path, ec);
                if (ec) errors += "清理离线临时目录失败: " + ec.message() + "；";
            }
            return errors;
        }));
}


void Application::prepareRawCaptureImportReplay(const plot::RawCaptureFileData& capture)
{
    // 离线结果是可替换快照；每次 seek/模式切换都整体重建，不能叠加旧 generation 的日志和事件。
    dockStore_.clearScriptRows();
    // 核心流程：协议准备由 worker 异步完成；这里只在准备完成后清空离线展示状态。
    if (offlineScriptWorker_) static_cast<void>(offlineScriptWorker_->drainOutputs());
    const auto discarded = clearPendingRealtimeBacklog();
    logRealtimeBacklogDiscard(discarded);
    if (rawCaptureReplay_.replaceWaveLabels) {
        importedActualLabels_.clear();
        importedDefaultLabels_.clear();
        rawCaptureReplay_.replaceWaveLabels = false;
    }
    dockStore_.waveState().buffer.setDataSource(plot::WaveDataSource::Imported);
    importedRebuildGeneration_ = rawCaptureReplay_.generation;
    // 重新导入/协议变更使历史切换任务/旧确认失效。
    invalidateHistorySwitchWork("重新导入");
    resetWaveHistoryForTrigger(WaveResetViewportTrigger::RawImport);
    dockStore_.waveState().defaultChannelSpecs.clear();
    dockStore_.clearReceiveRows();
    dockStore_.clearTransferFrameRows();
    transferProfiles_.reset();
    pendingTransferFrameRows_.clear();
    transferFrameParser_ = offlineScriptWorker_ ? makeTransferFrameParserState(offlineScriptWorker_->snapshot())
                                                : std::nullopt;
    replayReceiveHistory_ = true;
    auto& wave = dockStore_.waveState();
    wave.rawCapture = capture;
    if (protocolSwitchReplay_) importedWaveIncomplete_ = true;
    wave.view.sampleFrequencyHz = capture.sampleFrequencyHz;
    wave.view.sampleFrequencyInput = formatFrequencyInput(capture.sampleFrequencyHz);
    wave.view.sampleFrequencyError.clear();
    wave.buffer.setHistoryTrimSuspended(true);
    rawCaptureReplayChunkBytes_ = resolveRawCaptureReplayChunkBytes(
        offlineScriptWorker_ ? offlineScriptWorker_->snapshot().streamBuffer : std::nullopt);
    rawCaptureReplayPendingBytes_ = 0U;
    rawCaptureReplayBatchBytes_.clear();
    rawCaptureReplayBatchContext_ = {};
    loggingFacade_.info("raw_capture",
                        "raw import prepared kind=raw_import endpoint=" + capture.protocolDir +
                            " bytes=" + std::to_string(capture.payload.size()) +
                            " events=" + std::to_string(capture.events.size()));
}


transport::ConnectionContext Application::makeRawCaptureReplayContext(const plot::RawCaptureFileData& capture) const
{
    transport::ConnectionContext replayContext;
    replayContext.endpoint = "psraw-import";
    replayContext.connectionId = 0;
    replayContext.timestampMs = capture.capturedAtMs == 0 ? nowMs() : capture.capturedAtMs;
    replayContext.readyForIo = false;
    return replayContext;
}


bool Application::replayRawCaptureEvents(const plot::RawCaptureFileData& capture, std::string& error)
{
    loggingFacade_.info("raw_capture",
                        "raw replay events kind=raw_replay endpoint=" + capture.protocolDir +
                            " events=" + std::to_string(capture.events.size()));
    auto replayContext = makeRawCaptureReplayContext(capture);
    suppressRawCaptureProfileEvents_ = true;
    suppressRawCapturePlotSetupEvents_ = true;

    for (const auto& recordedEvent : capture.events) {
        replayContext.timestampMs =
            recordedEvent.timestampMs == 0 ? replayContext.timestampMs : recordedEvent.timestampMs;
        if (recordedEvent.type == plot::RawCaptureEventType::RxBytes) {
            enqueueRawCaptureBytes(replayContext, recordedEvent.bytes);
            continue;
        }
        // 核心流程：配置事件必须在前一批 RX 字节完全处理后再生效，保持导入事件顺序。
        flushRawCaptureReplayBatch();
        if (!replayRawCaptureEvent(recordedEvent, replayContext, error)) {
            cancelRawCaptureImportReplay();
            return false;
        }
    }

    flushRawCaptureReplayBatch();
    flushScriptOutputsUnbounded();
    suppressRawCaptureProfileEvents_ = false;
    suppressRawCapturePlotSetupEvents_ = false;
    return true;
}


bool Application::pumpRawCaptureReplay(std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        return false;
    }
    if (rawCaptureReplay_.phase == OfflineReplayPhase::Preparing) {
        if (offlineScriptWorker_) static_cast<void>(offlineScriptWorker_->drainOutputs());
        if (!rawCaptureReplay_.preparation.valid() ||
            rawCaptureReplay_.preparation.wait_for(std::chrono::seconds(0)) != std::future_status::ready ||
            (offlineScriptWorker_ && !offlineScriptWorker_->inputIdle())) {
            return true;
        }
        const auto result = rawCaptureReplay_.preparation.get();
        rawCaptureReplay_.preparation = {};
        if (!result.ok) {
            rawCaptureReplay_.phase = OfflineReplayPhase::Failed;
            rawCaptureReplay_.error = result.lastError;
            rawCaptureReplay_.playing = false;
            error = "离线协议准备失败: " + result.lastError;
            return false;
        }
        rawCaptureReplay_.hasStreamSchema = result.snapshot.streamBuffer.has_value() && !result.snapshot.streamFrames.empty();
        prepareRawCaptureImportReplay(rawCaptureReplay_.capture);
        rawCaptureReplay_.context = makeRawCaptureReplayContext(rawCaptureReplay_.capture);
        rawCaptureReplay_.phase = rawCaptureReplay_.targetEventIndex > 0 || rawCaptureReplay_.playing
                                      ? OfflineReplayPhase::Replaying
                                      : OfflineReplayPhase::Paused;
        rawCaptureReplay_.playing = rawCaptureReplay_.targetEventIndex > 0 || rawCaptureReplay_.playing;
        // 准备完成后必须从下一帧才推进前缀，避免调用方观察到一半完成的 seek 状态。
        rawCaptureReplay_.lastPumpMs = nowMs();
        return true;
    }
    if (rawCaptureReplay_.phase == OfflineReplayPhase::Draining) {
        return drainRawCaptureReplay(error);
    }
    if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw) {
        static_cast<void>(flushOfflineScriptOutputs());
        if (offlineScriptWorker_ && offlineScriptWorker_->idle()) {
            rawCaptureReplay_.completedLuaInputs = rawCaptureReplay_.eventIndex;
        }
    }
    if (!rawCaptureReplay_.playing || rawCaptureReplay_.phase == OfflineReplayPhase::Failed ||
        rawCaptureReplay_.phase == OfflineReplayPhase::Cancelled) {
        return false;
    }

    const auto now = nowMs();
    if (rawCaptureReplay_.lastPumpMs == 0) {
        rawCaptureReplay_.lastPumpMs = now;
    }
    rawCaptureReplay_.accumulatedMs +=
        static_cast<double>(now - rawCaptureReplay_.lastPumpMs) * rawCaptureReplay_.speed;
    rawCaptureReplay_.lastPumpMs = now;
    rawCaptureReplay_.phase = OfflineReplayPhase::Replaying;

    const auto started = std::chrono::steady_clock::now();
    std::size_t events = 0;
    std::size_t bytes = 0;
    bool changed = false;
    const auto eventCount = rawCaptureReplay_.capture.events.size();
    while (rawCaptureReplay_.eventIndex < eventCount && events < kRawCaptureReplayEventsPerPump &&
           bytes < kRawCaptureReplayBytesPerPump &&
           std::chrono::steady_clock::now() - started < kRawCaptureReplayPumpBudget) {
        if (rawCaptureReplay_.profileFence.valid()) {
            if (rawCaptureReplay_.profileFence.wait_for(std::chrono::seconds(0)) != std::future_status::ready) break;
            const auto result = rawCaptureReplay_.profileFence.get();
            rawCaptureReplay_.profileFence = {};
            if (!result.first) {
                error = result.second;
                rawCaptureReplay_.phase = OfflineReplayPhase::Failed;
                rawCaptureReplay_.error = error;
                rawCaptureReplay_.playing = false;
                return false;
            }
        }
        if (rawCaptureReplay_.targetEventIndex > 0 &&
            rawCaptureReplay_.eventIndex >= rawCaptureReplay_.targetEventIndex) {
            // 已提交到 seek 的半开上界后只排空，不能因 worker 尚忙继续投递未来事件。
            if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw &&
                rawCaptureReplay_.completedLuaInputs < rawCaptureReplay_.targetEventIndex) break;
            rawCaptureReplay_.playing = rawCaptureReplay_.resumeAfterSeek;
            rawCaptureReplay_.phase = OfflineReplayPhase::Draining;
            rawCaptureReplay_.pacing = rawCaptureReplay_.pacingAfterSeek;
            rawCaptureReplay_.targetEventIndex = 0;
            rawCaptureReplay_.resumeAfterSeek = false;
            break;
        }
        const auto& current = rawCaptureReplay_.capture.events[rawCaptureReplay_.eventIndex];
        const auto streamBuffer = offlineScriptWorker_ ? offlineScriptWorker_->snapshot().streamBuffer : std::nullopt;
        const bool slicedLegacy = current.type == plot::RawCaptureEventType::RxBytes && current.sequence == 0 &&
            streamBuffer && current.bytes.size() > streamBuffer->capacity;
        if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw && slicedLegacy &&
            !offlineScriptWorker_->inputIdle()) break;
        double waitMs = 0.0;
        if (rawCaptureReplay_.pacing == OfflineReplayPacing::OriginalTimeline &&
            !rawCaptureReplay_.legacyEventStarted) {
            const auto previousTimestamp = rawCaptureReplay_.eventIndex == 0
                                               ? rawCaptureReplay_.capture.capturedAtMs
                                               : rawCaptureReplay_.capture.events[rawCaptureReplay_.eventIndex - 1].timestampMs;
            if (current.timestampMs > previousTimestamp) waitMs = static_cast<double>(current.timestampMs - previousTimestamp);
            if (waitMs > rawCaptureReplay_.accumulatedMs) break;
            rawCaptureReplay_.accumulatedMs -= waitMs;
        }
        if (current.bytes.size() > kRawCaptureReplayBytesPerPump && events > 0) break;
        if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw) {
            if (!offlineScriptWorker_) break;
            const bool controlEvent = current.type != plot::RawCaptureEventType::RxBytes;
            if ((controlEvent && (!offlineScriptWorker_->idle() || !pendingScriptPlotAppends_.empty())) ||
                (!controlEvent && offlineScriptWorker_->pendingRxBytes() >= kRawCaptureReplayBytesPerPump)) {
                break;
            }
        }
        const auto submittedIndex = rawCaptureReplay_.eventIndex;
        if (!replayRawCaptureEventAt(submittedIndex, error)) {
            rawCaptureReplay_.playing = false;
            rawCaptureReplay_.phase = OfflineReplayPhase::Failed;
            rawCaptureReplay_.error = error;
            return false;
        }
        ++events;
        bytes += current.bytes.size();
        changed = true;
        // 同一个 legacy 事件每轮至多投递一个有界切片，不忙等 worker 或绕过 pump 预算。
        if (rawCaptureReplay_.eventIndex == submittedIndex) break;
        if (rawCaptureReplay_.targetEventIndex > 0 &&
            rawCaptureReplay_.eventIndex >= rawCaptureReplay_.targetEventIndex) {
            // 已提交到半开前缀边界；等待该输入的 callback/output 完整 drain 后再宣布 seek 完成。
            break;
        }
    }

    if (rawCaptureReplay_.eventIndex >= eventCount) {
        rawCaptureReplay_.playing = false;
        rawCaptureReplay_.phase = OfflineReplayPhase::Draining;
        const auto drained = drainRawCaptureReplay(error);
        // 历史重建与回放推进共用主循环，来源代次在 pumpHistoryRebuild 内校验。
        // 历史重建只由 pumpOnce 推进一次，避免一轮重复消耗字节预算。
        return drained || changed;
    }
    // pumpOnce 统一推进历史任务。
    return changed;
}


bool Application::drainRawCaptureReplay(std::string& error)
{
    if (rawCaptureReplay_.profileFence.valid()) {
        if (rawCaptureReplay_.profileFence.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return true;
        const auto result = rawCaptureReplay_.profileFence.get();
        rawCaptureReplay_.profileFence = {};
        if (!result.first) {
            error = result.second;
            rawCaptureReplay_.phase = OfflineReplayPhase::Failed;
            rawCaptureReplay_.error = error;
            return false;
        }
    }
    flushOfflineScriptOutputs();
    flushPendingTransferFrameRows(transferFrameRowsPerPump());
    const auto snapshot = offlineScriptWorker_ ? offlineScriptWorker_->snapshot() : scripting::ScriptRuntimeSnapshot{};
    if (protocolSwitchReplay_ && !snapshot.lastError.empty()) {
        error = snapshot.lastError;
        rawCaptureReplay_.error = error;
        rawCaptureReplay_.phase = OfflineReplayPhase::Failed;
        rawCaptureReplay_.playing = false;
        return false;
    }
    if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw &&
        (!offlineScriptWorker_ || !offlineScriptWorker_->idle() || snapshot.pendingWorkerRxBytes > 0U ||
         snapshot.inputQueueSize > 0U || snapshot.outputQueueSize > 0U || snapshot.pendingPlotAppends > 0U ||
         !pendingScriptPlotAppends_.empty())) {
        return true;
    }
    // idle 之后再 drain 一次，覆盖 callback 结束与输出发布之间的边界。
    if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw && flushOfflineScriptOutputs()) return true;
    if (!pendingTransferFrameRows_.empty()) return true;
    const bool resumeAfterDrain = rawCaptureReplay_.playing;
    finishRawCaptureImportReplay();
    rawCaptureReplay_.playing = rawCaptureReplay_.eventIndex >= rawCaptureReplay_.capture.events.size()
                                    ? false
                                    : (rawCaptureReplay_.targetEventIndex == 0 ? resumeAfterDrain : rawCaptureReplay_.playing);
    rawCaptureReplay_.phase = rawCaptureReplay_.eventIndex >= rawCaptureReplay_.capture.events.size()
                                  ? OfflineReplayPhase::Completed
                                  : (rawCaptureReplay_.playing ? OfflineReplayPhase::Replaying : OfflineReplayPhase::Paused);
    rawCaptureReplay_.lastPumpMs = 0;
    rawCaptureReplay_.accumulatedMs = 0.0;
    return true;
}


bool Application::replayRawCaptureEventAt(const std::size_t eventIndex, std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        error = "尚未载入原始回放时间轴";
        return false;
    }
    if (eventIndex >= rawCaptureReplay_.capture.events.size()) {
        return true;
    }

    auto& event = rawCaptureReplay_.capture.events[eventIndex];
    const auto buffer = offlineScriptWorker_ ? offlineScriptWorker_->snapshot().streamBuffer : std::nullopt;
    if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw &&
        event.type == plot::RawCaptureEventType::RxBytes && event.sequence == 0 &&
        buffer && event.bytes.size() > buffer->capacity) {
        // legacy 无 callback 边界；保持单事件位置，只在前一切片及输出已消费后推进。
        flushOfflineScriptOutputs();
        if (!offlineScriptWorker_->inputIdle()) return true;
        auto& offset = rawCaptureReplay_.legacyByteOffset;
        if (!rawCaptureReplay_.legacyEventStarted) {
            rawCaptureReplay_.legacyEventStarted = true;
            event.displayPosition = ++transferPosition_;
            rawCaptureReplay_.context.timestampMs = event.timestampMs == 0
                ? rawCaptureReplay_.context.timestampMs : event.timestampMs;
            appendTransferRow({.timestampMs = rawCaptureReplay_.context.timestampMs, .direction = "RX",
                .endpoint = event.endpoint.empty() ? rawCaptureReplay_.context.endpoint : event.endpoint,
                .bytes = event.bytes, .displayPosition = event.displayPosition});
        }
        if (offset < event.bytes.size()) {
            const auto end = (std::min)(event.bytes.size(), offset + resolveRawCaptureReplayChunkBytes(buffer));
            const transport::TransportBytesEvent slice{rawCaptureReplay_.context,
                std::vector<std::uint8_t>(event.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                         event.bytes.begin() + static_cast<std::ptrdiff_t>(end))};
            offset += offlineScriptWorker_->tryPostTransportBytes(slice, slice.bytes.size(), false);
            return true;
        }
        offset = 0;
        rawCaptureReplay_.legacyEventStarted = false;
        rawCaptureReplay_.eventIndex = eventIndex + 1;
        return true;
    }
    event.displayPosition = ++transferPosition_;
    rawCaptureReplay_.context.timestampMs =
        event.timestampMs == 0 ? rawCaptureReplay_.context.timestampMs : event.timestampMs;
    suppressRawCaptureProfileEvents_ = true;
    suppressRawCapturePlotSetupEvents_ = true;
    if (!replayRawCaptureEvent(event, rawCaptureReplay_.context, error)) {
        suppressRawCaptureProfileEvents_ = false;
        suppressRawCapturePlotSetupEvents_ = false;
        return false;
    }
    suppressRawCaptureProfileEvents_ = false;
    suppressRawCapturePlotSetupEvents_ = false;
    rawCaptureReplay_.eventIndex = eventIndex + 1;
    return true;
}


bool Application::replayRawCaptureEvent(const plot::RawCaptureEvent& event,
                                        transport::ConnectionContext& replayContext,
                                        std::string& error)
{
    if (event.type == plot::RawCaptureEventType::TxBytes) {
        appendTransferRow({.timestampMs = event.timestampMs, .direction = "TX",
            .endpoint = event.endpoint.empty() ? replayContext.endpoint : event.endpoint,
            .bytes = event.bytes, .message = event.writeStatus,
            .messageIsWriteStatus = event.type == plot::RawCaptureEventType::TxBytes,
            .displayPosition = event.displayPosition});
        return true;
    }
    if (event.type == plot::RawCaptureEventType::ProfileSet ||
        event.type == plot::RawCaptureEventType::ProfileClear) {
        const scripting::StreamRuntimeProfileEvent profile{
            .cleared = event.type == plot::RawCaptureEventType::ProfileClear,
            .frameName = event.profile.frameName,
            .length = event.type == plot::RawCaptureEventType::ProfileClear ? 0U : event.profile.length,
            .channelMap = event.type == plot::RawCaptureEventType::ProfileClear
                              ? std::vector<std::size_t>{}
                              : event.profile.channelMap,
        };
        std::string frameError;
        if (protocolSwitchReplay_ && !transferFrameParser_) {
            error = "新协议不兼容原始 profile 事件：未声明 stream schema";
            return false;
        }
        if (!applyTransferFrameRuntimeProfileEvent(profile, frameError)) {
            error = frameError;
            return false;
        }
        if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw) {
            if (!offlineScriptWorker_) {
                error = "离线脚本 worker 不可用";
                return false;
            }
            rawCaptureReplay_.profileFence = offlineScriptWorker_->applyStreamRuntimeProfileEventAsync(profile).share();
        }
        return true;
    }
    if (event.type == plot::RawCaptureEventType::PlotSetup) {
        auto& wave = dockStore_.waveState();
        // 核心流程：raw 回放可能先由 Lua on_open/on_bytes 产生同一 setup；完全一致时跳过，避免 reset_history
        // 二次清空样本。
        const auto setup = offlineWaveRebuilding() ? normalizeImportedSetup(event.plotSetup) : event.plotSetup;
        if (!sameAppliedPlotSetup(wave, setup)) {
            applyPlotSetup(setup, offlineWaveRebuilding());
        }
        return true;
    }
    if (!event.bytes.empty()) {
        appendTransferRow(dock::ReceiveRow{
            .timestampMs = replayContext.timestampMs,
            .direction = "RX",
            .endpoint = event.endpoint.empty() ? replayContext.endpoint : event.endpoint,
            .bytes = event.bytes,
            .message = {},
            .displayPosition = event.displayPosition,
        });
        if (rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw) {
            if (!offlineScriptWorker_) {
                error = "离线脚本 worker 不可用";
                return false;
            }
            // legacy 大事件由时间轴的持久 offset 推进；现代事件保持一个 command/callback。
            offlineScriptWorker_->postTransportBytes(transport::TransportBytesEvent{replayContext, event.bytes}, false);
        }
    }
    return true;
}


bool Application::applyRawCaptureRuntimeProfileEvent(const plot::RawCaptureEvent& event,
                                                     bool cleared,
                                                     std::string& error)
{
    std::string profileError;
    if (!scriptWorker_.applyStreamRuntimeProfileEvent(
            scripting::StreamRuntimeProfileEvent{
                .cleared = cleared,
                .frameName = event.profile.frameName,
                .length = cleared ? 0 : event.profile.length,
                .channelMap = cleared ? std::vector<std::size_t>{} : event.profile.channelMap,
            },
            profileError)) {
        error = cleared ? "导入 profile_clear 失败: " + profileError : "导入 profile_set 失败: " + profileError;
        return false;
    }

    scriptWorker_.waitIdle();
    flushScriptOutputs();
    return true;
}


void Application::replayRawCaptureBytes(const transport::ConnectionContext& replayContext,
                                        const std::vector<std::uint8_t>& bytes)
{
    enqueueRawCaptureBytes(replayContext, bytes);
    flushRawCaptureReplayBatch();
}


void Application::enqueueRawCaptureBytes(const transport::ConnectionContext& replayContext,
                                         const std::vector<std::uint8_t>& bytes)
{
    if (bytes.empty()) {
        return;
    }

    loggingFacade_.trace("raw_capture",
                         "raw replay bytes" + transportContextFields("raw_replay", replayContext) +
                             " bytes=" + std::to_string(bytes.size()) +
                             payloadPreviewSuffix(loggingFacade_.currentConfig(), bytes));
    dockStore_.appendReceiveRow(dock::ReceiveRow{
        .timestampMs = replayContext.timestampMs,
        .direction = "RX",
        .endpoint = replayContext.endpoint,
        .bytes = bytes,
        .message = {},
    });
    const auto chunkBytes = (std::max)(std::size_t{1U}, rawCaptureReplayChunkBytes_);
    std::size_t cursor = 0;
    while (cursor < bytes.size()) {
        const auto chunkSize = (std::min)(chunkBytes, bytes.size() - cursor);
        std::vector<std::uint8_t> chunk(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(cursor + chunkSize));
        appendTransferFrameRows(dock::ReceiveRow{
            .timestampMs = replayContext.timestampMs,
            .direction = "RX",
            .endpoint = replayContext.endpoint,
            .bytes = chunk,
            .message = {},
        });
        cursor += chunkSize;
    }

    rawCaptureReplayBatchContext_ = replayContext;
    cursor = 0;
    if (!rawCaptureReplayBatchBytes_.empty()) {
        const auto required = chunkBytes - rawCaptureReplayBatchBytes_.size();
        const auto copied = (std::min)(required, bytes.size());
        rawCaptureReplayBatchBytes_.insert(
            rawCaptureReplayBatchBytes_.end(), bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(copied));
        cursor = copied;
        if (rawCaptureReplayBatchBytes_.size() == chunkBytes) {
            scriptWorker_.postTransportBytes(
                transport::TransportBytesEvent{rawCaptureReplayBatchContext_, std::move(rawCaptureReplayBatchBytes_)},
                false);
            rawCaptureReplayBatchBytes_.clear();
            rawCaptureReplayPendingBytes_ += chunkBytes;
        }
    }
    while (bytes.size() - cursor >= chunkBytes) {
        std::vector<std::uint8_t> chunk(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                                        bytes.begin() + static_cast<std::ptrdiff_t>(cursor + chunkBytes));
        scriptWorker_.postTransportBytes(
            transport::TransportBytesEvent{rawCaptureReplayBatchContext_, std::move(chunk)}, false);
        cursor += chunkBytes;
        rawCaptureReplayPendingBytes_ += chunkBytes;
        if (rawCaptureReplayPendingBytes_ >= kRawCaptureReplaySyncBatchBytes) {
            flushRawCaptureReplayBatch();
        }
    }
    if (cursor < bytes.size()) {
        rawCaptureReplayBatchBytes_.insert(
            rawCaptureReplayBatchBytes_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end());
    }
    if (rawCaptureReplayPendingBytes_ + rawCaptureReplayBatchBytes_.size() >= kRawCaptureReplaySyncBatchBytes) {
        flushRawCaptureReplayBatch();
    }
}


void Application::flushRawCaptureReplayBatch()
{
    if (!rawCaptureReplayBatchBytes_.empty()) {
        rawCaptureReplayPendingBytes_ += rawCaptureReplayBatchBytes_.size();
        scriptWorker_.postTransportBytes(
            transport::TransportBytesEvent{rawCaptureReplayBatchContext_, std::move(rawCaptureReplayBatchBytes_)},
            false);
        rawCaptureReplayBatchBytes_.clear();
    }
    if (rawCaptureReplayPendingBytes_ == 0U) {
        return;
    }
    rawCaptureReplayPendingBytes_ = 0U;
    if (runtimeConfig_.scripting.workerEnabled) {
        // 兼容旧同步导入 API；正式离线时间轴使用独立 worker 的分帧状态机。
        if (offlineScriptWorker_) offlineScriptWorker_->waitIdle();
        else scriptWorker_.waitIdle();
    }
    if (offlineScriptWorker_) flushOfflineScriptOutputsUnbounded();
    else flushScriptOutputsUnbounded();
    flushPendingTransferFrameRows(std::numeric_limits<std::size_t>::max());
}


void Application::finishRawCaptureImportReplay()
{
    auto& wave = dockStore_.waveState();
    if (offlineScriptWorker_) flushOfflineScriptOutputsUnbounded();
    else flushScriptOutputsUnbounded();
    flushPendingTransferFrameRows(std::numeric_limits<std::size_t>::max());
    const auto importedSnapshot =
        wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), false);
    std::size_t importedHistoryLimit = 0;
    for (const auto& channel : importedSnapshot.channels) {
        importedHistoryLimit = (std::max)(importedHistoryLimit, channel.totalSamples);
    }
    freezeImportedChannelLabels();
    if (rawCaptureReplay_.eventIndex >= rawCaptureReplay_.capture.events.size())
        importedRebuildGeneration_.reset();
    wave.buffer.setHistoryTrimSuspended(false);
    wave.buffer.preserveHistoryLimitAtLeast(importedHistoryLimit);
    syncDockState();
    wave.statusMessage = protocolSwitchReplay_ ? "协议切换重算完成，波形及协议帧已更新" : "原始波形已导入";
    if (protocolSwitchReplay_) {
        importedWaveIncomplete_ = rawCaptureReplay_.capture.incomplete;
        protocolSwitchReplay_ = false;
    }
    loggingFacade_.info("raw_capture",
                        "raw import finished kind=raw_import endpoint=" + dockStore_.luaState().protocolDir);
}


void Application::cancelRawCaptureImportReplay()
{
    ++rawCaptureReplay_.generation;
    rawCaptureReplay_.profileFence = {};
    suppressRawCaptureProfileEvents_ = false;
    suppressRawCapturePlotSetupEvents_ = false;
    rawCaptureReplayPendingBytes_ = 0U;
    rawCaptureReplayBatchBytes_.clear();
    pendingScriptPlotAppends_.clear();
    pendingTransferFrameRows_.clear();
    dockStore_.waveState().buffer.setHistoryTrimSuspended(false);
}


bool Application::importWaveRawCapture(const plot::RawCaptureFileData& capture, std::string& error)
{
    if (capture.waveform) {
        if (!importWaveCsvData(*capture.waveform, error)) return false;
        return (capture.events.empty() && capture.payload.empty()) || importRawRecords(capture, error);
    }
    if (!loadRawCaptureReplayTimeline(capture,
                                      OfflineReplayProcessing::CurrentProtocol,
                                      OfflineReplayPacing::FastBatchParse,
                                      error)) {
        return false;
    }
    rawCaptureReplay_.playing = true;
    return true;
}


bool Application::loadRawCaptureReplayTimeline(const plot::RawCaptureFileData& capture, std::string& error)
{
    return loadRawCaptureReplayTimeline(
        capture, OfflineReplayProcessing::BrowseRaw, OfflineReplayPacing::OriginalTimeline, error);
}


bool Application::loadRawCaptureReplayTimeline(const plot::RawCaptureFileData& capture,
                                               const OfflineReplayProcessing processing,
                                               const OfflineReplayPacing pacing,
                                               std::string& error,
                                               const session::SessionPackageData* package)
{
    if (!validateRawCaptureImport(capture, error)) return false;
    if (capture.events.empty() && capture.payload.empty()) {
        error = "原始波形没有可回放事件";
        return false;
    }

    // capture 可能就是现有 timeline 的成员：必须先复制，再清理/重置旧状态。
    auto replayCapture = capture;
    const bool replaceWaveLabels = !capture.waveform.has_value();
    const auto nextGeneration = rawCaptureReplay_.generation + 1U;
    cleanupOfflineReplayResources();
    cancelRawCaptureImportReplay();
    protocolSwitchReplay_ = false;
    importedRawAssociated_ = true;
    if (replayCapture.events.empty() && !replayCapture.payload.empty()) {
        for (std::size_t offset = 0; offset < replayCapture.payload.size(); offset += kRawCaptureReplayLargeChunkBytes) {
            const auto end = (std::min)(replayCapture.payload.size(), offset + kRawCaptureReplayLargeChunkBytes);
            replayCapture.events.push_back(plot::RawCaptureEvent{
                .type = plot::RawCaptureEventType::RxBytes,
                .timestampMs = replayCapture.capturedAtMs,
                .bytes = std::vector<std::uint8_t>(replayCapture.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                                                   replayCapture.payload.begin() + static_cast<std::ptrdiff_t>(end)),
                .profile = {},
                .plotSetup = {},
                .endpoint = replayCapture.source,
                .sequence = 0U,
            });
        }
    }
    rawCaptureReplay_ = RawCaptureReplayState{};
    rawCaptureReplay_.generation = nextGeneration;
    rawCaptureReplay_.loaded = true;
    rawCaptureReplay_.capture = std::move(replayCapture);
    rawCaptureReplay_.replaceWaveLabels = replaceWaveLabels;
    rawCaptureReplay_.context = makeRawCaptureReplayContext(rawCaptureReplay_.capture);
    rawCaptureReplay_.speed = 1.0;
    rawCaptureReplay_.pacing = pacing;
    rawCaptureReplay_.phase = OfflineReplayPhase::Paused;
    if (!prepareOfflineReplayProtocol(rawCaptureReplay_.capture, processing, package, error)) {
        rawCaptureReplay_.phase = OfflineReplayPhase::Failed;
        rawCaptureReplay_.error = error;
        return false;
    }
    if (processing == OfflineReplayProcessing::BrowseRaw) {
        prepareRawCaptureBrowse(rawCaptureReplay_.capture);
        rawCaptureReplay_.phase = OfflineReplayPhase::Paused;
    }
    dockStore_.waveState().statusMessage = "原始回放时间轴已载入，默认不执行 Lua";
    return true;
}


void Application::unloadRawCaptureReplayTimeline()
{
    if (!rawCaptureReplay_.loaded) return;
    const auto nextGeneration = rawCaptureReplay_.generation + 1U;
    cancelRawCaptureImportReplay();
    cleanupOfflineReplayResources();
    rawCaptureReplay_ = RawCaptureReplayState{};
    rawCaptureReplay_.generation = nextGeneration;
    rawCaptureReplay_.phase = OfflineReplayPhase::Cancelled;
    dockStore_.waveState().statusMessage = "原始回放时间轴已卸载";
}


bool Application::playRawCaptureReplay(std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        error = "尚未载入原始回放时间轴";
        return false;
    }
    if (rawCaptureReplay_.phase == OfflineReplayPhase::Completed) {
        return seekRawCaptureReplay(0, error) && playRawCaptureReplay(error);
    }
    rawCaptureReplay_.playing = true;
    if (rawCaptureReplay_.phase != OfflineReplayPhase::Preparing) rawCaptureReplay_.phase = OfflineReplayPhase::Replaying;
    rawCaptureReplay_.lastPumpMs = nowMs();
    return true;
}


void Application::pauseRawCaptureReplay()
{
    rawCaptureReplay_.playing = false;
    if (rawCaptureReplay_.loaded && rawCaptureReplay_.phase != OfflineReplayPhase::Completed &&
        rawCaptureReplay_.phase != OfflineReplayPhase::Failed) {
        rawCaptureReplay_.phase = OfflineReplayPhase::Paused;
    }
}


bool Application::stepRawCaptureReplay(std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        error = "尚未载入原始回放时间轴";
        return false;
    }
    if (rawCaptureReplay_.phase == OfflineReplayPhase::Preparing) {
        error = "离线协议仍在准备中";
        return false;
    }
    rawCaptureReplay_.playing = false;
    const auto ok = replayRawCaptureEventAt(rawCaptureReplay_.eventIndex, error);
    if (ok) {
        rawCaptureReplay_.phase = rawCaptureReplay_.eventIndex >= rawCaptureReplay_.capture.events.size()
                                      ? OfflineReplayPhase::Draining
                                      : OfflineReplayPhase::Paused;
        flushPendingTransferFrameRows(transferFrameRowsPerPump());
    }
    return ok;
}


bool Application::seekRawCaptureReplay(const std::size_t eventIndex, std::string& error)
{
    if (!rawCaptureReplay_.loaded) {
        error = "尚未载入原始回放时间轴";
        return false;
    }

    auto next = rawCaptureReplay_;
    next.eventIndex = 0;
    next.legacyByteOffset = 0;
    next.legacyEventStarted = false;
    next.targetEventIndex = (std::min)(eventIndex, next.capture.events.size());
    next.resumeAfterSeek = rawCaptureReplay_.playing;
    next.pacingAfterSeek = rawCaptureReplay_.pacing;
    next.pacing = OfflineReplayPacing::FastBatchParse;
    next.playing = next.targetEventIndex > 0;
    next.phase = OfflineReplayPhase::Paused;
    next.lastPumpMs = nowMs();
    next.accumulatedMs = 0.0;
    next.generation = rawCaptureReplay_.generation + 1U;
    next.preparation = {};
    next.profileFence = {};
    // seek 使历史切换任务/旧确认失效，不能把过期结果提交到新来源；同时清过期逐帧结果并回原始视图。
    invalidateHistorySwitchWork("seek");
    dockStore_.clearTransferFrameRows();
    pendingTransferFrameRows_.clear();
    dockStore_.receiveState().displayMode = dock::TransferLogDisplayMode::RawChunks;
    if (next.processing == OfflineReplayProcessing::PackageProtocol) {
        next.protocolDirectory.clear();
        next.temporaryProtocolDir.clear();
    }
    cleanupOfflineReplayResources();
    rawCaptureReplay_ = std::move(next);

    if (rawCaptureReplay_.processing == OfflineReplayProcessing::BrowseRaw) {
        prepareRawCaptureBrowse(rawCaptureReplay_.capture);
        rawCaptureReplay_.phase = rawCaptureReplay_.playing ? OfflineReplayPhase::Replaying : OfflineReplayPhase::Paused;
        return true;
    }
    if (!prepareOfflineReplayProtocol(rawCaptureReplay_.capture, rawCaptureReplay_.processing, nullptr, error)) {
        rawCaptureReplay_.phase = OfflineReplayPhase::Failed;
        rawCaptureReplay_.error = error;
        return false;
    }
    dockStore_.waveState().statusMessage = "正在准备干净离线状态并分批定位...";
    return true;
}


void Application::setRawCaptureReplaySpeed(const double speed)
{
    rawCaptureReplay_.speed = (std::max)(0.1, (std::min)(speed, 16.0));
}


Application::RawCaptureReplayStatus Application::rawCaptureReplayStatus() const
{
    const auto eventCount = rawCaptureReplay_.capture.events.size();
    const double progress = eventCount == 0U
                                ? 0.0
                                : (std::min)(1.0, static_cast<double>(rawCaptureReplay_.eventIndex) /
                                                      static_cast<double>(eventCount));
    const auto snapshot = offlineScriptWorker_ ? offlineScriptWorker_->snapshot() : scripting::ScriptRuntimeSnapshot{};
    return RawCaptureReplayStatus{
        .loaded = rawCaptureReplay_.loaded,
        .playing = rawCaptureReplay_.playing,
        .eventIndex = rawCaptureReplay_.eventIndex,
        .eventCount = eventCount,
        .progress = progress,
        .speed = rawCaptureReplay_.speed,
        .phase = rawCaptureReplay_.phase,
        .processing = rawCaptureReplay_.processing,
        .pacing = rawCaptureReplay_.pacing,
        .protocolSource = rawCaptureReplay_.protocolSource,
        .inputBacklog = eventCount > rawCaptureReplay_.eventIndex ? eventCount - rawCaptureReplay_.eventIndex : 0U,
        .workerBacklog = snapshot.inputQueueSize + snapshot.pendingWorkerRxBytes,
        .outputBacklog = snapshot.outputQueueSize + pendingScriptPlotAppends_.size() + pendingTransferFrameRows_.size(),
        .hasStreamSchema = rawCaptureReplay_.processing == OfflineReplayProcessing::BrowseRaw
                               ? transferFrameParser_.has_value()
                               : rawCaptureReplay_.hasStreamSchema,
        .truncated = rawCaptureReplay_.capture.truncated,
        .error = rawCaptureReplay_.error,
    };
}


bool Application::applyOfflineScriptOutputBatch(scripting::ScriptRuntimeOutputBatch batch)
{
    // 离线输出只允许影响解析可见结果；物理发送、请求、弹窗、文件弹窗和运行状态更新一律丢弃。
    batch.txRequests.clear();
    batch.requestGuardResets.clear();
    batch.requestDoneResults.clear();
    batch.dialogRequests.clear();
    batch.fileDialogRequests.clear();
    batch.statusUpdates.clear();
    batch.oscilloscopeRunningUpdates.clear();
    bool changed = false;
    changed = applyScriptTransportStats(batch) || changed;
    changed = applyScriptRuntimeProfileEvents(batch) || changed;
    changed = applyScriptUiAndLogOutputs(batch) || changed;
    if (offlineWaveRebuilding()) changed = applyScriptPlotOutputs(batch, true) || changed;
    return changed;
}


bool Application::flushOfflineScriptOutputs()
{
    if (!offlineScriptWorker_) return false;
    bool changed = false;
    const auto flushBudgetMs = outputFlushBudgetMs();
    const auto flushStartedAt = nowUs();
    if (offlineWaveRebuilding()) changed = applyScriptPlotOutputs(scripting::ScriptRuntimeOutputBatch{}, true);
    auto batch = offlineScriptWorker_->drainOneOutput();
    while (batch.has_value()) {
        changed = applyOfflineScriptOutputBatch(std::move(*batch)) || changed;
        if (flushBudgetMs > 0.0 && (nowUs() - flushStartedAt) >= static_cast<std::uint64_t>(flushBudgetMs * 1000.0)) break;
        batch = offlineScriptWorker_->drainOneOutput();
    }
    return changed;
}


bool Application::flushOfflineScriptOutputsUnbounded()
{
    if (!offlineScriptWorker_) return false;
    bool changed = offlineWaveRebuilding() && applyScriptPlotOutputs(scripting::ScriptRuntimeOutputBatch{}, true);
    auto batch = offlineScriptWorker_->drainOneOutput();
    while (batch.has_value()) {
        changed = applyOfflineScriptOutputBatch(std::move(*batch)) || changed;
        batch = offlineScriptWorker_->drainOneOutput();
    }
    return changed;
}

} // namespace protoscope::app
