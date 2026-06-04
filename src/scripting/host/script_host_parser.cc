#include "script_host_service.hpp"

namespace protoscope::scripting {

class ScriptParser final : public IScriptHostService {
public:
    std::string_view id() const override { return "script_parser"; }
};

} // namespace protoscope::scripting
