#include "wave_component.hpp"

namespace protoscope::ui {

class WaveChannelRenderer final : public IWaveComponent {
public:
    std::string_view id() const override { return "wave_channel_renderer"; }
    void draw(WaveContext&) override {}
};

} // namespace protoscope::ui
