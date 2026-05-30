#include "script_host_service.hpp"

namespace protoscope::scripting {

class ScriptLoader final : public IScriptHostService {
public:
    std::string_view id() const override { return "script_loader"; }
};

} // namespace protoscope::scripting
