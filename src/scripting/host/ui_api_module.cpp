#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {

void ScriptHost::registerUiApi(sol::table& proto) {
    sol::table uiApi = luaState().create_table();
    uiApi.set_function("alert", [this](const sol::object& opts) {
        std::string error;
        const auto dialog = protoDialog(DialogKind::Alert, opts, error);
        if (!dialog.has_value()) {
            return std::make_tuple(sol::make_object(luaState(), sol::lua_nil),
                                   sol::make_object(luaState(), error));
        }
        return std::make_tuple(sol::make_object(luaState(), dialog->id),
                               sol::make_object(luaState(), sol::lua_nil));
    });
    uiApi.set_function("confirm", [this](const sol::object& opts) {
        std::string error;
        const auto dialog = protoDialog(DialogKind::Confirm, opts, error);
        if (!dialog.has_value()) {
            return std::make_tuple(sol::make_object(luaState(), sol::lua_nil),
                                   sol::make_object(luaState(), error));
        }
        return std::make_tuple(sol::make_object(luaState(), dialog->id),
                               sol::make_object(luaState(), sol::lua_nil));
    });
    proto["ui"] = uiApi;
}

} // namespace protoscope::scripting
