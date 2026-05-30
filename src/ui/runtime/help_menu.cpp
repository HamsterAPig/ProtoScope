#include "runtime_module.hpp"

namespace protoscope::ui {

class HelpMenu final : public IRuntimeMenu {
public:
    std::string_view id() const override { return "help_menu"; }
    void drawMenu(RuntimeUiContext&) override {}
};

} // namespace protoscope::ui
