#include "runtime_dock.hpp"

namespace protoscope::ui {

class ProtocolDock final : public IRuntimeDock {
public:
    std::string_view id() const override { return "protocol_dock"; }
    std::string_view title() const override { return "协议脚本 / 动态控件"; }
    bool visible() const override { return visible_; }
    void setVisible(bool visible) override { visible_ = visible; }
    void drawDock(RuntimeUiContext&) override {}

private:
    bool visible_{true};
};

} // namespace protoscope::ui
