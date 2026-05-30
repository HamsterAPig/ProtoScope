#include "wave_component.hpp"

namespace protoscope::ui {

class WaveMeasurementOverlay final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_measurement_overlay"; }
    void draw(WaveContext&) override {}
};

} // namespace protoscope::ui
