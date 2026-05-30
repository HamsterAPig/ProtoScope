#include "runtime_controller.hpp"

namespace protoscope::ui {

class WorkspaceController final : public IRuntimeController {
public:
    std::string_view id() const override { return "workspace_controller"; }
};

} // namespace protoscope::ui
