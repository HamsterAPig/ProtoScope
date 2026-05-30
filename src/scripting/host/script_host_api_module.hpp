#pragma once

#include "script_host_context.hpp"

#include <sol/sol.hpp>

#include <string_view>

namespace protoscope::scripting {

class IScriptHostApiModule {
public:
    virtual ~IScriptHostApiModule() = default;

    virtual std::string_view id() const = 0;
    virtual void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) = 0;
};

} // namespace protoscope::scripting
