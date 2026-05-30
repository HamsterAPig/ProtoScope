#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class StatusApiModule final : public IScriptHostApiModule {
public:
    std::string_view id() const override { return "status_api_module"; }
    void registerApi(ScriptHostContextInternal&, sol::table&) override {}
};

} // namespace protoscope::scripting
