#pragma once

#include "protoscope/plot/wave_state.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace YAML {
class Node;
}

namespace protoscope::ui {

struct ProtocolDockVisibilityState {
    bool showCommDock{true};
    bool showProtocolDock{true};
    bool showTransferDock{true};
    bool showRequestTraceDock{true};
    bool showOfflineReplayDock{true};
    bool showLogDock{true};
    bool showScriptDock{true};
    bool showWaveDock{true};
    std::unordered_map<std::string, bool> luaDockVisibility;
};

YAML::Node encodeWaveProtocolState(const plot::WaveDockState& wave);
void decodeWaveProtocolState(const YAML::Node& node, plot::WaveDockState& wave);
void storeWaveProtocolState(YAML::Node& root, std::string_view protocolKey, const plot::WaveDockState& wave);
void restoreWaveProtocolState(const YAML::Node& root, std::string_view protocolKey, plot::WaveDockState& wave);
YAML::Node encodeElfStaticAddressState(std::string_view path);
std::string decodeElfStaticAddressPath(const YAML::Node& node);
void storeElfStaticAddressPath(YAML::Node& root, std::string_view protocolKey, std::string_view path);
std::string restoreElfStaticAddressPath(const YAML::Node& root, std::string_view protocolKey);
YAML::Node encodeDockVisibilityState(const ProtocolDockVisibilityState& state);
void decodeDockVisibilityState(const YAML::Node& node, ProtocolDockVisibilityState& state);
void storeDockVisibilityState(YAML::Node& root, std::string_view protocolKey, const ProtocolDockVisibilityState& state);
void restoreDockVisibilityState(const YAML::Node& root,
                                std::string_view protocolKey,
                                ProtocolDockVisibilityState& state);

} // namespace protoscope::ui
