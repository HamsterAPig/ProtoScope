#pragma once

#include "protoscope/ui/ui_host_context.hpp"

#include <string_view>

namespace protoscope::ui {

class IRuntimeModule {
public:
    virtual ~IRuntimeModule() = default;

    virtual std::string_view id() const = 0;
    virtual void initialize(RuntimeUiContext&) {}
    virtual void sync(RuntimeUiContext&) {}
    virtual void shutdown(RuntimeUiContext&) {}
};

class IRuntimeMenu : public IRuntimeModule {
public:
    virtual void drawMenu(RuntimeUiContext& ctx) = 0;
};

} // namespace protoscope::ui
