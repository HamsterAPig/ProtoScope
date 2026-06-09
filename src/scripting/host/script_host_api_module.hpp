#pragma once

#include "script_host_context.hpp"

#include <memory>
#include <string_view>

#include <sol/sol.hpp>

namespace protoscope::scripting {

class ScriptHost;

class IScriptHostApiModule {
public:
    virtual ~IScriptHostApiModule() = default;

    virtual std::string_view id() const = 0;
    virtual void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) = 0;
};

class ScriptHostApiModuleBase : public IScriptHostApiModule {
public:
    ScriptHostApiModuleBase(ScriptHost& host, std::string_view moduleId) : host_(host), moduleId_(moduleId) {}

    std::string_view id() const override { return moduleId_; }

protected:
    ScriptHost& host_;

private:
    std::string_view moduleId_;
};

template <typename Module> std::unique_ptr<IScriptHostApiModule> makeScriptHostApiModule(ScriptHost& host)
{
    return std::make_unique<Module>(host);
}

std::unique_ptr<IScriptHostApiModule> makeCoreApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeTxApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeStatusApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeUiApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeFileApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makePlotApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeControlApiModule(ScriptHost& host);
std::unique_ptr<IScriptHostApiModule> makeCodecApiModule(ScriptHost& host);

} // namespace protoscope::scripting
