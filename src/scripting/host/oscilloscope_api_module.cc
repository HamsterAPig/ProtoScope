#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class OscilloscopeScriptHostApiModule final : public ScriptHostApiModuleBase {
public:
    explicit OscilloscopeScriptHostApiModule(ScriptHost& host)
        : ScriptHostApiModuleBase(host, "oscilloscope_api_module")
    {
    }

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        auto* host = &host_;
        sol::table oscilloscopeApi = ctx.lua.create_table();
        oscilloscopeApi.set_function("set_running", [host](const sol::object& running) {
            if (!running.valid() || running.get_type() == sol::type::lua_nil || !running.is<bool>()) {
                host->protoLog("error", "proto.oscilloscope.set_running 调用失败: running 必须是 boolean");
                return;
            }
            host->protoOscilloscopeSetRunning(running.as<bool>());
        });
        proto["oscilloscope"] = oscilloscopeApi;
    }
};

std::unique_ptr<IScriptHostApiModule> makeOscilloscopeApiModule(ScriptHost& host)
{
    return makeScriptHostApiModule<OscilloscopeScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
