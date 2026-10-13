#include "protoscope/config/settings.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace protoscope::config {
namespace {
// 固定四页的直接字段访问，不创建反射描述或动态表单。
template<class F> void generalFields(AppConfig& out, const AppConfig& source, F&& field)
{
#define FIELD(path) field(out.path, source.path, #path)
    FIELD(gui.theme); FIELD(gui.showAppHeader);
    FIELD(gui.interactionFeedback.enabled); FIELD(gui.interactionFeedback.statusDurationMs);
    FIELD(app.autoSave.enabled); FIELD(app.autoSave.intervalMs); FIELD(app.configHotReload.enabled);
    FIELD(gui.window.width); FIELD(gui.window.height); FIELD(gui.window.maximized);
    FIELD(gui.rendererBackend); FIELD(gui.font.chineseGlyphRange);
#undef FIELD
}
template<class F> void waveFields(AppConfig& out, const AppConfig& source, F&& field)
{
#define FIELD(name) field(out.gui.wave.name, source.gui.wave.name, "gui.wave." #name)
    FIELD(controlMode); FIELD(displayFormula); FIELD(gridDivisionReadoutMode);
    FIELD(channelScaleWheelEnabled); FIELD(channelScaleWheelAcceleration);
    FIELD(channelCardWidthMode); FIELD(channelCardFixedWidth); FIELD(channelCardAdaptiveRatio);
    FIELD(channelDoubleClickAction); FIELD(xAxisDoubleClickAction); FIELD(yAxisDoubleClickAction);
    FIELD(yAxisDoubleClickAdjustOffset); FIELD(hiddenChannelPolicy); FIELD(cursorExtremeSnapPolicy);
    FIELD(mouseYOffsetDragMode); FIELD(legendOverlayOpenMode); FIELD(legendOverlayDoubleClickAutoCollapse);
    FIELD(interactionAnimationEnabled); FIELD(zoomSelectionAutoExit); FIELD(fullscreenMode);
#undef FIELD
}
template<class F> void loggingFields(AppConfig& out, const AppConfig& source, F&& field)
{
#define FIELD(path) field(out.path, source.path, #path)
    FIELD(logging.level); FIELD(logging.filePath); FIELD(logging.payloadPreview.enabled);
    FIELD(logging.payloadPreview.maxBytes); FIELD(logging.maxFileSizeBytes); FIELD(logging.maxFiles);
    FIELD(gui.logHistory.transferRawLimit); FIELD(gui.logHistory.transferFrameLimit);
    FIELD(gui.logHistory.hostLimit); FIELD(gui.logHistory.scriptLimit); FIELD(gui.logHistory.requestTraceLimit);
    FIELD(gui.sendHistoryLimit);
    FIELD(gui.historyOnParsedSwitch); FIELD(gui.historyScope);
#undef FIELD
}
template<class F> void performanceFields(AppConfig& out, const AppConfig& source, F&& field)
{
#define FIELD(path) field(out.path, source.path, #path)
    FIELD(performance.adaptive.enabled); FIELD(performance.adaptive.maxMultiplier);
    FIELD(performance.scale); FIELD(app.fpsLimit);
    FIELD(gui.wave.maxRenderPointsPerChannel); FIELD(gui.wave.maxRenderVertices); FIELD(gui.wave.overviewMaxSamples);
    FIELD(gui.realtimeBacklog.rxChunkBytesPerPump); FIELD(gui.realtimeBacklog.transferFrameRowsPerPump);
    FIELD(gui.realtimeBacklog.plotAppendsPerPump); FIELD(scripting.workerOutputFlushBudgetMs);
    FIELD(performance.explicitOverrides.realtimeBacklogRxChunkBytesPerPump);
    FIELD(performance.explicitOverrides.realtimeBacklogTransferFrameRowsPerPump);
    FIELD(performance.explicitOverrides.realtimeBacklogPlotAppendsPerPump);
    FIELD(performance.explicitOverrides.workerOutputFlushBudgetMs);
    // 部分应用会把实际 worker 预算标为显式；恢复倍率缺省必须能够解除这些标记。
    FIELD(performance.explicitOverrides.workerRxQueueLimitBytes);
    FIELD(performance.explicitOverrides.workerMemoryBudgetBytes);
    FIELD(performance.explicitOverrides.workerOutputQueueLimit);
    FIELD(performance.explicitOverrides.workerBatchBytes);
#undef FIELD
}
template<class F> void settingsFields(AppConfig& out, const AppConfig& source, F&& field)
{
    generalFields(out, source, field); waveFields(out, source, field);
    loggingFields(out, source, field); performanceFields(out, source, field);
}
bool validateChanged(const AppConfig& value, const std::vector<std::string>& changed, std::string& error)
{
    const auto has = [&](const char* name) { return std::find(changed.begin(), changed.end(), name) != changed.end(); };
    const auto check = [&](const char* name, bool valid) {
        if (has(name) && !valid && error.empty()) error = std::string(name) + ": 非法数值、枚举或路径";
    };
    check("gui.window.width", value.gui.window.width > 0);
    check("gui.window.height", value.gui.window.height > 0);
    check("gui.theme", !value.gui.theme.empty() && value.gui.theme.find('\0') == std::string::npos);
    check("logging.filePath", value.logging.filePath.find('\0') == std::string::npos);
    check("logging.maxFiles", value.logging.maxFiles <= 200000);
    check("performance.adaptive.maxMultiplier", std::isfinite(value.performance.adaptive.maxMultiplier) &&
          value.performance.adaptive.maxMultiplier >= .25 && value.performance.adaptive.maxMultiplier <= 4);
    check("performance.scale", std::isfinite(value.performance.scale) && value.performance.scale > 0);
    check("gui.wave.channelCardFixedWidth", std::isfinite(value.gui.wave.channelCardFixedWidth) &&
          value.gui.wave.channelCardFixedWidth > 0 && value.gui.wave.channelCardFixedWidth <= (std::numeric_limits<float>::max)());
    check("gui.wave.channelCardAdaptiveRatio", std::isfinite(value.gui.wave.channelCardAdaptiveRatio) &&
          value.gui.wave.channelCardAdaptiveRatio > 0 &&
          value.gui.wave.channelCardAdaptiveRatio <= (std::numeric_limits<double>::max)() / (std::numeric_limits<float>::max)());
    check("scripting.workerOutputFlushBudgetMs", std::isfinite(value.scripting.workerOutputFlushBudgetMs) &&
          value.scripting.workerOutputFlushBudgetMs >= 0 &&
          static_cast<long double>(value.scripting.workerOutputFlushBudgetMs) * 1000 <
              static_cast<long double>((std::numeric_limits<std::uint64_t>::max)()));
    const auto enumCheck = [&]<class E>(const char* name, E current, E last) {
        check(name, static_cast<int>(current) >= 0 && static_cast<int>(current) <= static_cast<int>(last));
    };
    enumCheck("logging.level", value.logging.level, LogLevel::Error);
    enumCheck("gui.historyOnParsedSwitch", value.gui.historyOnParsedSwitch,
              TransferLogHistoryOnParsedSwitch::SkipHistory);
    enumCheck("gui.historyScope", value.gui.historyScope, TransferLogHistoryScope::RetainedCapture);
    enumCheck("gui.rendererBackend", value.gui.rendererBackend, GuiRendererBackend::D3D11Warp);
#if !defined(_WIN32)
    check("gui.rendererBackend", value.gui.rendererBackend == GuiRendererBackend::OpenGL);
#endif
    enumCheck("gui.font.chineseGlyphRange", value.gui.font.chineseGlyphRange, GuiFontChineseGlyphRange::Full);
#define ENUM(name, last) enumCheck("gui.wave." #name, value.gui.wave.name, last)
    ENUM(controlMode, plot::WaveControlMode::LegacyGlobal);
    ENUM(displayFormula, plot::WaveDisplayFormula::ScaleThenOffset);
    ENUM(gridDivisionReadoutMode, plot::WaveGridDivisionReadoutMode::RawValue);
    ENUM(channelScaleWheelAcceleration, plot::WaveChannelScaleWheelAcceleration::Log);
    ENUM(channelCardWidthMode, plot::WaveChannelCardWidthMode::Adaptive);
    ENUM(channelDoubleClickAction, plot::WaveChannelDoubleClickAction::ResetOffset);
    ENUM(xAxisDoubleClickAction, plot::WaveXAxisDoubleClickAction::FitVisibleWindow);
    ENUM(yAxisDoubleClickAction, plot::WaveYAxisDoubleClickAction::FitActiveChannel);
    ENUM(hiddenChannelPolicy, plot::WaveHiddenChannelPolicy::ExcludeFromDerivedViews);
    ENUM(cursorExtremeSnapPolicy, plot::WaveCursorExtremeSnapPolicy::ViewportZone);
    ENUM(mouseYOffsetDragMode, plot::WaveMouseYOffsetDragMode::Disabled);
    ENUM(legendOverlayOpenMode, plot::WaveLegendOverlayOpenMode::Disabled);
    ENUM(fullscreenMode, GuiWaveFullscreenMode::Overlay);
#undef ENUM
    return error.empty();
}
}

bool recomputePerformanceDefaults(AppConfig& value, std::string& error, bool clampOverflow)
{
    error.clear();
    if (!std::isfinite(value.performance.scale) || value.performance.scale <= 0) {
        error = "performance.scale: 必须为有限正数"; return false;
    }
    auto result = value;
    const AppConfig defaults;
    const double scale = value.performance.adaptive.enabled ? 1.0 : value.performance.scale;
    const auto integer = [&](std::size_t& target, std::size_t base, bool explicitValue) {
        if (explicitValue) return;
        const auto scaled = std::round(static_cast<long double>(base) * scale);
        // 用精确的 2^digits 排他上界，避免 double 精度把 SIZE_MAX 舍入为 2^64 后发生未定义转换。
        const auto exclusiveUpper = std::ldexp(1.0L, std::numeric_limits<std::size_t>::digits);
        if (!std::isfinite(scaled) || scaled >= exclusiveUpper) {
            if (clampOverflow) target = (std::numeric_limits<std::size_t>::max)();
            else error = "performance.scale: 吞吐或资源预算溢出";
            return;
        }
        target = base == 0 ? 0 : (std::max)(std::size_t{1}, static_cast<std::size_t>(scaled));
    };
    const auto& flags = value.performance.explicitOverrides;
#define BUDGET(path, flag) integer(result.path, defaults.path, flags.flag)
    BUDGET(receive.transportReadBufferBytes, receiveTransportReadBufferBytes);
    BUDGET(scripting.workerRxQueueLimitBytes, workerRxQueueLimitBytes);
    BUDGET(scripting.workerMemoryBudgetBytes, workerMemoryBudgetBytes);
    BUDGET(scripting.workerOutputQueueLimit, workerOutputQueueLimit);
    BUDGET(scripting.workerBatchBytes, workerBatchBytes);
    BUDGET(gui.realtimeBacklog.rxChunkBytesPerPump, realtimeBacklogRxChunkBytesPerPump);
    BUDGET(gui.realtimeBacklog.transferFrameRowsPerPump, realtimeBacklogTransferFrameRowsPerPump);
    BUDGET(gui.realtimeBacklog.plotAppendsPerPump, realtimeBacklogPlotAppendsPerPump);
    BUDGET(gui.realtimeBacklog.rawFirstBacklogWarnBytes, realtimeBacklogRawFirstBacklogWarnBytes);
#undef BUDGET
    if (!flags.workerOutputFlushBudgetMs) result.scripting.workerOutputFlushBudgetMs = defaults.scripting.workerOutputFlushBudgetMs * scale;
    const auto flushUpper = std::ldexp(1.0L, std::numeric_limits<std::uint64_t>::digits);
    if (!std::isfinite(result.scripting.workerOutputFlushBudgetMs) || result.scripting.workerOutputFlushBudgetMs < 0 ||
        static_cast<long double>(result.scripting.workerOutputFlushBudgetMs) * 1000 >= flushUpper) {
        if (clampOverflow) {
            auto& flush = result.scripting.workerOutputFlushBudgetMs;
            if (std::isnan(flush)) flush = defaults.scripting.workerOutputFlushBudgetMs;
            else if (flush < 0) flush = 0;
            else flush = std::nextafter(static_cast<double>(flushUpper / 1000), 0.0);
        } else error = "输出 flush 时间预算溢出";
    }
    if (!error.empty()) return false;
    value = std::move(result);
    return true;
}

SettingsMergeResult mergeSettingsChanges(const AppConfig& baseline, const AppConfig& draft,
                                        const AppConfig& latest, bool overwriteConflicts)
{
    SettingsMergeResult result;
    result.config = latest;
    // 同位置固定白名单访问，避免把整个旧草稿覆盖最新运行态。
    auto before = baseline;
    auto current = latest;
    std::vector<bool> changed;
    settingsFields(before, draft, [&](auto& old, const auto& next, const char*) { changed.push_back(!(old == next)); });
    std::size_t index = 0;
    settingsFields(current, baseline, [&](auto& live, const auto& old, const char* name) {
        if (changed[index++] && !(live == old)) result.conflicts.emplace_back(name);
    });
    index = 0;
    settingsFields(result.config, draft, [&](auto& live, const auto& next, const char* name) {
        if (changed[index++]) {
            if (live == next) std::erase(result.conflicts, std::string(name));
            if (!(live == next)) result.changedFields.emplace_back(name);
            live = next;
        }
    });
    // 值与显式性作为同一个语义字段合并；恢复缺省数字不被误标为手动覆盖。
    auto derived = draft;
    std::string derivedError;
    recomputePerformanceDefaults(derived, derivedError);
    const auto budget = [&](auto oldValue, auto nextValue, auto liveValue, auto derivedValue,
                            bool oldFlag, bool nextFlag, bool liveFlag, auto& target, bool& targetFlag,
                            const char* name, const char* flagName) {
        std::erase(result.conflicts, std::string(name));
        std::erase(result.conflicts, std::string(flagName));
        if (oldValue == nextValue && oldFlag == nextFlag) { target = liveValue; targetFlag = liveFlag; return; }
        if (oldFlag == nextFlag && !nextFlag && !(oldValue == nextValue) && !(nextValue == derivedValue)) nextFlag = true;
        if ((!(liveValue == oldValue) || liveFlag != oldFlag) &&
            (!(liveValue == nextValue) || liveFlag != nextFlag)) result.conflicts.emplace_back(name);
        target = nextValue; targetFlag = nextFlag;
    };
#define BUDGET(path, flag) budget(baseline.path, draft.path, latest.path, derived.path, baseline.performance.explicitOverrides.flag, draft.performance.explicitOverrides.flag, latest.performance.explicitOverrides.flag, result.config.path, result.config.performance.explicitOverrides.flag, #path, "performance.explicitOverrides." #flag)
    BUDGET(gui.realtimeBacklog.rxChunkBytesPerPump, realtimeBacklogRxChunkBytesPerPump);
    BUDGET(gui.realtimeBacklog.transferFrameRowsPerPump, realtimeBacklogTransferFrameRowsPerPump);
    BUDGET(gui.realtimeBacklog.plotAppendsPerPump, realtimeBacklogPlotAppendsPerPump);
    BUDGET(scripting.workerOutputFlushBudgetMs, workerOutputFlushBudgetMs);
#undef BUDGET
    if (!validateChanged(result.config, result.changedFields, result.error)) return result;
    const bool recompute = baseline.performance.scale != draft.performance.scale ||
        baseline.performance.adaptive.enabled != draft.performance.adaptive.enabled ||
        baseline.performance.explicitOverrides.realtimeBacklogRxChunkBytesPerPump != draft.performance.explicitOverrides.realtimeBacklogRxChunkBytesPerPump ||
        baseline.performance.explicitOverrides.realtimeBacklogTransferFrameRowsPerPump != draft.performance.explicitOverrides.realtimeBacklogTransferFrameRowsPerPump ||
        baseline.performance.explicitOverrides.realtimeBacklogPlotAppendsPerPump != draft.performance.explicitOverrides.realtimeBacklogPlotAppendsPerPump ||
        baseline.performance.explicitOverrides.workerOutputFlushBudgetMs != draft.performance.explicitOverrides.workerOutputFlushBudgetMs ||
        baseline.performance.explicitOverrides.workerRxQueueLimitBytes != draft.performance.explicitOverrides.workerRxQueueLimitBytes ||
        baseline.performance.explicitOverrides.workerMemoryBudgetBytes != draft.performance.explicitOverrides.workerMemoryBudgetBytes ||
        baseline.performance.explicitOverrides.workerOutputQueueLimit != draft.performance.explicitOverrides.workerOutputQueueLimit ||
        baseline.performance.explicitOverrides.workerBatchBytes != draft.performance.explicitOverrides.workerBatchBytes;
    if (recompute) {
        recomputePerformanceDefaults(result.config, result.error);
        // 倍率引起的隐含变更也不能静默覆盖其它入口已改的同项预算。
        const auto derivedConflict = [&](auto old, auto live, auto next, const char* name) {
            if (!(old == live) && !(next == live) &&
                std::find(result.conflicts.begin(), result.conflicts.end(), name) == result.conflicts.end())
                result.conflicts.emplace_back(name);
        };
#define DERIVED(path) derivedConflict(baseline.path, latest.path, result.config.path, #path)
        DERIVED(receive.transportReadBufferBytes); DERIVED(scripting.workerRxQueueLimitBytes);
        DERIVED(scripting.workerMemoryBudgetBytes); DERIVED(scripting.workerOutputQueueLimit);
        DERIVED(scripting.workerBatchBytes); DERIVED(gui.realtimeBacklog.rawFirstBacklogWarnBytes);
        DERIVED(gui.realtimeBacklog.rxChunkBytesPerPump); DERIVED(gui.realtimeBacklog.transferFrameRowsPerPump);
        DERIVED(gui.realtimeBacklog.plotAppendsPerPump); DERIVED(scripting.workerOutputFlushBudgetMs);
#undef DERIVED
    }
    result.changedFields.clear();
    auto latestCopy = latest;
    settingsFields(latestCopy, result.config, [&](auto& old, const auto& next, const char* name) {
        if (!(old == next)) result.changedFields.emplace_back(name);
    });
    if (overwriteConflicts) result.conflicts.clear();
    return result;
}

void SettingsDraft::restoreDefaults(SettingsPage page)
{
    const AppConfig defaults;
    const auto copy = [](auto& target, const auto& source, const char*) { target = source; };
    switch (page) {
        case SettingsPage::General: generalFields(value, defaults, copy); break;
        case SettingsPage::Wave: waveFields(value, defaults, copy); break;
        case SettingsPage::Logging: loggingFields(value, defaults, copy); break;
        case SettingsPage::Performance: {
            const auto flags = value.performance.explicitOverrides;
            performanceFields(value, defaults, copy);
            // 页面默认只恢复四个展示标记，隐藏资源覆盖必须由明确恢复操作解除。
            value.performance.explicitOverrides.workerRxQueueLimitBytes = flags.workerRxQueueLimitBytes;
            value.performance.explicitOverrides.workerMemoryBudgetBytes = flags.workerMemoryBudgetBytes;
            value.performance.explicitOverrides.workerOutputQueueLimit = flags.workerOutputQueueLimit;
            value.performance.explicitOverrides.workerBatchBytes = flags.workerBatchBytes;
            break;
        }
    }
}
void SettingsDraft::followUnedited(const AppConfig& current)
{
    auto old = baseline;
    std::vector<bool> edited;
    settingsFields(old, value, [&](auto& before, const auto& next, const char*) { edited.push_back(!(before == next)); });
    std::size_t index = 0;
    settingsFields(value, current, [&](auto& draft, const auto& live, const char*) { if (!edited[index++]) draft = live; });
    index = 0;
    settingsFields(baseline, current, [&](auto& before, const auto& live, const char*) { if (!edited[index++]) before = live; });
}

bool SettingsDraft::hasChanges() const
{
    auto copy = baseline;
    bool changed = false;
    settingsFields(copy, value, [&](auto& old, const auto& next, const char*) { changed = changed || !(old == next); });
    return changed;
}
SettingsPersistence::SettingsPersistence(const ConfigStore& store, std::filesystem::path path)
    : store_(store), path_(std::move(path)), saved_(store_.snapshot(path_)) {}
bool SettingsPersistence::save(const AppConfig& current, std::string& error)
{
    if (!store_.saveChecked(path_, current, saved_, error)) return false;
    saved_ = store_.snapshot(path_);
    if (!saved_.error.empty()) { error = "配置已写入，但读取保存后快照失败: " + saved_.error; return false; }
    return true;
}
bool SettingsPersistence::acceptExternalSnapshot(std::string& error)
{
    const auto current = store_.snapshot(path_);
    if (!current.error.empty()) { error = current.error; return false; }
    saved_ = current;
    return true;
}
bool SettingsPersistence::hasExternalChange() const { return store_.hasChanged(saved_); }

} // namespace protoscope::config
