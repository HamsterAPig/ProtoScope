#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class UiScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit UiScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "ui_api_module"; }

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        auto* host = &host_;
        sol::state_view lua = ctx.lua;
        sol::table uiApi = lua.create_table();
        uiApi.set_function("alert", [host, lua](const sol::object& opts) {
            std::string error;
            const auto dialog = host->protoDialog(DialogKind::Alert, opts, error);
            if (!dialog.has_value()) {
                return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, error));
            }
            return std::make_tuple(sol::make_object(lua, dialog->id), sol::make_object(lua, sol::lua_nil));
        });
        uiApi.set_function("confirm", [host, lua](const sol::object& opts) {
            std::string error;
            const auto dialog = host->protoDialog(DialogKind::Confirm, opts, error);
            if (!dialog.has_value()) {
                return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, error));
            }
            return std::make_tuple(sol::make_object(lua, dialog->id), sol::make_object(lua, sol::lua_nil));
        });
        proto["ui"] = uiApi;
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeUiApiModule(ScriptHost& host)
{
    return std::make_unique<UiScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
