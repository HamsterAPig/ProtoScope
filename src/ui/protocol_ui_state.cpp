#include "protoscope/ui/protocol_ui_state.hpp"

#include <yaml-cpp/yaml.h>

namespace protoscope::ui {
namespace {

std::string axisSourceName(plot::WaveTimeAxisSource source) {
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

plot::WaveTimeAxisSource parseAxisSource(const std::string& value) {
    if (value == "sample_frequency") {
        return plot::WaveTimeAxisSource::SampleFrequency;
    }
    if (value == "script_time") {
        return plot::WaveTimeAxisSource::ScriptTime;
    }
    return plot::WaveTimeAxisSource::SampleIndex;
}

std::string snapModeName(plot::WaveCursorSnapMode mode) {
    switch (mode) {
    case plot::WaveCursorSnapMode::SmartSnap:
        return "smart";
    case plot::WaveCursorSnapMode::ModifierSnap:
        return "modifier";
    }
    return "smart";
}

plot::WaveCursorSnapMode parseSnapMode(const std::string& value) {
    return value == "modifier" ? plot::WaveCursorSnapMode::ModifierSnap : plot::WaveCursorSnapMode::SmartSnap;
}

std::string snapScopeName(plot::WaveCursorSnapScope scope) {
    switch (scope) {
    case plot::WaveCursorSnapScope::AllChannels:
        return "all_channels";
    case plot::WaveCursorSnapScope::ActiveChannel:
        return "active_channel";
    }
    return "all_channels";
}

plot::WaveCursorSnapScope parseSnapScope(const std::string& value) {
    return value == "active_channel" ? plot::WaveCursorSnapScope::ActiveChannel : plot::WaveCursorSnapScope::AllChannels;
}

void applyChannelOverrides(plot::WaveDockState& wave) {
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

YAML::Node encodeWaveProtocolState(const plot::WaveDockState& wave) {
    YAML::Node node;
    const auto& view = wave.view;
    node["auto_follow_latest"] = view.autoFollowLatest;
    node["pause_auto_follow_on_interaction"] = view.pauseAutoFollowOnInteraction;
    node["lock_vertical_range"] = view.lockVerticalRange;
    node["show_points_when_sparse"] = view.showPointsWhenSparse;
    node["show_axis_labels"] = view.showAxisLabels;
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
    node["locked_cursor_interval"] = view.lockedCursorInterval;
    node["tools_collapsed"] = wave.toolsCollapsed;
    node["overview_collapsed"] = wave.overviewCollapsed;
    node["tools_expanded_width"] = wave.toolsExpandedWidth;
    node["overview_panel_height"] = wave.overviewPanelHeight;
    node["overview_collapsed_height"] = wave.overviewCollapsedHeight;

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
        if (!overrideState.labelOverridden && !overrideState.scaleOverridden && !overrideState.offsetOverridden) {
            continue;
        }
        YAML::Node entry;
        entry["channel_index"] = channelIndex;
        entry["label_overridden"] = overrideState.labelOverridden;
        entry["scale_overridden"] = overrideState.scaleOverridden;
        entry["offset_overridden"] = overrideState.offsetOverridden;
        entry["label"] = overrideState.label;
        entry["scale"] = overrideState.scale;
        entry["offset"] = overrideState.offset;
        overridesNode.push_back(entry);
    }
    node["channel_overrides"] = overridesNode;
    return node;
}

void decodeWaveProtocolState(const YAML::Node& node, plot::WaveDockState& wave) {
    if (!node) {
        return;
    }

    auto& view = wave.view;
    view.autoFollowLatest = node["auto_follow_latest"].as<bool>(view.autoFollowLatest);
    view.pauseAutoFollowOnInteraction = node["pause_auto_follow_on_interaction"].as<bool>(view.pauseAutoFollowOnInteraction);
    view.lockVerticalRange = node["lock_vertical_range"].as<bool>(view.lockVerticalRange);
    view.showPointsWhenSparse = node["show_points_when_sparse"].as<bool>(view.showPointsWhenSparse);
    view.showAxisLabels = node["show_axis_labels"].as<bool>(view.showAxisLabels);
    view.showHoverReadout = node["show_hover_readout"].as<bool>(view.showHoverReadout);
    view.showCursors = node["show_cursors"].as<bool>(view.showCursors);
    view.showMeasurementOverlay = node["show_measurement_overlay"].as<bool>(view.showMeasurementOverlay);
    view.phosphorGlowEnabled = node["phosphor_glow_enabled"].as<bool>(view.phosphorGlowEnabled);
    view.cursorIntervalLocked = node["cursor_interval_locked"].as<bool>(view.cursorIntervalLocked);
    view.measurementChannelIndex = node["measurement_channel_index"].as<std::size_t>(view.measurementChannelIndex);
    view.sampleFrequencyHz = node["sample_frequency_hz"].as<double>(view.sampleFrequencyHz);
    view.sampleFrequencyInput = node["sample_frequency_input"].as<std::string>(view.sampleFrequencyInput);
    view.timeAxisSource = parseAxisSource(node["time_axis_source"].as<std::string>(axisSourceName(view.timeAxisSource)));
    view.cursorSnapMode = parseSnapMode(node["cursor_snap_mode"].as<std::string>(snapModeName(view.cursorSnapMode)));
    view.cursorSnapScope = parseSnapScope(node["cursor_snap_scope"].as<std::string>(snapScopeName(view.cursorSnapScope)));
    view.lockedCursorInterval = node["locked_cursor_interval"].as<double>(view.lockedCursorInterval);
    wave.toolsCollapsed = node["tools_collapsed"].as<bool>(wave.toolsCollapsed);
    wave.overviewCollapsed = node["overview_collapsed"].as<bool>(wave.overviewCollapsed);
    wave.toolsExpandedWidth = node["tools_expanded_width"].as<float>(wave.toolsExpandedWidth);
    wave.overviewPanelHeight = node["overview_panel_height"].as<float>(wave.overviewPanelHeight);
    wave.overviewCollapsedHeight = node["overview_collapsed_height"].as<float>(wave.overviewCollapsedHeight);

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
            overrideState.scaleOverridden = entry["scale_overridden"].as<bool>(overrideState.scaleOverridden);
            overrideState.offsetOverridden = entry["offset_overridden"].as<bool>(overrideState.offsetOverridden);
            overrideState.label = entry["label"].as<std::string>(overrideState.label);
            overrideState.scale = entry["scale"].as<double>(overrideState.scale);
            overrideState.offset = entry["offset"].as<double>(overrideState.offset);
        }
    }

    applyChannelOverrides(wave);
}

void storeWaveProtocolState(YAML::Node& root, std::string_view protocolKey, const plot::WaveDockState& wave) {
    root["protocols"][std::string(protocolKey)]["wave"] = encodeWaveProtocolState(wave);
}

void restoreWaveProtocolState(const YAML::Node& root, std::string_view protocolKey, plot::WaveDockState& wave) {
    decodeWaveProtocolState(root["protocols"][std::string(protocolKey)]["wave"], wave);
}

} // namespace protoscope::ui
