#include "runtime_dock.hpp"

namespace protoscope::ui {

class CommDock final : public IRuntimeDock {
public:
    std::string_view id() const override { return "comm_dock"; }
    std::string_view title() const override { return "通讯配置"; }
    bool visible() const override { return visible_; }
    void setVisible(bool visible) override { visible_ = visible; }
    void drawDock(RuntimeUiContext&) override {}

private:
    bool visible_{true};
};

} // namespace protoscope::ui
