#include "protoscope/app/application.hpp"
#include "protoscope/config/settings.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace protoscope::app {
namespace {
bool resourceSettingsEqual(const config::AppConfig& a, const config::AppConfig& b)
{
#define SAME(path) if (a.path != b.path) return false
    SAME(receive.transportReadBufferBytes);
    SAME(scripting.workerRxQueueLimitBytes); SAME(scripting.workerMemoryBudgetBytes);
    SAME(scripting.workerOutputQueueLimit); SAME(scripting.workerBatchBytes);
#define FLAG(name) SAME(performance.explicitOverrides.name)
    FLAG(receiveTransportReadBufferBytes); FLAG(workerRxQueueLimitBytes); FLAG(workerMemoryBudgetBytes);
    FLAG(workerOutputQueueLimit); FLAG(workerBatchBytes);
    SAME(gui.realtimeBacklog.rawFirstBacklogWarnBytes); FLAG(realtimeBacklogRawFirstBacklogWarnBytes);
    SAME(gui.realtimeBacklog.rxChunkBytesPerPump); FLAG(realtimeBacklogRxChunkBytesPerPump);
    SAME(gui.realtimeBacklog.transferFrameRowsPerPump); FLAG(realtimeBacklogTransferFrameRowsPerPump);
    SAME(gui.realtimeBacklog.plotAppendsPerPump); FLAG(realtimeBacklogPlotAppendsPerPump);
    SAME(scripting.workerOutputFlushBudgetMs); FLAG(workerOutputFlushBudgetMs);
#undef FLAG
#undef SAME
    return true;
}
}

SettingsApplyResult Application::applySettingsChanges(const config::AppConfig& baseline,
    const config::AppConfig& draft, SettingsApplyOptions options)
{
    SettingsApplyResult result;
    if (pendingSettingsConfig_ || dataTransferStatus().active) {
        result.error = "设置应用尚未完成或文件导入导出正在运行"; return result;
    }
    const auto latest = captureConfig();
    const bool confirmationValid = options.confirmationId != 0 && options.confirmationId == settingsConfirmationId_ &&
        settingsConfirmationRuntime_ && settingsConfirmationDraft_;
    auto draftCheck = settingsConfirmationDraft_ ? config::SettingsDraft(*settingsConfirmationDraft_) : config::SettingsDraft(draft);
    draftCheck.value = draft;
    bool confirmedImpactUnchanged = false;
    if (confirmationValid) {
        const auto impact = config::mergeSettingsChanges(*settingsConfirmationRuntime_, *settingsConfirmationDraft_, latest);
        const auto& confirmed = *settingsConfirmationRuntime_;
        const bool resourceChange = !resourceSettingsEqual(confirmed, *settingsConfirmationDraft_) ||
            confirmed.performance.scale != settingsConfirmationDraft_->performance.scale ||
            confirmed.performance.adaptive.enabled != settingsConfirmationDraft_->performance.adaptive.enabled;
        confirmedImpactUnchanged = impact.error.empty() && impact.conflicts.empty() &&
            (!resourceChange || resourceSettingsEqual(confirmed, latest));
    }
    const bool accepted = confirmationValid && confirmedImpactUnchanged && !draftCheck.hasChanges() &&
        resourceSettingsEqual(*settingsConfirmationDraft_, draft);
    const auto requireConfirmation = [&] {
        settingsConfirmationRuntime_ = latest; settingsConfirmationDraft_ = draft;
        result.confirmationId = ++settingsConfirmationId_;
        result.status = SettingsApplyStatus::ConfirmationRequired;
    };
    auto merged = config::mergeSettingsChanges(baseline, draft, latest, options.overwriteConflicts && accepted);
    result.error = merged.error; result.conflicts = merged.conflicts;
    if (!result.error.empty()) return result;
    if (!result.conflicts.empty()) { requireConfirmation(); return result; }
    const auto& value = merged.config;
    if (value.gui.interactionFeedback.statusDurationMs != latest.gui.interactionFeedback.statusDurationMs &&
        value.gui.interactionFeedback.statusDurationMs > (std::numeric_limits<std::uint64_t>::max)() - nowMs()) {
        result.error = "状态提示持续时间溢出"; return result;
    }
    const auto& oldHistory = latest.gui.logHistory;
    const auto& history = value.gui.logHistory;
    const bool trim = history.transferRawLimit < oldHistory.transferRawLimit ||
        history.transferFrameLimit < oldHistory.transferFrameLimit || history.hostLimit < oldHistory.hostLimit ||
        history.scriptLimit < oldHistory.scriptLimit || history.requestTraceLimit < oldHistory.requestTraceLimit ||
        value.gui.sendHistoryLimit < latest.gui.sendHistoryLimit;
    if (trim && !(options.confirmHistoryTrim && accepted)) {
        requireConfirmation();
        result.error = "历史容量缩减需要确认"; return result;
    }
    if (value.gui.theme != latest.gui.theme) {
        if (!options.applyTheme) { result.error = "主题修改需要宿主 ThemeManager 提交回调"; return result; }
        try {
            if (options.prepareTheme && !options.prepareTheme(value.gui.theme, result.error)) {
                if (result.error.empty()) result.error = "主题资源准备失败";
                return result;
            }
        } catch (const std::exception& ex) { result.error = ex.what(); return result; }
        catch (...) { result.error = "主题资源准备抛出未知异常"; return result; }
    }
    const bool loggingChanged = value.logging.level != latest.logging.level ||
        value.logging.filePath != latest.logging.filePath || value.logging.maxFiles != latest.logging.maxFiles ||
        value.logging.maxFileSizeBytes != latest.logging.maxFileSizeBytes ||
        value.logging.payloadPreview.enabled != latest.logging.payloadPreview.enabled ||
        value.logging.payloadPreview.maxBytes != latest.logging.payloadPreview.maxBytes;
    if (loggingChanged && !loggingFacade_.prepareSettingsConfig(value.logging, result.error)) return result;
    result.restartRequired = value.gui.window.width != latest.gui.window.width ||
        value.gui.window.height != latest.gui.window.height || value.gui.window.maximized != latest.gui.window.maximized ||
        value.gui.rendererBackend != latest.gui.rendererBackend ||
        value.gui.font.chineseGlyphRange != latest.gui.font.chineseGlyphRange;
    result.nextConnectionRequired = value.receive.transportReadBufferBytes != latest.receive.transportReadBufferBytes;
    const bool workerChanged = value.scripting.workerRxQueueLimitBytes != latest.scripting.workerRxQueueLimitBytes ||
        value.scripting.workerMemoryBudgetBytes != latest.scripting.workerMemoryBudgetBytes ||
        value.scripting.workerOutputQueueLimit != latest.scripting.workerOutputQueueLimit ||
        value.scripting.workerBatchBytes != latest.scripting.workerBatchBytes;
    if (workerChanged) {
        pendingSettingsConfig_ = value;
        pendingSettingsBaseline_ = latest;
        pendingSettingsOptions_ = std::move(options);
        try {
            settingsBudgetFence_ = scriptWorker_.applyBudgetSettingsAsync(value.scripting.workerRxQueueLimitBytes,
                value.scripting.workerMemoryBudgetBytes, value.scripting.workerOutputQueueLimit, value.scripting.workerBatchBytes);
        } catch (const std::exception& ex) {
            pendingSettingsConfig_.reset(); pendingSettingsBaseline_.reset(); pendingSettingsOptions_ = {};
            loggingFacade_.discardSettingsConfig(); result.error = ex.what();
            settingsApplyResult_ = result; return result;
        } catch (...) {
            pendingSettingsConfig_.reset(); pendingSettingsBaseline_.reset(); pendingSettingsOptions_ = {};
            loggingFacade_.discardSettingsConfig(); result.error = "预算入队抛出未知异常";
            settingsApplyResult_ = result; return result;
        }
        result.status = SettingsApplyStatus::Pending;
        settingsApplyResult_ = result;
        return result;
    }
    try {
        if (value.gui.theme != latest.gui.theme && !options.applyTheme(value.gui.theme, result.error)) {
            loggingFacade_.discardSettingsConfig();
            if (result.error.empty()) result.error = "主题提交失败";
            settingsApplyResult_ = result; return result;
        }
    } catch (const std::exception& ex) {
        loggingFacade_.discardSettingsConfig(); result.error = ex.what(); settingsApplyResult_ = result; return result;
    } catch (...) {
        loggingFacade_.discardSettingsConfig(); result.error = "主题提交抛出未知异常"; settingsApplyResult_ = result; return result;
    }
    if (merged.changedFields.empty() && settingsPartialSaveBlocked_) {
        loggingFacade_.discardSettingsConfig();
        result.error = "部分运行态尚未确认；请恢复worker预算或明确接受当前实际运行态";
        result.partiallyApplied = true;
        settingsApplyResult_ = result;
        return result;
    }
    if (!merged.changedFields.empty()) commitSettingsChanges(value);
    settingsConfirmationRuntime_.reset(); settingsConfirmationDraft_.reset();
    result.status = SettingsApplyStatus::Applied;
    settingsApplyResult_ = result;
    return result;
}

SettingsApplyResult Application::pollSettingsApply()
{
    if (!pendingSettingsConfig_) return settingsApplyResult_;
    bool workerApplied = false;
    const auto retainWorkerBudgets = [&] {
        // 部分成功必须成为可保存的真实运行态：单项标记显式，旧倍率不再重算这些数字。
        const auto& value = *pendingSettingsConfig_;
        // 恢复信息属于未完成事务，不随窗口刷新/关闭或后续失败重试丢失。
        if (!settingsWorkerOriginalFlags_) settingsWorkerOriginalFlags_ = pendingSettingsBaseline_->performance.explicitOverrides;
        runtimeConfig_.scripting.workerRxQueueLimitBytes = value.scripting.workerRxQueueLimitBytes;
        runtimeConfig_.scripting.workerMemoryBudgetBytes = value.scripting.workerMemoryBudgetBytes;
        runtimeConfig_.scripting.workerOutputQueueLimit = value.scripting.workerOutputQueueLimit;
        runtimeConfig_.scripting.workerBatchBytes = value.scripting.workerBatchBytes;
        auto& flags = runtimeConfig_.performance.explicitOverrides;
        flags.workerRxQueueLimitBytes = true;
        flags.workerMemoryBudgetBytes = true;
        flags.workerOutputQueueLimit = true;
        flags.workerBatchBytes = true;
        settingsApplyResult_.partiallyApplied = true;
        settingsPartialSaveBlocked_ = true;
        dockStore_.markDirty("worker 资源预算已完成，其它设置未全部应用");
    };
    try {
        if (!settingsBudgetFence_.valid()) throw std::runtime_error("worker 预算完成句柄失效");
        if (settingsBudgetFence_.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
            return settingsApplyResult_;
        if (!settingsBudgetFence_.get()) throw std::runtime_error("worker 拒绝预算设置或已停止");
        workerApplied = true;
        // 先合并旧静态运行快照，之后再记真实完成值，避免自己引入预算冲突。
        const auto current = captureConfig();
        auto merged = config::mergeSettingsChanges(*pendingSettingsBaseline_, *pendingSettingsConfig_, current);
        if (!resourceSettingsEqual(*pendingSettingsBaseline_, current))
            merged.conflicts.emplace_back("performance.resources");
        retainWorkerBudgets();
        if (dataTransferStatus().active) throw std::runtime_error("完成期间文件导入导出启动；仅 worker 预算已应用");
        if (!merged.error.empty()) throw std::runtime_error(merged.error);
        if (!merged.conflicts.empty()) {
            settingsApplyResult_.status = SettingsApplyStatus::ConfirmationRequired;
            settingsApplyResult_.conflicts = std::move(merged.conflicts);
            settingsApplyResult_.error = "等待 worker 期间发生冲突；资源预算已应用，请刷新或确认剩余变更";
            settingsConfirmationRuntime_ = captureConfig();
            settingsConfirmationDraft_ = *pendingSettingsConfig_;
            settingsApplyResult_.confirmationId = ++settingsConfirmationId_;
        } else {
            const auto& value = merged.config;
            const bool logChanged = value.logging.level != current.logging.level ||
                value.logging.filePath != current.logging.filePath || value.logging.maxFiles != current.logging.maxFiles ||
                value.logging.maxFileSizeBytes != current.logging.maxFileSizeBytes ||
                value.logging.payloadPreview.enabled != current.logging.payloadPreview.enabled ||
                value.logging.payloadPreview.maxBytes != current.logging.payloadPreview.maxBytes;
            loggingFacade_.discardSettingsConfig();
            if (logChanged && !loggingFacade_.prepareSettingsConfig(value.logging, settingsApplyResult_.error))
                throw std::runtime_error(settingsApplyResult_.error);
            if (value.gui.theme != current.gui.theme && (!pendingSettingsOptions_.applyTheme ||
                !pendingSettingsOptions_.applyTheme(value.gui.theme, settingsApplyResult_.error)))
                throw std::runtime_error(settingsApplyResult_.error.empty() ? "主题提交失败" : settingsApplyResult_.error);
            commitSettingsChanges(value);
            settingsApplyResult_.status = SettingsApplyStatus::Applied;
            settingsApplyResult_.partiallyApplied = false;
            settingsApplyResult_.error.clear();
        }
    } catch (const std::exception& ex) {
        if (workerApplied) retainWorkerBudgets();
        settingsApplyResult_.status = SettingsApplyStatus::Failed;
        settingsApplyResult_.error = ex.what();
    } catch (...) {
        if (workerApplied) retainWorkerBudgets();
        settingsApplyResult_.status = SettingsApplyStatus::Failed;
        settingsApplyResult_.error = "设置完成抛出未知异常";
    }
    loggingFacade_.discardSettingsConfig();
    pendingSettingsConfig_.reset();
    pendingSettingsBaseline_.reset();
    pendingSettingsOptions_ = {};
    settingsBudgetFence_ = {};
    if (settingsApplyResult_.status != SettingsApplyStatus::ConfirmationRequired) {
        settingsConfirmationRuntime_.reset(); settingsConfirmationDraft_.reset();
    }
    return settingsApplyResult_;
}
void Application::commitSettingsChanges(const config::AppConfig& value)
{
    const auto old = captureConfig();
    runtimeConfig_ = value;
    auto& state = dockStore_.configState();
    state.autoSaveEnabled = value.app.autoSave.enabled;
    state.autoSaveIntervalMs = value.app.autoSave.intervalMs;
    state.configHotReloadEnabled = value.app.configHotReload.enabled;
    state.fpsLimit = value.app.fpsLimit;
    auto& wave = dockStore_.waveState();
#define COPY(name) wave.view.name = value.gui.wave.name
    COPY(controlMode); COPY(displayFormula); COPY(gridDivisionReadoutMode);
    COPY(channelScaleWheelEnabled); COPY(channelScaleWheelAcceleration);
    COPY(channelCardWidthMode); COPY(channelCardFixedWidth); COPY(channelCardAdaptiveRatio);
    COPY(channelDoubleClickAction); COPY(xAxisDoubleClickAction); COPY(yAxisDoubleClickAction);
    COPY(yAxisDoubleClickAdjustOffset); COPY(hiddenChannelPolicy); COPY(cursorExtremeSnapPolicy);
    COPY(mouseYOffsetDragMode); COPY(interactionAnimationEnabled); COPY(zoomSelectionAutoExit);
    COPY(maxRenderPointsPerChannel); COPY(maxRenderVertices); COPY(overviewMaxSamples);
#undef COPY
    wave.view.effectiveInteractionAnimationEnabled = value.gui.interactionFeedback.enabled && value.gui.wave.interactionAnimationEnabled;
    if (old.gui.wave.displayFormula != value.gui.wave.displayFormula) {
        auto view = wave.buffer.viewConfig(); view.displayFormula = value.gui.wave.displayFormula;
        wave.buffer.setViewConfig(view);
    }
    wave.legendOverlay.openMode = value.gui.wave.legendOverlayOpenMode;
    wave.legendOverlay.doubleClickAutoCollapse = value.gui.wave.legendOverlayDoubleClickAutoCollapse;
    if (old.gui.wave.legendOverlayOpenMode != value.gui.wave.legendOverlayOpenMode &&
        value.gui.wave.legendOverlayOpenMode != plot::WaveLegendOverlayOpenMode::DoubleClick)
        wave.legendOverlay.expanded = false;
    if (old.gui.logHistory.transferRawLimit != value.gui.logHistory.transferRawLimit ||
        old.gui.logHistory.transferFrameLimit != value.gui.logHistory.transferFrameLimit ||
        old.gui.logHistory.hostLimit != value.gui.logHistory.hostLimit ||
        old.gui.logHistory.scriptLimit != value.gui.logHistory.scriptLimit ||
        old.gui.logHistory.requestTraceLimit != value.gui.logHistory.requestTraceLimit)
        applyHistoryLimits(value.gui.logHistory);
    if (old.gui.sendHistoryLimit != value.gui.sendHistoryLimit)
        dock::trimSendHistory(dockStore_.sendState(), (std::min)(value.gui.sendHistoryLimit, std::size_t{200}));
    if (old.performance.adaptive.enabled != value.performance.adaptive.enabled ||
        old.performance.adaptive.maxMultiplier != value.performance.adaptive.maxMultiplier)
        adaptivePerformance_.configure(value.performance.adaptive);
    applyAdaptiveWaveBudget(); syncAdaptivePerformanceStatus();
    loggingFacade_.commitSettingsConfig();
    settingsPartialSaveBlocked_ = false;
    settingsWorkerOriginalFlags_.reset();
    dockStore_.markDirty("应用设置");
}

bool Application::preparePartialWorkerRecovery(config::SettingsDraft& draft) const
{
    if (!settingsPartialSaveBlocked_ || pendingSettingsConfig_ || !settingsWorkerOriginalFlags_) return false;
    const auto& original = *settingsWorkerOriginalFlags_;
    const auto& actual = runtimeConfig_.performance.explicitOverrides;
#define FLAG(name) draft.baseline.performance.explicitOverrides.name = actual.name; \
    draft.value.performance.explicitOverrides.name = original.name
    FLAG(workerRxQueueLimitBytes);
    FLAG(workerMemoryBudgetBytes);
    FLAG(workerOutputQueueLimit);
    FLAG(workerBatchBytes);
#undef FLAG
    return true;
}

bool Application::acceptPartialSettings(std::string& error)
{
    error.clear();
    if (pendingSettingsConfig_ || dataTransferStatus().active || !settingsPartialSaveBlocked_) {
        error = "没有可接受的部分运行态，或设置/文件传输活动中";
        return false;
    }
    // 明确接受真实已生效的预算与显式标记，不假装恢复其余草稿，不自动保存。
    settingsPartialSaveBlocked_ = false;
    settingsWorkerOriginalFlags_.reset();
    settingsApplyResult_ = {};
    settingsConfirmationRuntime_.reset(); settingsConfirmationDraft_.reset();
    dockStore_.markDirty("已明确接受当前部分运行态，尚未保存");
    return true;
}

bool Application::saveSettings(config::SettingsPersistence& persistence, std::string& error)
{
    if (settingsSaveBlocked() || dataTransferStatus().active) {
        error = "设置应用尚未完成或文件导入导出正在运行"; return false;
    }
    if (!persistence.save(captureConfig(), error)) return false;
    dockStore_.clearDirty("设置已保存");
    return true;
}

AdaptivePerformanceBudget Application::effectiveSettingsPerformance() const
{
    if (adaptivePerformance_.enabled()) return adaptivePerformance_.budget();
    const auto value = captureConfig();
    // 展示实际零值回退，不能把保存的静态零值误报为运行预算。
    const config::AppConfig defaults;
    return {(std::max)(std::uint32_t{1}, value.app.fpsLimit),
        value.gui.wave.maxRenderPointsPerChannel == 0 ? defaults.gui.wave.maxRenderPointsPerChannel : value.gui.wave.maxRenderPointsPerChannel,
        value.gui.wave.maxRenderVertices == 0 ? defaults.gui.wave.maxRenderVertices : value.gui.wave.maxRenderVertices,
        value.gui.wave.overviewMaxSamples, rxBytesPerPump(), transferFrameRowsPerPump(), plotAppendsPerPump(), outputFlushBudgetMs()};
}

} // namespace protoscope::app
