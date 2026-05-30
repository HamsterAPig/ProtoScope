#include "script_file_system_facade.hpp"

namespace protoscope::scripting {

class ScriptFileSystemFacade final : public IScriptFileSystemFacade {
public:
    std::string_view id() const override { return "script_file_system_facade"; }

    std::tuple<sol::object, sol::object> open(ScriptHostContextInternal&, const std::string&, const sol::object&) override {
        return {};
    }
    std::tuple<sol::object, sol::object> read(ScriptHostContextInternal&, std::uint64_t, const sol::object&) override {
        return {};
    }
    std::tuple<sol::object, sol::object> write(ScriptHostContextInternal&, std::uint64_t, const sol::object&) override {
        return {};
    }
    std::tuple<sol::object, sol::object> close(ScriptHostContextInternal&, std::uint64_t) override {
        return {};
    }
    std::tuple<sol::object, sol::object> stat(ScriptHostContextInternal&, const std::string&) override {
        return {};
    }
    std::tuple<sol::object, sol::object> sendFile(ScriptHostContextInternal&, const std::string&, const sol::object&) override {
        return {};
    }
};

} // namespace protoscope::scripting
