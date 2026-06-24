#pragma once

#include <string>

#include <imgui.h>

namespace protoscope::app {
class Application;
}

namespace protoscope::plot {
struct ViewConfig;
struct WaveDisplayData;
struct WaveDockState;
enum class WaveToolsDrawer;
} // namespace protoscope::plot

namespace protoscope::ui {

struct WaveOverlayFrame;

void addItemHelp(const char* text);
bool drawToolbarActionButton(const char* label, const char* help, const ImVec2& size = ImVec2(0.0F, 0.0F));
bool drawToolbarToggleButton(const char* label, bool active, const char* help, const ImVec2& size = ImVec2(0.0F, 0.0F));
void drawWaveToolbar(app::Application& application,
                     plot::WaveDockState& wave,
                     const plot::ViewConfig& config,
                     const plot::WaveDisplayData& displayData,
                     bool fullscreenActive = false,
                     bool* fullscreenToggleRequested = nullptr);
void drawWaveToolsDrawer(app::Application& application,
                         plot::WaveDockState& wave,
                         const plot::ViewConfig& config,
                         const plot::WaveDisplayData& displayData,
                         plot::WaveToolsDrawer drawer,
                         bool fullscreenActive = false,
                         bool* fullscreenToggleRequested = nullptr);

class WaveDockRenderer {
public:
    explicit WaveDockRenderer(app::Application& application);

    void draw(bool& showWaveDock,
              bool fullscreenActive = false,
              bool* fullscreenToggleRequested = nullptr,
              bool shortcutFocusOverride = false);
    void drawOverlay(bool fullscreenActive, bool* fullscreenToggleRequested);

private:
    void drawContent(const ImVec2& available,
                     bool fullscreenActive,
                     bool* fullscreenToggleRequested,
                     bool shortcutFocusOverride = false,
                     WaveOverlayFrame* overlayFrame = nullptr);
    void syncWaveViewToLatest();
    void handleWaveShortcuts(bool dockFocused, bool fullscreenActive, bool* fullscreenToggleRequested);
    static std::string formatMetric(double value, const char* baseUnit);

private:
    app::Application& application_;
};

} // namespace protoscope::ui
