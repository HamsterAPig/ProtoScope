#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class StatusScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit StatusScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "status_api_module"; }

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override {
        auto* host = &host_;
        sol::table statusApi = ctx.lua.create_table();
        statusApi.set_function("set", [host](const std::string& text, const sol::object& opts) {
            host->protoStatusSet(text, opts);
        });
        statusApi.set_function("clear", [host]() {
            host->protoStatusClear();
        });
        proto["status"] = statusApi;
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeStatusApiModule(ScriptHost& host) {
    return std::make_unique<StatusScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
