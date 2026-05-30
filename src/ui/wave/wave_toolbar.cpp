#include "wave_component.hpp"

namespace protoscope::ui {

class WaveToolbar final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_toolbar"; }
    void draw(WaveContext&) override {}
};

} // namespace protoscope::ui
