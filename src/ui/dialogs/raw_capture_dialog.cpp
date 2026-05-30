#include "runtime_dialog.hpp"

namespace protoscope::ui {

class RawCaptureDialog final : public IRuntimeDialog {
public:
    std::string_view id() const override { return "raw_capture_dialog"; }
    bool active() const override { return active_; }
    void requestOpen(RuntimeUiContext&) override { active_ = true; }
    void drawDialog(RuntimeUiContext&) override {}

private:
    bool active_{false};
};

} // namespace protoscope::ui
