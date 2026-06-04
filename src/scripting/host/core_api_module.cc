#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

class CoreScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit CoreScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "core_api_module"; }

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override {
        auto* host = &host_;
        sol::state_view lua = ctx.lua;
        proto.set_function("log", [host](const std::string& level, const std::string& message) {
            host->protoLog(level, message);
        });
        proto.set_function("emit", [host](const std::string& name, const sol::object& payload) {
            host->protoEmit(name, script_host_lua::serializeLuaObject(payload));
        });
        proto.set_function("set_timer", [host](const std::string& name, std::uint64_t delayMs) {
            host->protoSetTimer(name, delayMs);
        });
        proto.set_function("cancel_timer", [host](const std::string& name) {
            host->protoCancelTimer(name);
        });
        auto streamApi = lua.create_table();
        streamApi.set_function("set_profile", [host, lua](const sol::object& profile) {
            std::string error;
            if (host->setStreamRuntimeProfile(profile, error)) {
                return std::make_tuple(sol::make_object(lua, true), sol::make_object(lua, sol::lua_nil));
            }
            return std::make_tuple(sol::make_object(lua, false), sol::make_object(lua, error));
        });
        streamApi.set_function("clear_profile", [host, lua](const sol::object& frameName) {
            std::string error;
            if (host->clearStreamRuntimeProfile(frameName, error)) {
                return std::make_tuple(sol::make_object(lua, true), sol::make_object(lua, sol::lua_nil));
            }
            return std::make_tuple(sol::make_object(lua, false), sol::make_object(lua, error));
        });
        proto["stream"] = streamApi;
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeCoreApiModule(ScriptHost& host) {
    return std::make_unique<CoreScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
