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
} // namespace protoscope::plot

namespace protoscope::ui {

void addItemHelp(const char* text);
bool drawToolbarActionButton(const char* label, const char* help, const ImVec2& size = ImVec2(0.0F, 0.0F));
bool drawToolbarToggleButton(const char* label, bool active, const char* help, const ImVec2& size = ImVec2(0.0F, 0.0F));
void drawWaveToolbar(app::Application& application,
                     plot::WaveDockState& wave,
                     const plot::ViewConfig& config,
                     const plot::WaveDisplayData& displayData);

class WaveDockRenderer {
public:
    explicit WaveDockRenderer(app::Application& application);

    void draw(bool& showWaveDock);

private:
    void syncWaveViewToLatest();
    void handleWaveShortcuts(bool dockFocused);
    static std::string formatMetric(double value, const char* baseUnit);

private:
    app::Application& application_;
};

} // namespace protoscope::ui
