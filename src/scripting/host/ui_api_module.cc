#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

class UiScriptHostApiModule final : public ScriptHostApiModuleBase {
public:
    explicit UiScriptHostApiModule(ScriptHost& host) : ScriptHostApiModuleBase(host, "ui_api_module") {}

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        auto* host = &host_;
        sol::state_view lua = ctx.lua;
        sol::table uiApi = lua.create_table();
        uiApi.set_function("alert", [host, lua](const sol::object& opts) {
            std::string error;
            const auto dialog = host->protoDialog(DialogKind::Alert, opts, error);
            if (!dialog.has_value()) {
                return script_host_lua::luaNilError(lua, error);
            }
            return script_host_lua::luaValueOk(lua, dialog->id);
        });
        uiApi.set_function("confirm", [host, lua](const sol::object& opts) {
            std::string error;
            const auto dialog = host->protoDialog(DialogKind::Confirm, opts, error);
            if (!dialog.has_value()) {
                return script_host_lua::luaNilError(lua, error);
            }
            return script_host_lua::luaValueOk(lua, dialog->id);
        });
        proto["ui"] = uiApi;
    }
};

std::unique_ptr<IScriptHostApiModule> makeUiApiModule(ScriptHost& host)
{
    return makeScriptHostApiModule<UiScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
