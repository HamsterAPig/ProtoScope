#include "protoscope/ui/protocol_ui_state.hpp"

#include <algorithm>
#include <string>

#include <yaml-cpp/yaml.h>

namespace protoscope::ui {
namespace {

    std::string axisSourceName(plot::WaveTimeAxisSource source)
    {
        switch (source) {
            case plot::WaveTimeAxisSource::SampleIndex:
                return "sample_index";
            case plot::WaveTimeAxisSource::SampleFrequency:
                return "sample_frequency";
            case plot::WaveTimeAxisSource::ScriptTime:
                return "script_time";
        }
        return "sample_index";
    }

    plot::WaveTimeAxisSource parseAxisSource(const std::string& value)
    {
        if (value == "sample_frequency") {
            return plot::WaveTimeAxisSource::SampleFrequency;
        }
        if (value == "script_time") {
            return plot::WaveTimeAxisSource::ScriptTime;
        }
        return plot::WaveTimeAxisSource::SampleIndex;
    }

    std::string snapModeName(plot::WaveCursorSnapMode mode)
    {
        switch (mode) {
            case plot::WaveCursorSnapMode::SmartSnap:
                return "smart";
            case plot::WaveCursorSnapMode::ModifierSnap:
                return "modifier";
        }
        return "smart";
    }

    plot::WaveCursorSnapMode parseSnapMode(const std::string& value)
    {
        return value == "modifier" ? plot::WaveCursorSnapMode::ModifierSnap : plot::WaveCursorSnapMode::SmartSnap;
    }

    std::string snapScopeName(plot::WaveCursorSnapScope scope)
    {
        switch (scope) {
            case plot::WaveCursorSnapScope::AllChannels:
                return "all_channels";
            case plot::WaveCursorSnapScope::ActiveChannel:
                return "active_channel";
        }
        return "all_channels";
    }

    plot::WaveCursorSnapScope parseSnapScope(const std::string& value)
    {
        return value == "active_channel" ? plot::WaveCursorSnapScope::ActiveChannel
                                         : plot::WaveCursorSnapScope::AllChannels;
    }

    std::string extremeSnapPolicyName(plot::WaveCursorExtremeSnapPolicy policy)
    {
        switch (policy) {
            case plot::WaveCursorExtremeSnapPolicy::NearestWaveform:
                return "nearest_waveform";
            case plot::WaveCursorExtremeSnapPolicy::ViewportZone:
                return "viewport_zone";
        }
        return "nearest_waveform";
    }

    plot::WaveCursorExtremeSnapPolicy parseExtremeSnapPolicy(const std::string& value)
    {
        return value == "viewport_zone" ? plot::WaveCursorExtremeSnapPolicy::ViewportZone
                                        : plot::WaveCursorExtremeSnapPolicy::NearestWaveform;
    }

    std::string measurementReferenceModeName(plot::WaveMeasurementReferenceMode mode)
    {
        return mode == plot::WaveMeasurementReferenceMode::ManualValue ? "manual_value" : "channel";
    }

    plot::WaveMeasurementReferenceMode parseMeasurementReferenceMode(const std::string& value)
    {
        return value == "manual_value" ? plot::WaveMeasurementReferenceMode::ManualValue
                                       : plot::WaveMeasurementReferenceMode::Channel;
    }

    std::string fftPointCountStateName(plot::WaveFftPointCount value)
    {
        return plot::fftPointCountName(value);
    }

    plot::WaveFftPointCount parseFftPointCount(const std::string& value)
    {
        if (value == "Visible" || value == "visible_samples") {
            return plot::WaveFftPointCount::VisibleSamples;
        }
        if (value == "Auto 2^n" || value == "Auto" || value == "auto_power_of_two") {
            return plot::WaveFftPointCount::Auto;
        }
        if (value == "Manual" || value == "manual") {
            return plot::WaveFftPointCount::Manual;
        }
        if (value == "256") {
            return plot::WaveFftPointCount::N256;
        }
        if (value == "512") {
            return plot::WaveFftPointCount::N512;
        }
        if (value == "1024") {
            return plot::WaveFftPointCount::N1024;
        }
        if (value == "2048") {
            return plot::WaveFftPointCount::N2048;
        }
        if (value == "4096") {
            return plot::WaveFftPointCount::N4096;
        }
        if (value == "8192") {
            return plot::WaveFftPointCount::N8192;
        }
        if (value == "16384") {
            return plot::WaveFftPointCount::N16384;
        }
        return plot::WaveFftPointCount::VisibleSamples;
    }

    std::string fftWindowStateName(plot::WaveFftWindow value)
    {
        switch (value) {
            case plot::WaveFftWindow::Rectangular:
                return "rectangular";
            case plot::WaveFftWindow::Hann:
                return "hann";
            case plot::WaveFftWindow::Hamming:
                return "hamming";
            case plot::WaveFftWindow::BlackmanHarris:
                return "blackman_harris";
        }
        return "hann";
    }

    plot::WaveFftWindow parseFftWindow(const std::string& value)
    {
        if (value == "rectangular") {
            return plot::WaveFftWindow::Rectangular;
        }
        if (value == "hamming") {
            return plot::WaveFftWindow::Hamming;
        }
        if (value == "blackman_harris") {
            return plot::WaveFftWindow::BlackmanHarris;
        }
        return plot::WaveFftWindow::Hann;
    }

    std::string fftMagnitudeModeStateName(plot::WaveFftMagnitudeMode value)
    {
        return value == plot::WaveFftMagnitudeMode::Decibel ? "db" : "linear";
    }

    plot::WaveFftMagnitudeMode parseFftMagnitudeMode(const std::string& value)
    {
        return value == "db" ? plot::WaveFftMagnitudeMode::Decibel : plot::WaveFftMagnitudeMode::Linear;
    }

    std::string fftFundamentalModeStateName(plot::WaveFftFundamentalMode value)
    {
        return value == plot::WaveFftFundamentalMode::Manual ? "manual" : "auto";
    }

    plot::WaveFftFundamentalMode parseFftFundamentalMode(const std::string& value)
    {
        return value == "manual" ? plot::WaveFftFundamentalMode::Manual : plot::WaveFftFundamentalMode::Auto;
    }

    YAML::Node encodeMeasurementSelection(const plot::WaveMeasurementSelection& selection)
    {
        YAML::Node node;
        node["cursor_a"] = selection.cursorA;
        node["cursor_b"] = selection.cursorB;
        node["delta_time"] = selection.deltaTime;
        node["delta_value"] = selection.deltaValue;
        node["frequency"] = selection.frequency;
        node["period"] = selection.period;
        node["sample_count"] = selection.sampleCount;
        node["span"] = selection.span;
        node["min"] = selection.min;
        node["max"] = selection.max;
        node["peak_to_peak"] = selection.peakToPeak;
        node["mean"] = selection.mean;
        node["rms"] = selection.rms;
        node["median"] = selection.median;
        node["p95"] = selection.p95;
        node["p99"] = selection.p99;
        node["variance"] = selection.variance;
        node["stddev"] = selection.stddev;
        node["cv"] = selection.cv;
        node["mad"] = selection.mad;
        node["median_abs_dev"] = selection.medianAbsDev;
        node["iqr"] = selection.iqr;
        node["p95_spread"] = selection.p95Spread;
        node["high_width"] = selection.highWidth;
        node["low_width"] = selection.lowWidth;
        node["duty_cycle"] = selection.dutyCycle;
        node["rise_time"] = selection.riseTime;
        node["fall_time"] = selection.fallTime;
        node["edge_count"] = selection.edgeCount;
        node["absolute_error"] = selection.absoluteError;
        node["relative_error_percent"] = selection.relativeErrorPercent;
        node["mean_error"] = selection.meanError;
        node["mse"] = selection.mse;
        node["rmse"] = selection.rmse;
        node["mae"] = selection.mae;
        node["max_abs_error"] = selection.maxAbsError;
        node["bias"] = selection.bias;
        return node;
    }

    void decodeMeasurementSelection(const YAML::Node& node, plot::WaveMeasurementSelection& selection)
    {
        if (!node || !node.IsMap()) {
            return;
        }
        selection.cursorA = node["cursor_a"].as<bool>(selection.cursorA);
        selection.cursorB = node["cursor_b"].as<bool>(selection.cursorB);
        selection.deltaTime = node["delta_time"].as<bool>(selection.deltaTime);
        selection.deltaValue = node["delta_value"].as<bool>(selection.deltaValue);
        selection.frequency = node["frequency"].as<bool>(selection.frequency);
        selection.period = node["period"].as<bool>(selection.period);
        selection.sampleCount = node["sample_count"].as<bool>(selection.sampleCount);
        selection.span = node["span"].as<bool>(selection.span);
        selection.min = node["min"].as<bool>(selection.min);
        selection.max = node["max"].as<bool>(selection.max);
        selection.peakToPeak = node["peak_to_peak"].as<bool>(selection.peakToPeak);
        selection.mean = node["mean"].as<bool>(selection.mean);
        selection.rms = node["rms"].as<bool>(selection.rms);
        selection.median = node["median"].as<bool>(selection.median);
        selection.p95 = node["p95"].as<bool>(selection.p95);
        selection.p99 = node["p99"].as<bool>(selection.p99);
        selection.variance = node["variance"].as<bool>(selection.variance);
        selection.stddev = node["stddev"].as<bool>(selection.stddev);
        selection.cv = node["cv"].as<bool>(selection.cv);
        selection.mad = node["mad"].as<bool>(selection.mad);
        selection.medianAbsDev = node["median_abs_dev"].as<bool>(selection.medianAbsDev);
        selection.iqr = node["iqr"].as<bool>(selection.iqr);
        selection.p95Spread = node["p95_spread"].as<bool>(selection.p95Spread);
        selection.highWidth = node["high_width"].as<bool>(selection.highWidth);
        selection.lowWidth = node["low_width"].as<bool>(selection.lowWidth);
        selection.dutyCycle = node["duty_cycle"].as<bool>(selection.dutyCycle);
        selection.riseTime = node["rise_time"].as<bool>(selection.riseTime);
        selection.fallTime = node["fall_time"].as<bool>(selection.fallTime);
        selection.edgeCount = node["edge_count"].as<bool>(selection.edgeCount);
        selection.absoluteError = node["absolute_error"].as<bool>(selection.absoluteError);
        selection.relativeErrorPercent = node["relative_error_percent"].as<bool>(selection.relativeErrorPercent);
        selection.meanError = node["mean_error"].as<bool>(selection.meanError);
        selection.mse = node["mse"].as<bool>(selection.mse);
        selection.rmse = node["rmse"].as<bool>(selection.rmse);
        selection.mae = node["mae"].as<bool>(selection.mae);
        selection.maxAbsError = node["max_abs_error"].as<bool>(selection.maxAbsError);
        selection.bias = node["bias"].as<bool>(selection.bias);
    }

    void applyChannelOverrides(plot::WaveDockState& wave)
    {
        const auto channelCount = wave.buffer.channelCount();
        for (std::size_t channelIndex = 0; channelIndex < channelCount; ++channelIndex) {
            const auto spec = wave.buffer.channelSpec(channelIndex);
            if (!spec.has_value()) {
                continue;
            }
            auto updated = *spec;
            if (channelIndex < wave.channelOverrides.size()) {
                const auto& overrideState = wave.channelOverrides[channelIndex];
                if (overrideState.labelOverridden) {
                    updated.label = overrideState.label;
                }
                if (overrideState.ratioOverridden) {
                    updated.ratio = overrideState.ratio;
                }
                if (overrideState.scaleOverridden) {
                    updated.scale = overrideState.scale;
                }
                if (overrideState.offsetOverridden) {
                    updated.offset = overrideState.offset;
                }
            }
            wave.buffer.setChannelSpec(channelIndex, updated);
        }
        if (channelCount == 0) {
            wave.view.measurementChannelIndex = 0;
            for (auto& cursor : wave.view.cursors) {
                cursor.channelIndex = 0;
            }
            return;
        }
        const std::size_t lastIndex = channelCount - 1;
        wave.view.measurementChannelIndex = (std::min)(wave.view.measurementChannelIndex, lastIndex);
        for (auto& cursor : wave.view.cursors) {
            cursor.channelIndex = (std::min)(cursor.channelIndex, lastIndex);
        }
    }

} // namespace

YAML::Node encodeWaveProtocolState(const plot::WaveDockState& wave)
{
    YAML::Node node;
    const auto& view = wave.view;
    node["auto_follow_latest"] = view.autoFollowLatest;
    node["pause_auto_follow_on_interaction"] = view.pauseAutoFollowOnInteraction;
    node["lock_vertical_range"] = view.lockVerticalRange;
    node["show_points_when_sparse"] = view.showPointsWhenSparse;
    node["show_axis_labels"] = view.showAxisLabels;
    node["show_channel_legend"] = view.showChannelLegend;
    node["show_fft_legend"] = view.showFftLegend;
    node["show_hover_readout"] = view.showHoverReadout;
    node["show_cursors"] = view.showCursors;
    node["show_measurement_overlay"] = view.showMeasurementOverlay;
    node["phosphor_glow_enabled"] = view.phosphorGlowEnabled;
    node["cursor_interval_locked"] = view.cursorIntervalLocked;
    node["measurement_channel_index"] = view.measurementChannelIndex;
    node["sample_frequency_hz"] = view.sampleFrequencyHz;
    node["sample_frequency_input"] = view.sampleFrequencyInput;
    node["time_axis_source"] = axisSourceName(view.timeAxisSource);
    node["cursor_snap_mode"] = snapModeName(view.cursorSnapMode);
    node["cursor_snap_scope"] = snapScopeName(view.cursorSnapScope);
    node["cursor_extreme_snap_policy"] = extremeSnapPolicyName(view.cursorExtremeSnapPolicy);
    node["locked_cursor_interval"] = view.lockedCursorInterval;
    node["measurement"] = encodeMeasurementSelection(view.measurement);
    node["measurement_reference_mode"] = measurementReferenceModeName(view.referenceMode);
    node["reference_channel_index"] = view.referenceChannelIndex;
    node["manual_reference_value"] = view.manualReferenceValue;
    YAML::Node fftNode;
    fftNode["enabled"] = view.fft.enabled;
    fftNode["point_count"] = fftPointCountStateName(view.fft.pointCount);
    fftNode["window"] = fftWindowStateName(view.fft.window);
    fftNode["magnitude_mode"] = fftMagnitudeModeStateName(view.fft.magnitudeMode);
    fftNode["fundamental_mode"] = fftFundamentalModeStateName(view.fft.fundamentalMode);
    fftNode["manual_fundamental_hz"] = view.fft.manualFundamentalHz;
    fftNode["manual_point_count"] = view.fft.manualPointCount;
    fftNode["auto_max_point_count"] = view.fft.autoMaxPointCount;
    fftNode["source_window_valid"] = view.fftSourceWindowValid;
    fftNode["source_min_time"] = view.fftSourceMinTime;
    fftNode["source_max_time"] = view.fftSourceMaxTime;
    fftNode["frequency_min"] = view.fftFrequencyMin;
    fftNode["frequency_max"] = view.fftFrequencyMax;
    fftNode["magnitude_min"] = view.fftMagnitudeMin;
    fftNode["magnitude_max"] = view.fftMagnitudeMax;
    fftNode["phase_min"] = view.fftPhaseMin;
    fftNode["phase_max"] = view.fftPhaseMax;
    YAML::Node fftChannelsNode;
    for (std::size_t channelIndex = 0; channelIndex < wave.fftChannelEnabled.size(); ++channelIndex) {
        if (wave.fftChannelEnabled[channelIndex] == 0) {
            continue;
        }
        fftChannelsNode.push_back(channelIndex);
    }
    fftNode["channel_enabled"] = fftChannelsNode;
    node["fft"] = fftNode;
    node["tools_collapsed"] = wave.toolsCollapsed;
    node["overview_collapsed"] = wave.overviewCollapsed;
    node["legend_collapsed"] = wave.legendCollapsed;
    node["tools_expanded_width"] = wave.toolsExpandedWidth;
    node["overview_panel_height"] = wave.overviewPanelHeight;
    node["overview_collapsed_height"] = wave.overviewCollapsedHeight;
    YAML::Node hiddenChannelsNode;
    for (const auto& label : wave.hiddenChannelLabels) {
        if (!label.empty()) {
            hiddenChannelsNode.push_back(label);
        }
    }
    node["hidden_channel_labels"] = hiddenChannelsNode;

    YAML::Node cursorsNode;
    for (const auto& cursor : view.cursors) {
        YAML::Node cursorNode;
        cursorNode["enabled"] = cursor.enabled;
        cursorNode["pinned"] = cursor.pinned;
        cursorNode["channel_index"] = cursor.channelIndex;
        cursorNode["time"] = cursor.time;
        cursorNode["value"] = cursor.value;
        cursorsNode.push_back(cursorNode);
    }
    node["cursors"] = cursorsNode;

    YAML::Node overridesNode;
    for (std::size_t channelIndex = 0; channelIndex < wave.channelOverrides.size(); ++channelIndex) {
        const auto& overrideState = wave.channelOverrides[channelIndex];
        if (!overrideState.labelOverridden && !overrideState.ratioOverridden && !overrideState.scaleOverridden &&
            !overrideState.offsetOverridden) {
            continue;
        }
        YAML::Node entry;
        entry["channel_index"] = channelIndex;
        entry["label_overridden"] = overrideState.labelOverridden;
        entry["ratio_overridden"] = overrideState.ratioOverridden;
        entry["scale_overridden"] = overrideState.scaleOverridden;
        entry["offset_overridden"] = overrideState.offsetOverridden;
        entry["label"] = overrideState.label;
        entry["ratio"] = overrideState.ratio;
        entry["scale"] = overrideState.scale;
        entry["offset"] = overrideState.offset;
        overridesNode.push_back(entry);
    }
    node["channel_overrides"] = overridesNode;
    return node;
}

void decodeWaveProtocolState(const YAML::Node& node, plot::WaveDockState& wave)
{
    if (!node) {
        wave.hiddenChannelLabels.clear();
        wave.legendVisibilityRestorePending = true;
        return;
    }

    auto& view = wave.view;
    view.autoFollowLatest = node["auto_follow_latest"].as<bool>(view.autoFollowLatest);
    view.pauseAutoFollowOnInteraction =
        node["pause_auto_follow_on_interaction"].as<bool>(view.pauseAutoFollowOnInteraction);
    view.lockVerticalRange = node["lock_vertical_range"].as<bool>(view.lockVerticalRange);
    view.showPointsWhenSparse = node["show_points_when_sparse"].as<bool>(view.showPointsWhenSparse);
    view.showAxisLabels = node["show_axis_labels"].as<bool>(view.showAxisLabels);
    view.showChannelLegend = node["show_channel_legend"].as<bool>(view.showChannelLegend);
    view.showFftLegend = node["show_fft_legend"].as<bool>(view.showFftLegend);
    view.showHoverReadout = node["show_hover_readout"].as<bool>(view.showHoverReadout);
    view.showCursors = node["show_cursors"].as<bool>(view.showCursors);
    view.showMeasurementOverlay = node["show_measurement_overlay"].as<bool>(view.showMeasurementOverlay);
    view.phosphorGlowEnabled = node["phosphor_glow_enabled"].as<bool>(view.phosphorGlowEnabled);
    view.cursorIntervalLocked = node["cursor_interval_locked"].as<bool>(view.cursorIntervalLocked);
    view.measurementChannelIndex = node["measurement_channel_index"].as<std::size_t>(view.measurementChannelIndex);
    view.sampleFrequencyHz = node["sample_frequency_hz"].as<double>(view.sampleFrequencyHz);
    view.sampleFrequencyInput = node["sample_frequency_input"].as<std::string>(view.sampleFrequencyInput);
    view.timeAxisSource =
        parseAxisSource(node["time_axis_source"].as<std::string>(axisSourceName(view.timeAxisSource)));
    view.cursorSnapMode = parseSnapMode(node["cursor_snap_mode"].as<std::string>(snapModeName(view.cursorSnapMode)));
    view.cursorSnapScope =
        parseSnapScope(node["cursor_snap_scope"].as<std::string>(snapScopeName(view.cursorSnapScope)));
    view.cursorExtremeSnapPolicy = parseExtremeSnapPolicy(
        node["cursor_extreme_snap_policy"].as<std::string>(extremeSnapPolicyName(view.cursorExtremeSnapPolicy)));
    view.lockedCursorInterval = node["locked_cursor_interval"].as<double>(view.lockedCursorInterval);
    decodeMeasurementSelection(node["measurement"], view.measurement);
    view.referenceMode = parseMeasurementReferenceMode(
        node["measurement_reference_mode"].as<std::string>(measurementReferenceModeName(view.referenceMode)));
    view.referenceChannelIndex = node["reference_channel_index"].as<std::size_t>(view.referenceChannelIndex);
    view.manualReferenceValue = node["manual_reference_value"].as<double>(view.manualReferenceValue);
    const auto fftNode = node["fft"];
    if (fftNode && fftNode.IsMap()) {
        view.fft.enabled = fftNode["enabled"].as<bool>(view.fft.enabled);
        view.fft.pointCount =
            parseFftPointCount(fftNode["point_count"].as<std::string>(fftPointCountStateName(view.fft.pointCount)));
        view.fft.window = parseFftWindow(fftNode["window"].as<std::string>(fftWindowStateName(view.fft.window)));
        view.fft.magnitudeMode = parseFftMagnitudeMode(
            fftNode["magnitude_mode"].as<std::string>(fftMagnitudeModeStateName(view.fft.magnitudeMode)));
        view.fft.fundamentalMode = parseFftFundamentalMode(
            fftNode["fundamental_mode"].as<std::string>(fftFundamentalModeStateName(view.fft.fundamentalMode)));
        view.fft.manualFundamentalHz = fftNode["manual_fundamental_hz"].as<double>(view.fft.manualFundamentalHz);
        view.fft.manualPointCount = fftNode["manual_point_count"].as<std::size_t>(view.fft.manualPointCount);
        view.fft.autoMaxPointCount = fftNode["auto_max_point_count"].as<std::size_t>(view.fft.autoMaxPointCount);
        view.fftSourceWindowValid = fftNode["source_window_valid"].as<bool>(view.fftSourceWindowValid);
        view.fftSourceMinTime = fftNode["source_min_time"].as<double>(view.fftSourceMinTime);
        view.fftSourceMaxTime = fftNode["source_max_time"].as<double>(view.fftSourceMaxTime);
        view.fftFrequencyMin = fftNode["frequency_min"].as<double>(view.fftFrequencyMin);
        view.fftFrequencyMax = fftNode["frequency_max"].as<double>(view.fftFrequencyMax);
        view.fftMagnitudeMin = fftNode["magnitude_min"].as<double>(view.fftMagnitudeMin);
        view.fftMagnitudeMax = fftNode["magnitude_max"].as<double>(view.fftMagnitudeMax);
        view.fftPhaseMin = fftNode["phase_min"].as<double>(view.fftPhaseMin);
        view.fftPhaseMax = fftNode["phase_max"].as<double>(view.fftPhaseMax);
        view.fftViewportInitialized = view.fftFrequencyMax > view.fftFrequencyMin &&
                                      view.fftMagnitudeMax > view.fftMagnitudeMin &&
                                      view.fftPhaseMax > view.fftPhaseMin;
        wave.fftChannelEnabled.clear();
        const auto channelsNode = fftNode["channel_enabled"];
        if (channelsNode && channelsNode.IsSequence()) {
            for (const auto& entry : channelsNode) {
                const auto channelIndex = entry.as<std::size_t>(0);
                if (channelIndex >= wave.fftChannelEnabled.size()) {
                    wave.fftChannelEnabled.resize(channelIndex + 1, 0);
                }
                wave.fftChannelEnabled[channelIndex] = 1;
            }
        }
        wave.cachedFftKeyValid = false;
    }
    wave.toolsCollapsed = node["tools_collapsed"].as<bool>(wave.toolsCollapsed);
    wave.overviewCollapsed = node["overview_collapsed"].as<bool>(wave.overviewCollapsed);
    wave.legendCollapsed = node["legend_collapsed"].as<bool>(wave.legendCollapsed);
    wave.toolsExpandedWidth = node["tools_expanded_width"].as<float>(wave.toolsExpandedWidth);
    wave.overviewPanelHeight = node["overview_panel_height"].as<float>(wave.overviewPanelHeight);
    wave.overviewCollapsedHeight = node["overview_collapsed_height"].as<float>(wave.overviewCollapsedHeight);
    wave.hiddenChannelLabels.clear();
    const auto hiddenChannelsNode = node["hidden_channel_labels"];
    if (hiddenChannelsNode && hiddenChannelsNode.IsSequence()) {
        for (const auto& entry : hiddenChannelsNode) {
            const auto label = entry.as<std::string>("");
            if (label.empty() || std::find(wave.hiddenChannelLabels.begin(), wave.hiddenChannelLabels.end(), label) !=
                                     wave.hiddenChannelLabels.end()) {
                continue;
            }
            wave.hiddenChannelLabels.push_back(label);
        }
    }
    wave.legendVisibilityRestorePending = true;

    const auto cursorsNode = node["cursors"];
    for (std::size_t index = 0; index < view.cursors.size(); ++index) {
        if (!cursorsNode || !cursorsNode[index]) {
            continue;
        }
        auto& cursor = view.cursors[index];
        const auto cursorNode = cursorsNode[index];
        cursor.enabled = cursorNode["enabled"].as<bool>(cursor.enabled);
        cursor.pinned = cursorNode["pinned"].as<bool>(cursor.pinned);
        cursor.channelIndex = cursorNode["channel_index"].as<std::size_t>(cursor.channelIndex);
        cursor.time = cursorNode["time"].as<double>(cursor.time);
        cursor.value = cursorNode["value"].as<double>(cursor.value);
    }

    wave.channelOverrides.clear();
    const auto overridesNode = node["channel_overrides"];
    if (overridesNode && overridesNode.IsSequence()) {
        for (const auto& entry : overridesNode) {
            const auto channelIndex = entry["channel_index"].as<std::size_t>(0);
            if (channelIndex >= wave.channelOverrides.size()) {
                wave.channelOverrides.resize(channelIndex + 1);
            }
            auto& overrideState = wave.channelOverrides[channelIndex];
            overrideState.labelOverridden = entry["label_overridden"].as<bool>(overrideState.labelOverridden);
            overrideState.ratioOverridden = entry["ratio_overridden"].as<bool>(overrideState.ratioOverridden);
            overrideState.scaleOverridden = entry["scale_overridden"].as<bool>(overrideState.scaleOverridden);
            overrideState.offsetOverridden = entry["offset_overridden"].as<bool>(overrideState.offsetOverridden);
            overrideState.label = entry["label"].as<std::string>(overrideState.label);
            overrideState.ratio = entry["ratio"].as<double>(overrideState.ratio);
            overrideState.scale = entry["scale"].as<double>(overrideState.scale);
            overrideState.offset = entry["offset"].as<double>(overrideState.offset);
        }
    }

    applyChannelOverrides(wave);
}

void storeWaveProtocolState(YAML::Node& root, std::string_view protocolKey, const plot::WaveDockState& wave)
{
    root["protocols"][std::string(protocolKey)]["wave"] = encodeWaveProtocolState(wave);
}

void restoreWaveProtocolState(const YAML::Node& root, std::string_view protocolKey, plot::WaveDockState& wave)
{
    decodeWaveProtocolState(root["protocols"][std::string(protocolKey)]["wave"], wave);
}

YAML::Node encodeElfStaticAddressState(std::string_view path)
{
    YAML::Node node;
    node["path"] = std::string(path);
    return node;
}

std::string decodeElfStaticAddressPath(const YAML::Node& node)
{
    if (!node || !node.IsMap()) {
        return {};
    }
    return node["path"].as<std::string>("");
}

void storeElfStaticAddressPath(YAML::Node& root, std::string_view protocolKey, std::string_view path)
{
    auto protocolNode = root["protocols"][std::string(protocolKey)];
    if (path.empty()) {
        protocolNode.remove("elf_static_address");
        return;
    }
    protocolNode["elf_static_address"] = encodeElfStaticAddressState(path);
}

std::string restoreElfStaticAddressPath(const YAML::Node& root, std::string_view protocolKey)
{
    const auto protocolsNode = root["protocols"];
    if (!protocolsNode || !protocolsNode.IsMap()) {
        return {};
    }
    const auto protocolNode = protocolsNode[std::string(protocolKey)];
    if (!protocolNode || !protocolNode.IsMap()) {
        return {};
    }
    return decodeElfStaticAddressPath(protocolNode["elf_static_address"]);
}

YAML::Node encodeDockVisibilityState(const ProtocolDockVisibilityState& state)
{
    YAML::Node node;
    YAML::Node staticNode;
    staticNode["comm"] = state.showCommDock;
    staticNode["protocol"] = state.showProtocolDock;
    staticNode["transfer"] = state.showTransferDock;
    staticNode["log"] = state.showLogDock;
    staticNode["script"] = state.showScriptDock;
    staticNode["wave"] = state.showWaveDock;
    node["static"] = staticNode;

    YAML::Node luaNode;
    for (const auto& [stableId, visible] : state.luaDockVisibility) {
        luaNode[stableId] = visible;
    }
    node["lua"] = luaNode;
    return node;
}

void decodeDockVisibilityState(const YAML::Node& node, ProtocolDockVisibilityState& state)
{
    if (!node || !node.IsMap()) {
        return;
    }

    if (const auto staticNode = node["static"]) {
        state.showCommDock = staticNode["comm"].as<bool>(state.showCommDock);
        state.showProtocolDock = staticNode["protocol"].as<bool>(state.showProtocolDock);
        state.showTransferDock = staticNode["transfer"].as<bool>(state.showTransferDock);
        state.showLogDock = staticNode["log"].as<bool>(state.showLogDock);
        state.showScriptDock = staticNode["script"].as<bool>(state.showScriptDock);
        state.showWaveDock = staticNode["wave"].as<bool>(state.showWaveDock);
    }

    if (const auto luaNode = node["lua"]; luaNode && luaNode.IsMap()) {
        for (const auto& entry : luaNode) {
            const auto stableId = entry.first.as<std::string>("");
            if (stableId.empty()) {
                continue;
            }
            state.luaDockVisibility[stableId] = entry.second.as<bool>(true);
        }
    }
}

void storeDockVisibilityState(YAML::Node& root, std::string_view protocolKey, const ProtocolDockVisibilityState& state)
{
    root["protocols"][std::string(protocolKey)]["dock_visibility"] = encodeDockVisibilityState(state);
}

void restoreDockVisibilityState(const YAML::Node& root,
                                std::string_view protocolKey,
                                ProtocolDockVisibilityState& state)
{
    decodeDockVisibilityState(root["protocols"][std::string(protocolKey)]["dock_visibility"], state);
}

} // namespace protoscope::ui
