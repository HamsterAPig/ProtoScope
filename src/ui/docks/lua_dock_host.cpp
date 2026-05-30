#include "runtime_dock.hpp"

namespace protoscope::ui {

class LuaDockHost final : public IRuntimeDock {
public:
    std::string_view id() const override { return "lua_dock_host"; }
    std::string_view title() const override { return "Lua 动态 Dock"; }
    bool visible() const override { return visible_; }
    void setVisible(bool visible) override { visible_ = visible; }
    void drawDock(RuntimeUiContext&) override {}

private:
    bool visible_{true};
};

} // namespace protoscope::ui
