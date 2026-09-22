#include "script_host_api_module.hpp"
#include "script_data_session.hpp"

namespace protoscope::scripting {
namespace {
class DataApiModule final : public ScriptHostApiModuleBase {
public:
    explicit DataApiModule(ScriptHost& host) : ScriptHostApiModuleBase(host, "data") {}
    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        ctx.dataSession.registerApi(ctx.lua, proto);
    }
};
}
std::unique_ptr<IScriptHostApiModule> makeDataApiModule(ScriptHost& host)
{
    return makeScriptHostApiModule<DataApiModule>(host);
}
} // namespace protoscope::scripting
