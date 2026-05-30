#include "runtime_controller.hpp"

namespace protoscope::ui {

class ConfigController final : public IRuntimeController {
public:
    std::string_view id() const override { return "config_controller"; }
};

} // namespace protoscope::ui
