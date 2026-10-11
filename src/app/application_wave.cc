#include "protoscope/app/application.hpp"
#include "protoscope/plot/wave_math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace protoscope::app {

namespace {

    bool sameColor(const std::optional<std::array<float, 4>>& left, const std::optional<std::array<float, 4>>& right)
    {
        if (left.has_value() != right.has_value()) {
            return false;
        }
        if (!left.has_value()) {
            return true;
        }
        for (std::size_t index = 0; index < left->size(); ++index) {
            if (std::abs((*left)[index] - (*right)[index]) > 1e-6F) {
                return false;
            }
        }
        return true;
    }

    bool sameLineWidth(const std::optional<float>& left, const std::optional<float>& right)
    {
        if (left.has_value() != right.has_value()) {
            return false;
        }
        if (!left.has_value()) {
            return true;
        }
        return std::abs(*left - *right) <= 1e-6F;
    }

    bool sameBitDisplayIdentity(const plot::BitDisplaySpec& left, const plot::BitDisplaySpec& right)
    {
        return left.enabled == right.enabled && left.firstBit == right.firstBit && left.bitCount == right.bitCount;
    }

    bool sameBitDisplaySpec(const plot::BitDisplaySpec& left, const plot::BitDisplaySpec& right)
    {
        return sameBitDisplayIdentity(left, right) && std::abs(left.yOffset - right.yOffset) <= 1e-12 &&
               left.hoverReadout == right.hoverReadout;
    }

    bool sameChannelSpecs(const std::vector<plot::ChannelSpec>& setupChannels, const plot::OscilloscopeBuffer& buffer)
    {
        if (setupChannels.size() != buffer.channelCount()) {
            return false;
        }
        for (std::size_t i = 0; i < setupChannels.size(); ++i) {
            const auto current = buffer.channelSpec(i);
            if (!current.has_value()) {
                return false;
            }
            const auto& setup = setupChannels[i];
            if (current->label != setup.label || current->unit != setup.unit ||
                !sameColor(current->color, setup.color) || !sameLineWidth(current->lineWidth, setup.lineWidth) ||
                !sameBitDisplayIdentity(current->bitDisplay, setup.bitDisplay)) {
                return false;
            }
        }
        return true;
    }

    bool sameChannelSpecs(const std::vector<plot::ChannelSpec>& setupChannels,
                          const std::vector<plot::ChannelSpec>& previousDefaults)
    {
        if (setupChannels.size() != previousDefaults.size()) {
            return false;
        }
        for (std::size_t i = 0; i < setupChannels.size(); ++i) {
            const auto& setup = setupChannels[i];
            const auto& previous = previousDefaults[i];
            if (previous.label != setup.label || previous.unit != setup.unit ||
                !sameColor(previous.color, setup.color) || !sameLineWidth(previous.lineWidth, setup.lineWidth) ||
                !sameBitDisplayIdentity(previous.bitDisplay, setup.bitDisplay)) {
                return false;
            }
        }
        return true;
    }

    bool sameFullChannelSpecs(const std::vector<plot::ChannelSpec>& setupChannels,
                              const plot::OscilloscopeBuffer& buffer)
    {
        if (!sameChannelSpecs(setupChannels, buffer)) {
            return false;
        }
        for (std::size_t index = 0; index < setupChannels.size(); ++index) {
            const auto current = buffer.channelSpec(index);
            const auto& setup = setupChannels[index];
            if (!current.has_value() || std::abs(current->ratio - setup.ratio) > 1e-12 ||
                std::abs(current->scale - setup.scale) > 1e-12 || std::abs(current->offset - setup.offset) > 1e-12 ||
                !sameBitDisplaySpec(current->bitDisplay, setup.bitDisplay)) {
                return false;
            }
        }
        return true;
    }

    bool sameChannelIdentity(const std::vector<plot::ChannelSpec>& setupChannels,
                             const plot::OscilloscopeBuffer& buffer)
    {
        if (setupChannels.size() != buffer.channelCount()) {
            return false;
        }
        for (std::size_t i = 0; i < setupChannels.size(); ++i) {
            const auto current = buffer.channelSpec(i);
            if (!current.has_value()) {
                return false;
            }
            const auto& setup = setupChannels[i];
            if (current->label != setup.label || current->unit != setup.unit ||
                !sameBitDisplayIdentity(current->bitDisplay, setup.bitDisplay)) {
                return false;
            }
        }
        return true;
    }

    bool sameChannelIdentity(const std::vector<plot::ChannelSpec>& setupChannels,
                             const std::vector<plot::ChannelSpec>& previousDefaults)
    {
        if (setupChannels.size() != previousDefaults.size()) {
            return false;
        }
        for (std::size_t i = 0; i < setupChannels.size(); ++i) {
            const auto& setup = setupChannels[i];
            const auto& previous = previousDefaults[i];
            if (previous.label != setup.label || previous.unit != setup.unit ||
                !sameBitDisplayIdentity(previous.bitDisplay, setup.bitDisplay)) {
                return false;
            }
        }
        return true;
    }

    plot::RawCapturePlotSetupEventData toRawPlotSetup(const scripting::PlotSetup& setup)
    {
        plot::RawCapturePlotSetupEventData rawSetup;
        rawSetup.source = setup.source;
        rawSetup.view = setup.view;
        rawSetup.resetHistory = setup.resetHistory;
        rawSetup.channels.reserve(setup.channels.size());
        for (const auto& channel : setup.channels) {
            rawSetup.channels.push_back(plot::ChannelSpec{
                .label = channel.label,
                .unit = channel.unit,
                .ratio = channel.ratio,
                .scale = channel.scale,
                .offset = channel.offset,
                .color = channel.color,
                .lineWidth = channel.lineWidth,
                .bitDisplay = channel.bitDisplay,
            });
        }
        return rawSetup;
    }

    using ScriptPlotAppendRequest = std::pair<std::size_t, plot::WaveAppendRequest>;

    struct ScriptPlotAppendDrainState {
        std::vector<ScriptPlotAppendRequest> selected;
        std::vector<ScriptPlotAppendRequest> deferred;
        std::unordered_map<std::string, std::size_t> selectedIndexes;
        std::size_t maxSelectedKeys{0};
    };

    std::string scriptPlotAppendKey(std::size_t channelIndex, const std::string& source)
    {
        auto key = std::to_string(channelIndex);
        key.push_back('\x1F');
        key.append(source);
        return key;
    }

    ScriptPlotAppendRequest takePendingScriptPlotAppend(std::deque<ScriptPlotAppendRequest>& pending)
    {
        auto append = std::move(pending.front());
        pending.pop_front();
        return append;
    }

    void mergePlotAppendSamples(plot::WaveAppendRequest& target, plot::WaveAppendRequest& source)
    {
        target.samples.insert(target.samples.end(),
                              std::make_move_iterator(source.samples.begin()),
                              std::make_move_iterator(source.samples.end()));
    }

    bool shouldDeferNewScriptPlotAppend(const ScriptPlotAppendDrainState& state, const std::string& key)
    {
        return !state.selectedIndexes.contains(key) && state.selectedIndexes.size() >= state.maxSelectedKeys;
    }

    void deferScriptPlotAppend(ScriptPlotAppendDrainState& state, ScriptPlotAppendRequest& append)
    {
        auto& [channelIndex, request] = append;
        state.deferred.emplace_back(channelIndex, std::move(request));
    }

    void selectOrMergeScriptPlotAppend(ScriptPlotAppendDrainState& state,
                                       ScriptPlotAppendRequest& append,
                                       std::string key)
    {
        auto& [channelIndex, request] = append;
        const auto [position, inserted] = state.selectedIndexes.emplace(std::move(key), state.selected.size());
        if (inserted) {
            state.selected.emplace_back(channelIndex, std::move(request));
            return;
        }
        mergePlotAppendSamples(state.selected[position->second].second, request);
    }

    void drainScriptPlotAppendCandidate(ScriptPlotAppendDrainState& state, ScriptPlotAppendRequest append)
    {
        auto& [channelIndex, request] = append;
        const auto key = scriptPlotAppendKey(channelIndex, request.source);
        if (shouldDeferNewScriptPlotAppend(state, key)) {
            deferScriptPlotAppend(state, append);
            return;
        }
        selectOrMergeScriptPlotAppend(state, append, key);
    }

    void restoreDeferredScriptPlotAppends(std::deque<ScriptPlotAppendRequest>& pending,
                                          std::vector<ScriptPlotAppendRequest>& deferred)
    {
        for (auto& append : deferred) {
            pending.push_back(std::move(append));
        }
    }

    std::vector<ScriptPlotAppendRequest> mergeNonEmptyScriptPlotAppends(std::vector<ScriptPlotAppendRequest> requests)
    {
        std::vector<ScriptPlotAppendRequest> mergedRequests;
        std::unordered_map<std::string, std::size_t> mergedIndexes;
        mergedRequests.reserve(requests.size());
        for (auto append : requests) {
            auto& [channelIndex, request] = append;
            if (request.samples.empty()) {
                continue;
            }
            const auto key = scriptPlotAppendKey(channelIndex, request.source);
            const auto [position, inserted] = mergedIndexes.emplace(key, mergedRequests.size());
            if (inserted) {
                mergedRequests.emplace_back(channelIndex, std::move(request));
                continue;
            }
            mergePlotAppendSamples(mergedRequests[position->second].second, request);
        }
        return mergedRequests;
    }

    bool nearlyEqual(double left, double right);
    bool sameWaveViewState(const plot::WaveViewState& view, const plot::ViewConfig& config);

    bool nearlyEqual(double left, double right)
    {
        return std::abs(left - right) <= 1e-12;
    }

    bool sameWaveViewState(const plot::WaveViewState& view, const plot::ViewConfig& config)
    {
        const double defaultVisibleDuration =
            (std::max)(view.minVisibleTimeSpan, (std::max)(config.timeScale * 1000.0, config.timeScale));
        if (view.defaultViewportXScale == plot::WaveResetViewportScaleMode::ProtocolDefault &&
            !nearlyEqual(view.visibleDuration, defaultVisibleDuration)) {
            return false;
        }
        if (view.defaultViewportYScale == plot::WaveResetViewportScaleMode::ProtocolDefault &&
            (!nearlyEqual(view.manualVerticalMin, config.verticalMin) ||
             !nearlyEqual(view.manualVerticalMax, config.verticalMax))) {
            return false;
        }
        return true;
    }

} // namespace

std::string Application::formatFrequencyInput(double valueHz)
{
    if (!(std::isfinite(valueHz)) || valueHz <= 0.0) {
        return {};
    }
    std::ostringstream out;
    out << valueHz;
    return out.str();
}


bool Application::sameAppliedPlotSetup(const plot::WaveDockState& wave, const plot::RawCapturePlotSetupEventData& setup)
{
    const auto currentConfig = wave.buffer.viewConfig();
    return sameFullChannelSpecs(setup.channels, wave.buffer) &&
           nearlyEqual(currentConfig.timeScale, setup.view.timeScale) &&
           currentConfig.timeUnit == setup.view.timeUnit &&
           nearlyEqual(currentConfig.verticalMin, setup.view.verticalMin) &&
           nearlyEqual(currentConfig.verticalMax, setup.view.verticalMax) &&
           currentConfig.verticalUnit == setup.view.verticalUnit &&
           currentConfig.historyLimit == setup.view.historyLimit && sameWaveViewState(wave.view, setup.view);
}


void Application::applyLegacyResetViewport(const plot::ViewConfig& protocolView, const bool resetVerticalRange)
{
    auto& view = dockStore_.waveState().view;
    view.visibleDuration =
        (std::max)(view.minVisibleTimeSpan, (std::max)(protocolView.timeScale * 1000.0, protocolView.timeScale));
    if (resetVerticalRange) {
        view.manualVerticalMin = protocolView.verticalMin;
        view.manualVerticalMax = protocolView.verticalMax;
        view.viewMinValue = protocolView.verticalMin;
        view.viewMaxValue = protocolView.verticalMax;
    }
    view.defaultViewportXScale = plot::WaveResetViewportScaleMode::ProtocolDefault;
    view.defaultViewportYScale = resetVerticalRange ? plot::WaveResetViewportScaleMode::ProtocolDefault
                                                    : plot::WaveResetViewportScaleMode::Preserve;
    view.defaultViewportXAnchor = plot::WaveResetViewportAnchor::Latest;
    view.initialized = false;
    view.defaultViewportPending = true;
    view.defaultViewportLegacyBehavior = true;
}


bool Application::applyResetViewportPolicy(const WaveResetViewportTrigger trigger, const plot::ViewConfig& protocolView)
{
    const auto& policy = runtimeConfig_.gui.wave.resetViewport;
    bool enabled = false;
    switch (trigger) {
        case WaveResetViewportTrigger::PlotSetupReset:
            enabled = policy.applyOnPlotSetupReset;
            break;
        case WaveResetViewportTrigger::ManualClear:
            enabled = policy.applyOnManualClear;
            break;
        case WaveResetViewportTrigger::RawImport:
            enabled = policy.applyOnRawImport;
            break;
    }
    if (!enabled) {
        return false;
    }

    auto& view = dockStore_.waveState().view;
    view.defaultViewportXScale = policy.xScale;
    view.defaultViewportYScale = policy.yScale;
    view.defaultViewportXAnchor = policy.xAnchor;
    if (policy.xScale == plot::WaveResetViewportScaleMode::ProtocolDefault) {
        view.visibleDuration =
            (std::max)(view.minVisibleTimeSpan, (std::max)(protocolView.timeScale * 1000.0, protocolView.timeScale));
    } else {
        view.visibleDuration = (std::max)(view.visibleDuration, view.minVisibleTimeSpan);
    }
    if (policy.yScale == plot::WaveResetViewportScaleMode::ProtocolDefault) {
        view.manualVerticalMin = protocolView.verticalMin;
        view.manualVerticalMax = protocolView.verticalMax;
        view.viewMinValue = protocolView.verticalMin;
        view.viewMaxValue = protocolView.verticalMax;
    }
    if (policy.autoFollow == plot::WaveResetViewportAutoFollowMode::Enable) {
        view.autoFollowLatest = true;
    } else if (policy.autoFollow == plot::WaveResetViewportAutoFollowMode::Disable) {
        view.autoFollowLatest = false;
    }
    view.initialized = false;
    view.defaultViewportPending = true;
    view.defaultViewportLegacyBehavior = false;
    return true;
}


bool Application::offlineWaveRebuilding() const
{
    return offlineScriptWorker_ && importedRebuildGeneration_ == rawCaptureReplay_.generation &&
           rawCaptureReplay_.loaded && rawCaptureReplay_.phase != OfflineReplayPhase::Failed &&
           rawCaptureReplay_.phase != OfflineReplayPhase::Cancelled &&
           rawCaptureReplay_.processing != OfflineReplayProcessing::BrowseRaw;
}


void Application::freezeImportedChannelLabels()
{
    auto& wave = dockStore_.waveState();
    while (wave.defaultChannelSpecs.size() < wave.buffer.channelCount())
        wave.defaultChannelSpecs.push_back(*wave.buffer.channelSpec(wave.defaultChannelSpecs.size()));
    for (std::size_t i = importedActualLabels_.size(); i < wave.buffer.channelCount(); ++i)
        importedActualLabels_.push_back(wave.buffer.channelSpec(i)->label);
    for (std::size_t i = importedDefaultLabels_.size(); i < wave.defaultChannelSpecs.size(); ++i)
        importedDefaultLabels_.push_back(wave.defaultChannelSpecs[i].label);
}


void Application::switchToLiveWaveSource()
{
    if (!dockStore_.waveState().buffer.importedLabelsReadOnly()) return;
    if (rawCaptureReplay_.loaded) unloadRawCaptureReplayTimeline();
    logRealtimeBacklogDiscard(clearPendingRealtimeBacklog());
    resetWaveHistory();
    importedActualLabels_.clear();
    importedDefaultLabels_.clear();
    importedRebuildGeneration_.reset();
    dockStore_.waveState().buffer.setHistoryTrimSuspended(false);
    dockStore_.waveState().buffer.setDataSource(plot::WaveDataSource::Live);
}


plot::RawCapturePlotSetupEventData Application::normalizeImportedSetup(plot::RawCapturePlotSetupEventData setup) const
{
    for (std::size_t i = 0; i < setup.channels.size() && i < importedActualLabels_.size(); ++i)
        setup.channels[i].label = importedActualLabels_[i];
    return setup;
}


bool Application::applyPlotSetup(const plot::RawCapturePlotSetupEventData& input, bool rebuilding)
{
    const auto setup = rebuilding ? normalizeImportedSetup(input) : input;
    auto& wave = dockStore_.waveState();
    if (wave.buffer.importedLabelsReadOnly() && !rebuilding) {
        bool changed = false;
        // 导入只更新共有位置的显示规格；绝不能进入会清历史、改通道数或视窗的全量路径。
        const auto count = (std::min)(setup.channels.size(), wave.buffer.channelCount());
        for (std::size_t index = 0; index < count; ++index) {
            const auto currentSpec = *wave.buffer.channelSpec(index);
            auto defaultSpec = setup.channels[index];
            defaultSpec.label = index < wave.defaultChannelSpecs.size()
                                    ? wave.defaultChannelSpecs[index].label : currentSpec.label;
            auto effectiveSpec = defaultSpec;
            // 默认基线与用户显示覆盖独立，Lua 配置不得清除覆盖或借默认名称间接改名。
            if (index < wave.channelOverrides.size()) {
                const auto& overrideState = wave.channelOverrides[index];
                if (overrideState.ratioOverridden) effectiveSpec.ratio = overrideState.ratio;
                if (overrideState.scaleOverridden) effectiveSpec.scale = overrideState.scale;
                if (overrideState.offsetOverridden) effectiveSpec.offset = overrideState.offset;
                if (overrideState.colorOverridden) effectiveSpec.color = overrideState.color;
                if (overrideState.bitYOffsetOverridden) effectiveSpec.bitDisplay.yOffset = overrideState.bitYOffset;
            }
            effectiveSpec.label = currentSpec.label;
            if (index < wave.defaultChannelSpecs.size())
                wave.defaultChannelSpecs[index] = std::move(defaultSpec);
            wave.buffer.setChannelSpec(index, std::move(effectiveSpec));
            changed = true;
        }
        return changed;
    }
    const auto previousConfig = wave.buffer.viewConfig();
    const auto previousDefaultChannelSpecs = wave.defaultChannelSpecs;
    const bool configChanged = !nearlyEqual(previousConfig.timeScale, setup.view.timeScale) ||
                               previousConfig.timeUnit != setup.view.timeUnit ||
                               !nearlyEqual(previousConfig.verticalMin, setup.view.verticalMin) ||
                               !nearlyEqual(previousConfig.verticalMax, setup.view.verticalMax) ||
                               previousConfig.verticalUnit != setup.view.verticalUnit ||
                               previousConfig.historyLimit != setup.view.historyLimit;
    const bool hasPreviousDefaults = !previousDefaultChannelSpecs.empty();
    const bool channelsChanged = hasPreviousDefaults ? !sameChannelSpecs(setup.channels, previousDefaultChannelSpecs)
                                                     : !sameChannelSpecs(setup.channels, wave.buffer);
    const bool channelIdentityChanged = hasPreviousDefaults
                                            ? !sameChannelIdentity(setup.channels, previousDefaultChannelSpecs)
                                            : !sameChannelIdentity(setup.channels, wave.buffer);

    // 核心流程：Lua 和 psraw 回放共用同一套配置应用逻辑，确保通道默认值、覆盖状态和视图重置一致。
    if (setup.resetHistory) {
        wave.buffer.clear();
    }
    auto viewConfig = setup.view;
    viewConfig.displayFormula = wave.view.displayFormula;
    wave.buffer.setViewConfig(viewConfig);
    wave.buffer.configureChannels(setup.channels.size());
    wave.defaultChannelSpecs.clear();
    wave.defaultChannelSpecs.reserve(setup.channels.size());
    const bool shouldResetOverrides = channelIdentityChanged;
    if (shouldResetOverrides) {
        wave.channelOverrides.clear();
    }
    wave.channelOverrides.resize(setup.channels.size());
    for (std::size_t index = 0; index < setup.channels.size(); ++index) {
        auto defaultSpec = setup.channels[index];
        if (rebuilding && defaultSpec.label.empty()) defaultSpec.label = "CH" + std::to_string(index + 1);
        if (rebuilding && index < importedDefaultLabels_.size())
            defaultSpec.label = importedDefaultLabels_[index];
        auto effectiveSpec = defaultSpec;
        if (!shouldResetOverrides && index < wave.channelOverrides.size()) {
            const auto& overrideState = wave.channelOverrides[index];
            if (overrideState.labelOverridden) {
                effectiveSpec.label = overrideState.label;
            }
            if (overrideState.ratioOverridden) {
                effectiveSpec.ratio = overrideState.ratio;
            }
            if (overrideState.scaleOverridden) {
                effectiveSpec.scale = overrideState.scale;
            }
            if (overrideState.offsetOverridden) {
                effectiveSpec.offset = overrideState.offset;
            }
            if (overrideState.colorOverridden) {
                effectiveSpec.color = overrideState.color;
            }
            if (overrideState.bitYOffsetOverridden) {
                effectiveSpec.bitDisplay.yOffset = overrideState.bitYOffset;
            }
        }
        if (rebuilding && index < importedActualLabels_.size())
            effectiveSpec.label = importedActualLabels_[index];
        wave.defaultChannelSpecs.push_back(defaultSpec);
        if (rebuilding) wave.buffer.setChannelSpecForImport(index, std::move(effectiveSpec));
        else wave.buffer.setChannelSpec(index, std::move(effectiveSpec));
    }
    const bool verticalDefaultsChanged = !nearlyEqual(previousConfig.verticalMin, setup.view.verticalMin) ||
                                         !nearlyEqual(previousConfig.verticalMax, setup.view.verticalMax);
    const bool shouldResetView = setup.resetHistory || configChanged || channelsChanged;
    if (shouldResetView) {
        if (!applyResetViewportPolicy(WaveResetViewportTrigger::PlotSetupReset, setup.view)) {
            applyLegacyResetViewport(setup.view, setup.resetHistory || verticalDefaultsChanged);
        }
        wave.statusMessage = "Lua 已更新波形通道配置";
    }
    return true;
}


bool Application::requestOscilloscopeToggle(bool currentRunning, bool targetRunning)
{
    if (dataTransferStatus().active) return false;
    if (targetRunning) switchToLiveWaveSource();
    transport::ConnectionContext context;
    if (activeConnection_.has_value()) {
        context = *activeConnection_;
    } else {
        // 核心流程：示波器启停由 Lua 决定，未连接时仍允许脚本基于 detached 上下文处理本地动作。
        context.endpoint = "detached";
        context.connectionId = 0;
        context.timestampMs = nowMs();
        context.readyForIo = false;
    }

    try {
        const bool accepted = scriptWorker_.requestOscilloscopeToggle(context, currentRunning, targetRunning);
        flushScriptOutputs();
        syncDockState();
        return accepted;
    } catch (const std::exception& ex) {
        setStatusMessage(std::string("示波器切换请求失败: ") + ex.what(), false);
    } catch (...) {
        setStatusMessage("示波器切换请求失败: 未知异常", false);
    }
    flushScriptOutputs();
    syncDockState();
    return false;
}


bool Application::importWaveCsvData(const plot::WaveCsvData& data, std::string& error)
{
    if (!validateOfflineReplayTransport(error)) return false;
    if (data.channels.empty()) {
        error = "波形 CSV 未包含任何通道";
        return false;
    }

    auto& wave = dockStore_.waveState();
    logRealtimeBacklogDiscard(clearPendingRealtimeBacklog());
    importedActualLabels_.clear();
    importedDefaultLabels_.clear();
    importedRebuildGeneration_.reset();
    wave.buffer.setDataSource(plot::WaveDataSource::Imported);
    wave.buffer.clear();
    wave.buffer.setHistoryTrimSuspended(true);
    wave.buffer.setViewConfig(data.view);
    wave.buffer.configureChannels(data.channels.size());
    wave.defaultChannelSpecs.clear();
    wave.channelOverrides.clear();
    wave.defaultChannelSpecs.reserve(data.channels.size());
    std::size_t maxSampleCount = 0;
    for (std::size_t channelIndex = 0; channelIndex < data.channels.size(); ++channelIndex) {
        const auto& csvChannel = data.channels[channelIndex];
        auto spec = csvChannel.spec;
        spec.label = csvChannel.label.empty() ? "CH" + std::to_string(channelIndex + 1) : csvChannel.label;
        spec.unit = csvChannel.unit;
        wave.buffer.setChannelSpecForImport(channelIndex, spec);
        wave.defaultChannelSpecs.push_back(*wave.buffer.channelSpec(channelIndex));
        if (!csvChannel.samples.empty()) {
            // 核心流程：CSV time 是外部数据的显示时间，直接作为脚本时间轴写入，不再按采样率重解释。
            wave.buffer.appendImported(channelIndex, csvChannel.samples, csvChannel.sampleIndexOffset);
            maxSampleCount = (std::max)(maxSampleCount, csvChannel.samples.size());
        }
    }
    freezeImportedChannelLabels();
    wave.buffer.setImportedSource(data.source);
    importedWaveIncomplete_ = data.incomplete;
    importedWaveRange_ = data.rangeDescription;
    wave.buffer.preserveHistoryLimitAtLeast(maxSampleCount);
    wave.analysisMarkers.clear();
    wave.channelSummaries.clear();
    wave.hiddenChannelIndices.clear();
    wave.view.sampleFrequencyHz = data.timeAxis == "frequency" ? data.sampleFrequencyHz : 0.0;
    wave.view.sampleFrequencyInput = formatFrequencyInput(wave.view.sampleFrequencyHz);
    wave.view.timeAxisSource = data.timeAxis == "frequency" ? plot::WaveTimeAxisSource::SampleFrequency :
        data.timeAxis == "index" ? plot::WaveTimeAxisSource::SampleIndex : plot::WaveTimeAxisSource::ScriptTime;
    wave.view.defaultViewportPending = true;
    wave.view.defaultViewportLegacyBehavior = false;
    wave.view.autoFollowLatest = false;
    wave.statusMessage = "波形 CSV 已导入";
    syncDockState();
    return true;
}


std::optional<plot::WaveCsvData> Application::captureWaveData(const plot::CsvExportRange& range, std::string& error) const
{
    if (!plot::validateCsvExportRange(range, error)) return std::nullopt;
    const auto& wave = dockStore_.waveState();
    auto snapshot =
        wave.buffer.snapshot(-std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity(), false);
    plot::WaveDisplayData displayData;
    plot::buildQueryDisplayDataInto(snapshot, wave.view.sampleFrequencyHz, 2, displayData);

    plot::WaveCsvData data;
    data.sampleFrequencyHz = wave.view.sampleFrequencyHz;
    data.view = wave.buffer.viewConfig();
    data.source = snapshot.source;
    data.incomplete = importedWaveIncomplete_;
    data.timeAxis = displayData.axisSource == plot::WaveTimeAxisSource::SampleFrequency ? "frequency" :
        displayData.axisSource == plot::WaveTimeAxisSource::SampleIndex ? "index" : "script";
    const auto resolved = plot::resolveCsvExportTimeRange(range);
    data.rangeDescription = importedWaveRange_;
    if (resolved) {
        std::ostringstream description;
        description << (range.kind == plot::CsvExportRangeKind::CurrentView ? "view:" :
                        range.kind == plot::CsvExportRangeKind::CursorPair ? "cursors:" : "manual:")
                    << std::setprecision(17) << resolved->first << ',' << resolved->second;
        data.rangeDescription = description.str();
    }
    std::size_t sampleCount = 0;
    data.channels.reserve(snapshot.channels.size());
    for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
        plot::WaveCsvChannel channel{
            .label = snapshot.channels[channelIndex].label,
            .unit = snapshot.channels[channelIndex].unit,
        };
        const auto& source = snapshot.channels[channelIndex];
        channel.spec = *wave.buffer.channelSpec(channelIndex);
        channel.sampleIndexOffset = source.sampleIndexOffset;
        for (std::size_t i = 0; i < source.totalSamples; ++i) {
            const double time = data.timeAxis == "frequency" ?
                static_cast<double>(source.sampleIndexOffset + i) / data.sampleFrequencyHz :
                data.timeAxis == "index" ? static_cast<double>(source.sampleIndexOffset + i) : source.samples[i].time;
            if (resolved && (time < resolved->first || time > resolved->second)) continue;
            if (channel.samples.empty()) channel.sampleIndexOffset += i;
            channel.samples.push_back(source.samples[i]);
            ++sampleCount;
        }
        data.channels.push_back(std::move(channel));
    }
    if (sampleCount == 0) {
        error = "所选波形范围没有样本";
        return std::nullopt;
    }
    return data;
}


void Application::resetWaveHistoryForTrigger(const WaveResetViewportTrigger trigger)
{
    importedWaveIncomplete_ = false;
    importedWaveRange_ = "full";
    auto& wave = dockStore_.waveState();
    wave.buffer.clear();
    wave.rawCapture = {};
    wave.view.auxiliaryCursors.clear();
    wave.analysisMarkers.clear();
    wave.channelSummaries.clear();
    wave.view.autoFollowLatest = true;
    if (applyResetViewportPolicy(trigger, wave.buffer.viewConfig())) {
        wave.view.centerTime = 0.0;
        wave.view.viewMinTime = 0.0;
        wave.view.viewMaxTime = wave.view.visibleDuration;
        wave.statusMessage = "波形历史已清空";
        return;
    }
    wave.view.initialized = false;
    wave.view.centerTime = 0.0;
    wave.view.viewMinTime = 0.0;
    wave.view.viewMaxTime = wave.view.visibleDuration;
    wave.view.viewMinValue = wave.view.manualVerticalMin;
    wave.view.viewMaxValue = wave.view.manualVerticalMax;
    wave.view.defaultViewportPending = true;
    wave.view.defaultViewportLegacyBehavior = true;
    wave.statusMessage = "波形历史已清空";
}


void Application::resetWaveHistory()
{
    if (dataTransferStatus().active) return;
    resetWaveHistoryForTrigger(WaveResetViewportTrigger::ManualClear);
}


bool Application::exportWaveAnalysisReport(const std::filesystem::path& path, std::string& error) const
{
    const auto& wave = dockStore_.waveState();
    try {
        if (path.has_parent_path()) {
            std::error_code directoryError;
            std::filesystem::create_directories(path.parent_path(), directoryError);
            if (directoryError) {
                error = "创建分析报告目录失败: " + directoryError.message();
                return false;
            }
        }

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            error = "无法写入波形分析报告";
            return false;
        }

        const auto& view = wave.view;
        out << "section,key,value\n";
        out << "summary,protocol," << csvEscape(dockStore_.luaState().protocolName) << '\n';
        out << "summary,protocol_dir," << csvEscape(dockStore_.luaState().protocolDir) << '\n';
        out << "summary,sample_frequency_hz," << view.sampleFrequencyHz << '\n';
        out << "summary,view_min_time," << view.viewMinTime << '\n';
        out << "summary,view_max_time," << view.viewMaxTime << '\n';
        out << "summary,raw_capture_bytes," << wave.rawCapture.payload.size() << '\n';
        out << "summary,raw_capture_events," << wave.rawCapture.events.size() << '\n';
        out << "summary,markers," << wave.analysisMarkers.size() << '\n';
        out << '\n';
        out << "marker,id,label,note,channel_index,start_time,end_time\n";
        for (const auto& marker : wave.analysisMarkers) {
            out << "marker," << marker.id << ',' << csvEscape(marker.label) << ',' << csvEscape(marker.note) << ','
                << marker.channelIndex << ',' << marker.startTime << ',' << marker.endTime << '\n';
        }
        if (!out.good()) {
            error = "写入波形分析报告失败";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}


bool Application::applyScriptOscilloscopeOutputs(const scripting::ScriptRuntimeOutputBatch& batch)
{
    bool changed = false;
    auto& wave = dockStore_.waveState();
    for (const auto& update : batch.oscilloscopeRunningUpdates) {
        if (wave.oscilloscopeRunning != update.running) {
            wave.oscilloscopeRunning = update.running;
            changed = true;
        }
    }
    return changed;
}


bool Application::applyScriptPlotOutputs(const scripting::ScriptRuntimeOutputBatch& batch, bool rebuilding)
{
    const bool setupChanged = applyScriptPlotSetups(batch.plotSetups, rebuilding);
    // 导入时普通输出不得入队，也不得消费离线预算残留；权限只随有效离线 generation 续 drain。
    if (dockStore_.waveState().buffer.importedLabelsReadOnly() && !rebuilding) return setupChanged;
    enqueueScriptPlotAppends(batch.plotAppends);
    auto appendRequests = drainScriptPlotAppendsForPump(plotAppendsPerPump());
    return appendScriptPlotRequestsToWave(std::move(appendRequests), rebuilding) || setupChanged;
}


bool Application::applyScriptPlotSetups(const std::vector<scripting::PlotSetup>& setups, bool rebuilding)
{
    bool changed = false;
    for (const auto& setup : setups) {
        auto rawSetup = toRawPlotSetup(setup);
        changed = applyPlotSetup(rawSetup, rebuilding) || changed;
        // 导入只应用非名称显示规格，不能把含有被拒绝结构字段的整份 Lua 配置记成采集事件。
        if (dockStore_.waveState().buffer.importedLabelsReadOnly()) continue;
        const auto timestampMs = activeConnection_.has_value() ? activeConnection_->timestampMs : nowMs();
        recordPlotSetupSnapshot(rawSetup, timestampMs);
    }
    return changed;
}


void Application::enqueueScriptPlotAppends(const std::vector<std::pair<std::size_t, plot::WaveAppendRequest>>& appends)
{
    const auto hardLimit = (std::max)(runtimeConfig_.scripting.workerOutputQueueLimit, std::size_t{1U});
    for (auto append : appends) {
        if (pendingScriptPlotAppends_.size() >= hardLimit) {
            loggingFacade_.warn("worker", "plot 输出达到硬上界，已丢弃超限离线输出");
            break;
        }
        pendingScriptPlotAppends_.push_back(std::move(append));
    }
}


std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> Application::drainScriptPlotAppendsForPump(
    const std::size_t maxPlotAppends)
{
    ScriptPlotAppendDrainState drainState{};
    drainState.maxSelectedKeys = maxPlotAppends;
    while (!pendingScriptPlotAppends_.empty()) {
        auto append = takePendingScriptPlotAppend(pendingScriptPlotAppends_);
        drainScriptPlotAppendCandidate(drainState, std::move(append));
    }

    // 保持预算外的不同源 append 原始顺序，留到后续 pump 继续处理。
    restoreDeferredScriptPlotAppends(pendingScriptPlotAppends_, drainState.deferred);
    return std::move(drainState.selected);
}


bool Application::appendScriptPlotRequestsToWave(std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> requests,
                                                 bool rebuilding)
{
    if (dockStore_.waveState().buffer.importedLabelsReadOnly() && !rebuilding) return false;
    bool changed = false;
    auto mergedRequests = mergeNonEmptyScriptPlotAppends(std::move(requests));
    auto& wave = dockStore_.waveState();
    for (auto& [channelIndex, request] : mergedRequests) {
        if (wave.buffer.append(channelIndex, std::move(request))) {
            if (rebuilding) {
                // 高索引 push 会隐式建立所有低索引位置，必须分别恢复两组冻结名称。
                for (std::size_t index = 0; index < wave.buffer.channelCount() &&
                     index < importedActualLabels_.size(); ++index) {
                    auto spec = *wave.buffer.channelSpec(index);
                    spec.label = importedActualLabels_[index];
                    wave.buffer.setChannelSpecForImport(index, std::move(spec));
                }
                while (wave.defaultChannelSpecs.size() < wave.buffer.channelCount())
                    wave.defaultChannelSpecs.push_back(*wave.buffer.channelSpec(wave.defaultChannelSpecs.size()));
                for (std::size_t index = 0; index < wave.defaultChannelSpecs.size() &&
                     index < importedDefaultLabels_.size(); ++index)
                    wave.defaultChannelSpecs[index].label = importedDefaultLabels_[index];
            }
            changed = true;
        }
    }
    return changed;
}


bool Application::flushScriptPlots()
{
    return false;
}

} // namespace protoscope::app
