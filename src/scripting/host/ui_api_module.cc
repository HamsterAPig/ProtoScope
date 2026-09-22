#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"
#include "business_ui_session.hpp"

namespace protoscope::scripting {

class UiScriptHostApiModule final : public ScriptHostApiModuleBase {
public:
    explicit UiScriptHostApiModule(ScriptHost& host) : ScriptHostApiModuleBase(host, "ui_api_module") {}

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        auto* host = &host_;
        sol::state_view lua = ctx.lua;
        sol::table uiApi = lua.create_table();
        // 捕获当前待加载 runtime 的状态，失败重载不会污染旧运行时的菜单。
        auto* businessUi = &ctx.businessUi;
        uiApi.set_function("set_menu", [businessUi,state=lua.lua_state()](const sol::table& items) {
            sol::state_view view(state);
            std::string error;
            if (!setBusinessMenu(*businessUi,items,error)) return script_host_lua::luaNilError(view,error);
            return script_host_lua::luaValueOk(view,true);
        });
        uiApi.set_function("update_menu", [businessUi,state=lua.lua_state()](const std::string& id,const sol::table& patch) {
            sol::state_view view(state);
            std::string error;
            if (!updateBusinessMenu(*businessUi,id,patch,error)) return script_host_lua::luaNilError(view,error);
            return script_host_lua::luaValueOk(view,true);
        });
        uiApi.set_function("show_dock", [host,state=lua.lua_state()](const std::string& id,const sol::object& visible) {
            sol::state_view view(state);
            std::string error;
            if (visible.get_type()!=sol::type::boolean)
                return script_host_lua::luaNilError(view,"visible must be boolean");
            if (!host->showBusinessDock(id,visible.as<bool>(),error)) return script_host_lua::luaNilError(view,error);
            return script_host_lua::luaValueOk(view,true);
        });
        uiApi.set_function("update_control", [host, state = lua.lua_state()](const std::string& id, const sol::table& patch) {
            sol::state_view view(state);
            auto patches = view.create_table();
            patches[id] = patch;
            std::string error;
            if (!host->updateControlProperties(patches, error)) return script_host_lua::luaNilError(view, error);
            return script_host_lua::luaValueOk(view, true);
        });
        uiApi.set_function("update_controls", [host, state = lua.lua_state()](const sol::table& patches) {
            sol::state_view view(state);
            std::string error;
            if (!host->updateControlProperties(patches, error)) return script_host_lua::luaNilError(view, error);
            return script_host_lua::luaValueOk(view, true);
        });
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
