#pragma once

#include <cstddef>
#include <limits>
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

struct WaveOscilloscopeToggleRequest {
    bool currentRunning{false};
    bool targetRunning{false};
};

struct WaveFrameState {
    std::vector<std::size_t> visibleChannelIndices{};
    std::optional<std::size_t> hoveredChannelIndex{};
    std::optional<WaveOscilloscopeToggleRequest> oscilloscopeToggleRequest{};
    bool resetHistoryRequested{false};
    bool requestFit{false};
    bool requestResetOffset{false};
    bool syncLatest{false};
};

inline void deferOscilloscopeToggle(WaveFrameState& frame, bool currentRunning, bool targetRunning)
{
    frame.oscilloscopeToggleRequest = WaveOscilloscopeToggleRequest{currentRunning, targetRunning};
}

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
    float measurementSafeRightX{std::numeric_limits<float>::infinity()};
    bool fullscreenActive{false};
    bool* fullscreenToggleRequested{nullptr};
    WaveOverlayFrame* overlayFrame{nullptr};
};

} // namespace protoscope::ui
