#pragma once

#include <cstddef>
#include <optional>
#include <vector>

struct ImGuiIO;

namespace protoscope::app {
class Application;
}

namespace protoscope::plot {
struct ViewConfig;
struct WaveDockState;
struct WaveLayoutSizes;
struct WaveViewState;
struct WaveSnapshot;
} // namespace protoscope::plot

namespace protoscope::ui {

struct WaveOverlayFrame;
struct WaveFrameData;

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
    const plot::ViewConfig* config{nullptr};
    const plot::WaveLayoutSizes* layout{nullptr};
    WaveFrameData* renderFrame{nullptr};
    float availableWidth{0.0F};
    float availableHeight{0.0F};
    float contentWidth{0.0F};
    float toolsWidth{0.0F};
    bool fullscreenActive{false};
    bool* fullscreenToggleRequested{nullptr};
    WaveOverlayFrame* overlayFrame{nullptr};
};

} // namespace protoscope::ui
