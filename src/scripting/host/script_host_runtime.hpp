#pragma once

#include "script_host_context.hpp"

namespace protoscope::scripting {

class ScriptHostRuntimeFacade {
public:
    explicit ScriptHostRuntimeFacade(ScriptHost& host) : host_(host) {}

    ScriptHost& host() { return host_; }
    const ScriptHost& host() const { return host_; }

private:
    ScriptHost& host_;
};

} // namespace protoscope::scripting
