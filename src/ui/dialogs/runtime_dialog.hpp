#pragma once

#include "../runtime/runtime_module.hpp"

namespace protoscope::ui {

class IRuntimeDialog : public IRuntimeModule {
public:
    virtual bool active() const = 0;
    virtual void requestOpen(RuntimeUiContext& ctx) = 0;
    virtual void drawDialog(RuntimeUiContext& ctx) = 0;
};

} // namespace protoscope::ui
