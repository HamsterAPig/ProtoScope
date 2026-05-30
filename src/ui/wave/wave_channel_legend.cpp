#include "wave_component.hpp"

namespace protoscope::ui {

class WaveChannelLegend final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_channel_legend"; }
    void draw(WaveContext&) override {}
};

} // namespace protoscope::ui
