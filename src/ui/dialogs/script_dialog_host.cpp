#include "runtime_dialog.hpp"

namespace protoscope::ui {

class ScriptDialogHost final : public IRuntimeDialog {
public:
    std::string_view id() const override { return "script_dialog_host"; }
    bool active() const override { return active_; }
    void requestOpen(RuntimeUiContext&) override { active_ = true; }
    void drawDialog(RuntimeUiContext&) override {}

private:
    bool active_{false};
};

} // namespace protoscope::ui
