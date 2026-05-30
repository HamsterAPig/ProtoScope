#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {

void ScriptHost::registerTxApi(sol::table& proto) {
    proto.set_function("send", [this](const sol::object& payload, const sol::object& opts) {
        std::string error;
        const auto request = protoSendLike(TxRequestKind::Send, payload, opts, error);
        if (!request.has_value()) {
            return std::make_tuple(sol::make_object(luaState(), sol::lua_nil),
                                   sol::make_object(luaState(), error));
        }
        return std::make_tuple(sol::make_object(luaState(), request->id),
                               sol::make_object(luaState(), sol::lua_nil));
    });
    proto.set_function("request", [this](const sol::object& payload, const sol::object& opts) {
        std::string error;
        const auto request = protoSendLike(TxRequestKind::Request, payload, opts, error);
        if (!request.has_value()) {
            return std::make_tuple(sol::make_object(luaState(), sol::lua_nil),
                                   sol::make_object(luaState(), error));
        }
        return std::make_tuple(sol::make_object(luaState(), request->id),
                               sol::make_object(luaState(), sol::lua_nil));
    });
    proto.set_function("request_done", [this](const sol::object& result) {
        std::string error;
        if (protoRequestDone(result, error)) {
            return std::make_tuple(sol::make_object(luaState(), true),
                                   sol::make_object(luaState(), sol::lua_nil));
        }
        return std::make_tuple(sol::make_object(luaState(), false),
                               sol::make_object(luaState(), error));
    });
}

} // namespace protoscope::scripting
