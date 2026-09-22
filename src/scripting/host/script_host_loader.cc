#include "script_host_internal.hpp"
#include "control_properties.hpp"
#include "control_data_binding.hpp"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <optional>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace protoscope::scripting {

namespace {

    bool canReuseControlValue(const ControlDescriptor& previous,
                              const ControlDescriptor& next,
                              const ControlValue& value)
    {
        if (previous.type != next.type || !validateControlValue(next, value)) {
            return false;
        }
        switch (next.type) {
        case ControlType::Label:
        case ControlType::Readout:
        case ControlType::Indicator:
        case ControlType::Progress:
        case ControlType::Button:
        case ControlType::ValueTable:
        case ControlType::DataTable:
            return false;
        case ControlType::Combo:
        case ControlType::RadioGroup: {
            const auto index = std::get<int>(value);
            return index >= 0 && static_cast<std::size_t>(index) < next.comboOptions.size() &&
                   static_cast<std::size_t>(index) < previous.comboOptions.size() &&
                   previous.comboOptions[index] == next.comboOptions[index];
        }
        case ControlType::InputFloat:
            return std::isfinite(std::get<float>(value));
        case ControlType::TxSequence:
            // 发送字段的结构或选项变化后旧帧不再可信，整体回退到脚本默认值。
            return previous.txSequenceFields.size() == next.txSequenceFields.size() &&
                std::equal(previous.txSequenceFields.begin(), previous.txSequenceFields.end(),
                           next.txSequenceFields.begin(), [](const auto& lhs, const auto& rhs) {
                    return lhs.id == rhs.id && lhs.type == rhs.type && lhs.options.size() == rhs.options.size() &&
                        std::equal(lhs.options.begin(), lhs.options.end(), rhs.options.begin(),
                                   [](const auto& a, const auto& b) { return a.value == b.value; });
                });
        default:
            return true;
        }
    }

    std::optional<std::filesystem::path> resolveScriptFilePath(const std::string& path, std::string& error)
    {
        if (path.empty()) {
            error = "脚本路径为空";
            return std::nullopt;
        }

        try {
            auto filePath = std::filesystem::path(path);
            std::error_code fileProbeError;
            const bool fileExists = std::filesystem::exists(filePath, fileProbeError);
            if (fileProbeError) {
                error = "检查脚本文件失败: " + fileProbeError.message() + ": " + path;
                return std::nullopt;
            }
            if (!fileExists) {
                error = "未找到脚本文件: " + path;
                return std::nullopt;
            }

            const bool regularFile = std::filesystem::is_regular_file(filePath, fileProbeError);
            if (fileProbeError) {
                error = "检查脚本文件失败: " + fileProbeError.message() + ": " + path;
                return std::nullopt;
            }
            if (!regularFile) {
                error = "脚本路径不是普通文件: " + path;
                return std::nullopt;
            }

            return filePath;
        } catch (const std::exception& ex) {
            error = std::string("检查脚本文件失败: ") + ex.what();
            return std::nullopt;
        } catch (...) {
            error = "检查脚本文件失败: 未知异常";
            return std::nullopt;
        }
    }

    std::string buildLuaPackagePath(sol::state_view lua, const std::string& protocolDirectory)
    {
        const auto pkgPath = lua["package"]["path"].get<std::string>();
        const auto protocolParent = std::filesystem::path(protocolDirectory).parent_path().generic_string();
        return protocolDirectory + "/?.lua;" + protocolDirectory + "/?/init.lua;" +
               (protocolParent.empty() ? std::string() : protocolParent + "/?.lua;") +
               (protocolParent.empty() ? std::string() : protocolParent + "/?/init.lua;") + pkgPath;
    }

} // namespace

ScriptHost::LoadSnapshot ScriptHost::captureLoadSnapshot()
{
    return LoadSnapshot{
        .scriptLoaded = scriptLoaded_,
        .scriptPath = scriptPath_,
        .protocolDirectory = protocolDirectory_,
        .controlValues = std::move(controlValues_),
        .events = std::move(events_),
        .logs = std::move(logs_),
        .txRequests = std::move(txRequests_),
        .requestGuardResets = std::move(requestGuardResets_),
        .plotSetups = std::move(plotSetups_),
        .plotAppends = std::move(plotAppends_),
        .requestDoneResults = std::move(requestDoneResults_),
        .statusUpdates = std::move(statusUpdates_),
        .oscilloscopeRunningUpdates = std::move(oscilloscopeRunningUpdates_),
        .dialogRequests = std::move(dialogRequests_),
        .fileDialogRequests = std::move(fileDialogRequests_),
        .timers = std::move(timers_),
        .fileHandles = std::move(fileHandles_),
        .fileSendJobs = std::move(fileSendJobs_),
        .dialogAuthorizedPaths = std::move(dialogAuthorizedPaths_),
        .activeConnection = std::move(activeConnection_),
        .requestAwaitingCompletion = requestAwaitingCompletion_,
    };
}

void ScriptHost::restoreLoadSnapshot(LoadSnapshot&& snapshot, std::string message)
{
    scriptLoaded_ = snapshot.scriptLoaded;
    scriptPath_ = std::move(snapshot.scriptPath);
    protocolDirectory_ = std::move(snapshot.protocolDirectory);
    controlValues_ = std::move(snapshot.controlValues);
    events_ = std::move(snapshot.events);
    logs_ = std::move(snapshot.logs);
    txRequests_ = std::move(snapshot.txRequests);
    requestGuardResets_ = std::move(snapshot.requestGuardResets);
    plotSetups_ = std::move(snapshot.plotSetups);
    plotAppends_ = std::move(snapshot.plotAppends);
    requestDoneResults_ = std::move(snapshot.requestDoneResults);
    statusUpdates_ = std::move(snapshot.statusUpdates);
    oscilloscopeRunningUpdates_ = std::move(snapshot.oscilloscopeRunningUpdates);
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
}

void ScriptHost::resetForScriptLoad(const std::string& path, const std::string& protocolDirectory)
{
    scriptLoaded_ = false;
    scriptPath_ = path;
    protocolDirectory_ = protocolDirectory;
    lastError_.clear();
    events_.clear();
    logs_.clear();
    txRequests_.clear();
    requestGuardResets_.clear();
    plotSetups_.clear();
    plotAppends_.clear();
    requestDoneResults_.clear();
    statusUpdates_.clear();
    oscilloscopeRunningUpdates_.clear();
    dialogRequests_.clear();
    fileDialogRequests_.clear();
    timers_.clear();
    fileHandles_.clear();
    fileSendJobs_.clear();
    dialogAuthorizedPaths_.clear();
    activeConnection_.reset();
    requestAwaitingCompletion_ = false;
}

void ScriptHost::configureLuaRuntimeForScriptLoad(Runtime& runtime, const std::string& protocolDirectory)
{
    runtime.data = std::make_unique<ScriptDataSession>(
        storageRoot_, canonicalPath(protocolDirectory).generic_string());
    runtime.lua.open_libraries(sol::lib::base,
                               sol::lib::math,
                               sol::lib::package,
                               sol::lib::string,
                               sol::lib::table,
                               sol::lib::utf8,
                               sol::lib::os);

    auto& lua = runtime.lua;
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
    lua["package"]["path"] = buildLuaPackagePath(lua, protocolDirectory);
    auto proto = lua.create_named_table("proto");

    // 核心流程：所有脚本侧能力统一经由模块注册器挂到 proto.*，避免加载流程继续膨胀。
    registerLuaApi(runtime, proto);
}

std::unique_ptr<ScriptHost::LoadedScript> ScriptHost::loadScriptIntoRuntime(Runtime& runtime,
                                                                            const std::string& path,
                                                                            std::string& error)
{
    auto& lua = runtime.lua;
    LuaExecutionScope execution(lua.lua_state(), runtime.execution, *stopSignal_, executionConfig_.loadTimeoutMs);
    auto scriptResult = lua.safe_script_file(path, &sol::script_pass_on_error);
    if (!scriptResult.valid()) {
        error = "执行脚本失败: " + protectedCallError(scriptResult);
        return nullptr;
    }

    std::string streamError;
    auto streamSchema = parseLoadedStreamSchema(lua, runtime.streamCallbacks, streamError);
    if (!streamError.empty()) {
        error = std::move(streamError);
        return nullptr;
    }

    std::string parseError;
    auto parsedDocks = parseDockDescriptors(lua, parseError);
    if (!parsedDocks.has_value()) {
        error = std::move(parseError);
        return nullptr;
    }
    runtime.data->loadSchemas(lua);
    validateControlBindings(*parsedDocks,runtime.data->schemas());
    validateDataTables(*parsedDocks,runtime.data->schemas());
    if (runtime.execution.failure != nullptr || stopSignal_->load(std::memory_order_relaxed)) {
        error = runtime.execution.failure != nullptr ? runtime.execution.failure : "Lua execution stopped";
        return nullptr;
    }

    auto loadedScript = std::make_unique<LoadedScript>();
    loadedScript->streamSchema = std::move(streamSchema);
    loadedScript->docks = std::move(*parsedDocks);
    return loadedScript;
}

void ScriptHost::commitLoadedScript(std::unique_ptr<Runtime> runtime,
                                    std::unique_ptr<LoadedScript> loadedScript,
                                    const std::unordered_map<std::string, ControlValue>& previousControlValues,
                                    const std::string& path,
                                    const std::string& protocolDirectory)
{
    std::vector<ControlDescriptor> nextControls;
    std::unordered_map<std::string, ControlValue> nextControlValues;
    for (auto& dock : loadedScript->docks) {
        for (auto& control : dock.controls) {
            control.runtimeGeneration = runtimeGeneration_ + 1;
            nextControls.push_back(control);
            const auto existing = previousControlValues.find(control.id);
            const auto* previous = findControlDescriptor(controls_, control.id);
            // 控件 ID 相同不足以复用；实测表值和发送运行状态属于当前运行时。
            auto value = existing != previousControlValues.end() && previous != nullptr &&
                         canReuseControlValue(*previous, control, existing->second)
                ? existing->second : defaultValueFor(control);
            if (auto* sequence = std::get_if<TxSequenceValue>(&value)) {
                sequence->running = false;
            }
            nextControlValues[control.id] = std::move(value);
        }
    }

    runtime->stream = std::move(loadedScript->streamSchema);
    runtime_ = std::move(runtime);
    ++runtimeGeneration_;
    docks_ = std::move(loadedScript->docks);
    controls_ = std::move(nextControls);
    controlValues_ = std::move(nextControlValues);
    runtime_->tables=std::make_unique<DataTableSession>(controls_,*runtime_->data);
    configureDataBindings();
    scriptPath_ = path;
    protocolDirectory_ = protocolDirectory;
    lastError_.clear();
    scriptLoaded_ = true;
}

bool ScriptHost::loadScriptFile(const std::string& path)
{
    std::string error;
    const auto filePath = resolveScriptFilePath(path, error);
    if (!filePath.has_value()) {
        setLastError(std::move(error));
        protoLog("error", lastError_);
        return false;
    }

    auto snapshot = captureLoadSnapshot();
    auto restoreFailure = [&](std::string message) {
        restoreLoadSnapshot(std::move(snapshot), std::move(message));
        return false;
    };

    const auto nextProtocolDirectory = filePath->parent_path().generic_string();
    resetForScriptLoad(path, nextProtocolDirectory);

    auto nextRuntime = std::make_unique<Runtime>();

    try {
        configureLuaRuntimeForScriptLoad(*nextRuntime, nextProtocolDirectory);
        auto loadedScript = loadScriptIntoRuntime(*nextRuntime, path, error);
        if (!loadedScript) {
            return restoreFailure(std::move(error));
        }

        // 新会话读取已提交缓存前排空旧写入；加载失败仍保留旧 runtime 和回调。
        if (runtime_ && runtime_->data) runtime_->data->waitIdle();
        nextRuntime->data->activate();
        commitLoadedScript(
            std::move(nextRuntime), std::move(loadedScript), snapshot.controlValues, path, nextProtocolDirectory);
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
