#include "protoscope/scripting/script_host.hpp"

#include <sol/sol.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <sstream>
#include <string_view>

namespace protoscope::scripting {

namespace {

constexpr const char* kDefaultDockId = "protocol";
constexpr const char* kDefaultDockTitle = "协议动作";

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
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
    case sol::type::string:
        return "string";
    case sol::type::number:
        return "number";
    case sol::type::boolean:
        return "boolean";
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
    case sol::type::string:
        return object.as<std::string>();
    case sol::type::boolean:
        return object.as<bool>() ? "true" : "false";
    case sol::type::number: {
        std::ostringstream builder;
        builder << object.as<double>();
        return builder.str();
    }
    case sol::type::table: {
        if (depth >= 3) {
            return "{...}";
        }
        std::ostringstream builder;
        builder << "{";
        bool first = true;
        for (const auto& pair : object.as<sol::table>()) {
            if (!first) {
                builder << ", ";
            }
            first = false;
            builder << serializeLuaObject(pair.first, depth + 1) << "=" << serializeLuaObject(pair.second, depth + 1);
        }
        builder << "}";
        return builder.str();
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
        break;
    case ControlType::InputText:
        if (object.is<std::string>()) {
            return object.as<std::string>();
        }
        break;
    case ControlType::InputInt:
        if (object.is<int>()) {
            return object.as<int>();
        }
        if (object.is<double>()) {
            return static_cast<int>(object.as<double>());
        }
        break;
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
        break;
    case ControlType::Combo: {
        int index = 0;
        if (object.is<int>()) {
            index = object.as<int>();
        } else if (object.is<double>()) {
            index = static_cast<int>(object.as<double>());
        } else {
            error = "combo 控件仅支持 number";
            return std::nullopt;
        }
        if (descriptor.comboOptions.empty()) {
            return 0;
        }
        return std::clamp(index, 0, static_cast<int>(descriptor.comboOptions.size()) - 1);
    }
    }

    error = "控件 " + descriptor.id + " 类型不匹配，实际收到 " + luaTypeName(object.get_type());
    return std::nullopt;
}

sol::object controlValueToLua(sol::state_view lua,
                              const ControlDescriptor* descriptor,
                              const ControlValue& value) {
    if (descriptor == nullptr) {
        return sol::make_object(lua, sol::lua_nil);
    }

    return std::visit(
        [&lua](const auto& current) -> sol::object {
            return sol::make_object(lua, current);
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
        error = "控件项必须是 table";
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

std::optional<std::vector<ControlDescriptor>> parseControlList(const sol::object& object, std::string& error) {
    if (!object.is<sol::table>()) {
        error = "控件列表必须返回 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    std::vector<ControlDescriptor> controls;
    controls.reserve(table.size());
    for (std::size_t index = 1; index <= table.size(); ++index) {
        auto descriptor = parseControlDescriptor(table[index], error);
        if (!descriptor.has_value()) {
            return std::nullopt;
        }
        controls.push_back(std::move(*descriptor));
    }
    return controls;
}

std::optional<std::vector<DockDescriptor>> parseDockDescriptors(sol::state_view lua,
                                                                std::string& error) {
    const sol::object uiObject = lua["ui"];
    if (uiObject.valid() && uiObject.get_type() != sol::type::lua_nil) {
        if (!uiObject.is<sol::protected_function>()) {
            error = "ui 必须是 function";
            return std::nullopt;
        }

        auto uiFunction = uiObject.as<sol::protected_function>();
        auto uiResult = uiFunction();
        if (!uiResult.valid()) {
            error = "ui() 执行失败";
            return std::nullopt;
        }

        const sol::object docksObject = uiResult.get<sol::object>();
        if (!docksObject.is<sol::table>()) {
            error = "ui() 必须返回 table";
            return std::nullopt;
        }

        const auto dockTable = docksObject.as<sol::table>();
        std::vector<DockDescriptor> docks;
        docks.reserve(dockTable.size());
        for (std::size_t index = 1; index <= dockTable.size(); ++index) {
            const sol::object dockObject = dockTable[index];
            if (!dockObject.is<sol::table>()) {
                error = "ui() 每个 dock 都必须是 table";
                return std::nullopt;
            }

            const auto dockEntry = dockObject.as<sol::table>();
            DockDescriptor dock;
            dock.id = dockEntry.get_or("id", std::string());
            dock.title = dockEntry.get_or("title", std::string());
            if (dock.id.empty() || dock.title.empty()) {
                error = "dock 必须提供 id 和 title";
                return std::nullopt;
            }

            const sol::object controlsObject = dockEntry["controls"];
            auto controls = parseControlList(controlsObject, error);
            if (!controls.has_value()) {
                return std::nullopt;
            }
            dock.controls = std::move(*controls);
            docks.push_back(std::move(dock));
        }
        return docks;
    }

    const sol::object controlsObject = lua["controls"];
    if (!controlsObject.valid() || controlsObject.get_type() == sol::type::lua_nil) {
        return std::vector<DockDescriptor>{};
    }
    if (!controlsObject.is<sol::protected_function>()) {
        error = "controls 必须是 function";
        return std::nullopt;
    }

    auto controlsFunction = controlsObject.as<sol::protected_function>();
    auto controlsResult = controlsFunction();
    if (!controlsResult.valid()) {
        error = "controls() 执行失败";
        return std::nullopt;
    }

    const sol::object descriptorsObject = controlsResult.get<sol::object>();
    auto controls = parseControlList(descriptorsObject, error);
    if (!controls.has_value()) {
        return std::nullopt;
    }

    DockDescriptor dock;
    dock.id = kDefaultDockId;
    dock.title = kDefaultDockTitle;
    dock.controls = std::move(*controls);
    return std::vector<DockDescriptor>{std::move(dock)};
}

sol::table makeContextTable(sol::state_view lua, const transport::ConnectionContext& connection) {
    sol::table table = lua.create_table();
    table["kind"] = kindName(connection.kind);
    table["endpoint"] = connection.endpoint;
    table["connection_id"] = connection.connectionId;
    table["timestamp_ms"] = connection.timestampMs;
    table["ready_for_io"] = connection.readyForIo;
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

std::optional<double> luaNumberField(const sol::table& table, const char* key) {
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<double>()) {
        return std::nullopt;
    }
    return object.as<double>();
}

std::optional<std::string> luaStringField(const sol::table& table, const char* key) {
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<std::string>()) {
        return std::nullopt;
    }
    return object.as<std::string>();
}

std::optional<bool> luaBoolField(const sol::table& table, const char* key) {
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<bool>()) {
        return std::nullopt;
    }
    return object.as<bool>();
}

std::optional<PlotSetup> parsePlotSetup(const sol::object& object, std::string& error) {
    if (!object.is<sol::table>()) {
        error = "plot.setup 参数必须是 table";
        return std::nullopt;
    }
    const sol::table table = object.as<sol::table>();

    PlotSetup setup{};
    setup.source = luaStringField(table, "source").value_or("");
    setup.resetHistory = luaBoolField(table, "reset_history").value_or(false);

    const sol::object channelsObject = table["channels"];
    if (!channelsObject.is<sol::table>()) {
        error = "plot.setup.channels 必须是 table";
        return std::nullopt;
    }
    const sol::table channelsTable = channelsObject.as<sol::table>();
    for (std::size_t index = 1; index <= channelsTable.size(); ++index) {
        const sol::object channelObject = channelsTable[index];
        if (!channelObject.is<sol::table>()) {
            error = "plot.setup.channels[" + std::to_string(index) + "] 必须是 table";
            return std::nullopt;
        }
        const sol::table channelTable = channelObject.as<sol::table>();
        PlotChannelDescriptor descriptor{};
        descriptor.label = luaStringField(channelTable, "label").value_or("CH" + std::to_string(index));
        descriptor.unit = luaStringField(channelTable, "unit").value_or("");
        descriptor.offset = luaNumberField(channelTable, "offset").value_or(0.0);
        setup.channels.push_back(std::move(descriptor));
    }
    if (setup.channels.empty()) {
        error = "plot.setup.channels 不能为空";
        return std::nullopt;
    }

    setup.view.timeScale = luaNumberField(table, "time_scale").value_or(1.0);
    setup.view.timeUnit = luaStringField(table, "time_unit").value_or("s");
    setup.view.verticalMin = luaNumberField(table, "vertical_min").value_or(-1.0);
    setup.view.verticalMax = luaNumberField(table, "vertical_max").value_or(1.0);
    setup.view.verticalUnit = luaStringField(table, "vertical_unit").value_or("V");
    const double historyLimit = luaNumberField(table, "history_limit").value_or(200000.0);
    setup.view.historyLimit = historyLimit <= 1.0 ? 1U : static_cast<std::size_t>(historyLimit);
    return setup;
}

std::optional<plot::WaveAppendRequest> parsePlotAppend(const sol::object& object, std::string& error) {
    if (!object.is<sol::table>()) {
        error = "plot.push 参数必须是 table";
        return std::nullopt;
    }
    const sol::table table = object.as<sol::table>();
    plot::WaveAppendRequest request{};
    request.source = luaStringField(table, "source").value_or("");

    const sol::object samplesObject = table["samples"];
    if (!samplesObject.is<sol::table>()) {
        error = "plot.push.samples 必须是 table";
        return std::nullopt;
    }
    const sol::table samplesTable = samplesObject.as<sol::table>();
    for (std::size_t index = 1; index <= samplesTable.size(); ++index) {
        const sol::object sampleObject = samplesTable[index];
        if (!sampleObject.is<sol::table>()) {
            error = "plot.push.samples[" + std::to_string(index) + "] 必须是 table";
            return std::nullopt;
        }
        const sol::table sampleTable = sampleObject.as<sol::table>();
        const auto time = luaNumberField(sampleTable, "t");
        const auto value = luaNumberField(sampleTable, "y");
        if (!time.has_value() || !value.has_value()) {
            error = "plot.push.samples[" + std::to_string(index) + "] 必须包含数字字段 t / y";
            return std::nullopt;
        }
        request.samples.push_back(plot::WaveSample{.time = *time, .value = *value});
    }
    if (request.samples.empty()) {
        error = "plot.push.samples 不能为空";
        return std::nullopt;
    }
    return request;
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
    sol::table plotApi = runtime_->lua.create_table();
    plotApi.set_function("setup", [this](const sol::object& payload) {
        std::string error;
        const auto setup = parsePlotSetup(payload, error);
        if (!setup.has_value()) {
            protoLog("error", "proto.plot.setup 调用失败: " + error);
            return;
        }
        protoPlotSetup(*setup);
    });
    plotApi.set_function("push", [this](int channelIndex, const sol::object& payload) {
        if (channelIndex <= 0) {
            protoLog("error", "proto.plot.push 调用失败: channelIndex 必须从 1 开始");
            return;
        }
        std::string error;
        const auto request = parsePlotAppend(payload, error);
        if (!request.has_value()) {
            protoLog("error", "proto.plot.push 调用失败: " + error);
            return;
        }
        protoPlotPush(static_cast<std::size_t>(channelIndex - 1), *request);
    });
    proto["plot"] = plotApi;
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
            protoLog("warn", "proto.set_control 调用失败: " + error);
            return;
        }
        controlValues_[id] = *converted;
    });
    proto.set_function("crc16_modbus", [](const sol::object& payload) -> std::uint16_t {
        std::string error;
        const auto bytes = bytesFromLuaObject(payload, error);
        return bytes.has_value() ? protocol_utils::crc16Modbus(*bytes) : 0U;
    });
    proto.set_function("crc16_ccitt_false", [](const sol::object& payload) -> std::uint16_t {
        std::string error;
        const auto bytes = bytesFromLuaObject(payload, error);
        return bytes.has_value() ? protocol_utils::crc16CcittFalse(*bytes) : 0U;
    });
    proto.set_function("crc32_ieee", [](const sol::object& payload) -> std::uint32_t {
        std::string error;
        const auto bytes = bytesFromLuaObject(payload, error);
        return bytes.has_value() ? protocol_utils::crc32Ieee(*bytes) : 0U;
    });

    auto scriptResult = lua.safe_script_file(path, &sol::script_pass_on_error);
    if (!scriptResult.valid()) {
        setLastError("执行脚本失败: " + protectedCallError(scriptResult));
        protoLog("error", lastError_);
        return false;
    }

    std::string parseError;
    const auto parsedDocks = parseDockDescriptors(lua, parseError);
    if (!parsedDocks.has_value()) {
        setLastError(parseError);
        protoLog("error", lastError_);
        return false;
    }

    docks_ = *parsedDocks;
    controls_.clear();
    std::unordered_map<std::string, ControlValue> nextControlValues;
    for (const auto& dock : docks_) {
        for (const auto& control : dock.controls) {
            controls_.push_back(control);
            const auto existing = controlValues_.find(control.id);
            nextControlValues[control.id] = existing == controlValues_.end() ? defaultValueFor(control) : existing->second;
        }
    }
    controlValues_ = std::move(nextControlValues);
    lastError_.clear();
    scriptLoaded_ = true;
    return true;
}

bool ScriptHost::loadProtocolDirectory(const std::string& directory) {
    const auto path = std::filesystem::path(directory) / "main.lua";
    return loadScriptFile(path.generic_string());
}

void ScriptHost::resetRuntime() {
    scriptLoaded_ = false;
    lastError_.clear();
    docks_.clear();
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
    if (activeConnection_.has_value() && activeConnection_->connectionId == event.context.connectionId) {
        activeConnection_.reset();
    }
}

void ScriptHost::onTransportError(const transport::TransportErrorEvent& event) {
    callbackOnError(ScriptHostContext{event.context}, event.message);
}

void ScriptHost::onTransportBytes(const transport::TransportBytesEvent& event) {
    if (event.context.readyForIo) {
        activeConnection_ = event.context;
    }
    callbackOnBytes(ScriptHostContext{event.context}, event.bytes);
}

void ScriptHost::onControl(const transport::ConnectionContext& ctx, const std::string& id, const ControlValue& value) {
    const auto* descriptor = findControlDescriptor(controls_, id);
    if (descriptor == nullptr) {
        return;
    }
    controlValues_[id] = value;
    callbackOnControl(ScriptHostContext{ctx}, id, value);
}

void ScriptHost::tick(std::uint64_t currentMs) {
    std::vector<std::string> dueTimers;
    dueTimers.reserve(timers_.size());
    for (const auto& [name, timer] : timers_) {
        if (timer.active && currentMs >= timer.dueAtMs) {
            dueTimers.push_back(name);
        }
    }

    for (const auto& name : dueTimers) {
        auto iter = timers_.find(name);
        if (iter != timers_.end()) {
            iter->second.active = false;
        }
        if (activeConnection_.has_value()) {
            callbackOnTimer(ScriptHostContext{*activeConnection_}, name);
        } else {
            transport::ConnectionContext context;
            context.kind = transport::TransportKind::TcpClient;
            context.endpoint = "detached";
            context.connectionId = 0;
            context.timestampMs = currentMs;
            context.readyForIo = false;
            callbackOnTimer(ScriptHostContext{context}, name);
        }
    }
}

std::vector<ControlDescriptor> ScriptHost::controlsSnapshot() const {
    return controls_;
}

std::vector<ControlSnapshot> ScriptHost::controlStatesSnapshot() const {
    std::vector<ControlSnapshot> snapshot;
    snapshot.reserve(controls_.size());
    for (const auto& control : controls_) {
        const auto iter = controlValues_.find(control.id);
        snapshot.push_back(ControlSnapshot{
            .descriptor = control,
            .value = iter == controlValues_.end() ? defaultValueFor(control) : iter->second,
        });
    }
    return snapshot;
}

std::vector<DockDescriptor> ScriptHost::dockDescriptorsSnapshot() const {
    return docks_;
}

std::vector<DockSnapshot> ScriptHost::dockSnapshots() const {
    std::vector<DockSnapshot> docks;
    docks.reserve(docks_.size());
    for (const auto& dock : docks_) {
        DockSnapshot snapshot;
        snapshot.descriptor = dock;
        snapshot.controls.reserve(dock.controls.size());
        for (const auto& control : dock.controls) {
            const auto iter = controlValues_.find(control.id);
            snapshot.controls.push_back(ControlSnapshot{
                .descriptor = control,
                .value = iter == controlValues_.end() ? defaultValueFor(control) : iter->second,
            });
        }
        docks.push_back(std::move(snapshot));
    }
    return docks;
}

std::vector<ScriptEvent> ScriptHost::drainEvents() {
    auto drained = std::move(events_);
    events_.clear();
    return drained;
}

std::vector<ScriptLog> ScriptHost::drainLogs() {
    auto drained = std::move(logs_);
    logs_.clear();
    return drained;
}

std::vector<std::vector<std::uint8_t>> ScriptHost::drainSendQueue() {
    auto drained = std::move(sendQueue_);
    sendQueue_.clear();
    return drained;
}

std::vector<PlotSetup> ScriptHost::drainPlotSetups() {
    auto drained = std::move(plotSetups_);
    plotSetups_.clear();
    return drained;
}

std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> ScriptHost::drainPlotAppends() {
    auto drained = std::move(plotAppends_);
    plotAppends_.clear();
    return drained;
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

void ScriptHost::protoPlotSetup(const PlotSetup& setup) {
    plotSetups_.push_back(setup);
}

void ScriptHost::protoPlotPush(std::size_t channelIndex, const plot::WaveAppendRequest& request) {
    plotAppends_.push_back(std::make_pair(channelIndex, request));
}

std::string ScriptHost::valueToString(const ControlValue& value) {
    return std::visit(
        [](const auto& current) {
            std::ostringstream builder;
            builder << current;
            return builder.str();
        },
        value);
}

void ScriptHost::setLastError(std::string message) {
    lastError_ = std::move(message);
}

} // namespace protoscope::scripting
