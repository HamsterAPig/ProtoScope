#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

class PlotScriptHostApiModule final : public ScriptHostApiModuleBase {
public:
    explicit PlotScriptHostApiModule(ScriptHost& host) : ScriptHostApiModuleBase(host, "plot_api_module") {}

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        auto* host = &host_;
        sol::table plotApi = ctx.lua.create_table();
        plotApi.set_function("setup", [host](const sol::object& payload) {
            std::string error;
            const auto setup = script_host_lua::parsePlotSetup(payload, error);
            if (!setup.has_value()) {
                host->protoLog("error", "proto.plot.setup 调用失败: " + error);
                return;
            }
            host->protoPlotSetup(*setup);
        });
        plotApi.set_function("push", [host](int channelIndex, const sol::object& payload) {
            if (channelIndex <= 0) {
                host->protoLog("error", "proto.plot.push 调用失败: channelIndex 必须从 1 开始");
                return;
            }
            std::string error;
            const auto request = script_host_lua::parsePlotAppend(payload, error);
            if (!request.has_value()) {
                host->protoLog("error", "proto.plot.push 调用失败: " + error);
                return;
            }
            host->protoPlotPush(static_cast<std::size_t>(channelIndex - 1), *request);
        });
        proto["plot"] = plotApi;
    }
};

std::unique_ptr<IScriptHostApiModule> makePlotApiModule(ScriptHost& host)
{
    return makeScriptHostApiModule<PlotScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
