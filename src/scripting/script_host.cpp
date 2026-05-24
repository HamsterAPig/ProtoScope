#include "protoscope/scripting/script_host.hpp"

#include <sol/sol.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace protoscope::scripting {

namespace {

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

std::string kindName(transport::TransportKind kind) {
    switch (kind) {
    case transport::TransportKind::TcpClient:
        return "tcp_client";
    case transport::TransportKind::TcpServer:
        return "tcp_server";
    case transport::TransportKind::Serial:
        return "serial";
    }
    return "unknown";
}

ControlValue defaultValueFor(const ControlDescriptor& descriptor) {
    switch (descriptor.type) {
    case ControlType::Button:
        return false;
    case ControlType::InputText:
        return descriptor.textDefault;
    case ControlType::InputInt:
        return descriptor.intDefault;
    case ControlType::InputFloat:
        return descriptor.floatDefault;
    case ControlType::Checkbox:
        return descriptor.boolDefault;
    case ControlType::Combo:
        return descriptor.comboDefaultIndex;
    }
    return false;
}

const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls, const std::string& id) {
    for (const auto& control : controls) {
        if (control.id == id) {
            return &control;
        }
    }
    return nullptr;
}

std::string luaTypeName(sol::type type) {
    switch (type) {
    case sol::type::lua_nil:
        return "nil";
    case sol::type::boolean:
        return "boolean";
    case sol::type::number:
        return "number";
    case sol::type::string:
        return "string";
    case sol::type::table:
        return "table";
    case sol::type::function:
        return "function";
    default:
        return "unknown";
    }
}

std::string serializeLuaObject(const sol::object& object, int depth = 0) {
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return "nil";
    }

    switch (object.get_type()) {
    case sol::type::boolean:
        return object.as<bool>() ? "true" : "false";
    case sol::type::number: {
        std::ostringstream stream;
        stream << object.as<double>();
        return stream.str();
    }
    case sol::type::string:
        return object.as<std::string>();
    case sol::type::table: {
        if (depth >= 3) {
            return "{...}";
        }
        std::ostringstream stream;
        stream << "{";
        bool first = true;
        for (const auto& pair : object.as<sol::table>()) {
            if (!first) {
                stream << ", ";
            }
            first = false;
            stream << serializeLuaObject(pair.first, depth + 1) << "=" << serializeLuaObject(pair.second, depth + 1);
        }
        stream << "}";
        return stream.str();
    }
    default:
        return "<" + luaTypeName(object.get_type()) + ">";
    }
}

std::optional<ControlType> parseControlType(std::string_view value) {
    if (value == "button") {
        return ControlType::Button;
    }
    if (value == "input_text") {
        return ControlType::InputText;
    }
    if (value == "input_int") {
        return ControlType::InputInt;
    }
    if (value == "input_float") {
        return ControlType::InputFloat;
    }
    if (value == "checkbox") {
        return ControlType::Checkbox;
    }
    if (value == "combo") {
        return ControlType::Combo;
    }
    return std::nullopt;
}

std::optional<ControlValue> controlValueFromLua(const ControlDescriptor& descriptor,
                                                const sol::object& object,
                                                std::string& error) {
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return defaultValueFor(descriptor);
    }

    switch (descriptor.type) {
    case ControlType::Button:
    case ControlType::Checkbox:
        if (object.is<bool>()) {
            return object.as<bool>();
        }
        if (object.is<int>()) {
            return object.as<int>() != 0;
        }
        if (object.is<double>()) {
            return object.as<double>() != 0.0;
        }
        error = "期望 bool";
        return std::nullopt;
    case ControlType::InputText:
        if (object.is<std::string>()) {
            return object.as<std::string>();
        }
        error = "期望 string";
        return std::nullopt;
    case ControlType::InputInt:
        if (object.is<int>()) {
            return object.as<int>();
        }
        if (object.is<double>()) {
            return static_cast<int>(object.as<double>());
        }
        error = "期望 integer";
        return std::nullopt;
    case ControlType::InputFloat:
        if (object.is<float>()) {
            return object.as<float>();
        }
        if (object.is<double>()) {
            return static_cast<float>(object.as<double>());
        }
        if (object.is<int>()) {
            return static_cast<float>(object.as<int>());
        }
        error = "期望 number";
        return std::nullopt;
    case ControlType::Combo: {
        int index = 1;
        if (object.is<int>()) {
            index = object.as<int>();
        } else if (object.is<double>()) {
            index = static_cast<int>(object.as<double>());
        } else {
            error = "期望 integer";
            return std::nullopt;
        }

        if (descriptor.comboOptions.empty()) {
            return 0;
        }

        index = std::clamp(index, 1, static_cast<int>(descriptor.comboOptions.size()));
        return index - 1;
    }
    }

    error = "不支持的控件类型";
    return std::nullopt;
}

sol::object controlValueToLua(sol::state_view lua,
                              const ControlDescriptor* descriptor,
                              const ControlValue& value) {
    return std::visit(
        [&](const auto& current) -> sol::object {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, int>) {
                if (descriptor != nullptr && descriptor->type == ControlType::Combo) {
                    return sol::make_object(lua, current + 1);
                }
                return sol::make_object(lua, current);
            } else {
                return sol::make_object(lua, current);
            }
        },
        value);
}

std::optional<std::vector<std::uint8_t>> bytesFromLuaObject(const sol::object& object, std::string& error) {
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return std::vector<std::uint8_t>{};
    }

    if (object.is<std::string>()) {
        const auto parsed = protocol_utils::hexToBytes(object.as<std::string>());
        if (!parsed.has_value()) {
            error = "HEX 字符串解析失败";
        }
        return parsed;
    }

    if (!object.is<sol::table>()) {
        error = "仅支持 hex 字符串或 number[]";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    std::vector<std::uint8_t> result;
    result.reserve(table.size());
    for (std::size_t index = 1; index <= table.size(); ++index) {
        const sol::object value = table[index];
        if (!value.valid() || (!value.is<int>() && !value.is<double>())) {
            error = "number[] 中存在非数字元素";
            return std::nullopt;
        }
        int byte = value.is<int>() ? value.as<int>() : static_cast<int>(value.as<double>());
        if (byte < 0 || byte > 255) {
            error = "number[] 元素必须在 0-255 之间";
            return std::nullopt;
        }
        result.push_back(static_cast<std::uint8_t>(byte));
    }
    return result;
}

std::optional<ControlDescriptor> parseControlDescriptor(const sol::object& object, std::string& error) {
    if (!object.is<sol::table>()) {
        error = "controls() 每一项都必须是 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    const std::string typeText = table.get_or("type", std::string());
    const auto controlType = parseControlType(typeText);
    if (!controlType.has_value()) {
        error = "未知控件类型: " + typeText;
        return std::nullopt;
    }

    ControlDescriptor descriptor;
    descriptor.type = *controlType;
    descriptor.id = table.get_or("id", std::string());
    descriptor.label = table.get_or("label", std::string());
    if (descriptor.id.empty() || descriptor.label.empty()) {
        error = "控件必须提供 id 和 label";
        return std::nullopt;
    }

    switch (descriptor.type) {
    case ControlType::Button:
        break;
    case ControlType::InputText:
        descriptor.textDefault = table.get_or("default", std::string());
        break;
    case ControlType::InputInt:
        descriptor.intDefault = table.get_or("default", 0);
        break;
    case ControlType::InputFloat:
        descriptor.floatDefault = table.get_or("default", 0.0F);
        break;
    case ControlType::Checkbox:
        descriptor.boolDefault = table.get_or("default", false);
        break;
    case ControlType::Combo: {
        const sol::object optionsObject = table["options"];
        if (!optionsObject.valid() || !optionsObject.is<sol::table>()) {
            error = "combo 控件必须提供 options";
            return std::nullopt;
        }

        const auto options = optionsObject.as<sol::table>();
        for (std::size_t index = 1; index <= options.size(); ++index) {
            const sol::object option = options[index];
            if (!option.is<std::string>()) {
                error = "combo options 必须全部是 string";
                return std::nullopt;
            }
            descriptor.comboOptions.push_back(option.as<std::string>());
        }

        if (descriptor.comboOptions.empty()) {
            error = "combo options 不能为空";
            return std::nullopt;
        }

        const int defaultIndex = table.get_or("default", 1);
        descriptor.comboDefaultIndex =
            std::clamp(defaultIndex, 1, static_cast<int>(descriptor.comboOptions.size())) - 1;
        break;
    }
    }

    return descriptor;
}

sol::table makeContextTable(sol::state_view lua, const transport::ConnectionContext& connection) {
    sol::table table = lua.create_table();
    table["kind"] = kindName(connection.kind);
    table["endpoint"] = connection.endpoint;
    table["connection_id"] = connection.connectionId;
    table["timestamp_ms"] = connection.timestampMs;
    return table;
}

sol::table makeBytesTable(sol::state_view lua, const std::vector<std::uint8_t>& bytes) {
    sol::table table = lua.create_table(static_cast<int>(bytes.size()), 0);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        table[index + 1] = bytes[index];
    }
    return table;
}

std::string protectedCallError(sol::protected_function_result& result) {
    sol::error error = result;
    return error.what();
}

} // namespace

struct ScriptHost::Runtime {
    sol::state lua;
};

ScriptHost::ScriptHost()
    : runtime_(std::make_unique<Runtime>()) {}

ScriptHost::~ScriptHost() = default;

bool ScriptHost::loadScriptFile(const std::string& path) {
    resetRuntime();

    scriptPath_ = path;
    const std::filesystem::path filePath(path);
    protocolDirectory_ = filePath.parent_path().generic_string();

    if (path.empty()) {
        setLastError("脚本路径为空");
        protoLog("error", lastError_);
        return false;
    }

    if (!std::filesystem::exists(filePath)) {
        setLastError("未找到脚本文件: " + path);
        protoLog("error", lastError_);
        return false;
    }

    runtime_ = std::make_unique<Runtime>();
    runtime_->lua.open_libraries(
        sol::lib::base,
        sol::lib::math,
        sol::lib::package,
        sol::lib::string,
        sol::lib::table,
        sol::lib::utf8,
        sol::lib::os);

    auto& lua = runtime_->lua;
    auto proto = lua.create_named_table("proto");

    // 核心流程：所有脚本侧能力统一经由 proto.* 回调回宿主，避免脚本直接接触底层 I/O 与 UI 状态。
    proto.set_function("log", [this](const std::string& level, const std::string& message) {
        protoLog(level, message);
    });
    proto.set_function("send", [this](const sol::object& payload) {
        std::string error;
        const auto maybeBytes = bytesFromLuaObject(payload, error);
        if (!maybeBytes.has_value()) {
            protoLog("error", "proto.send 调用失败: " + error);
            return;
        }
        protoSend(*maybeBytes);
    });
    proto.set_function("emit", [this](const std::string& name, const sol::object& payload) {
        protoEmit(name, serializeLuaObject(payload));
    });
    proto.set_function("set_timer", [this](const std::string& name, std::uint64_t delayMs) {
        protoSetTimer(name, delayMs);
    });
    proto.set_function("cancel_timer", [this](const std::string& name) {
        protoCancelTimer(name);
    });
    proto.set_function("get_control", [this](const std::string& id) {
        sol::state_view view(runtime_->lua.lua_state());
        const auto iter = controlValues_.find(id);
        if (iter == controlValues_.end()) {
            return sol::make_object(view, sol::lua_nil);
        }
        return controlValueToLua(view, findControlDescriptor(controls_, id), iter->second);
    });
    proto.set_function("set_control", [this](const std::string& id, const sol::object& value) {
        const auto* descriptor = findControlDescriptor(controls_, id);
        if (descriptor == nullptr) {
            protoLog("warn", "proto.set_control 未找到控件: " + id);
            return;
        }

        std::string error;
        const auto converted = controlValueFromLua(*descriptor, value, error);
        if (!converted.has_value()) {
            protoLog("error", "proto.set_control 更新失败: " + id + " -> " + error);
            return;
        }
        controlValues_[id] = *converted;
    });

    sol::protected_function_result scriptResult = lua.safe_script_file(path, sol::script_pass_on_error);
    if (!scriptResult.valid()) {
        setLastError("脚本装载失败: " + protectedCallError(scriptResult));
        protoLog("error", lastError_);
        return false;
    }

    const sol::object controlsObject = lua["controls"];
    if (controlsObject.valid() && controlsObject.get_type() != sol::type::lua_nil) {
        if (!controlsObject.is<sol::protected_function>()) {
            setLastError("controls 必须是 function");
            protoLog("error", lastError_);
            return false;
        }

        sol::protected_function controlsFunction = controlsObject.as<sol::protected_function>();
        sol::protected_function_result controlsResult = controlsFunction();
        if (!controlsResult.valid()) {
            setLastError("controls() 执行失败: " + protectedCallError(controlsResult));
            protoLog("error", lastError_);
            return false;
        }

        const sol::object descriptorsObject = controlsResult.get<sol::object>();
        if (!descriptorsObject.is<sol::table>()) {
            setLastError("controls() 必须返回 table");
            protoLog("error", lastError_);
            return false;
        }

        std::vector<ControlDescriptor> nextControls;
        std::unordered_map<std::string, ControlValue> nextControlValues;
        const auto descriptors = descriptorsObject.as<sol::table>();
        for (std::size_t index = 1; index <= descriptors.size(); ++index) {
            std::string error;
            const auto descriptor = parseControlDescriptor(descriptors[index], error);
            if (!descriptor.has_value()) {
                setLastError("controls() 定义非法: " + error);
                protoLog("error", lastError_);
                return false;
            }

            nextControls.push_back(*descriptor);
            nextControlValues[descriptor->id] = defaultValueFor(*descriptor);
        }

        controls_ = std::move(nextControls);
        controlValues_ = std::move(nextControlValues);
    }

    scriptLoaded_ = true;
    setLastError({});
    protoLog("info", "已加载 Lua 脚本: " + path);
    return true;
}

bool ScriptHost::loadProtocolDirectory(const std::string& directory) {
    const std::filesystem::path root(directory);
    protocolDirectory_ = root.generic_string();
    return loadScriptFile((root / "main.lua").generic_string());
}

void ScriptHost::resetRuntime() {
    scriptLoaded_ = false;
    lastError_.clear();
    controls_.clear();
    controlValues_.clear();
    events_.clear();
    logs_.clear();
    sendQueue_.clear();
    timers_.clear();
    activeConnection_.reset();
    runtime_ = std::make_unique<Runtime>();
}

void ScriptHost::onTransportOpen(const transport::TransportOpenEvent& event) {
    activeConnection_ = event.context;
    callbackOnOpen(ScriptHostContext{event.context});
}

void ScriptHost::onTransportClose(const transport::TransportCloseEvent& event) {
    callbackOnClose(ScriptHostContext{event.context});
    timers_.clear();
    activeConnection_.reset();
}

void ScriptHost::onTransportError(const transport::TransportErrorEvent& event) {
    callbackOnError(ScriptHostContext{event.context}, event.message);
}

void ScriptHost::onTransportBytes(const transport::TransportBytesEvent& event) {
    callbackOnBytes(ScriptHostContext{event.context}, event.bytes);
}

void ScriptHost::onControl(const transport::ConnectionContext& ctx, const std::string& id, const ControlValue& value) {
    controlValues_[id] = value;
    callbackOnControl(ScriptHostContext{ctx}, id, value);
}

void ScriptHost::invokeAction(const transport::ConnectionContext& ctx, const std::string& actionName) {
    controlValues_[actionName] = true;
    callbackOnControl(ScriptHostContext{ctx}, actionName, true);
}

void ScriptHost::tick(std::uint64_t currentMs) {
    if (!scriptLoaded_ || !activeConnection_.has_value()) {
        return;
    }

    std::vector<std::string> dueNames;
    for (const auto& [name, timer] : timers_) {
        if (timer.active && currentMs >= timer.dueAtMs) {
            dueNames.push_back(name);
        }
    }

    for (const auto& name : dueNames) {
        auto iter = timers_.find(name);
        if (iter != timers_.end()) {
            iter->second.active = false;
        }
        callbackOnTimer(ScriptHostContext{*activeConnection_}, name);
    }
}

std::vector<ControlDescriptor> ScriptHost::controlsSnapshot() const {
    return controls_;
}

std::vector<ControlSnapshot> ScriptHost::controlStatesSnapshot() const {
    std::vector<ControlSnapshot> snapshots;
    snapshots.reserve(controls_.size());
    for (const auto& control : controls_) {
        const auto iter = controlValues_.find(control.id);
        snapshots.push_back(ControlSnapshot{
            .descriptor = control,
            .value = (iter == controlValues_.end()) ? defaultValueFor(control) : iter->second,
        });
    }
    return snapshots;
}

std::vector<ScriptEvent> ScriptHost::drainEvents() {
    std::vector<ScriptEvent> result;
    result.swap(events_);
    return result;
}

std::vector<ScriptLog> ScriptHost::drainLogs() {
    std::vector<ScriptLog> result;
    result.swap(logs_);
    return result;
}

std::vector<std::vector<std::uint8_t>> ScriptHost::drainSendQueue() {
    std::vector<std::vector<std::uint8_t>> result;
    result.swap(sendQueue_);
    return result;
}

std::optional<std::uint64_t> ScriptHost::nextWakeupAtMs() const {
    std::optional<std::uint64_t> nextWakeup;
    for (const auto& [_, timer] : timers_) {
        if (!timer.active) {
            continue;
        }
        if (!nextWakeup.has_value() || timer.dueAtMs < *nextWakeup) {
            nextWakeup = timer.dueAtMs;
        }
    }
    return nextWakeup;
}

const std::string& ScriptHost::scriptPath() const {
    return scriptPath_;
}

const std::string& ScriptHost::protocolDirectory() const {
    return protocolDirectory_;
}

const std::string& ScriptHost::lastError() const {
    return lastError_;
}

void ScriptHost::callbackOnOpen(const ScriptHostContext& ctx) {
    if (!scriptLoaded_ || !runtime_) {
        return;
    }

    const sol::object callbackObject = runtime_->lua["on_open"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }

    sol::protected_function callback = callbackObject.as<sol::protected_function>();
    sol::state_view view(runtime_->lua.lua_state());
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection));
    if (!result.valid()) {
        protoLog("error", "on_open 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnClose(const ScriptHostContext& ctx) {
    if (!scriptLoaded_ || !runtime_) {
        return;
    }

    const sol::object callbackObject = runtime_->lua["on_close"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }

    sol::protected_function callback = callbackObject.as<sol::protected_function>();
    sol::state_view view(runtime_->lua.lua_state());
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection));
    if (!result.valid()) {
        protoLog("error", "on_close 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnError(const ScriptHostContext& ctx, const std::string& message) {
    if (!scriptLoaded_ || !runtime_) {
        return;
    }

    const sol::object callbackObject = runtime_->lua["on_error"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }

    sol::protected_function callback = callbackObject.as<sol::protected_function>();
    sol::state_view view(runtime_->lua.lua_state());
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection), message);
    if (!result.valid()) {
        protoLog("error", "on_error 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnBytes(const ScriptHostContext& ctx, const std::vector<std::uint8_t>& bytes) {
    if (!scriptLoaded_ || !runtime_) {
        return;
    }

    const sol::object callbackObject = runtime_->lua["on_bytes"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }

    sol::protected_function callback = callbackObject.as<sol::protected_function>();
    sol::state_view view(runtime_->lua.lua_state());
    sol::protected_function_result result =
        callback(makeContextTable(view, ctx.connection), makeBytesTable(view, bytes));
    if (!result.valid()) {
        protoLog("error", "on_bytes 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnTimer(const ScriptHostContext& ctx, const std::string& timerName) {
    if (!scriptLoaded_ || !runtime_) {
        return;
    }

    const sol::object callbackObject = runtime_->lua["on_timer"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }

    sol::protected_function callback = callbackObject.as<sol::protected_function>();
    sol::state_view view(runtime_->lua.lua_state());
    sol::protected_function_result result = callback(makeContextTable(view, ctx.connection), timerName);
    if (!result.valid()) {
        protoLog("error", "on_timer 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::callbackOnControl(const ScriptHostContext& ctx, const std::string& id, const ControlValue& value) {
    if (!scriptLoaded_ || !runtime_) {
        return;
    }

    const sol::object callbackObject = runtime_->lua["on_control"];
    if (!callbackObject.valid() || callbackObject.get_type() == sol::type::lua_nil) {
        return;
    }

    sol::protected_function callback = callbackObject.as<sol::protected_function>();
    sol::state_view view(runtime_->lua.lua_state());
    sol::protected_function_result result =
        callback(makeContextTable(view, ctx.connection), id, controlValueToLua(view, findControlDescriptor(controls_, id), value));
    if (!result.valid()) {
        protoLog("error", "on_control 执行失败: " + protectedCallError(result));
    }
}

void ScriptHost::protoSend(const std::vector<std::uint8_t>& bytes) {
    sendQueue_.push_back(bytes);
}

void ScriptHost::protoLog(const std::string& level, const std::string& message) {
    logs_.push_back(ScriptLog{
        .level = level,
        .message = message,
        .timestampMs = nowMs(),
    });
}

void ScriptHost::protoEmit(const std::string& eventName, const std::string& payload) {
    events_.push_back(ScriptEvent{
        .name = eventName,
        .payload = payload,
        .timestampMs = nowMs(),
    });
}

void ScriptHost::protoSetTimer(const std::string& name, std::uint64_t intervalMs) {
    timers_[name] = TimerState{
        .name = name,
        .dueAtMs = nowMs() + intervalMs,
        .active = true,
    };
}

void ScriptHost::protoCancelTimer(const std::string& name) {
    const auto iter = timers_.find(name);
    if (iter != timers_.end()) {
        iter->second.active = false;
    }
}

std::string ScriptHost::valueToString(const ControlValue& value) {
    return std::visit(
        [](const auto& current) {
            using T = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<T, bool>) {
                return current ? std::string("true") : std::string("false");
            } else if constexpr (std::is_same_v<T, int>) {
                return std::to_string(current);
            } else if constexpr (std::is_same_v<T, float>) {
                std::ostringstream stream;
                stream << current;
                return stream.str();
            } else {
                return current;
            }
        },
        value);
}

void ScriptHost::setLastError(std::string message) {
    lastError_ = std::move(message);
}

} // namespace protoscope::scripting
