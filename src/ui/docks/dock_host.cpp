#include "runtime_dock.hpp"

namespace protoscope::ui {

class DockHost final : public IRuntimeModule {
public:
    std::string_view id() const override { return "dock_host"; }
};

} // namespace protoscope::ui
