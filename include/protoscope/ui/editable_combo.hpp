#pragma once

#include <functional>
#include <string>
#include <vector>

namespace protoscope::ui {

struct EditableComboResult {
    bool edited{false};
    bool selectedFromList{false};
    bool valid{true};
    std::string value;
};

EditableComboResult drawEditableCombo(const char* label,
                                      std::string& draft,
                                      const std::vector<std::string>& options,
                                      const std::function<bool(const std::string&)>& validator = {});

} // namespace protoscope::ui
