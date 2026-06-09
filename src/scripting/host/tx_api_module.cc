#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

class TxScriptHostApiModule final : public ScriptHostApiModuleBase {
public:
    explicit TxScriptHostApiModule(ScriptHost& host) : ScriptHostApiModuleBase(host, "tx_api_module") {}

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        auto* host = &host_;
        sol::state_view lua = ctx.lua;
        proto.set_function("send", [host, lua](const sol::object& payload, const sol::object& opts) {
            std::string error;
            const auto request = host->protoSendLike(TxRequestKind::Send, payload, opts, error);
            if (!request.has_value()) {
                return script_host_lua::luaNilError(lua, error);
            }
            return script_host_lua::luaValueOk(lua, request->id);
        });
        proto.set_function("request", [host, lua](const sol::object& payload, const sol::object& opts) {
            std::string error;
            const auto request = host->protoSendLike(TxRequestKind::Request, payload, opts, error);
            if (!request.has_value()) {
                return script_host_lua::luaNilError(lua, error);
            }
            return script_host_lua::luaValueOk(lua, request->id);
        });
        proto.set_function("request_guarded", [host, lua](const sol::object& payload, const sol::object& opts) {
            std::string error;
            const auto request = host->protoSendLike(TxRequestKind::Request, payload, opts, error, true);
            if (!request.has_value()) {
                return script_host_lua::luaNilError(lua, error);
            }
            return script_host_lua::luaValueOk(lua, request->id);
        });
        proto.set_function("reset_request_guard", [host]() {
            host->protoResetRequestGuard();
            return true;
        });
        proto.set_function("request_done", [host, lua](const sol::object& result) {
            std::string error;
            return script_host_lua::luaOkResult(lua, host->protoRequestDone(result, error), error);
        });
    }
};

std::unique_ptr<IScriptHostApiModule> makeTxApiModule(ScriptHost& host)
{
    return makeScriptHostApiModule<TxScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
