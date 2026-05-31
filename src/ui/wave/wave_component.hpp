#pragma once

#include "wave_context.hpp"

#include <string_view>

namespace protoscope::ui {

class IWaveComponent {
public:
    virtual ~IWaveComponent() = default;

    virtual std::string_view id() const = 0;
    virtual void prepare(WaveContext&) {}
    virtual void draw(WaveContext&) {}
    virtual void handleInput(WaveContext&) {}
    virtual void commit(WaveContext&) {}
};

class IWaveToolbarSection {
public:
    virtual ~IWaveToolbarSection() = default;

    virtual std::string_view id() const = 0;
    virtual void draw(app::Application& application, plot::WaveDockState& wave) = 0;
};

} // namespace protoscope::ui
