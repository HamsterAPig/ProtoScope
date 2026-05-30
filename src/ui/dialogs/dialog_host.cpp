#include "runtime_dialog.hpp"

namespace protoscope::ui {

class DialogHost final : public IRuntimeModule {
public:
    std::string_view id() const override { return "dialog_host"; }
};

} // namespace protoscope::ui
