#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class TxScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit TxScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "tx_api_module"; }

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override {
        auto* host = &host_;
        sol::state_view lua = ctx.lua;
        proto.set_function("send", [host, lua](const sol::object& payload, const sol::object& opts) {
            std::string error;
            const auto request = host->protoSendLike(TxRequestKind::Send, payload, opts, error);
            if (!request.has_value()) {
                return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, error));
            }
            return std::make_tuple(sol::make_object(lua, request->id), sol::make_object(lua, sol::lua_nil));
        });
        proto.set_function("request", [host, lua](const sol::object& payload, const sol::object& opts) {
            std::string error;
            const auto request = host->protoSendLike(TxRequestKind::Request, payload, opts, error);
            if (!request.has_value()) {
                return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, error));
            }
            return std::make_tuple(sol::make_object(lua, request->id), sol::make_object(lua, sol::lua_nil));
        });
        proto.set_function("request_done", [host, lua](const sol::object& result) {
            std::string error;
            if (host->protoRequestDone(result, error)) {
                return std::make_tuple(sol::make_object(lua, true), sol::make_object(lua, sol::lua_nil));
            }
            return std::make_tuple(sol::make_object(lua, false), sol::make_object(lua, error));
        });
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeTxApiModule(ScriptHost& host) {
    return std::make_unique<TxScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
