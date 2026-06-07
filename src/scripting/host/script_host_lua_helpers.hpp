#pragma once

#include "protoscope/scripting/script_host.hpp"

#include <optional>
#include <string>
#include <vector>

namespace protoscope::scripting {
namespace script_host_lua {

    std::string serializeLuaObject(const sol::object& object, int depth = 0);
    const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls,
                                                   const std::string& id);
    std::optional<ControlValue> controlValueFromLua(const ControlDescriptor& descriptor,
                                                    const sol::object& object,
                                                    std::string& error);
    sol::object controlValueToLua(sol::state_view lua, const ControlDescriptor* descriptor, const ControlValue& value);
    std::optional<std::vector<std::uint8_t>> bytesFromLuaObject(const sol::object& object, std::string& error);
    std::optional<PlotSetup> parsePlotSetup(const sol::object& object, std::string& error);
    std::optional<plot::WaveAppendRequest> parsePlotAppend(const sol::object& object, std::string& error);

} // namespace script_host_lua
} // namespace protoscope::scripting
