#pragma once

#include <imgui.h>

#include <string>

namespace protoscope::app {
class Application;
}
namespace protoscope::plot {
struct WaveDockState;
}

namespace protoscope::ui {

void addItemHelp(const char* text);
bool drawToolbarActionButton(const char* label, const char* help, const ImVec2& size = ImVec2(0.0F, 0.0F));
bool drawToolbarToggleButton(const char* label,
                             bool active,
                             const char* help,
                             const ImVec2& size = ImVec2(0.0F, 0.0F));
bool drawToolbarCheckbox(const char* label, bool* value, const char* help);
void drawWaveToolbar(app::Application& application, plot::WaveDockState& wave);

class WaveDockRenderer {
public:
    explicit WaveDockRenderer(app::Application& application);

    void draw(bool& showWaveDock);

private:
    void syncWaveViewToLatest();
    static std::string formatMetric(double value, const char* baseUnit);

private:
    app::Application& application_;
};

} // namespace protoscope::ui
