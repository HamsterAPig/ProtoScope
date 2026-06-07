#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_internal.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <sol/sol.hpp>

namespace protoscope::scripting {

std::size_t ProtoBuffer::size() const
{
    return bytes.size();
}

ProtoBuffer ProtoBuffer::slice(std::size_t offset, std::size_t size) const
{
    if (offset >= bytes.size()) {
        return {};
    }
    const auto end = std::min(bytes.size(), offset + size);
    return ProtoBuffer{std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                                 bytes.begin() + static_cast<std::ptrdiff_t>(end))};
}

std::string ProtoBuffer::toHex(std::size_t maxBytes) const
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    const auto limit = maxBytes == 0 ? bytes.size() : std::min(bytes.size(), maxBytes);
    std::string text;
    text.reserve(limit * 3);
    for (std::size_t index = 0; index < limit; ++index) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        const auto value = bytes[index];
        text.push_back(kHex[(value >> 4U) & 0x0FU]);
        text.push_back(kHex[value & 0x0FU]);
    }
    return text;
}

constexpr const char* kDefaultDockId = "protocol";
constexpr const char* kDefaultDockTitle = "协议动作";

std::string readStringField(const sol::table& table, const char* key, std::string fallback = {})
{
    // 避免 sol2 字符串字段读取在 GCC 15 内联后触发数组边界误报。
    const sol::object value = table[key];
    if (!value.valid() || value.get_type() == sol::type::lua_nil || !value.is<std::string>()) {
        return fallback;
    }
    return value.as<std::string>();
}

std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

double elapsedMilliseconds(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end)
{
    return std::chrono::duration<double, std::milli>(end - start).count();
}

StreamParseBatch makeStreamParseBatchSnapshot(const StreamParseBatch& batch, bool lowOverhead)
{
    if (!lowOverhead) {
        return batch;
    }

    StreamParseBatch snapshot;
    snapshot.errors = batch.errors;
    snapshot.bufferSize = batch.bufferSize;
    snapshot.bufferCapacity = batch.bufferCapacity;
    snapshot.nearOverflow = batch.nearOverflow;
    snapshot.overflowed = batch.overflowed;
    snapshot.droppedBytes = batch.droppedBytes;
    // 核心流程：低开销模式的实时回调仍使用完整 batch，本地调试快照只保留摘要与错误，避免成功帧大 raw/字段重复常驻。
    return snapshot;
}

std::string kindName(transport::TransportKind kind)
{
    return std::string(transport::transportKindId(kind));
}

std::string txKindName(TxRequestKind kind)
{
    switch (kind) {
        case TxRequestKind::Send:
            return "send";
        case TxRequestKind::Request:
            return "request";
    }
    return "send";
}

std::string txEventStateName(TxEventState state)
{
    switch (state) {
        case TxEventState::Sent:
            return "sent";
        case TxEventState::Completed:
            return "completed";
        case TxEventState::Failed:
            return "failed";
        case TxEventState::Timeout:
            return "timeout";
        case TxEventState::Rejected:
            return "rejected";
        case TxEventState::Dropped:
            return "dropped";
        case TxEventState::Canceled:
            return "canceled";
    }
    return "rejected";
}

std::string dialogKindName(DialogKind kind)
{
    switch (kind) {
        case DialogKind::Alert:
            return "alert";
        case DialogKind::Confirm:
            return "confirm";
    }
    return "alert";
}

std::string fileDialogKindName(FileDialogKind kind)
{
    switch (kind) {
        case FileDialogKind::OpenFile:
            return "open_file";
        case FileDialogKind::SaveFile:
            return "save_file";
        case FileDialogKind::OpenDir:
            return "open_dir";
    }
    return "open_file";
}

std::optional<std::array<float, 4>> parseColorText(std::string_view text)
{
    if (text.size() != 7 && text.size() != 9) {
        return std::nullopt;
    }
    if (text.front() != '#') {
        return std::nullopt;
    }

    auto parseComponent = [&](std::size_t offset) -> std::optional<float> {
        const std::string component{text.substr(offset, 2)};
        char* end = nullptr;
        const auto value = std::strtoul(component.c_str(), &end, 16);
        if (end == nullptr || *end != '\0' || value > 255UL) {
            return std::nullopt;
        }
        return static_cast<float>(value) / 255.0F;
    };

    const auto red = parseComponent(1);
    const auto green = parseComponent(3);
    const auto blue = parseComponent(5);
    const auto alpha = text.size() == 9 ? parseComponent(7) : std::optional<float>{1.0F};
    if (!red.has_value() || !green.has_value() || !blue.has_value() || !alpha.has_value()) {
        return std::nullopt;
    }
    return std::array<float, 4>{*red, *green, *blue, *alpha};
}

ControlValue defaultValueFor(const ControlDescriptor& descriptor)
{
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
        case ControlType::ElfSymbolCombo:
            return ElfSymbolValue{};
    }
    return false;
}

double finiteOrDefault(double value, double fallback)
{
    return std::isfinite(value) ? value : fallback;
}

std::string luaTypeName(sol::type type)
{
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

std::string serializeLuaObject(const sol::object& object, int depth)
{
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
                builder << serializeLuaObject(pair.first, depth + 1) << "="
                        << serializeLuaObject(pair.second, depth + 1);
            }
            builder << "}";
            return builder.str();
        }
        default:
            return "<" + luaTypeName(object.get_type()) + ">";
    }
}

std::optional<ControlType> parseControlType(std::string_view value)
{
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
    if (value == "elf_symbol_combo") {
        return ControlType::ElfSymbolCombo;
    }
    return std::nullopt;
}

std::optional<ControlLabelPosition> parseControlLabelPosition(std::string_view value)
{
    if (value == "left") {
        return ControlLabelPosition::Left;
    }
    if (value == "right") {
        return ControlLabelPosition::Right;
    }
    return std::nullopt;
}

bool controlAllowsEmptyLabel(ControlType type)
{
    return type == ControlType::Checkbox || type == ControlType::InputText || type == ControlType::InputInt ||
           type == ControlType::InputFloat;
}

bool isValidDockAnchor(std::string_view value)
{
    return value == "left" || value == "left_bottom" || value == "right_top" || value == "right_mid" ||
           value == "right_bottom" || value == "main_bottom";
}

std::optional<ControlValue> controlValueFromLua(const ControlDescriptor& descriptor,
                                                const sol::object& object,
                                                std::string& error)
{
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
        case ControlType::ElfSymbolCombo: {
            if (object.get_type() == sol::type::lua_nil) {
                return ElfSymbolValue{};
            }
            if (!object.is<sol::table>()) {
                error = "elf_symbol_combo 控件仅支持 table 或 nil";
                return std::nullopt;
            }

            const auto table = object.as<sol::table>();
            ElfSymbolValue symbol;
            symbol.label = readStringField(table, "label");
            symbol.value = readStringField(table, "value");
            symbol.type = readStringField(table, "type");
            if (symbol.label.empty() || symbol.value.empty() || symbol.type.empty()) {
                error = "elf_symbol_combo 控件值必须包含 label、value、type";
                return std::nullopt;
            }
            return symbol;
        }
    }

    error = "控件 " + descriptor.id + " 类型不匹配，实际收到 " + luaTypeName(object.get_type());
    return std::nullopt;
}

sol::object controlValueToLua(sol::state_view lua, const ControlDescriptor* descriptor, const ControlValue& value)
{
    if (descriptor == nullptr) {
        return sol::make_object(lua, sol::lua_nil);
    }

    if (descriptor->type == ControlType::ElfSymbolCombo) {
        const auto* symbol = std::get_if<ElfSymbolValue>(&value);
        if (symbol == nullptr || symbol->label.empty() || symbol->value.empty() || symbol->type.empty()) {
            return sol::make_object(lua, sol::lua_nil);
        }
        sol::table table = lua.create_table();
        table["label"] = symbol->label;
        table["value"] = symbol->value;
        table["type"] = symbol->type;
        return sol::make_object(lua, table);
    }

    return std::visit(
        [&lua](const auto& current) -> sol::object {
            using ValueType = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<ValueType, ElfSymbolValue>) {
                return sol::make_object(lua, sol::lua_nil);
            } else {
                return sol::make_object(lua, current);
            }
        },
        value);
}

std::optional<std::vector<std::uint8_t>> bytesFromLuaObject(const sol::object& object, std::string& error)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return std::vector<std::uint8_t>{};
    }

    if (object.get_type() == sol::type::string) {
        const auto parsed = protocol_utils::hexToBytes(object.as<std::string>());
        if (!parsed.has_value()) {
            error = "HEX 字符串解析失败";
        }
        return parsed;
    }

    if (object.get_type() == sol::type::userdata && object.is<ProtoBuffer>()) {
        return object.as<ProtoBuffer>().bytes;
    }

    if (object.get_type() != sol::type::table) {
        error = "仅支持 hex 字符串、ProtoBuffer 或 number[]";
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

std::optional<ControlDescriptor> parseControlDescriptor(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "控件项必须是 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    const std::string typeText = readStringField(table, "type");
    const auto controlType = parseControlType(typeText);
    if (!controlType.has_value()) {
        error = "未知控件类型: " + typeText;
        return std::nullopt;
    }

    ControlDescriptor descriptor;
    descriptor.type = *controlType;
    descriptor.id = readStringField(table, "id");
    descriptor.label = readStringField(table, "label");
    const sol::object labelPositionObject = table["label_position"];
    if (labelPositionObject.valid() && labelPositionObject.get_type() != sol::type::lua_nil) {
        if (!labelPositionObject.is<std::string>()) {
            error = "控件 label_position 必须是字符串 'left' 或 'right'";
            return std::nullopt;
        }
        const auto labelPosition = parseControlLabelPosition(labelPositionObject.as<std::string>());
        if (!labelPosition.has_value()) {
            error = "控件 label_position 仅支持 'left' 或 'right'";
            return std::nullopt;
        }
        descriptor.labelPosition = *labelPosition;
    }
    if (descriptor.id.empty()) {
        error = "控件必须提供 id";
        return std::nullopt;
    }
    if (descriptor.label.empty() && !controlAllowsEmptyLabel(descriptor.type)) {
        // 核心流程：只有可用控件自身形态表达含义的紧凑型控件允许隐藏可见 label。
        error = "控件必须提供 label";
        return std::nullopt;
    }

    switch (descriptor.type) {
        case ControlType::Button:
            break;
        case ControlType::InputText:
            descriptor.textDefault = readStringField(table, "default");
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
        case ControlType::ElfSymbolCombo: {
            const sol::object debounceObject = table["debounce_ms"];
            if (debounceObject.valid() && debounceObject.get_type() != sol::type::lua_nil) {
                descriptor.debounceMs = debounceObject.as<int>();
                descriptor.debounceMsConfigured = true;
            }
            const sol::object limitObject = table["limit"];
            int limit = static_cast<int>(descriptor.limit);
            if (limitObject.valid() && limitObject.get_type() != sol::type::lua_nil) {
                limit = limitObject.as<int>();
                descriptor.limitConfigured = true;
            }
            if (descriptor.debounceMs <= 0) {
                error = "elf_symbol_combo debounce_ms 必须大于 0";
                return std::nullopt;
            }
            if (limit <= 0) {
                error = "elf_symbol_combo limit 必须大于 0";
                return std::nullopt;
            }
            descriptor.limit = static_cast<std::size_t>(limit);
            break;
        }
    }

    return descriptor;
}

std::optional<std::vector<ControlDescriptor>> parseControlList(const sol::object& object, std::string& error)
{
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

bool readOptionalBoolField(
    const sol::table& table, std::string_view field, bool defaultValue, const std::string& path, std::string& error)
{
    const sol::object value = table[std::string(field)];
    if (!value.valid() || value.get_type() == sol::type::lua_nil) {
        return defaultValue;
    }
    if (!value.is<bool>()) {
        error = path + "." + std::string(field) + " 必须是 boolean";
        return defaultValue;
    }
    return value.as<bool>();
}

std::optional<float> readOptionalFloatField(
    const sol::table& table, std::string_view field, float defaultValue, const std::string& path, std::string& error)
{
    const sol::object value = table[std::string(field)];
    if (!value.valid() || value.get_type() == sol::type::lua_nil) {
        return defaultValue;
    }
    if (!value.is<double>() && !value.is<int>()) {
        error = path + "." + std::string(field) + " 必须是 number";
        return std::nullopt;
    }
    const double number = value.is<double>() ? value.as<double>() : static_cast<double>(value.as<int>());
    if (!std::isfinite(number) || number < 0.0) {
        error = path + "." + std::string(field) + " 必须是非负 number";
        return std::nullopt;
    }
    return static_cast<float>(number);
}

bool registerLayoutControlUse(const DockDescriptor& dock,
                              const std::unordered_map<std::string, std::size_t>& controlsById,
                              std::unordered_set<std::string>& usedControls,
                              const std::string& controlId,
                              const std::string& path,
                              LayoutNodeDescriptor& node,
                              std::string& error)
{
    if (controlId.empty()) {
        error = path + ".id 不能为空";
        return false;
    }
    const auto controlIter = controlsById.find(controlId);
    if (controlIter == controlsById.end()) {
        error = "dock '" + dock.id + "' 的 layout 引用了未声明控件 '" + controlId + "'";
        return false;
    }
    if (!usedControls.emplace(controlId).second) {
        error = "dock '" + dock.id + "' 的 layout 重复引用控件 '" + controlId + "'";
        return false;
    }
    node.controlId = controlId;
    node.controlIndex = controlIter->second;
    return true;
}

std::optional<LayoutNodeDescriptor> parseLayoutNode(const DockDescriptor& dock,
                                                    const sol::object& object,
                                                    const std::unordered_map<std::string, std::size_t>& controlsById,
                                                    std::unordered_set<std::string>& usedControls,
                                                    const std::string& path,
                                                    std::string& error);

std::optional<std::vector<LayoutNodeDescriptor>> parseLayoutChildren(
    const DockDescriptor& dock,
    const sol::table& table,
    const std::unordered_map<std::string, std::size_t>& controlsById,
    std::unordered_set<std::string>& usedControls,
    const std::string& path,
    std::string& error)
{
    const sol::object childrenObject = table["children"];
    if (!childrenObject.valid() || childrenObject.get_type() == sol::type::lua_nil ||
        !childrenObject.is<sol::table>()) {
        error = path + ".children 必须是非空数组";
        return std::nullopt;
    }
    const auto childrenTable = childrenObject.as<sol::table>();
    if (childrenTable.size() == 0) {
        error = path + ".children 必须是非空数组";
        return std::nullopt;
    }

    std::vector<LayoutNodeDescriptor> children;
    children.reserve(childrenTable.size());
    for (std::size_t index = 1; index <= childrenTable.size(); ++index) {
        auto child = parseLayoutNode(dock,
                                     childrenTable[index],
                                     controlsById,
                                     usedControls,
                                     path + ".children[" + std::to_string(index) + "]",
                                     error);
        if (!child.has_value()) {
            return std::nullopt;
        }
        children.push_back(std::move(*child));
    }
    return children;
}

std::optional<LayoutNodeDescriptor> parseLayoutNode(const DockDescriptor& dock,
                                                    const sol::object& object,
                                                    const std::unordered_map<std::string, std::size_t>& controlsById,
                                                    std::unordered_set<std::string>& usedControls,
                                                    const std::string& path,
                                                    std::string& error)
{
    if (!object.is<sol::table>()) {
        error = path + " 必须是 table";
        return std::nullopt;
    }

    const auto table = object.as<sol::table>();
    const sol::object typeObject = table["type"];
    if (!typeObject.valid() || typeObject.get_type() == sol::type::lua_nil || !typeObject.is<std::string>()) {
        error = path + ".type 必须是字符串";
        return std::nullopt;
    }

    const std::string type = typeObject.as<std::string>();
    LayoutNodeDescriptor node;
    if (type == "column" || type == "flow") {
        node.kind = type == "column" ? LayoutNodeKind::Column : LayoutNodeKind::Flow;
        if (type == "flow") {
            const auto spacing = readOptionalFloatField(table, "spacing", node.spacing, path, error);
            const auto runSpacing = readOptionalFloatField(table, "run_spacing", node.runSpacing, path, error);
            if (!spacing.has_value() || !runSpacing.has_value()) {
                return std::nullopt;
            }
            node.spacing = *spacing;
            node.runSpacing = *runSpacing;
        }
        auto children = parseLayoutChildren(dock, table, controlsById, usedControls, path, error);
        if (!children.has_value()) {
            return std::nullopt;
        }
        node.children = std::move(*children);
        return node;
    }
    if (type == "group" || type == "collapse") {
        const sol::object titleObject = table["title"];
        if (!titleObject.valid() || titleObject.get_type() == sol::type::lua_nil || !titleObject.is<std::string>()) {
            error = path + ".title 必须是字符串";
            return std::nullopt;
        }
        node.kind = type == "group" ? LayoutNodeKind::Group : LayoutNodeKind::Collapse;
        node.title = titleObject.as<std::string>();
        if (node.title.empty()) {
            error = path + ".title 不能为空";
            return std::nullopt;
        }
        if (type == "collapse") {
            node.defaultOpen = readOptionalBoolField(table, "default_open", true, path, error);
            if (!error.empty()) {
                return std::nullopt;
            }
        }
        auto children = parseLayoutChildren(dock, table, controlsById, usedControls, path, error);
        if (!children.has_value()) {
            return std::nullopt;
        }
        node.children = std::move(*children);
        return node;
    }
    if (type == "control") {
        node.kind = LayoutNodeKind::Control;
        const sol::object idObject = table["id"];
        if (!idObject.valid() || idObject.get_type() == sol::type::lua_nil || !idObject.is<std::string>()) {
            error = path + ".id 必须是字符串";
            return std::nullopt;
        }
        if (!registerLayoutControlUse(
                dock, controlsById, usedControls, idObject.as<std::string>(), path, node, error)) {
            return std::nullopt;
        }
        return node;
    }
    if (type == "text") {
        node.kind = LayoutNodeKind::Text;
        const sol::object textObject = table["text"];
        if (!textObject.valid() || textObject.get_type() == sol::type::lua_nil || !textObject.is<std::string>()) {
            error = path + ".text 必须是字符串";
            return std::nullopt;
        }
        node.text = textObject.as<std::string>();
        return node;
    }
    if (type == "separator") {
        node.kind = LayoutNodeKind::Separator;
        return node;
    }
    if (type == "spacer") {
        node.kind = LayoutNodeKind::Spacer;
        return node;
    }
    if (type == "table") {
        node.kind = LayoutNodeKind::Table;
        const sol::object columnsObject = table["columns"];
        if (!columnsObject.valid() || columnsObject.get_type() == sol::type::lua_nil || !columnsObject.is<double>()) {
            error = path + ".columns 必须是 >= 1 的整数";
            return std::nullopt;
        }
        const auto columnsValue = columnsObject.as<double>();
        if (!std::isfinite(columnsValue) || columnsValue < 1.0 || std::floor(columnsValue) != columnsValue) {
            error = path + ".columns 必须是 >= 1 的整数";
            return std::nullopt;
        }
        node.columns = static_cast<std::size_t>(columnsValue);
        node.borders = readOptionalBoolField(table, "borders", false, path, error);
        node.resizable = readOptionalBoolField(table, "resizable", true, path, error);
        node.rowBg = readOptionalBoolField(table, "row_bg", false, path, error);
        if (!error.empty()) {
            return std::nullopt;
        }
        node.sizing = table.get_or("sizing", std::string("stretch"));
        if (node.sizing != "stretch") {
            error = path + ".sizing 目前仅支持 'stretch'";
            return std::nullopt;
        }

        const sol::object rowsObject = table["rows"];
        if (!rowsObject.valid() || rowsObject.get_type() == sol::type::lua_nil || !rowsObject.is<sol::table>()) {
            error = path + ".rows 必须是非空数组";
            return std::nullopt;
        }
        const auto rowsTable = rowsObject.as<sol::table>();
        if (rowsTable.size() == 0) {
            error = path + ".rows 必须是非空数组";
            return std::nullopt;
        }
        node.rows.reserve(rowsTable.size());
        for (std::size_t rowIndex = 1; rowIndex <= rowsTable.size(); ++rowIndex) {
            const sol::object rowObject = rowsTable[rowIndex];
            if (!rowObject.is<sol::table>()) {
                error = path + ".rows[" + std::to_string(rowIndex) + "] 必须是 table";
                return std::nullopt;
            }
            const auto rowTable = rowObject.as<sol::table>();
            if (rowTable.size() > node.columns) {
                error = path + ".rows[" + std::to_string(rowIndex) + "] 的单元格数量不能超过 columns";
                return std::nullopt;
            }
            std::vector<LayoutNodeDescriptor> row;
            row.reserve(rowTable.size());
            for (std::size_t cellIndex = 1; cellIndex <= rowTable.size(); ++cellIndex) {
                auto cell =
                    parseLayoutNode(dock,
                                    rowTable[cellIndex],
                                    controlsById,
                                    usedControls,
                                    path + ".rows[" + std::to_string(rowIndex) + "][" + std::to_string(cellIndex) + "]",
                                    error);
                if (!cell.has_value()) {
                    return std::nullopt;
                }
                row.push_back(std::move(*cell));
            }
            node.rows.push_back(std::move(row));
        }
        return node;
    }

    error = path + ".type 不支持: " + type;
    return std::nullopt;
}

std::optional<DockLayoutDescriptor> parseDockLayout(const DockDescriptor& dock,
                                                    const sol::object& object,
                                                    std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "dock '" + dock.id + "' 的 layout 必须是 table";
        return std::nullopt;
    }

    std::unordered_map<std::string, std::size_t> controlsById;
    controlsById.reserve(dock.controls.size());
    for (std::size_t index = 0; index < dock.controls.size(); ++index) {
        const auto& control = dock.controls[index];
        if (!controlsById.emplace(control.id, index).second) {
            error = "dock '" + dock.id + "' 的控件 id 重复: " + control.id;
            return std::nullopt;
        }
    }

    std::unordered_set<std::string> usedControls;
    usedControls.reserve(dock.controls.size());
    auto root = parseLayoutNode(dock, object, controlsById, usedControls, "dock '" + dock.id + "' 的 layout", error);
    if (!root.has_value()) {
        return std::nullopt;
    }

    if (usedControls.size() != dock.controls.size()) {
        std::vector<std::string> missingControls;
        for (const auto& control : dock.controls) {
            if (!usedControls.contains(control.id)) {
                missingControls.push_back(control.id);
            }
        }
        std::ostringstream stream;
        stream << "dock '" << dock.id << "' 的 layout 缺少控件:";
        for (const auto& controlId : missingControls) {
            stream << ' ' << controlId;
        }
        error = stream.str();
        return std::nullopt;
    }

    DockLayoutDescriptor layout;
    layout.root = std::move(*root);
    return layout;
}

std::optional<DockDescriptor> parseSingleDockDescriptor(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "ui() 每个 dock 都必须是 table";
        return std::nullopt;
    }

    const auto dockEntry = object.as<sol::table>();
    DockDescriptor dock;
    dock.id = readStringField(dockEntry, "id");
    dock.title = readStringField(dockEntry, "title");
    dock.anchor = readStringField(dockEntry, "anchor", "left_bottom");
    dock.tabGroup = readStringField(dockEntry, "tab_group");
    if (dock.id.empty() || dock.title.empty()) {
        error = "dock 必须提供 id 和 title";
        return std::nullopt;
    }
    if (!isValidDockAnchor(dock.anchor)) {
        error = "dock anchor 不支持: " + dock.anchor;
        return std::nullopt;
    }

    const sol::object controlsObject = dockEntry["controls"];
    auto controls = parseControlList(controlsObject, error);
    if (!controls.has_value()) {
        return std::nullopt;
    }
    dock.controls = std::move(*controls);

    const sol::object layoutObject = dockEntry["layout"];
    if (layoutObject.valid() && layoutObject.get_type() != sol::type::lua_nil) {
        auto layout = parseDockLayout(dock, layoutObject, error);
        if (!layout.has_value()) {
            return std::nullopt;
        }
        dock.layout = std::move(*layout);
    }
    return dock;
}

std::optional<std::vector<DockDescriptor>> parseDockDescriptorList(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "ui() 必须返回 table";
        return std::nullopt;
    }

    const auto dockTable = object.as<sol::table>();
    std::vector<DockDescriptor> docks;
    docks.reserve(dockTable.size());
    for (std::size_t index = 1; index <= dockTable.size(); ++index) {
        auto dock = parseSingleDockDescriptor(dockTable[index], error);
        if (!dock.has_value()) {
            return std::nullopt;
        }
        docks.push_back(std::move(*dock));
    }
    return docks;
}

std::optional<std::vector<DockDescriptor>> parseUiDockDescriptors(const sol::object& uiObject, std::string& error)
{
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
    return parseDockDescriptorList(docksObject, error);
}

std::optional<std::vector<DockDescriptor>> parseDefaultDockDescriptors(const sol::object& controlsObject,
                                                                       std::string& error)
{
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

std::optional<std::vector<DockDescriptor>> parseDockDescriptors(sol::state_view lua, std::string& error)
{
    const sol::object uiObject = lua["ui"];
    if (uiObject.valid() && uiObject.get_type() != sol::type::lua_nil) {
        return parseUiDockDescriptors(uiObject, error);
    }

    return parseDefaultDockDescriptors(lua["controls"], error);
}

sol::table makeContextTable(sol::state_view lua, const transport::ConnectionContext& connection)
{
    sol::table table = lua.create_table();
    table["kind"] = kindName(connection.kind);
    table["endpoint"] = connection.endpoint;
    table["connection_id"] = connection.connectionId;
    table["timestamp_ms"] = connection.timestampMs;
    table["ready_for_io"] = connection.readyForIo;
    return table;
}

sol::table makeBytesTable(sol::state_view lua, const std::vector<std::uint8_t>& bytes)
{
    sol::table table = lua.create_table(static_cast<int>(bytes.size()), 0);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        table[index + 1] = bytes[index];
    }
    return table;
}

sol::table makeTxEventTable(sol::state_view lua, const TxEvent& event)
{
    sol::table table = lua.create_table();
    table["id"] = event.id;
    table["kind"] = txKindName(event.kind);
    table["state"] = txEventStateName(event.state);
    table["tag"] = event.tag;
    table["bytes"] = static_cast<int>(event.bytes);
    table["queued_ms"] = event.queuedMs;
    table["finished_ms"] = event.finishedMs;
    if (event.guarded) {
        table["guarded"] = true;
        table["attempt"] = static_cast<int>(event.attempt);
        table["max_attempts"] = static_cast<int>(event.maxAttempts);
    }
    if (event.guardState.has_value()) {
        table["guard_state"] = *event.guardState;
    }
    if (event.fileJobId != 0) {
        table["file_job_id"] = event.fileJobId;
        table["offset"] = event.offset;
        table["total"] = event.total;
        table["progress"] = event.progress;
    }
    if (event.error.has_value()) {
        table["error"] = *event.error;
    }
    return table;
}

sol::table makeDialogEventTable(sol::state_view lua, const DialogEvent& event)
{
    sol::table table = lua.create_table();
    table["id"] = event.id;
    table["kind"] = dialogKindName(event.kind);
    table["state"] = event.state;
    table["title"] = event.title;
    table["message"] = event.message;
    table["level"] = event.level;
    table["dedupe_key"] = event.dedupeKey;
    table["timestamp_ms"] = event.timestampMs;
    if (event.confirmed.has_value()) {
        table["confirmed"] = *event.confirmed;
    }
    return table;
}

sol::table makeFileDialogEventTable(sol::state_view lua, const FileDialogEvent& event)
{
    sol::table table = lua.create_table();
    table["id"] = event.id;
    table["kind"] = fileDialogKindName(event.kind);
    table["state"] = event.state;
    table["timestamp_ms"] = event.timestampMs;
    if (!event.path.empty()) {
        table["path"] = event.path;
    }
    if (!event.error.empty()) {
        table["error"] = event.error;
    }
    return table;
}

std::filesystem::path canonicalPath(const std::filesystem::path& path)
{
    std::error_code errorCode;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path), errorCode);
    if (errorCode) {
        canonical = std::filesystem::absolute(path).lexically_normal();
    }
    return canonical;
}

std::string comparablePath(std::filesystem::path path)
{
    path = path.lexically_normal();
    auto text = path.generic_string();
#if defined(_WIN32)
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return text;
}

bool isSameOrChildPath(const std::filesystem::path& root, const std::filesystem::path& path, bool recursive)
{
    const auto rootText = comparablePath(root);
    const auto pathText = comparablePath(path);
    if (rootText == pathText) {
        return true;
    }
    if (!recursive || rootText.empty()) {
        return false;
    }
    const auto prefix = rootText.back() == '/' ? rootText : rootText + "/";
    return pathText.rfind(prefix, 0) == 0;
}

std::size_t positiveSizeOrDefault(std::size_t value, std::size_t fallback)
{
    return value == 0 ? fallback : value;
}

std::string protectedCallError(sol::protected_function_result& result)
{
    sol::error error = result;
    return error.what();
}

std::optional<double> luaNumberField(const sol::table& table, const char* key)
{
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<double>()) {
        return std::nullopt;
    }
    return object.as<double>();
}

std::optional<std::string> luaStringField(const sol::table& table, const char* key)
{
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<std::string>()) {
        return std::nullopt;
    }
    return object.as<std::string>();
}

std::optional<bool> luaBoolField(const sol::table& table, const char* key)
{
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil || !object.is<bool>()) {
        return std::nullopt;
    }
    return object.as<bool>();
}

std::optional<DialogWindowOptions> luaDialogWindowOptions(const sol::table& table, std::string& error)
{
    const sol::object windowObject = table["window"];
    if (!windowObject.valid() || windowObject.get_type() == sol::type::lua_nil) {
        return DialogWindowOptions{};
    }
    if (!windowObject.is<sol::table>()) {
        error = "window 必须是 table";
        return std::nullopt;
    }

    const sol::table windowTable = windowObject.as<sol::table>();
    DialogWindowOptions options{};
    options.width = luaNumberField(windowTable, "width");
    options.height = luaNumberField(windowTable, "height");
    options.x = luaNumberField(windowTable, "x");
    options.y = luaNumberField(windowTable, "y");
    options.resizable = luaBoolField(windowTable, "resizable").value_or(true);
    options.movable = luaBoolField(windowTable, "movable").value_or(true);
    options.autoResize = luaBoolField(windowTable, "auto_resize").value_or(false);

    auto invalidPositiveNumber = [&](const std::optional<double>& value, const char* field) -> bool {
        if (!value.has_value()) {
            return false;
        }
        if (!std::isfinite(*value) || *value <= 0.0) {
            error = std::string("window.") + field + " 必须是正数";
            return true;
        }
        return false;
    };
    auto invalidFiniteNumber = [&](const std::optional<double>& value, const char* field) -> bool {
        if (!value.has_value()) {
            return false;
        }
        if (!std::isfinite(*value)) {
            error = std::string("window.") + field + " 必须是有限数值";
            return true;
        }
        return false;
    };

    if (invalidPositiveNumber(options.width, "width") || invalidPositiveNumber(options.height, "height") ||
        invalidFiniteNumber(options.x, "x") || invalidFiniteNumber(options.y, "y")) {
        return std::nullopt;
    }
    return options;
}

std::optional<FileDialogKind> resolveFileDialogKind(FileDialogKind kind, const sol::table& table, std::string& error)
{
    if (kind != FileDialogKind::OpenFile) {
        return kind;
    }

    const auto mode = luaStringField(table, "mode").value_or("open");
    if (mode == "save") {
        return FileDialogKind::SaveFile;
    }
    if (mode == "open") {
        return FileDialogKind::OpenFile;
    }

    error = "mode 必须是 open 或 save";
    return std::nullopt;
}

transport::ConnectionContext fileDialogConnectionContext(
    const std::optional<transport::ConnectionContext>& activeConnection, std::uint64_t createdAtMs)
{
    if (activeConnection.has_value()) {
        return *activeConnection;
    }

    transport::ConnectionContext connection{};
    connection.timestampMs = createdAtMs;
    connection.endpoint = "detached";
    return connection;
}

FileDialogFilter parseFileDialogFilter(const sol::table& filterTable)
{
    FileDialogFilter filter;
    filter.name = luaStringField(filterTable, "name").value_or("Files");

    const sol::object patternsObject = filterTable["patterns"];
    if (patternsObject.is<sol::table>()) {
        const auto patterns = patternsObject.as<sol::table>();
        for (std::size_t patternIndex = 1; patternIndex <= patterns.size(); ++patternIndex) {
            const sol::object pattern = patterns[patternIndex];
            if (pattern.is<std::string>()) {
                filter.patterns.push_back(pattern.as<std::string>());
            }
        }
    }
    return filter;
}

std::optional<std::vector<FileDialogFilter>> parseFileDialogFilters(const sol::table& table, std::string& error)
{
    std::vector<FileDialogFilter> parsed;
    const sol::object filtersObject = table["filters"];
    if (!filtersObject.valid() || filtersObject.get_type() == sol::type::lua_nil) {
        return parsed;
    }
    if (!filtersObject.is<sol::table>()) {
        error = "filters 必须是数组";
        return std::nullopt;
    }

    const auto filters = filtersObject.as<sol::table>();
    for (std::size_t index = 1; index <= filters.size(); ++index) {
        const sol::object filterObject = filters[index];
        if (!filterObject.is<sol::table>()) {
            error = "filters 元素必须是 table";
            return std::nullopt;
        }
        parsed.push_back(parseFileDialogFilter(filterObject.as<sol::table>()));
    }
    return parsed;
}

FileDialogRequest makeFileDialogRequest(std::uint64_t id,
                                        FileDialogKind kind,
                                        transport::ConnectionContext connection,
                                        const sol::table& table,
                                        std::vector<FileDialogFilter> filters,
                                        std::uint64_t createdAtMs)
{
    return FileDialogRequest{
        .id = id,
        .kind = kind,
        .connection = std::move(connection),
        .title = luaStringField(table, "title").value_or(kind == FileDialogKind::OpenDir ? "选择目录" : "选择文件"),
        .defaultPath = luaStringField(table, "default_path").value_or("."),
        .filters = std::move(filters),
        .createdAtMs = createdAtMs,
    };
}

std::optional<std::int64_t> luaIntegerValue(const sol::object& object)
{
    if (!object.valid() || object.get_type() == sol::type::lua_nil) {
        return std::nullopt;
    }
    if (object.is<int>()) {
        return object.as<int>();
    }
    if (object.is<double>()) {
        return static_cast<std::int64_t>(object.as<double>());
    }
    return std::nullopt;
}

PlotChannelDescriptor makePlotChannelDescriptor(const sol::table& channelTable, std::size_t index)
{
    PlotChannelDescriptor descriptor{};
    descriptor.label = luaStringField(channelTable, "label").value_or("CH" + std::to_string(index));
    descriptor.unit = luaStringField(channelTable, "unit").value_or("");
    descriptor.ratio = finiteOrDefault(luaNumberField(channelTable, "ratio").value_or(1.0), 1.0);
    descriptor.scale = finiteOrDefault(luaNumberField(channelTable, "scale").value_or(1.0), 1.0);
    descriptor.offset = finiteOrDefault(luaNumberField(channelTable, "offset").value_or(0.0), 0.0);
    return descriptor;
}

bool applyPlotChannelColor(PlotChannelDescriptor& descriptor,
                           const sol::table& channelTable,
                           std::size_t index,
                           std::string& error)
{
    const auto colorText = luaStringField(channelTable, "color");
    if (!colorText.has_value()) {
        return true;
    }

    descriptor.color = parseColorText(*colorText);
    if (descriptor.color.has_value()) {
        return true;
    }

    error = "plot.setup.channels[" + std::to_string(index) + "].color 必须是 #RRGGBB 或 #RRGGBBAA";
    return false;
}

std::optional<PlotChannelDescriptor> parsePlotChannelDescriptor(const sol::object& channelObject,
                                                                std::size_t index,
                                                                std::string& error)
{
    if (!channelObject.is<sol::table>()) {
        error = "plot.setup.channels[" + std::to_string(index) + "] 必须是 table";
        return std::nullopt;
    }

    const sol::table channelTable = channelObject.as<sol::table>();
    auto descriptor = makePlotChannelDescriptor(channelTable, index);
    if (!applyPlotChannelColor(descriptor, channelTable, index, error)) {
        return std::nullopt;
    }
    return descriptor;
}

std::optional<std::vector<PlotChannelDescriptor>> parsePlotSetupChannels(const sol::table& table, std::string& error)
{
    const sol::object channelsObject = table["channels"];
    if (!channelsObject.is<sol::table>()) {
        error = "plot.setup.channels 必须是 table";
        return std::nullopt;
    }

    std::vector<PlotChannelDescriptor> channels;
    const sol::table channelsTable = channelsObject.as<sol::table>();
    channels.reserve(channelsTable.size());
    for (std::size_t index = 1; index <= channelsTable.size(); ++index) {
        auto descriptor = parsePlotChannelDescriptor(channelsTable[index], index, error);
        if (!descriptor.has_value()) {
            return std::nullopt;
        }
        channels.push_back(std::move(*descriptor));
    }
    if (channels.empty()) {
        error = "plot.setup.channels 不能为空";
        return std::nullopt;
    }
    return channels;
}

plot::ViewConfig parsePlotSetupViewConfig(const sol::table& table)
{
    plot::ViewConfig view{};
    view.timeScale = luaNumberField(table, "time_scale").value_or(1.0);
    view.timeUnit = luaStringField(table, "time_unit").value_or("s");
    view.verticalMin = luaNumberField(table, "vertical_min").value_or(-1.0);
    view.verticalMax = luaNumberField(table, "vertical_max").value_or(1.0);
    view.verticalUnit = luaStringField(table, "vertical_unit").value_or("V");
    const double historyLimit = luaNumberField(table, "history_limit").value_or(0.0);
    view.historyLimit = historyLimit <= 0.0 ? 0U : static_cast<std::size_t>(historyLimit);
    return view;
}

std::optional<PlotSetup> parsePlotSetup(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "plot.setup 参数必须是 table";
        return std::nullopt;
    }
    const sol::table table = object.as<sol::table>();
    auto channels = parsePlotSetupChannels(table, error);
    if (!channels.has_value()) {
        return std::nullopt;
    }

    // 顶层解析只负责编排字段，channel 与 view 的细节由 helper 保持内聚。
    PlotSetup setup{};
    setup.source = luaStringField(table, "source").value_or("");
    setup.resetHistory = luaBoolField(table, "reset_history").value_or(false);
    setup.channels = std::move(*channels);
    setup.view = parsePlotSetupViewConfig(table);
    return setup;
}

std::optional<plot::WaveSample> parseExpandedPlotSample(const sol::table& sampleTable,
                                                        std::size_t index,
                                                        std::string& error)
{
    const auto time = luaNumberField(sampleTable, "t");
    const auto value = luaNumberField(sampleTable, "y");
    if (!time.has_value() || !value.has_value()) {
        error = "plot.push.samples[" + std::to_string(index) + "] 必须包含数字字段 t / y";
        return std::nullopt;
    }
    return plot::WaveSample{.time = *time, .value = *value};
}

std::optional<std::vector<plot::WaveSample>> parseExpandedPlotSamples(const sol::table& samplesTable,
                                                                      std::string& error)
{
    std::vector<plot::WaveSample> samples;
    samples.reserve(samplesTable.size());
    for (std::size_t index = 1; index <= samplesTable.size(); ++index) {
        const sol::object sampleObject = samplesTable[index];
        if (!sampleObject.is<sol::table>()) {
            error = "plot.push.samples[" + std::to_string(index) + "] 必须是 table";
            return std::nullopt;
        }

        const auto sample = parseExpandedPlotSample(sampleObject.as<sol::table>(), index, error);
        if (!sample.has_value()) {
            return std::nullopt;
        }
        samples.push_back(*sample);
    }
    return samples;
}

std::optional<plot::WaveSample> parseCompactPlotSample(
    const sol::object& valueObject, double t0, double dt, std::size_t index, std::string& error)
{
    if (!valueObject.is<double>() && !valueObject.is<int>()) {
        error = "plot.push.values[" + std::to_string(index) + "] 必须是 number";
        return std::nullopt;
    }

    const double time = t0 + dt * static_cast<double>(index - 1);
    return plot::WaveSample{.time = time, .value = valueObject.as<double>()};
}

std::optional<std::vector<plot::WaveSample>> parseCompactPlotSamples(const sol::table& table, std::string& error)
{
    const sol::object valuesObject = table["values"];
    if (!valuesObject.is<sol::table>()) {
        error = "plot.push.samples 或 compact plot.push.values 必须是 table";
        return std::nullopt;
    }

    const auto t0 = luaNumberField(table, "t0");
    const auto dt = luaNumberField(table, "dt");
    if (!t0.has_value() || !dt.has_value()) {
        error = "compact plot.push 必须包含数字字段 t0 / dt";
        return std::nullopt;
    }

    std::vector<plot::WaveSample> samples;
    const sol::table valuesTable = valuesObject.as<sol::table>();
    samples.reserve(valuesTable.size());
    for (std::size_t index = 1; index <= valuesTable.size(); ++index) {
        const auto sample = parseCompactPlotSample(valuesTable[index], *t0, *dt, index, error);
        if (!sample.has_value()) {
            return std::nullopt;
        }
        samples.push_back(*sample);
    }
    return samples;
}

std::optional<std::vector<plot::WaveSample>> parsePlotAppendSamples(const sol::table& table, std::string& error)
{
    // Lua API 同时支持逐点 samples 与紧凑 values，两条路径在这里统一成 WaveSample 列表。
    const sol::object samplesObject = table["samples"];
    if (samplesObject.is<sol::table>()) {
        return parseExpandedPlotSamples(samplesObject.as<sol::table>(), error);
    }
    return parseCompactPlotSamples(table, error);
}

std::optional<plot::WaveAppendRequest> parsePlotAppend(const sol::object& object, std::string& error)
{
    if (!object.is<sol::table>()) {
        error = "plot.push 参数必须是 table";
        return std::nullopt;
    }
    const sol::table table = object.as<sol::table>();
    auto samples = parsePlotAppendSamples(table, error);
    if (!samples.has_value()) {
        return std::nullopt;
    }
    if (samples->empty()) {
        error = "plot.push 采样不能为空";
        return std::nullopt;
    }

    plot::WaveAppendRequest request{};
    request.source = luaStringField(table, "source").value_or("");
    request.samples = std::move(*samples);
    return request;
}

const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls, const std::string& id)
{
    for (const auto& control : controls) {
        if (control.id == id) {
            return &control;
        }
    }
    return nullptr;
}

namespace script_host_lua {

    std::string serializeLuaObject(const sol::object& object, int depth)
    {
        return protoscope::scripting::serializeLuaObject(object, depth);
    }

    const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls,
                                                   const std::string& id)
    {
        return protoscope::scripting::findControlDescriptor(controls, id);
    }

    std::optional<ControlValue> controlValueFromLua(const ControlDescriptor& descriptor,
                                                    const sol::object& object,
                                                    std::string& error)
    {
        return protoscope::scripting::controlValueFromLua(descriptor, object, error);
    }

    sol::object controlValueToLua(sol::state_view lua, const ControlDescriptor* descriptor, const ControlValue& value)
    {
        return protoscope::scripting::controlValueToLua(lua, descriptor, value);
    }

    std::optional<std::vector<std::uint8_t>> bytesFromLuaObject(const sol::object& object, std::string& error)
    {
        return protoscope::scripting::bytesFromLuaObject(object, error);
    }

    std::optional<PlotSetup> parsePlotSetup(const sol::object& object, std::string& error)
    {
        return protoscope::scripting::parsePlotSetup(object, error);
    }

    std::optional<plot::WaveAppendRequest> parsePlotAppend(const sol::object& object, std::string& error)
    {
        return protoscope::scripting::parsePlotAppend(object, error);
    }

} // namespace script_host_lua

ScriptHost::ScriptHost() : runtime_(std::make_unique<Runtime>()) {}

ScriptHost::~ScriptHost() = default;
ScriptHost::ScriptHost(ScriptHost&&) noexcept = default;
ScriptHost& ScriptHost::operator=(ScriptHost&&) noexcept = default;

void ScriptHost::setFileIoConfig(FileIoConfig config)
{
    fileIoConfig_ = std::move(config);
}

std::optional<StreamBufferDefinition> ScriptHost::streamBufferDefinition() const
{
    if (!runtime_ || !runtime_->stream) {
        return std::nullopt;
    }
    return runtime_->stream->parser.bufferDefinition();
}

std::vector<StreamFrameDefinition> ScriptHost::streamFrameDefinitions() const
{
    if (!runtime_ || !runtime_->stream) {
        return {};
    }
    return runtime_->stream->parser.frameDefinitions();
}

bool ScriptHost::setStreamRuntimeProfile(const sol::object& profileObject, std::string& error)
{
    if (!runtime_ || !runtime_->stream) {
        error = "当前协议未启用 stream()";
        return false;
    }
    if (!profileObject.valid() || !profileObject.is<sol::table>()) {
        error = "stream profile 必须是 table";
        return false;
    }
    const auto table = profileObject.as<sol::table>();
    const auto frameName = luaStringField(table, "frame");
    const auto lengthValue = luaIntegerValue(table["length"]);
    if (!frameName.has_value() || frameName->empty()) {
        error = "stream profile.frame 不能为空";
        return false;
    }
    if (!lengthValue.has_value() || *lengthValue <= 0) {
        error = "stream profile.length 必须是正整数";
        return false;
    }

    StreamRuntimeProfile profile;
    profile.length = static_cast<std::size_t>(*lengthValue);
    const sol::object channelMapObject = table["channel_map"];
    if (channelMapObject.valid() && channelMapObject.get_type() != sol::type::lua_nil) {
        if (!channelMapObject.is<sol::table>()) {
            error = "stream profile.channel_map 必须是数组";
            return false;
        }
        const auto channelMapTable = channelMapObject.as<sol::table>();
        profile.channelMap.reserve(channelMapTable.size());
        for (std::size_t index = 1; index <= channelMapTable.size(); ++index) {
            const auto value = luaIntegerValue(channelMapTable[index]);
            if (!value.has_value() || *value <= 0) {
                error = "stream profile.channel_map 必须是从 1 开始的正整数数组";
                return false;
            }
            profile.channelMap.push_back(static_cast<std::size_t>(*value - 1));
        }
    }

    if (!runtime_->stream->parser.setRuntimeProfile(*frameName, profile, error)) {
        return false;
    }
    runtime_->streamRuntimeProfiles.insert_or_assign(*frameName, std::move(profile));
    streamRuntimeProfileEvents_.push_back(StreamRuntimeProfileEvent{
        .cleared = false,
        .frameName = *frameName,
        .length = runtime_->streamRuntimeProfiles[*frameName].length,
        .channelMap = runtime_->streamRuntimeProfiles[*frameName].channelMap,
    });
    return true;
}

bool ScriptHost::clearStreamRuntimeProfile(const sol::object& frameNameObject, std::string& error)
{
    if (!runtime_ || !runtime_->stream) {
        error = "当前协议未启用 stream()";
        return false;
    }
    std::optional<std::string> frameName;
    if (frameNameObject.valid() && frameNameObject.get_type() != sol::type::lua_nil) {
        if (!frameNameObject.is<std::string>()) {
            error = "stream.clear_profile(frame) 仅接受字符串或 nil";
            return false;
        }
        frameName = frameNameObject.as<std::string>();
    }
    if (!runtime_->stream->parser.clearRuntimeProfile(frameName, error)) {
        return false;
    }
    if (frameName.has_value()) {
        runtime_->streamRuntimeProfiles.erase(*frameName);
        streamRuntimeProfileEvents_.push_back(StreamRuntimeProfileEvent{
            .cleared = true,
            .frameName = *frameName,
            .length = 0,
            .channelMap = {},
        });
    } else {
        runtime_->streamRuntimeProfiles.clear();
        streamRuntimeProfileEvents_.push_back(StreamRuntimeProfileEvent{
            .cleared = true,
            .frameName = {},
            .length = 0,
            .channelMap = {},
        });
    }
    return true;
}

bool ScriptHost::applyStreamRuntimeProfileEvent(const StreamRuntimeProfileEvent& event, std::string& error)
{
    if (!runtime_ || !runtime_->stream) {
        error = "当前协议未启用 stream()";
        return false;
    }
    if (event.cleared) {
        const std::optional<std::string> frameName =
            event.frameName.empty() ? std::nullopt : std::optional<std::string>{event.frameName};
        if (!runtime_->stream->parser.clearRuntimeProfile(frameName, error)) {
            return false;
        }
        if (frameName.has_value()) {
            runtime_->streamRuntimeProfiles.erase(*frameName);
        } else {
            runtime_->streamRuntimeProfiles.clear();
        }
        streamRuntimeProfileEvents_.push_back(event);
        return true;
    }
    if (event.frameName.empty() || event.length == 0U) {
        error = "stream profile.frame 和 length 不能为空";
        return false;
    }

    StreamRuntimeProfile profile{
        .length = event.length,
        .channelMap = event.channelMap,
    };
    if (!runtime_->stream->parser.setRuntimeProfile(event.frameName, profile, error)) {
        return false;
    }
    runtime_->streamRuntimeProfiles.insert_or_assign(event.frameName, std::move(profile));
    streamRuntimeProfileEvents_.push_back(event);
    return true;
}

void ScriptHost::clearAllStreamRuntimeProfiles()
{
    if (!runtime_ || !runtime_->stream) {
        return;
    }
    runtime_->stream->parser.clearRuntimeProfiles();
    runtime_->streamRuntimeProfiles.clear();
}

std::optional<StreamParseBatch> ScriptHost::lastStreamParseBatch() const
{
    if (!runtime_ || !runtime_->stream) {
        return std::nullopt;
    }
    return runtime_->stream->lastBatch;
}

void ScriptHost::resetRuntime()
{
    scriptLoaded_ = false;
    lastError_.clear();
    docks_.clear();
    controls_.clear();
    controlValues_.clear();
    events_.clear();
    logs_.clear();
    txRequests_.clear();
    timers_.clear();
    requestDoneResults_.clear();
    statusUpdates_.clear();
    dialogRequests_.clear();
    fileDialogRequests_.clear();
    fileSendJobs_.clear();
    fileHandles_.clear();
    dialogAuthorizedPaths_.clear();
    activeConnection_.reset();
    requestAwaitingCompletion_ = false;
    runtime_ = std::make_unique<Runtime>();
}

void ScriptHost::onTransportOpen(const transport::TransportOpenEvent& event)
{
    activeConnection_ = event.context;
    if (runtime_->stream) {
        // runtime_profile 是脚本显式设置的解析契约，应持续生效直到 clear_profile()。
        runtime_->stream->parser.reset();
    }
    callbackOnOpen(ScriptHostContext{event.context});
}

void ScriptHost::onTransportClose(const transport::TransportCloseEvent& event)
{
    callbackOnClose(ScriptHostContext{event.context});
    if (activeConnection_.has_value() && activeConnection_->connectionId == event.context.connectionId) {
        activeConnection_.reset();
    }
    if (runtime_->stream) {
        runtime_->stream->parser.reset();
    }
}

void ScriptHost::onTransportError(const transport::TransportErrorEvent& event)
{
    callbackOnError(ScriptHostContext{event.context}, event.message);
}

void ScriptHost::beginTransportBytesEvent(const transport::TransportBytesEvent& event)
{
    lastTransportStats_ = ScriptHostTransportStats{
        .bytes = event.bytes.size(),
        .streamMode = runtime_ && runtime_->stream,
        .lastErrorSummary{},
    };
    if (event.context.readyForIo) {
        activeConnection_ = event.context;
    }
}

StreamParseBatch ScriptHost::parseTransportStreamBytes(const std::vector<std::uint8_t>& bytes,
                                                       std::chrono::steady_clock::time_point& parserFinishedAt)
{
    const auto parserStartedAt = std::chrono::steady_clock::now();
    const StreamParseOptions parseOptions{
        .includeFrameRaw = runtime_->stream->includeRawFrames || !runtime_->stream->lowOverhead,
    };
    const auto batch = runtime_->stream->parser.pushBytes(bytes, parseOptions);
    runtime_->stream->lastBatch = makeStreamParseBatchSnapshot(batch, runtime_->stream->lowOverhead);
    parserFinishedAt = std::chrono::steady_clock::now();
    lastTransportStats_.streamFrames = batch.frames.size();
    lastTransportStats_.streamErrors = batch.errors.size();
    lastTransportStats_.parserMs = elapsedMilliseconds(parserStartedAt, parserFinishedAt);
    return batch;
}

void ScriptHost::dispatchStreamParseErrors(const transport::ConnectionContext& context, const StreamParseBatch& batch)
{
    for (const auto& error : batch.errors) {
        callbackOnStreamError(ScriptHostContext{context}, error);
    }
}

void ScriptHost::updateStreamParseErrorSummary(const StreamParseBatch& batch)
{
    if (batch.errors.empty()) {
        lastTransportStats_.lastErrorSummary.clear();
        return;
    }

    std::unordered_map<StreamParseErrorCode, std::size_t> errorCounts;
    std::string crcFrameNames;
    std::size_t overflowDroppedBytes = 0;
    for (const auto& error : batch.errors) {
        errorCounts[error.code]++;
        if (error.code == StreamParseErrorCode::CrcMismatch && error.frameName.has_value()) {
            if (!crcFrameNames.empty()) {
                crcFrameNames += ", ";
            }
            crcFrameNames += *error.frameName;
        }
        if (error.code == StreamParseErrorCode::Overflow) {
            overflowDroppedBytes += error.droppedBytes;
        }
    }

    std::string summary;
    for (const auto& [code, count] : errorCounts) {
        if (!summary.empty()) {
            summary += ", ";
        }
        summary += std::string(streamParseErrorCodeName(code)) + " × " + std::to_string(count);
        if (code == StreamParseErrorCode::Overflow) {
            summary += " (dropped=" + std::to_string(overflowDroppedBytes) +
                       " bytes, capacity=" + std::to_string(batch.bufferCapacity) + " bytes)";
        }
    }
    protoLog("warn", "stream parse errors: " + summary);
    if (!crcFrameNames.empty()) {
        protoLog("warn", "  crc_mismatch frames: " + crcFrameNames);
    }
    lastTransportStats_.lastErrorSummary = std::move(summary);
}

void ScriptHost::dispatchStreamFrames(const transport::ConnectionContext& context,
                                      const std::vector<StreamParsedFrame>& frames)
{
    // 核心流程：同一 parser 链顺序产出完整帧；若脚本支持 on_batch，则只批量调用一次，避免重复 on_frame。
    if (!frames.empty() && !callbackOnStreamBatch(ScriptHostContext{context}, frames)) {
        for (const auto& frame : frames) {
            callbackOnStreamFrame(ScriptHostContext{context}, frame);
        }
    }
}

void ScriptHost::handleStreamTransportBytes(const transport::TransportBytesEvent& event,
                                            std::chrono::steady_clock::time_point startedAt)
{
    std::chrono::steady_clock::time_point parserFinishedAt;
    const auto batch = parseTransportStreamBytes(event.bytes, parserFinishedAt);
    const auto callbackStartedAt = parserFinishedAt;
    dispatchStreamParseErrors(event.context, batch);
    updateStreamParseErrorSummary(batch);
    dispatchStreamFrames(event.context, batch.frames);
    const auto finishedAt = std::chrono::steady_clock::now();
    lastTransportStats_.callbackMs = elapsedMilliseconds(callbackStartedAt, finishedAt);
    lastTransportStats_.totalMs = elapsedMilliseconds(startedAt, finishedAt);
}

void ScriptHost::handleRawTransportBytes(const transport::TransportBytesEvent& event,
                                         std::chrono::steady_clock::time_point startedAt)
{
    const auto callbackStartedAt = std::chrono::steady_clock::now();
    callbackOnBytes(ScriptHostContext{event.context}, event.bytes);
    const auto finishedAt = std::chrono::steady_clock::now();
    lastTransportStats_.callbackMs = elapsedMilliseconds(callbackStartedAt, finishedAt);
    lastTransportStats_.totalMs = elapsedMilliseconds(startedAt, finishedAt);
}

void ScriptHost::onTransportBytes(const transport::TransportBytesEvent& event)
{
    const auto startedAt = std::chrono::steady_clock::now();
    beginTransportBytesEvent(event);
    if (runtime_->stream) {
        handleStreamTransportBytes(event, startedAt);
        return;
    }
    handleRawTransportBytes(event, startedAt);
}

void ScriptHost::onControl(const transport::ConnectionContext& ctx, const std::string& id, const ControlValue& value)
{
    const auto* descriptor = findControlDescriptor(controls_, id);
    if (descriptor == nullptr) {
        return;
    }
    controlValues_[id] = value;
    callbackOnControl(ScriptHostContext{ctx}, id, value);
}

bool ScriptHost::setControlValue(const std::string& id, const ControlValue& value)
{
    const auto* descriptor = findControlDescriptor(controls_, id);
    if (descriptor == nullptr) {
        return false;
    }
    controlValues_[id] = value;
    return true;
}

void ScriptHost::tick(std::uint64_t currentMs)
{
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

std::vector<ControlDescriptor> ScriptHost::controlsSnapshot() const
{
    return controls_;
}

std::vector<ControlSnapshot> ScriptHost::controlStatesSnapshot() const
{
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

std::vector<DockDescriptor> ScriptHost::dockDescriptorsSnapshot() const
{
    return docks_;
}

std::vector<DockSnapshot> ScriptHost::dockSnapshots() const
{
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

std::vector<ScriptEvent> ScriptHost::drainEvents()
{
    auto drained = std::move(events_);
    events_.clear();
    return drained;
}

std::vector<ScriptLog> ScriptHost::drainLogs()
{
    auto drained = std::move(logs_);
    logs_.clear();
    return drained;
}

const ScriptHostTransportStats& ScriptHost::lastTransportStats() const
{
    return lastTransportStats_;
}

std::vector<TxRequest> ScriptHost::drainTxRequests()
{
    auto drained = std::move(txRequests_);
    txRequests_.clear();
    return drained;
}

std::vector<transport::ConnectionContext> ScriptHost::drainRequestGuardResets()
{
    auto drained = std::move(requestGuardResets_);
    requestGuardResets_.clear();
    return drained;
}

std::vector<PlotSetup> ScriptHost::drainPlotSetups()
{
    auto drained = std::move(plotSetups_);
    plotSetups_.clear();
    return drained;
}

std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> ScriptHost::drainPlotAppends()
{
    auto drained = std::move(plotAppends_);
    plotAppends_.clear();
    return drained;
}

std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> ScriptHost::drainPlotAppends(const std::size_t maxRequests)
{
    if (plotAppends_.empty() || maxRequests == 0U) {
        return {};
    }

    auto makeKey = [](const std::pair<std::size_t, plot::WaveAppendRequest>& append) {
        std::string key = std::to_string(append.first);
        key.push_back('\x1F');
        key.append(append.second.source);
        return key;
    };

    auto count = (std::min)(maxRequests, plotAppends_.size());
    for (;;) {
        std::unordered_set<std::string> keys;
        keys.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            keys.insert(makeKey(plotAppends_[index]));
        }

        auto extendedCount = count;
        for (std::size_t index = count; index < plotAppends_.size(); ++index) {
            if (keys.contains(makeKey(plotAppends_[index]))) {
                extendedCount = index + 1U;
            }
        }
        if (extendedCount == count) {
            break;
        }
        count = extendedCount;
    }

    std::vector<std::pair<std::size_t, plot::WaveAppendRequest>> drained;
    drained.reserve(count);
    auto end = plotAppends_.begin();
    std::advance(end, static_cast<std::ptrdiff_t>(count));
    std::move(plotAppends_.begin(), end, std::back_inserter(drained));
    plotAppends_.erase(plotAppends_.begin(), end);
    return drained;
}

std::size_t ScriptHost::pendingPlotAppendCount() const
{
    return plotAppends_.size();
}

RealtimeOutputDiscardCounts ScriptHost::clearPendingRealtimeOutputs()
{
    const RealtimeOutputDiscardCounts counts{
        .events = events_.size(),
        .logs = logs_.size(),
        .plotAppends = plotAppends_.size(),
    };
    events_.clear();
    logs_.clear();
    plotAppends_.clear();
    return counts;
}

std::vector<RequestDoneResult> ScriptHost::drainRequestDoneResults()
{
    auto drained = std::move(requestDoneResults_);
    requestDoneResults_.clear();
    return drained;
}

std::vector<StatusUpdate> ScriptHost::drainStatusUpdates()
{
    auto drained = std::move(statusUpdates_);
    statusUpdates_.clear();
    return drained;
}

std::vector<StreamRuntimeProfileEvent> ScriptHost::drainStreamRuntimeProfileEvents()
{
    auto drained = std::move(streamRuntimeProfileEvents_);
    streamRuntimeProfileEvents_.clear();
    return drained;
}

std::vector<DialogRequest> ScriptHost::drainDialogRequests()
{
    auto drained = std::move(dialogRequests_);
    dialogRequests_.clear();
    return drained;
}

std::vector<FileDialogRequest> ScriptHost::drainFileDialogRequests()
{
    auto drained = std::move(fileDialogRequests_);
    fileDialogRequests_.clear();
    return drained;
}

void ScriptHost::registerLuaApi(sol::state_view lua, sol::table& proto)
{
    ScriptHostQueues queues;
    ScriptHostContextInternal ctx{*this, queues, fileIoConfig_, activeConnection_, lua};
    std::array modules{
        makeCoreApiModule(*this),
        makeTxApiModule(*this),
        makeStatusApiModule(*this),
        makeUiApiModule(*this),
        makeFileApiModule(*this),
        makePlotApiModule(*this),
        makeControlApiModule(*this),
        makeCodecApiModule(*this),
    };

    // 核心流程：宿主只编排模块顺序，具体 Lua wire format 由各 API 模块原样注册。
    for (auto& module : modules) {
        module->registerApi(ctx, proto);
    }
}

std::optional<std::uint64_t> ScriptHost::nextWakeupAtMs() const
{
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

const std::string& ScriptHost::scriptPath() const
{
    return scriptPath_;
}

const std::string& ScriptHost::protocolDirectory() const
{
    return protocolDirectory_;
}

const std::string& ScriptHost::lastError() const
{
    return lastError_;
}

void ScriptHost::onTxEvent(const transport::ConnectionContext& ctx, const TxEvent& event)
{
    const bool releaseFileChunk = event.state == TxEventState::Sent || event.state == TxEventState::Rejected ||
                                  event.state == TxEventState::Dropped || event.state == TxEventState::Canceled ||
                                  event.state == TxEventState::Timeout;
    if (event.fileJobId != 0 && releaseFileChunk) {
        const auto iter = fileSendJobs_.find(event.fileJobId);
        if (iter != fileSendJobs_.end()) {
            iter->second.inflight = iter->second.inflight == 0 ? 0 : iter->second.inflight - 1;
            pumpFileSendJob(event.fileJobId);
        }
    }
    callbackOnTx(ScriptHostContext{ctx}, event);
}

void ScriptHost::onDialogEvent(const transport::ConnectionContext& ctx, const DialogEvent& event)
{
    callbackOnDialog(ScriptHostContext{ctx}, event);
}

void ScriptHost::onFileDialogEvent(const transport::ConnectionContext& ctx, const FileDialogEvent& event)
{
    if (event.state == "selected" && !event.path.empty() && fileIoConfig_.allowDialogPaths) {
        std::error_code errorCode;
        auto path = std::filesystem::weakly_canonical(std::filesystem::absolute(event.path), errorCode);
        if (errorCode) {
            path = std::filesystem::absolute(event.path).lexically_normal();
        }
        dialogAuthorizedPaths_.push_back(AuthorizedPath{
            .path = path,
            .recursive = event.kind == FileDialogKind::OpenDir,
            .readable = event.kind != FileDialogKind::SaveFile,
            .writable = event.kind != FileDialogKind::OpenFile,
        });
    }
    callbackOnFileDialog(ScriptHostContext{ctx}, event);
}

void ScriptHost::setRequestAwaitingCompletion(bool active)
{
    requestAwaitingCompletion_ = active;
}

sol::state& ScriptHost::luaState()
{
    return runtime_->lua;
}

sol::state_view ScriptHost::luaView()
{
    return sol::state_view(runtime_->lua.lua_state());
}

const std::vector<ControlDescriptor>& ScriptHost::controlDescriptors() const
{
    return controls_;
}

const ControlValue* ScriptHost::findControlValue(const std::string& id) const
{
    const auto iter = controlValues_.find(id);
    return iter == controlValues_.end() ? nullptr : &iter->second;
}

void ScriptHost::updateControlValue(const std::string& id, ControlValue value)
{
    controlValues_[id] = std::move(value);
}

std::optional<TxRequest> ScriptHost::protoSendLike(
    TxRequestKind kind, const sol::object& payload, const sol::object& opts, std::string& error, bool guarded)
{
    const std::string apiName = guarded ? "proto.request_guarded"
                                        : std::string(kind == TxRequestKind::Request ? "proto.request" : "proto.send");
    const auto maybeBytes = bytesFromLuaObject(payload, error);
    if (!maybeBytes.has_value()) {
        protoLog("error", apiName + " 调用失败: " + error);
        return std::nullopt;
    }

    TxRequest request{};
    request.id = nextTxRequestId();
    request.kind = kind;
    request.payload = *maybeBytes;
    request.timeoutMs = 0;
    request.guarded = guarded;
    request.attempt = 1;
    request.maxAttempts = 1;
    request.createdAtMs = nowMs();
    if (activeConnection_.has_value()) {
        request.connection = *activeConnection_;
    } else {
        request.connection.endpoint = "detached";
        request.connection.connectionId = 0;
        request.connection.timestampMs = request.createdAtMs;
        request.connection.readyForIo = false;
    }

    if (opts.valid() && opts.get_type() != sol::type::lua_nil) {
        if (!opts.is<sol::table>()) {
            error = "opts 必须是 table";
            protoLog("error", apiName + " 调用失败: " + error);
            return std::nullopt;
        }
        const sol::table options = opts.as<sol::table>();
        if (const auto timeoutMs = luaNumberField(options, "timeout_ms"); timeoutMs.has_value()) {
            request.timeoutMs = static_cast<std::uint64_t>(std::max(0.0, *timeoutMs));
        }
        request.tag = luaStringField(options, "tag").value_or("");
        if (guarded) {
            if (const auto maxAttempts = luaNumberField(options, "max_attempts"); maxAttempts.has_value()) {
                request.maxAttempts = static_cast<std::uint32_t>(std::max(1.0, *maxAttempts));
            }
        }
    }

    txRequests_.push_back(request);
    return request;
}

void ScriptHost::protoResetRequestGuard()
{
    transport::ConnectionContext connection{};
    if (activeConnection_.has_value()) {
        connection = *activeConnection_;
    } else {
        connection.endpoint = "detached";
        connection.connectionId = 0;
        connection.timestampMs = nowMs();
        connection.readyForIo = false;
    }
    requestGuardResets_.push_back(std::move(connection));
}

bool ScriptHost::protoRequestDone(const sol::object& result, std::string& error)
{
    if (!requestAwaitingCompletion_) {
        error = "当前没有活动 request";
        protoLog("warn", "proto.request_done 被忽略: " + error);
        return false;
    }

    RequestDoneResult requestDone{};
    requestDone.timestampMs = nowMs();
    if (result.valid() && result.get_type() != sol::type::lua_nil) {
        if (!result.is<sol::table>()) {
            error = "result 必须是 table";
            protoLog("error", "proto.request_done 调用失败: " + error);
            return false;
        }
        const sol::table table = result.as<sol::table>();
        requestDone.ok = luaBoolField(table, "ok").value_or(true);
        requestDone.message = luaStringField(table, "message").value_or("");
    }

    requestDoneResults_.push_back(requestDone);
    return true;
}

void ScriptHost::protoStatusSet(const std::string& text, const sol::object& opts)
{
    StatusUpdate update{};
    update.text = text;
    update.level = "info";
    update.timestampMs = nowMs();
    if (opts.valid() && opts.get_type() != sol::type::lua_nil && opts.is<sol::table>()) {
        update.level = luaStringField(opts.as<sol::table>(), "level").value_or(update.level);
    }
    statusUpdates_.push_back(std::move(update));
}

void ScriptHost::protoStatusClear()
{
    StatusUpdate update{};
    update.text.clear();
    update.level = "info";
    update.clear = true;
    update.timestampMs = nowMs();
    statusUpdates_.push_back(std::move(update));
}

std::optional<DialogRequest> ScriptHost::protoDialog(DialogKind kind, const sol::object& opts, std::string& error)
{
    if (!opts.is<sol::table>()) {
        error = "opts 必须是 table";
        protoLog(
            "error",
            std::string(kind == DialogKind::Alert ? "proto.ui.alert" : "proto.ui.confirm") + " 调用失败: " + error);
        return std::nullopt;
    }

    const sol::table table = opts.as<sol::table>();
    const auto createdAtMs = nowMs();
    transport::ConnectionContext connection{};
    if (activeConnection_.has_value()) {
        connection = *activeConnection_;
    } else {
        connection.endpoint = "detached";
        connection.connectionId = 0;
        connection.timestampMs = createdAtMs;
        connection.readyForIo = false;
    }
    const auto window = luaDialogWindowOptions(table, error);
    if (!window.has_value()) {
        protoLog(
            "error",
            std::string(kind == DialogKind::Alert ? "proto.ui.alert" : "proto.ui.confirm") + " 调用失败: " + error);
        return std::nullopt;
    }
    const DialogRequest request{
        .id = nextDialogId(),
        .kind = kind,
        .connection = connection,
        .title = luaStringField(table, "title").value_or(""),
        .message = luaStringField(table, "message").value_or(""),
        .level = luaStringField(table, "level").value_or("info"),
        .dedupeKey = luaStringField(table, "dedupe_key").value_or(""),
        .window = *window,
        .createdAtMs = createdAtMs,
    };
    if (request.title.empty() || request.message.empty()) {
        error = "title 和 message 不能为空";
        protoLog(
            "error",
            std::string(kind == DialogKind::Alert ? "proto.ui.alert" : "proto.ui.confirm") + " 调用失败: " + error);
        return std::nullopt;
    }
    dialogRequests_.push_back(request);
    return request;
}

std::optional<FileDialogRequest> ScriptHost::protoFileDialog(FileDialogKind kind,
                                                             const sol::object& opts,
                                                             std::string& error)
{
    if (!fileIoConfig_.enabled) {
        error = "scripting.file_io 已禁用";
        return std::nullopt;
    }
    if (!fileIoConfig_.dialog.enabled) {
        error = "scripting.file_io.dialog 已禁用";
        return std::nullopt;
    }
    if (!opts.is<sol::table>()) {
        error = "opts 必须是 table";
        return std::nullopt;
    }

    const auto table = opts.as<sol::table>();
    const auto resolvedKind = resolveFileDialogKind(kind, table, error);
    if (!resolvedKind.has_value()) {
        return std::nullopt;
    }

    const auto createdAtMs = nowMs();
    auto filters = parseFileDialogFilters(table, error);
    if (!filters.has_value()) {
        return std::nullopt;
    }

    // 成员函数只补齐宿主状态，Lua 参数解析保持在无状态 helper 中。
    const FileDialogRequest request = makeFileDialogRequest(nextFileDialogId(),
                                                            *resolvedKind,
                                                            fileDialogConnectionContext(activeConnection_, createdAtMs),
                                                            table,
                                                            std::move(*filters),
                                                            createdAtMs);
    fileDialogRequests_.push_back(request);
    return request;
}

std::uint64_t ScriptHost::nextTxRequestId()
{
    return nextTxRequestId_++;
}

std::uint64_t ScriptHost::nextDialogId()
{
    return nextDialogId_++;
}

std::uint64_t ScriptHost::nextFileDialogId()
{
    return nextFileDialogId_++;
}

std::uint64_t ScriptHost::nextFileHandleId()
{
    return nextFileHandleId_++;
}

std::uint64_t ScriptHost::nextFileJobId()
{
    return nextFileJobId_++;
}

void ScriptHost::protoLog(const std::string& level, const std::string& message)
{
    logs_.push_back(ScriptLog{
        .level = level,
        .message = message,
        .timestampMs = nowMs(),
    });
}

void ScriptHost::protoEmit(const std::string& eventName, const std::string& payload)
{
    events_.push_back(ScriptEvent{
        .name = eventName,
        .payload = payload,
        .timestampMs = nowMs(),
    });
}

void ScriptHost::protoSetTimer(const std::string& name, std::uint64_t intervalMs)
{
    timers_[name] = TimerState{
        .name = name,
        .dueAtMs = nowMs() + intervalMs,
        .active = true,
    };
}

void ScriptHost::protoCancelTimer(const std::string& name)
{
    const auto iter = timers_.find(name);
    if (iter != timers_.end()) {
        iter->second.active = false;
    }
}

void ScriptHost::protoPlotSetup(const PlotSetup& setup)
{
    plotSetups_.push_back(setup);
}

void ScriptHost::protoPlotPush(std::size_t channelIndex, const plot::WaveAppendRequest& request)
{
    plotAppends_.push_back(std::make_pair(channelIndex, request));
}

std::string ScriptHost::valueToString(const ControlValue& value)
{
    return std::visit(
        [](const auto& current) {
            using ValueType = std::decay_t<decltype(current)>;
            if constexpr (std::is_same_v<ValueType, ElfSymbolValue>) {
                if (current.label.empty() || current.value.empty() || current.type.empty()) {
                    return std::string("nil");
                }
                return current.label + "=" + current.value + " (" + current.type + ")";
            } else {
                std::ostringstream builder;
                builder << current;
                return builder.str();
            }
        },
        value);
}

void ScriptHost::setLastError(std::string message)
{
    lastError_ = std::move(message);
}

} // namespace protoscope::scripting
