#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

class ControlScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit ControlScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "control_api_module"; }

    void registerApi(ScriptHostContextInternal&, sol::table& proto) override {
        auto* host = &host_;
        proto.set_function("get_control", [host](const std::string& id) {
            sol::state_view view = host->luaView();
            const auto* value = host->findControlValue(id);
            if (value == nullptr) {
                return sol::make_object(view, sol::lua_nil);
            }
            return script_host_lua::controlValueToLua(
                view, script_host_lua::findControlDescriptor(host->controlDescriptors(), id), *value);
        });
        proto.set_function("set_control", [host](const std::string& id, const sol::object& value) {
            const auto* descriptor = script_host_lua::findControlDescriptor(host->controlDescriptors(), id);
            if (descriptor == nullptr) {
                host->protoLog("warn", "proto.set_control 未找到控件: " + id);
                return;
            }
            std::string error;
            const auto converted = script_host_lua::controlValueFromLua(*descriptor, value, error);
            if (!converted.has_value()) {
                host->protoLog("warn", "proto.set_control 调用失败: " + error);
                return;
            }
            host->updateControlValue(id, *converted);
        });
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeControlApiModule(ScriptHost& host) {
    return std::make_unique<ControlScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
