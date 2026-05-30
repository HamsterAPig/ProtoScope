#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class CoreApiModule final : public IScriptHostApiModule {
public:
    std::string_view id() const override { return "core_api_module"; }
    void registerApi(ScriptHostContextInternal&, sol::table&) override {}
};

} // namespace protoscope::scripting
