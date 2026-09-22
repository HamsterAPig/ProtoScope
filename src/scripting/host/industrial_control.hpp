#pragma once
#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {
bool applyIndustrialControlConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error);
std::optional<std::string> formatReadout(const sol::object& value, int precision, std::string& error);
} // namespace protoscope::scripting
