#pragma once

#include <cstddef>
#include <optional>
#include <vector>

struct ImGuiIO;

namespace protoscope::app { class Application; }
namespace protoscope::plot {
struct WaveDockState;
struct WaveViewState;
struct WaveSnapshot;
}

namespace protoscope::ui {

struct WaveFrameState {
    std::vector<std::size_t> visibleChannelIndices{};
    std::optional<std::size_t> hoveredChannelIndex{};
    bool requestFit{false};
    bool requestResetOffset{false};
    bool syncLatest{false};
};

struct WaveContext {
    app::Application& application;
    plot::WaveDockState& wave;
    plot::WaveViewState& view;
    const plot::WaveSnapshot& snapshot;
    ImGuiIO& io;
    WaveFrameState& frame;
};

} // namespace protoscope::ui
