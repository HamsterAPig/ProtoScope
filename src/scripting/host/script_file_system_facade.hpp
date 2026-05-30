#pragma once

#include "script_host_service.hpp"

#include <sol/sol.hpp>

#include <tuple>

namespace protoscope::scripting {

class IScriptFileSystemFacade : public IScriptHostService {
public:
    virtual std::tuple<sol::object, sol::object> open(ScriptHostContextInternal& ctx,
                                                      const std::string& path,
                                                      const sol::object& opts) = 0;
    virtual std::tuple<sol::object, sol::object> read(ScriptHostContextInternal& ctx,
                                                      std::uint64_t handle,
                                                      const sol::object& opts) = 0;
    virtual std::tuple<sol::object, sol::object> write(ScriptHostContextInternal& ctx,
                                                       std::uint64_t handle,
                                                       const sol::object& payload) = 0;
    virtual std::tuple<sol::object, sol::object> close(ScriptHostContextInternal& ctx,
                                                       std::uint64_t handle) = 0;
    virtual std::tuple<sol::object, sol::object> stat(ScriptHostContextInternal& ctx,
                                                      const std::string& path) = 0;
    virtual std::tuple<sol::object, sol::object> sendFile(ScriptHostContextInternal& ctx,
                                                          const std::string& path,
                                                          const sol::object& opts) = 0;
};

} // namespace protoscope::scripting
