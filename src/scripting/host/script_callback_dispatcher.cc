#include "script_callback_dispatcher.hpp"

#include "script_host_internal.hpp"

#include <exception>
#include <optional>
#include <string>

namespace protoscope::scripting {

std::optional<sol::protected_function> ScriptHost::resolveGlobalCallback(const char* name)
{
    if (!scriptLoaded_ || !runtime_) {
        return std::nullopt;
    }

    try {
        const sol::object callbackObject = runtime_->lua[name];
        if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
            return std::nullopt;
        }
        if (!callbackObject.is<sol::protected_function>()) {
            protoLog("error", std::string(name) + " 必须是 function");
            return std::nullopt;
        }
        return callbackObject.as<sol::protected_function>();
    } catch (const std::exception& ex) {
        protoLog("error", std::string(name) + " 读取失败: " + ex.what());
    } catch (...) {
        protoLog("error", std::string(name) + " 读取失败: 未知异常");
    }
    return std::nullopt;
}

void ScriptHost::callbackOnOpen(const ScriptHostContext& ctx)
{
    const auto callback = resolveGlobalCallback("on_open");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result = (*callback)(makeContextTable(view, ctx.connection));
        if (!result.valid()) {
            protoLog("error", "on_open 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_open 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_open 执行异常: 未知异常");
    }
}

void ScriptHost::callbackOnClose(const ScriptHostContext& ctx)
{
    const auto callback = resolveGlobalCallback("on_close");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result = (*callback)(makeContextTable(view, ctx.connection));
        if (!result.valid()) {
            protoLog("error", "on_close 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_close 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_close 执行异常: 未知异常");
    }
}

void ScriptHost::callbackOnError(const ScriptHostContext& ctx, const std::string& message)
{
    const auto callback = resolveGlobalCallback("on_error");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result = (*callback)(makeContextTable(view, ctx.connection), message);
        if (!result.valid()) {
            protoLog("error", "on_error 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_error 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_error 执行异常: 未知异常");
    }
}

void ScriptHost::callbackOnBytes(const ScriptHostContext& ctx, const std::vector<std::uint8_t>& bytes)
{
    const auto callback = resolveGlobalCallback("on_bytes");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result =
            (*callback)(makeContextTable(view, ctx.connection), makeBytesTable(view, bytes));
        if (!result.valid()) {
            protoLog("error", "on_bytes 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_bytes 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_bytes 执行异常: 未知异常");
    }
}

bool ScriptHost::callbackOnStreamBatch(const ScriptHostContext& ctx, const std::vector<StreamParsedFrame>& frames)
{
    if (!scriptLoaded_ || !runtime_ || !runtime_->stream || !runtime_->stream->onBatchCallbackKey.has_value()) {
        return false;
    }
    const auto callbackIter = runtime_->streamCallbacks.find(*runtime_->stream->onBatchCallbackKey);
    if (callbackIter == runtime_->streamCallbacks.end()) {
        protoLog("error", "stream.on_batch 回调未找到: " + *runtime_->stream->onBatchCallbackKey);
        return true;
    }

    try {
        sol::state_view view(runtime_->lua.lua_state());
        auto callback = callbackIter->second;
        auto result =
            callback(makeContextTable(view, ctx.connection),
                     makeStreamFrameArrayTable(
                         view, frames, runtime_->stream->includeRawFrames, runtime_->stream->includeFieldAliases));
        if (!result.valid()) {
            protoLog("error", "stream.on_batch 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("stream.on_batch 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "stream.on_batch 执行异常: 未知异常");
    }
    return true;
}

void ScriptHost::callbackOnStreamFrame(const ScriptHostContext& ctx, const StreamParsedFrame& frame)
{
    if (!scriptLoaded_ || !runtime_ || !runtime_->stream) {
        return;
    }

    const auto callbackKeyIter = runtime_->stream->frameCallbackKeys.find(frame.name);
    if (callbackKeyIter == runtime_->stream->frameCallbackKeys.end()) {
        return;
    }
    const auto callbackIter = runtime_->streamCallbacks.find(callbackKeyIter->second);
    if (callbackIter == runtime_->streamCallbacks.end()) {
        protoLog("error", "stream.on_frame 回调未找到: " + callbackKeyIter->second);
        return;
    }

    try {
        sol::state_view view(runtime_->lua.lua_state());
        auto callback = callbackIter->second;
        auto result =
            callback(makeContextTable(view, ctx.connection),
                     makeStreamFrameTable(
                         view, frame, runtime_->stream->includeRawFrames, runtime_->stream->includeFieldAliases));
        if (!result.valid()) {
            protoLog("error", "stream.on_frame 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("stream.on_frame 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "stream.on_frame 执行异常: 未知异常");
    }
}

void ScriptHost::callbackOnStreamError(const ScriptHostContext& ctx, const StreamParseError& error)
{
    if (!scriptLoaded_ || !runtime_ || !runtime_->stream || !runtime_->stream->onErrorCallbackKey.has_value()) {
        return;
    }
    const auto callbackIter = runtime_->streamCallbacks.find(*runtime_->stream->onErrorCallbackKey);
    if (callbackIter == runtime_->streamCallbacks.end()) {
        protoLog("error", "stream.on_error 回调未找到: " + *runtime_->stream->onErrorCallbackKey);
        return;
    }

    try {
        sol::state_view view(runtime_->lua.lua_state());
        auto callback = callbackIter->second;
        auto result = callback(makeContextTable(view, ctx.connection), makeStreamErrorTable(view, error));
        if (!result.valid()) {
            protoLog("error", "stream.on_error 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("stream.on_error 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "stream.on_error 执行异常: 未知异常");
    }
}

void ScriptHost::callbackOnTimer(const ScriptHostContext& ctx, const std::string& timerName)
{
    const auto callback = resolveGlobalCallback("on_timer");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result = (*callback)(makeContextTable(view, ctx.connection), timerName);
        if (!result.valid()) {
            protoLog("error", "on_timer 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_timer 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_timer 执行异常: 未知异常");
    }
}

void ScriptHost::callbackOnControl(const ScriptHostContext& ctx, const std::string& id, const ControlValue& value)
{
    const auto callback = resolveGlobalCallback("on_control");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result =
            (*callback)(makeContextTable(view, ctx.connection),
                        id,
                        controlValueToLua(view, findControlDescriptor(controls_, id), value));
        if (!result.valid()) {
            protoLog("error", "on_control 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_control 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_control 执行异常: 未知异常");
    }
}

bool ScriptHost::callbackOnOscilloscopeToggle(const ScriptHostContext& ctx, bool currentRunning, bool targetRunning)
{
    const auto callback = resolveGlobalCallback("on_oscilloscope_toggle");
    if (!callback.has_value()) {
        return false;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result =
            (*callback)(makeContextTable(view, ctx.connection), currentRunning, targetRunning);
        if (!result.valid()) {
            protoLog("error", "on_oscilloscope_toggle 执行失败: " + protectedCallError(result));
            return false;
        }
        const sol::object returned = result.get<sol::object>();
        if (!returned.is<bool>()) {
            protoLog("error", "on_oscilloscope_toggle 必须返回 boolean");
            return false;
        }
        return returned.as<bool>();
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_oscilloscope_toggle 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_oscilloscope_toggle 执行异常: 未知异常");
    }
    return false;
}

void ScriptHost::callbackOnTx(const ScriptHostContext& ctx, const TxEvent& event)
{
    const auto callback = resolveGlobalCallback("on_tx");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result =
            (*callback)(makeContextTable(view, ctx.connection), makeTxEventTable(view, event));
        if (!result.valid()) {
            protoLog("error", "on_tx 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_tx 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_tx 执行异常: 未知异常");
    }
}

void ScriptHost::callbackOnDialog(const ScriptHostContext& ctx, const DialogEvent& event)
{
    const auto callback = resolveGlobalCallback("on_dialog");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result =
            (*callback)(makeContextTable(view, ctx.connection), makeDialogEventTable(view, event));
        if (!result.valid()) {
            protoLog("error", "on_dialog 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_dialog 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_dialog 执行异常: 未知异常");
    }
}

void ScriptHost::callbackOnFileDialog(const ScriptHostContext& ctx, const FileDialogEvent& event)
{
    const auto callback = resolveGlobalCallback("on_file_dialog");
    if (!callback.has_value()) {
        return;
    }
    try {
        sol::state_view view(runtime_->lua.lua_state());
        sol::protected_function_result result =
            (*callback)(makeContextTable(view, ctx.connection), makeFileDialogEventTable(view, event));
        if (!result.valid()) {
            protoLog("error", "on_file_dialog 执行失败: " + protectedCallError(result));
        }
    } catch (const std::exception& ex) {
        protoLog("error", std::string("on_file_dialog 执行异常: ") + ex.what());
    } catch (...) {
        protoLog("error", "on_file_dialog 执行异常: 未知异常");
    }
}

} // namespace protoscope::scripting
