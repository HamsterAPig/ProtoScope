#include "script_callback_dispatcher.hpp"
#include "script_host_internal.hpp"

namespace protoscope::scripting {

void ScriptHost::callbackOnOpen(const ScriptHostContext& ctx) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_open"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection));
    if (!result.valid()) {
        protoLog("error", "on_open 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnClose(const ScriptHostContext& ctx) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_close"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection));
    if (!result.valid()) {
        protoLog("error", "on_close 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnError(const ScriptHostContext& ctx, const std::string& message) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_error"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection), message);
    if (!result.valid()) {
        protoLog("error", "on_error 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnBytes(const ScriptHostContext& ctx, const std::vector<std::uint8_t>& bytes) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_bytes"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection), makeBytesTable(view, bytes));
    if (!result.valid()) {
        protoLog("error", "on_bytes 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnStreamFrame(const ScriptHostContext& ctx, const StreamParsedFrame& frame) {
    if (!scriptLoaded_ || !runtime_->stream) {
        return;
    }

    const auto callbackIter = runtime_->stream->frameCallbacks.find(frame.name);
    if (callbackIter == runtime_->stream->frameCallbacks.end()) {
        return;
    }

    sol::state_view view(runtime_->lua.lua_state());
    auto callback = callbackIter->second;
    auto result = callback(makeContextTable(view, ctx.connection), makeStreamFrameTable(view, frame));
    if (!result.valid()) {
        protoLog("error", "stream.on_frame 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnStreamError(const ScriptHostContext& ctx, const StreamParseError& error) {
    if (!scriptLoaded_ || !runtime_->stream || !runtime_->stream->onError.valid()) {
        return;
    }

    sol::state_view view(runtime_->lua.lua_state());
    auto callback = runtime_->stream->onError;
    auto result = callback(makeContextTable(view, ctx.connection), makeStreamErrorTable(view, error));
    if (!result.valid()) {
        protoLog("error", "stream.on_error 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnTimer(const ScriptHostContext& ctx, const std::string& timerName) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_timer"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection), timerName);
    if (!result.valid()) {
        protoLog("error", "on_timer 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnControl(const ScriptHostContext& ctx, const std::string& id, const ControlValue& value) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_control"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result =
        callback(makeContextTable(view, ctx.connection), id, controlValueToLua(view, findControlDescriptor(controls_, id), value));
    if (!result.valid()) {
        protoLog("error", "on_control 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnTx(const ScriptHostContext& ctx, const TxEvent& event) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_tx"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection), makeTxEventTable(view, event));
    if (!result.valid()) {
        protoLog("error", "on_tx 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnDialog(const ScriptHostContext& ctx, const DialogEvent& event) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_dialog"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection), makeDialogEventTable(view, event));
    if (!result.valid()) {
        protoLog("error", "on_dialog 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnFileDialog(const ScriptHostContext& ctx, const FileDialogEvent& event) {
    if (!scriptLoaded_) {
        return;
    }
    sol::state_view view(runtime_->lua.lua_state());
    const sol::object callbackObject = runtime_->lua["on_file_dialog"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }
    auto callback = callbackObject.as<sol::protected_function>();
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection), makeFileDialogEventTable(view, event));
    if (!result.valid()) {
        protoLog("error", "on_file_dialog 执行失败: " + protectedCallError(result));
    }
}

} // namespace protoscope::scripting
