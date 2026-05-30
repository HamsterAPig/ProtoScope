#include "protoscope/scripting/script_host.hpp"

#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

void ScriptHost::registerPlotApi(sol::table& proto) {
    sol::table plotApi = luaState().create_table();
    plotApi.set_function("setup", [this](const sol::object& payload) {
        std::string error;
        const auto setup = script_host_lua::parsePlotSetup(payload, error);
        if (!setup.has_value()) {
            protoLog("error", "proto.plot.setup 调用失败: " + error);
            return;
        }
        protoPlotSetup(*setup);
    });
    plotApi.set_function("push", [this](int channelIndex, const sol::object& payload) {
        if (channelIndex <= 0) {
            protoLog("error", "proto.plot.push 调用失败: channelIndex 必须从 1 开始");
            return;
        }
        std::string error;
        const auto request = script_host_lua::parsePlotAppend(payload, error);
        if (!request.has_value()) {
            protoLog("error", "proto.plot.push 调用失败: " + error);
            return;
        }
        protoPlotPush(static_cast<std::size_t>(channelIndex - 1), *request);
    });
    proto["plot"] = plotApi;
}

} // namespace protoscope::scripting
