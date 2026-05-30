#include "protoscope/scripting/script_host.hpp"

#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

void ScriptHost::registerControlApi(sol::table& proto) {
    proto.set_function("get_control", [this](const std::string& id) {
        sol::state_view view = luaView();
        const auto* value = findControlValue(id);
        if (value == nullptr) {
            return sol::make_object(view, sol::lua_nil);
        }
        return script_host_lua::controlValueToLua(
            view, script_host_lua::findControlDescriptor(controlDescriptors(), id), *value);
    });
    proto.set_function("set_control", [this](const std::string& id, const sol::object& value) {
        const auto* descriptor = script_host_lua::findControlDescriptor(controlDescriptors(), id);
        if (descriptor == nullptr) {
            protoLog("warn", "proto.set_control 未找到控件: " + id);
            return;
        }
        std::string error;
        const auto converted = script_host_lua::controlValueFromLua(*descriptor, value, error);
        if (!converted.has_value()) {
            protoLog("warn", "proto.set_control 调用失败: " + error);
            return;
        }
        updateControlValue(id, *converted);
    });
}

} // namespace protoscope::scripting
