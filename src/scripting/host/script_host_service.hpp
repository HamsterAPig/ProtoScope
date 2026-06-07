#pragma once

#include "script_host_context.hpp"

#include <cstdint>
#include <string_view>

namespace protoscope::scripting {

class IScriptHostService {
public:
    virtual ~IScriptHostService() = default;

    virtual std::string_view id() const = 0;

    virtual void reset(ScriptHostContextInternal&) {}

    virtual void tick(ScriptHostContextInternal&, std::uint64_t) {}
};

} // namespace protoscope::scripting
