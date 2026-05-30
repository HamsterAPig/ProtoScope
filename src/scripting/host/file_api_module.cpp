#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {

void ScriptHost::registerFileApi(sol::table& proto) {
    sol::table fsApi = luaState().create_table();
    fsApi.set_function("open_file_dialog", [this](const sol::object& opts) {
        std::string error;
        const auto request = protoFileDialog(FileDialogKind::OpenFile, opts, error);
        if (!request.has_value()) {
            return std::make_tuple(sol::make_object(luaState(), sol::lua_nil),
                                   sol::make_object(luaState(), error));
        }
        return std::make_tuple(sol::make_object(luaState(), request->id),
                               sol::make_object(luaState(), sol::lua_nil));
    });
    fsApi.set_function("open_dir_dialog", [this](const sol::object& opts) {
        std::string error;
        const auto request = protoFileDialog(FileDialogKind::OpenDir, opts, error);
        if (!request.has_value()) {
            return std::make_tuple(sol::make_object(luaState(), sol::lua_nil),
                                   sol::make_object(luaState(), error));
        }
        return std::make_tuple(sol::make_object(luaState(), request->id),
                               sol::make_object(luaState(), sol::lua_nil));
    });
    fsApi.set_function("open", [this](const std::string& path, const sol::object& opts) {
        return protoFsOpen(path, opts);
    });
    fsApi.set_function("read", [this](std::uint64_t handle, const sol::object& opts) {
        return protoFsRead(handle, opts);
    });
    fsApi.set_function("write", [this](std::uint64_t handle, const sol::object& payload) {
        return protoFsWrite(handle, payload);
    });
    fsApi.set_function("close", [this](std::uint64_t handle) {
        return protoFsClose(handle);
    });
    fsApi.set_function("stat", [this](const std::string& path) {
        return protoFsStat(path);
    });
    fsApi.set_function("send_file", [this](const std::string& path, const sol::object& opts) {
        return protoFsSendFile(path, opts);
    });
    proto["fs"] = fsApi;
}

} // namespace protoscope::scripting
