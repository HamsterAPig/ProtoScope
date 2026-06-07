#pragma once

#include "runtime_module.hpp"

namespace protoscope::ui {

class IRuntimeController : public IRuntimeModule {
public:
    virtual void beforeFrame(RuntimeUiContext&) {}

    virtual void afterFrame(RuntimeUiContext&) {}
};

} // namespace protoscope::ui
