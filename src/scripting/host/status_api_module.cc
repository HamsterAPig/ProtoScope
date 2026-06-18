#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class StatusScriptHostApiModule final : public ScriptHostApiModuleBase {
public:
    explicit StatusScriptHostApiModule(ScriptHost& host) : ScriptHostApiModuleBase(host, "status_api_module") {}

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        auto* host = &host_;
        sol::table statusApi = ctx.lua.create_table();
        statusApi.set_function(
            "set", [host](const std::string& text, const sol::object& opts) { host->protoStatusSet(text, opts); });
        statusApi.set_function("clear", [host]() { host->protoStatusClear(); });
        proto["status"] = statusApi;
    }
};

std::unique_ptr<IScriptHostApiModule> makeStatusApiModule(ScriptHost& host)
{
    return makeScriptHostApiModule<StatusScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
