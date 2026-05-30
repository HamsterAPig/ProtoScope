#include "wave_component.hpp"

namespace protoscope::ui {

class WaveViewportController final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_viewport_controller"; }
    void prepare(WaveContext&) override {}
};

} // namespace protoscope::ui
