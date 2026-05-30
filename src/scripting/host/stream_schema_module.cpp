#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class StreamSchemaModule final : public IScriptHostApiModule {
public:
    std::string_view id() const override { return "stream_schema_module"; }
    void registerApi(ScriptHostContextInternal&, sol::table&) override {}
};

} // namespace protoscope::scripting
