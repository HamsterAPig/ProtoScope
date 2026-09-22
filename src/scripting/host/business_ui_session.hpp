#pragma once

#include "protoscope/scripting/business_ui.hpp"
#include <sol/sol.hpp>

namespace protoscope::scripting {
bool setBusinessMenu(BusinessUiSnapshot& state, const sol::table& items, std::string& error);
bool updateBusinessMenu(BusinessUiSnapshot& state, const std::string& id, const sol::table& patch, std::string& error);
BusinessMenuItem* findBusinessMenu(std::vector<BusinessMenuItem>& items, const std::string& id, bool requireEnabled);
} // namespace protoscope::scripting
