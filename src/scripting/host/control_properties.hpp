#pragma once

#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {
bool applyControlProperties(ControlDescriptor& descriptor, const sol::table& patch,
                            bool strict, std::string& error);
bool validateControlValue(const ControlDescriptor& descriptor, const ControlValue& value);
} // namespace protoscope::scripting
