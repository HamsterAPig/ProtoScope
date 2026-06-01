#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

class CoreScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit CoreScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "core_api_module"; }

    void registerApi(ScriptHostContextInternal&, sol::table& proto) override {
        auto* host = &host_;
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
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeCoreApiModule(ScriptHost& host) {
    return std::make_unique<CoreScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
