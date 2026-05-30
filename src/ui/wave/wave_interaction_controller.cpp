#include "wave_component.hpp"

namespace protoscope::ui {

class WaveInteractionController final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_interaction_controller"; }
    void handleInput(WaveContext&) override {}
};

} // namespace protoscope::ui
