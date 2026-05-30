#pragma once

#include "../runtime/runtime_module.hpp"

#include <string_view>

namespace protoscope::ui {

class IRuntimeDock : public IRuntimeModule {
public:
    virtual std::string_view title() const = 0;
    virtual bool visible() const = 0;
    virtual void setVisible(bool visible) = 0;
    virtual void drawDock(RuntimeUiContext& ctx) = 0;
};

} // namespace protoscope::ui
