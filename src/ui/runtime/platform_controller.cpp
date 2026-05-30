#include "runtime_controller.hpp"

namespace protoscope::ui {

class PlatformController final : public IRuntimeController {
public:
    std::string_view id() const override { return "platform_controller"; }
};

} // namespace protoscope::ui
