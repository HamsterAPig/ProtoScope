#include "runtime_module.hpp"

namespace protoscope::ui {

class MainMenu final : public IRuntimeMenu {
public:
    std::string_view id() const override { return "main_menu"; }
    void drawMenu(RuntimeUiContext&) override {}
};

} // namespace protoscope::ui
