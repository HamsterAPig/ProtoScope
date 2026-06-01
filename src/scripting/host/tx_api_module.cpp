#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class TxScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit TxScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "tx_api_module"; }

    void registerApi(ScriptHostContextInternal&, sol::table& proto) override {
        auto* host = &host_;
        proto.set_function("send", [host](const sol::object& payload, const sol::object& opts) {
            std::string error;
            const auto request = host->protoSendLike(TxRequestKind::Send, payload, opts, error);
            if (!request.has_value()) {
                return std::make_tuple(sol::make_object(host->luaState(), sol::lua_nil),
                                       sol::make_object(host->luaState(), error));
            }
            return std::make_tuple(sol::make_object(host->luaState(), request->id),
                                   sol::make_object(host->luaState(), sol::lua_nil));
        });
        proto.set_function("request", [host](const sol::object& payload, const sol::object& opts) {
            std::string error;
            const auto request = host->protoSendLike(TxRequestKind::Request, payload, opts, error);
            if (!request.has_value()) {
                return std::make_tuple(sol::make_object(host->luaState(), sol::lua_nil),
                                       sol::make_object(host->luaState(), error));
            }
            return std::make_tuple(sol::make_object(host->luaState(), request->id),
                                   sol::make_object(host->luaState(), sol::lua_nil));
        });
        proto.set_function("request_done", [host](const sol::object& result) {
            std::string error;
            if (host->protoRequestDone(result, error)) {
                return std::make_tuple(sol::make_object(host->luaState(), true),
                                       sol::make_object(host->luaState(), sol::lua_nil));
            }
            return std::make_tuple(sol::make_object(host->luaState(), false),
                                   sol::make_object(host->luaState(), error));
        });
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeTxApiModule(ScriptHost& host) {
    return std::make_unique<TxScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
