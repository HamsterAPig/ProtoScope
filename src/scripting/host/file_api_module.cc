#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

class FileScriptHostApiModule final : public ScriptHostApiModuleBase {
public:
    explicit FileScriptHostApiModule(ScriptHost& host) : ScriptHostApiModuleBase(host, "file_api_module") {}

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        auto* host = &host_;
        sol::state_view lua = ctx.lua;
        sol::table fsApi = lua.create_table();
        fsApi.set_function("open_file_dialog", [host, lua](const sol::object& opts) {
            std::string error;
            const auto request = host->protoFileDialog(FileDialogKind::OpenFile, opts, error);
            if (!request.has_value()) {
                return script_host_lua::luaNilError(lua, error);
            }
            return script_host_lua::luaValueOk(lua, request->id);
        });
        fsApi.set_function("open_dir_dialog", [host, lua](const sol::object& opts) {
            std::string error;
            const auto request = host->protoFileDialog(FileDialogKind::OpenDir, opts, error);
            if (!request.has_value()) {
                return script_host_lua::luaNilError(lua, error);
            }
            return script_host_lua::luaValueOk(lua, request->id);
        });
        fsApi.set_function("open", [host, lua](const std::string& path, const sol::object& opts) {
            return host->protoFsOpen(lua, path, opts);
        });
        fsApi.set_function("read", [host, lua](std::uint64_t handle, const sol::object& opts) {
            return host->protoFsRead(lua, handle, opts);
        });
        fsApi.set_function("write", [host, lua](std::uint64_t handle, const sol::object& payload) {
            return host->protoFsWrite(lua, handle, payload);
        });
        fsApi.set_function("close", [host, lua](std::uint64_t handle) { return host->protoFsClose(lua, handle); });
        fsApi.set_function("stat", [host, lua](const std::string& path) { return host->protoFsStat(lua, path); });
        fsApi.set_function("send_file", [host, lua](const std::string& path, const sol::object& opts) {
            return host->protoFsSendFile(lua, path, opts);
        });
        proto["fs"] = fsApi;
    }
};

std::unique_ptr<IScriptHostApiModule> makeFileApiModule(ScriptHost& host)
{
    return makeScriptHostApiModule<FileScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
