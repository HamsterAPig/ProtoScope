#include "script_host_internal.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <unordered_map>
#include <utility>

namespace protoscope::scripting {

bool ScriptHost::loadScriptFile(const std::string& path)
{
    if (path.empty()) {
        setLastError("脚本路径为空");
        protoLog("error", lastError_);
        return false;
    }

    std::filesystem::path filePath;
    try {
        filePath = std::filesystem::path(path);
        if (!std::filesystem::exists(filePath)) {
            setLastError("未找到脚本文件: " + path);
            protoLog("error", lastError_);
            return false;
        }
    } catch (const std::exception& ex) {
        setLastError(std::string("检查脚本文件失败: ") + ex.what());
        protoLog("error", lastError_);
        return false;
    } catch (...) {
        setLastError("检查脚本文件失败: 未知异常");
        protoLog("error", lastError_);
        return false;
    }

    struct LoadSnapshot {
        bool scriptLoaded{false};
        std::string scriptPath;
        std::string protocolDirectory;
        std::string lastError;
        std::unordered_map<std::string, ControlValue> controlValues;
        std::vector<ScriptEvent> events;
        std::vector<ScriptLog> logs;
        std::vector<TxRequest> txRequests;
        std::vector<PlotSetup> plotSetups;
        std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> plotAppends;
        std::vector<RequestDoneResult> requestDoneResults;
        std::vector<StatusUpdate> statusUpdates;
        std::vector<DialogRequest> dialogRequests;
        std::vector<FileDialogRequest> fileDialogRequests;
        std::unordered_map<std::string, TimerState> timers;
        std::unordered_map<std::uint64_t, std::unique_ptr<FileHandle>> fileHandles;
        std::unordered_map<std::uint64_t, FileSendJob> fileSendJobs;
        std::vector<AuthorizedPath> dialogAuthorizedPaths;
        std::optional<transport::ConnectionContext> activeConnection;
        bool requestAwaitingCompletion{false};
    };

    LoadSnapshot snapshot{
        .scriptLoaded = scriptLoaded_,
        .scriptPath = scriptPath_,
        .protocolDirectory = protocolDirectory_,
        .lastError = lastError_,
        .controlValues = std::move(controlValues_),
        .events = std::move(events_),
        .logs = std::move(logs_),
        .txRequests = std::move(txRequests_),
        .plotSetups = std::move(plotSetups_),
        .plotAppends = std::move(plotAppends_),
        .requestDoneResults = std::move(requestDoneResults_),
        .statusUpdates = std::move(statusUpdates_),
        .dialogRequests = std::move(dialogRequests_),
        .fileDialogRequests = std::move(fileDialogRequests_),
        .timers = std::move(timers_),
        .fileHandles = std::move(fileHandles_),
        .fileSendJobs = std::move(fileSendJobs_),
        .dialogAuthorizedPaths = std::move(dialogAuthorizedPaths_),
        .activeConnection = std::move(activeConnection_),
        .requestAwaitingCompletion = requestAwaitingCompletion_,
    };

    auto restoreFailure = [&](std::string message) {
        scriptLoaded_ = snapshot.scriptLoaded;
        scriptPath_ = std::move(snapshot.scriptPath);
        protocolDirectory_ = std::move(snapshot.protocolDirectory);
        controlValues_ = std::move(snapshot.controlValues);
        events_ = std::move(snapshot.events);
        logs_ = std::move(snapshot.logs);
        txRequests_ = std::move(snapshot.txRequests);
        plotSetups_ = std::move(snapshot.plotSetups);
        plotAppends_ = std::move(snapshot.plotAppends);
        requestDoneResults_ = std::move(snapshot.requestDoneResults);
        statusUpdates_ = std::move(snapshot.statusUpdates);
        dialogRequests_ = std::move(snapshot.dialogRequests);
        fileDialogRequests_ = std::move(snapshot.fileDialogRequests);
        timers_ = std::move(snapshot.timers);
        fileHandles_ = std::move(snapshot.fileHandles);
        fileSendJobs_ = std::move(snapshot.fileSendJobs);
        dialogAuthorizedPaths_ = std::move(snapshot.dialogAuthorizedPaths);
        activeConnection_ = std::move(snapshot.activeConnection);
        requestAwaitingCompletion_ = snapshot.requestAwaitingCompletion;
        setLastError(std::move(message));
        protoLog("error", lastError_);
        return false;
    };

    const auto nextProtocolDirectory = filePath.parent_path().generic_string();
    scriptLoaded_ = false;
    scriptPath_ = path;
    protocolDirectory_ = nextProtocolDirectory;
    lastError_.clear();
    events_.clear();
    logs_.clear();
    txRequests_.clear();
    plotSetups_.clear();
    plotAppends_.clear();
    requestDoneResults_.clear();
    statusUpdates_.clear();
    dialogRequests_.clear();
    fileDialogRequests_.clear();
    timers_.clear();
    fileHandles_.clear();
    fileSendJobs_.clear();
    dialogAuthorizedPaths_.clear();
    activeConnection_.reset();
    requestAwaitingCompletion_ = false;

    auto nextRuntime = std::make_unique<Runtime>();

    try {
        nextRuntime->lua.open_libraries(sol::lib::base,
                                        sol::lib::math,
                                        sol::lib::package,
                                        sol::lib::string,
                                        sol::lib::table,
                                        sol::lib::utf8,
                                        sol::lib::os);

        auto& lua = nextRuntime->lua;
        lua.new_usertype<ProtoBuffer>(
            "ProtoBuffer",
            "size",
            &ProtoBuffer::size,
            "slice",
            &ProtoBuffer::slice,
            "to_hex",
            &ProtoBuffer::toHex,
            "bytes",
            [this, bufferLua = sol::state_view(lua)](const ProtoBuffer& buffer, const sol::object& maxBytes) mutable {
                std::size_t limit = buffer.bytes.size();
                if (maxBytes.valid() && maxBytes.get_type() != sol::type::lua_nil && maxBytes.is<int>()) {
                    limit = std::min<std::size_t>(limit, static_cast<std::size_t>(std::max(0, maxBytes.as<int>())));
                }
                limit = std::min(limit, fileIoConfig_.maxChunkBytes);
                sol::table table = bufferLua.create_table(static_cast<int>(limit), 0);
                for (std::size_t index = 0; index < limit; ++index) {
                    table[index + 1] = buffer.bytes[index];
                }
                return table;
            });

        // 将协议脚本目录加入 Lua 模块搜索路径，使 main.lua 可 require 同目录模块。
        // 额外放开父目录，方便 protocols/<demo>/main.lua 共享 protocols/*.lua 公共脚本。
        // 使用 generic_string 格式，统一为 /，避免 Windows 反斜杠干扰 Lua package.path。
        const auto pkgPath = lua["package"]["path"].get<std::string>();
        const auto protocolParent = std::filesystem::path(nextProtocolDirectory).parent_path().generic_string();
        lua["package"]["path"] = nextProtocolDirectory + "/?.lua;" + nextProtocolDirectory + "/?/init.lua;" +
                                 (protocolParent.empty() ? std::string() : protocolParent + "/?.lua;") +
                                 (protocolParent.empty() ? std::string() : protocolParent + "/?/init.lua;") + pkgPath;
        auto proto = lua.create_named_table("proto");

        // 核心流程：所有脚本侧能力统一经由模块注册器挂到 proto.*，避免加载流程继续膨胀。
        registerLuaApi(lua, proto);

        auto scriptResult = lua.safe_script_file(path, &sol::script_pass_on_error);
        if (!scriptResult.valid()) {
            return restoreFailure("执行脚本失败: " + protectedCallError(scriptResult));
        }

        std::string streamError;
        auto streamSchema = parseLoadedStreamSchema(lua, nextRuntime->streamCallbacks, streamError);
        if (!streamError.empty()) {
            return restoreFailure(streamError);
        }

        std::string parseError;
        const auto parsedDocks = parseDockDescriptors(lua, parseError);
        if (!parsedDocks.has_value()) {
            return restoreFailure(parseError);
        }

        std::vector<ControlDescriptor> nextControls;
        std::unordered_map<std::string, ControlValue> nextControlValues;
        for (const auto& dock : *parsedDocks) {
            for (const auto& control : dock.controls) {
                nextControls.push_back(control);
                const auto existing = snapshot.controlValues.find(control.id);
                nextControlValues[control.id] =
                    existing == snapshot.controlValues.end() ? defaultValueFor(control) : existing->second;
            }
        }

        nextRuntime->stream = std::move(streamSchema);
        runtime_ = std::move(nextRuntime);
        docks_ = *parsedDocks;
        controls_ = std::move(nextControls);
        controlValues_ = std::move(nextControlValues);
        scriptPath_ = path;
        protocolDirectory_ = nextProtocolDirectory;
        lastError_.clear();
        scriptLoaded_ = true;
        return true;
    } catch (const std::exception& ex) {
        return restoreFailure(std::string("加载脚本异常: ") + ex.what());
    } catch (...) {
        return restoreFailure("加载脚本异常: 未知异常");
    }
}

bool ScriptHost::loadProtocolDirectory(const std::string& directory)
{
    const auto path = std::filesystem::path(directory) / "main.lua";
    return loadScriptFile(path.generic_string());
}

} // namespace protoscope::scripting
