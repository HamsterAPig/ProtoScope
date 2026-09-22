#pragma once

#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {

using ParseTabChildren = std::function<std::optional<std::vector<LayoutNodeDescriptor>>(
    const sol::table&, const std::string&, std::string&)>;

std::optional<LayoutNodeDescriptor> parseTabsLayout(
    const sol::table& table, const std::string& path, const ParseTabChildren& parseChildren, std::string& error);
bool registerTabSelections(DockDescriptor& dock, std::string& error);

} // namespace protoscope::scripting
