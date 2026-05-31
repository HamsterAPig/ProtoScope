#pragma once

#include "wave_component.hpp"

#include <string_view>

namespace protoscope::ui {

class WaveFftComponent final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_fft"; }
    void draw(WaveContext& context) override;
};

} // namespace protoscope::ui
