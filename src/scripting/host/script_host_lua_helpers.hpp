#pragma once

#include "protoscope/scripting/script_host.hpp"

#include <optional>
#include <string>
#include <tuple>
#include <utility>
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

    inline std::tuple<sol::object, sol::object> luaNilError(sol::state_view lua, const std::string& error)
    {
        return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, error));
    }

    template <typename T> std::tuple<sol::object, sol::object> luaValueOk(sol::state_view lua, T&& value)
    {
        return std::make_tuple(sol::make_object(lua, std::forward<T>(value)), sol::make_object(lua, sol::lua_nil));
    }

    template <typename T>
    std::tuple<sol::object, sol::object> luaOptionalValueResult(sol::state_view lua,
                                                                const std::optional<T>& value,
                                                                const std::string& error)
    {
        if (!value.has_value()) {
            return luaNilError(lua, error);
        }
        return luaValueOk(lua, *value);
    }

    inline std::tuple<sol::object, sol::object> luaOkResult(sol::state_view lua, bool ok, const std::string& error)
    {
        if (!ok) {
            return std::make_tuple(sol::make_object(lua, false), sol::make_object(lua, error));
        }
        return std::make_tuple(sol::make_object(lua, true), sol::make_object(lua, sol::lua_nil));
    }

} // namespace script_host_lua
} // namespace protoscope::scripting
