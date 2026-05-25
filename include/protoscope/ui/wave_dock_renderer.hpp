#pragma once

#include "protoscope/app/application.hpp"

namespace protoscope::ui {

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
