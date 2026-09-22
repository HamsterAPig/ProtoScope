#pragma once

#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {
bool parseControlBinding(ControlDescriptor& control, const sol::table& table, std::string& error);
void validateControlBindings(std::vector<DockDescriptor>& docks, const std::vector<data::Schema>& schemas);
} // namespace protoscope::scripting
