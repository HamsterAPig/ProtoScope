#include "runtime_module.hpp"

namespace protoscope::ui {

class LuaViewMenu final : public IRuntimeMenu {
public:
    std::string_view id() const override { return "lua_view_menu"; }
    void drawMenu(RuntimeUiContext&) override {}
};

} // namespace protoscope::ui
