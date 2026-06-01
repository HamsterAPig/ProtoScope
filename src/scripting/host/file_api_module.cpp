#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

namespace protoscope::scripting {

class FileScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit FileScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "file_api_module"; }

    void registerApi(ScriptHostContextInternal&, sol::table& proto) override {
        auto* host = &host_;
        sol::table fsApi = host->luaState().create_table();
        fsApi.set_function("open_file_dialog", [host](const sol::object& opts) {
            std::string error;
            const auto request = host->protoFileDialog(FileDialogKind::OpenFile, opts, error);
            if (!request.has_value()) {
                return std::make_tuple(sol::make_object(host->luaState(), sol::lua_nil),
                                       sol::make_object(host->luaState(), error));
            }
            return std::make_tuple(sol::make_object(host->luaState(), request->id),
                                   sol::make_object(host->luaState(), sol::lua_nil));
        });
        fsApi.set_function("open_dir_dialog", [host](const sol::object& opts) {
            std::string error;
            const auto request = host->protoFileDialog(FileDialogKind::OpenDir, opts, error);
            if (!request.has_value()) {
                return std::make_tuple(sol::make_object(host->luaState(), sol::lua_nil),
                                       sol::make_object(host->luaState(), error));
            }
            return std::make_tuple(sol::make_object(host->luaState(), request->id),
                                   sol::make_object(host->luaState(), sol::lua_nil));
        });
        fsApi.set_function("open", [host](const std::string& path, const sol::object& opts) {
            return host->protoFsOpen(path, opts);
        });
        fsApi.set_function("read", [host](std::uint64_t handle, const sol::object& opts) {
            return host->protoFsRead(handle, opts);
        });
        fsApi.set_function("write", [host](std::uint64_t handle, const sol::object& payload) {
            return host->protoFsWrite(handle, payload);
        });
        fsApi.set_function("close", [host](std::uint64_t handle) {
            return host->protoFsClose(handle);
        });
        fsApi.set_function("stat", [host](const std::string& path) {
            return host->protoFsStat(path);
        });
        fsApi.set_function("send_file", [host](const std::string& path, const sol::object& opts) {
            return host->protoFsSendFile(path, opts);
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
