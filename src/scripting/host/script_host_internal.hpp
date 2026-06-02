#pragma once

#include "protoscope/scripting/frame_stream_parser.hpp"
#include "protoscope/scripting/script_host.hpp"

#include <sol/sol.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace protoscope::scripting {

struct LoadedStreamSchema {
    explicit LoadedStreamSchema(StreamBufferDefinition buffer, std::vector<StreamFrameDefinition> frames)
        : parser(std::move(buffer), std::move(frames)) {}

    FrameStreamParser parser;
    std::unordered_map<std::string, std::string> frameCallbackKeys;
    std::optional<std::string> onErrorCallbackKey;
};

struct ScriptHost::Runtime {
    sol::state lua;
    std::unique_ptr<LoadedStreamSchema> stream;
    std::unordered_map<std::string, sol::protected_function> streamCallbacks;
    std::unordered_map<std::string, StreamRuntimeProfile> streamRuntimeProfiles;
};

struct ScriptHost::FileHandle {
    std::uint64_t id{0};
    std::filesystem::path path;
    std::fstream stream;
    bool readable{false};
    bool writable{false};
    std::uint64_t bytesWritten{0};
};

struct ScriptHost::AuthorizedPath {
    std::filesystem::path path;
    bool recursive{false};
    bool readable{true};
    bool writable{true};
};

struct ScriptHost::FileSendJob {
    std::uint64_t id{0};
    std::uint64_t handleId{0};
    TxRequestKind kind{TxRequestKind::Send};
    std::string tag;
    std::size_t chunkSize{0};
    std::uint64_t total{0};
    std::uint64_t nextOffset{0};
    std::size_t inflight{0};
    bool eof{false};
};

ControlValue defaultValueFor(const ControlDescriptor& descriptor);
const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls, const std::string& id);
sol::object controlValueToLua(sol::state_view lua, const ControlDescriptor* descriptor, const ControlValue& value);
std::optional<std::vector<std::uint8_t>> bytesFromLuaObject(const sol::object& object, std::string& error);
std::optional<std::vector<DockDescriptor>> parseDockDescriptors(sol::state_view lua, std::string& error);
std::unique_ptr<LoadedStreamSchema> parseLoadedStreamSchema(
    sol::state_view lua,
    std::unordered_map<std::string, sol::protected_function>& callbacks,
    std::string& error);

sol::table makeContextTable(sol::state_view lua, const transport::ConnectionContext& connection);
sol::table makeBytesTable(sol::state_view lua, const std::vector<std::uint8_t>& bytes);
sol::table makeTxEventTable(sol::state_view lua, const TxEvent& event);
sol::table makeDialogEventTable(sol::state_view lua, const DialogEvent& event);
sol::table makeFileDialogEventTable(sol::state_view lua, const FileDialogEvent& event);
sol::table makeStreamFrameTable(sol::state_view lua, const StreamParsedFrame& frame);
sol::table makeStreamErrorTable(sol::state_view lua, const StreamParseError& error);

std::string protectedCallError(sol::protected_function_result& result);
std::optional<double> luaNumberField(const sol::table& table, const char* key);
std::optional<std::string> luaStringField(const sol::table& table, const char* key);
std::optional<bool> luaBoolField(const sol::table& table, const char* key);
std::optional<std::int64_t> luaIntegerValue(const sol::object& object);
std::size_t positiveSizeOrDefault(std::size_t value, std::size_t fallback);
std::filesystem::path canonicalPath(const std::filesystem::path& path);
bool isSameOrChildPath(const std::filesystem::path& root, const std::filesystem::path& candidate, bool recursive);

} // namespace protoscope::scripting
