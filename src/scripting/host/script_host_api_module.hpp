#pragma once

#include "script_host_context.hpp"

#include <sol/sol.hpp>

#include <memory>
#include <string_view>

namespace protoscope::scripting {

class ScriptHost;

class IScriptHostApiModule {
public:
    virtual ~IScriptHostApiModule() = default;

    virtual std::string_view id() const = 0;
    virtual void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) = 0;
};

std::unique_ptr<IScriptHostApiModule> makeCoreApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeTxApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeStatusApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeUiApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeFileApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makePlotApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeControlApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeCodecApiModule(ScriptHost& host);

} // namespace protoscope::scripting
