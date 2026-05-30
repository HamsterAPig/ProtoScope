#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {

void ScriptHost::registerStatusApi(sol::table& proto) {
    sol::table statusApi = luaState().create_table();
    statusApi.set_function("set", [this](const std::string& text, const sol::object& opts) {
        protoStatusSet(text, opts);
    });
    statusApi.set_function("clear", [this]() {
        protoStatusClear();
    });
    proto["status"] = statusApi;
}

} // namespace protoscope::scripting
