#pragma once

#include "protoscope/plot/wave_state.hpp"

#include <string_view>

namespace YAML {
class Node;
}

namespace protoscope::ui {

YAML::Node encodeWaveProtocolState(const plot::WaveDockState& wave);
void decodeWaveProtocolState(const YAML::Node& node, plot::WaveDockState& wave);
void storeWaveProtocolState(YAML::Node& root, std::string_view protocolKey, const plot::WaveDockState& wave);
void restoreWaveProtocolState(const YAML::Node& root, std::string_view protocolKey, plot::WaveDockState& wave);

} // namespace protoscope::ui
