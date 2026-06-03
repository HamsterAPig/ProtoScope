#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class FileScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit FileScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "file_api_module"; }

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override {
        auto* host = &host_;
        sol::state_view lua = ctx.lua;
        sol::table fsApi = lua.create_table();
        fsApi.set_function("open_file_dialog", [host, lua](const sol::object& opts) {
            std::string error;
            const auto request = host->protoFileDialog(FileDialogKind::OpenFile, opts, error);
            if (!request.has_value()) {
                return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, error));
            }
            return std::make_tuple(sol::make_object(lua, request->id), sol::make_object(lua, sol::lua_nil));
        });
        fsApi.set_function("open_dir_dialog", [host, lua](const sol::object& opts) {
            std::string error;
            const auto request = host->protoFileDialog(FileDialogKind::OpenDir, opts, error);
            if (!request.has_value()) {
                return std::make_tuple(sol::make_object(lua, sol::lua_nil), sol::make_object(lua, error));
            }
            return std::make_tuple(sol::make_object(lua, request->id), sol::make_object(lua, sol::lua_nil));
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
        fsApi.set_function("close", [host, lua](std::uint64_t handle) {
            return host->protoFsClose(lua, handle);
        });
        fsApi.set_function("stat", [host, lua](const std::string& path) {
            return host->protoFsStat(lua, path);
        });
        fsApi.set_function("send_file", [host, lua](const std::string& path, const sol::object& opts) {
            return host->protoFsSendFile(lua, path, opts);
        });
        proto["fs"] = fsApi;
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeFileApiModule(ScriptHost& host) {
    return std::make_unique<FileScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
