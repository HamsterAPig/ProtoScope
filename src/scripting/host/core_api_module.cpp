#include "protoscope/scripting/script_host.hpp"

#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

void ScriptHost::registerCoreApi(sol::table& proto) {
    proto.set_function("log", [this](const std::string& level, const std::string& message) {
        protoLog(level, message);
    });
    proto.set_function("emit", [this](const std::string& name, const sol::object& payload) {
        protoEmit(name, script_host_lua::serializeLuaObject(payload));
    });
    proto.set_function("set_timer", [this](const std::string& name, std::uint64_t delayMs) {
        protoSetTimer(name, delayMs);
    });
    proto.set_function("cancel_timer", [this](const std::string& name) {
        protoCancelTimer(name);
    });
}

} // namespace protoscope::scripting
