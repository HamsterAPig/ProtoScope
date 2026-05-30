#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"

#include <sol/sol.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <functional>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace protoscope::scripting {

std::size_t ProtoBuffer::size() const {
    return bytes.size();
}

ProtoBuffer ProtoBuffer::slice(std::size_t offset, std::size_t size) const {
    if (offset >= bytes.size()) {
        return {};
    }
    const auto end = std::min(bytes.size(), offset + size);
    return ProtoBuffer{std::vector<std::uint8_t>(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                                bytes.begin() + static_cast<std::ptrdiff_t>(end))};
}

std::string ProtoBuffer::toHex(std::size_t maxBytes) const {
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

namespace {

constexpr const char* kDefaultDockId = "protocol";
constexpr const char* kDefaultDockTitle = "协议动作";

class FunctionScriptHostApiModule final : public IScriptHostApiModule {
public:
    using RegisterFn = std::function<void(sol::table&)>;

    FunctionScriptHostApiModule(std::string_view moduleId, RegisterFn registerFn)
        : moduleId_(moduleId),
          registerFn_(std::move(registerFn)) {}

    std::string_view id() const override {
        return moduleId_;
    }

    void registerApi(ScriptHostContextInternal&, sol::table& proto) override {
        registerFn_(proto);
    }

private:
    std::string_view moduleId_;
    RegisterFn registerFn_;
};

std::uint64_t nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string kindName(transport::TransportKind kind) {
    return std::string(transport::transportKindId(kind));
}

std::string txKindName(TxRequestKind kind) {
    switch (kind) {
    case TxRequestKind::Send:
        return "send";
    case TxRequestKind::Request:
        return "request";
    }
    return "send";
}

std::string txEventStateName(TxEventState state) {
    switch (state) {
    case TxEventState::Sent:
        return "sent";
    case TxEventState::Completed:
        return "completed";
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

std::string dialogKindName(DialogKind kind) {
    switch (kind) {
    case DialogKind::Alert:
        return "alert";
    case DialogKind::Confirm:
        return "confirm";
    }
    return "alert";
}

std::string fileDialogKindName(FileDialogKind kind) {
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

std::optional<std::array<float, 4>> parseColorText(std::string_view text) {
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
    case ControlType::ElfSymbolCombo:
        return ElfSymbolValue{};
    }
    return false;
}

double finiteOrDefault(double value, double fallback) {
    return std::isfinite(value) ? value : fallback;
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

std::string serializeLuaObject(const sol::object& object, int depth) {
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
    if (value == "elf_symbol_combo") {
        return ControlType::ElfSymbolCombo;
    }
    return std::nullopt;
}

bool isValidDockAnchor(std::string_view value) {
    return value == "left" || value == "left_bottom" || value == "right_top"
        || value == "right_mid" || value == "right_bottom" || value == "main_bottom";
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
        symbol.label = table.get_or("label", std::string());
        symbol.value = table.get_or("value", std::string());
        symbol.type = table.get_or("type", std::string());
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

sol::object controlValueToLua(sol::state_view lua,
                              const ControlDescriptor* descriptor,
                              const ControlValue& value) {
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

    if (object.is<ProtoBuffer>()) {
        return object.as<ProtoBuffer>().bytes;
    }

    if (!object.is<sol::table>()) {
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
    case ControlType::ElfSymbolCombo: {
        descriptor.debounceMs = table.get_or("debounce_ms", 150);
        const int limit = table.get_or("limit", 64);
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

bool registerFormControlUse(const DockDescriptor& dock,
                            const std::unordered_map<std::string, const ControlDescriptor*>& controlsById,
                            std::unordered_set<std::string>& usedControls,
                            const std::string& controlId,
                            std::string& error) {
    if (controlId.empty()) {
        error = "dock '" + dock.id + "' 的 form layout control 不能为空字符串";
        return false;
    }
    if (!controlsById.contains(controlId)) {
        error = "dock '" + dock.id + "' 的 form layout 引用了未声明控件 '" + controlId + "'";
        return false;
    }
    if (!usedControls.emplace(controlId).second) {
        error = "dock '" + dock.id + "' 的 form layout 重复引用控件 '" + controlId + "'";
        return false;
    }
    return true;
}

std::optional<std::vector<FormLayoutItemDescriptor>> parseFormLayoutItems(const DockDescriptor& dock,
                                                                          const sol::table& itemsTable,
                                                                          const std::unordered_map<std::string, const ControlDescriptor*>& controlsById,
                                                                          std::unordered_set<std::string>& usedControls,
                                                                          std::string& error,
                                                                          int depth = 0) {
    if (itemsTable.size() == 0) {
        error = "dock '" + dock.id + "' 的 form layout.items 不能为空";
        return std::nullopt;
    }

    // 核心流程：form 布局在解析阶段就完成控件引用校验，
    // 渲染层只按声明顺序输出，避免运行时再做结构推断。
    std::vector<FormLayoutItemDescriptor> items;
    items.reserve(itemsTable.size());
    for (std::size_t itemIndex = 1; itemIndex <= itemsTable.size(); ++itemIndex) {
        const sol::object itemObject = itemsTable[itemIndex];
        if (!itemObject.is<sol::table>()) {
            error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex) + "] 必须是 table";
            return std::nullopt;
        }

        const auto itemTable = itemObject.as<sol::table>();
        const bool hasControl = itemTable["control"].valid() && itemTable["control"].get_type() != sol::type::lua_nil;
        const bool hasControls = itemTable["controls"].valid() && itemTable["controls"].get_type() != sol::type::lua_nil;
        const bool hasGroup = itemTable["group"].valid() && itemTable["group"].get_type() != sol::type::lua_nil;
        const bool hasCollapse = itemTable["collapse"].valid() && itemTable["collapse"].get_type() != sol::type::lua_nil;
        const bool hasSeparator = itemTable["separator"].valid() && itemTable["separator"].get_type() != sol::type::lua_nil;
        const bool hasText = itemTable["text"].valid() && itemTable["text"].get_type() != sol::type::lua_nil;
        const int declaredKinds = static_cast<int>(hasControl) + static_cast<int>(hasControls) + static_cast<int>(hasGroup)
            + static_cast<int>(hasCollapse) + static_cast<int>(hasSeparator) + static_cast<int>(hasText);
        if (declaredKinds != 1) {
            error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex)
                + "] 必须且只能声明 control / controls / group / collapse / separator / text 之一";
            return std::nullopt;
        }

        FormLayoutItemDescriptor item;
        if (hasControl) {
            const sol::object controlObject = itemTable["control"];
            if (!controlObject.is<std::string>()) {
                error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex) + "].control 必须是字符串";
                return std::nullopt;
            }
            item.kind = FormLayoutItemKind::Control;
            item.controlId = controlObject.as<std::string>();
            if (!registerFormControlUse(dock, controlsById, usedControls, item.controlId, error)) {
                return std::nullopt;
            }
        } else if (hasControls) {
            const sol::object controlsObject = itemTable["controls"];
            if (!controlsObject.is<sol::table>()) {
                error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex) + "].controls 必须是非空数组";
                return std::nullopt;
            }
            const auto controlsTable = controlsObject.as<sol::table>();
            if (controlsTable.size() == 0) {
                error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex) + "].controls 不能为空";
                return std::nullopt;
            }
            item.kind = FormLayoutItemKind::Controls;
            item.controls.controlIds.reserve(controlsTable.size());
            for (std::size_t controlIndex = 1; controlIndex <= controlsTable.size(); ++controlIndex) {
                const sol::object controlIdObject = controlsTable[controlIndex];
                if (!controlIdObject.is<std::string>()) {
                    error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex)
                        + "].controls[" + std::to_string(controlIndex) + "] 必须是字符串";
                    return std::nullopt;
                }
                const auto controlId = controlIdObject.as<std::string>();
                if (!registerFormControlUse(dock, controlsById, usedControls, controlId, error)) {
                    return std::nullopt;
                }
                item.controls.controlIds.push_back(controlId);
            }
        } else if (hasGroup || hasCollapse) {
            if (depth >= 1) {
                error = "dock '" + dock.id + "' 的 form layout 仅支持一层 group/collapse 嵌套";
                return std::nullopt;
            }
            const char* keyName = hasGroup ? "group" : "collapse";
            const sol::object titleObject = itemTable[keyName];
            if (!titleObject.is<std::string>()) {
                error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex) + "]." + keyName + " 必须是字符串";
                return std::nullopt;
            }
            const sol::object nestedItemsObject = itemTable["items"];
            if (!nestedItemsObject.is<sol::table>()) {
                error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex) + "].items 必须是非空数组";
                return std::nullopt;
            }
            auto nestedItems = parseFormLayoutItems(
                dock,
                nestedItemsObject.as<sol::table>(),
                controlsById,
                usedControls,
                error,
                depth + 1);
            if (!nestedItems.has_value()) {
                return std::nullopt;
            }
            if (hasGroup) {
                item.kind = FormLayoutItemKind::Group;
                item.group = std::make_shared<FormGroupDescriptor>();
                item.group->title = titleObject.as<std::string>();
                item.group->items = std::move(*nestedItems);
            } else {
                item.kind = FormLayoutItemKind::Collapse;
                item.collapse = std::make_shared<FormCollapseDescriptor>();
                item.collapse->title = titleObject.as<std::string>();
                item.collapse->defaultOpen = itemTable.get_or("default_open", true);
                item.collapse->items = std::move(*nestedItems);
            }
        } else if (hasSeparator) {
            const sol::object separatorObject = itemTable["separator"];
            if (!separatorObject.is<bool>() || !separatorObject.as<bool>()) {
                error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex) + "].separator 必须是 true";
                return std::nullopt;
            }
            item.kind = FormLayoutItemKind::Separator;
        } else if (hasText) {
            const sol::object textObject = itemTable["text"];
            if (!textObject.is<std::string>()) {
                error = "dock '" + dock.id + "' 的 form layout.items[" + std::to_string(itemIndex) + "].text 必须是字符串";
                return std::nullopt;
            }
            item.kind = FormLayoutItemKind::Text;
            item.text.text = textObject.as<std::string>();
        }

        items.push_back(std::move(item));
    }

    return items;
}

std::optional<FormLayoutDescriptor> parseFormLayout(const DockDescriptor& dock,
                                                    const sol::table& layoutTable,
                                                    std::string& error) {
    const sol::object itemsObject = layoutTable["items"];
    if (!itemsObject.valid() || itemsObject.get_type() == sol::type::lua_nil || !itemsObject.is<sol::table>()) {
        error = "dock '" + dock.id + "' 的 form layout.items 必须是非空数组";
        return std::nullopt;
    }

    std::unordered_map<std::string, const ControlDescriptor*> controlsById;
    controlsById.reserve(dock.controls.size());
    for (const auto& control : dock.controls) {
        if (!controlsById.emplace(control.id, &control).second) {
            error = "dock '" + dock.id + "' 的控件 id 重复: " + control.id;
            return std::nullopt;
        }
    }

    std::unordered_set<std::string> usedControls;
    usedControls.reserve(dock.controls.size());

    auto items = parseFormLayoutItems(dock, itemsObject.as<sol::table>(), controlsById, usedControls, error);
    if (!items.has_value()) {
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
        stream << "dock '" << dock.id << "' 的 form layout 缺少控件:";
        for (const auto& controlId : missingControls) {
            stream << ' ' << controlId;
        }
        error = stream.str();
        return std::nullopt;
    }

    FormLayoutDescriptor layout;
    layout.items = std::move(*items);
    return layout;
}

std::optional<DockLayoutDescriptor> parseDockLayout(const DockDescriptor& dock,
                                                    const sol::object& object,
                                                    std::string& error) {
    if (!object.is<sol::table>()) {
        error = "dock '" + dock.id + "' 的 layout 必须是 table";
        return std::nullopt;
    }

    const auto layoutTable = object.as<sol::table>();
    const sol::object kindObject = layoutTable["kind"];
    if (!kindObject.valid() || kindObject.get_type() == sol::type::lua_nil || !kindObject.is<std::string>()) {
        error = "dock '" + dock.id + "' 的 layout.kind 必须是字符串 'table' 或 'form'";
        return std::nullopt;
    }

    const auto kind = kindObject.as<std::string>();
    if (kind == "form") {
        auto formLayout = parseFormLayout(dock, layoutTable, error);
        if (!formLayout.has_value()) {
            return std::nullopt;
        }
        DockLayoutDescriptor layout;
        layout.kind = DockLayoutKind::Form;
        layout.form = std::move(*formLayout);
        return layout;
    }

    if (kind != "table") {
        error = "dock '" + dock.id + "' 的 layout.kind 仅支持 'table' 或 'form'";
        return std::nullopt;
    }

    const sol::object columnsObject = layoutTable["columns"];
    if (!columnsObject.valid() || columnsObject.get_type() == sol::type::lua_nil || !columnsObject.is<double>()) {
        error = "dock '" + dock.id + "' 的 layout.columns 必须是 >= 1 的整数";
        return std::nullopt;
    }
    const auto columnsValue = columnsObject.as<double>();
    if (!std::isfinite(columnsValue) || columnsValue < 1.0 || std::floor(columnsValue) != columnsValue) {
        error = "dock '" + dock.id + "' 的 layout.columns 必须是 >= 1 的整数";
        return std::nullopt;
    }

    const sol::object rowsObject = layoutTable["rows"];
    if (!rowsObject.valid() || rowsObject.get_type() == sol::type::lua_nil || !rowsObject.is<sol::table>()) {
        error = "dock '" + dock.id + "' 的 layout.rows 必须是非空数组";
        return std::nullopt;
    }

    const auto rowsTable = rowsObject.as<sol::table>();
    if (rowsTable.size() == 0) {
        error = "dock '" + dock.id + "' 的 layout.rows 不能为空";
        return std::nullopt;
    }

    std::unordered_map<std::string, const ControlDescriptor*> controlsById;
    controlsById.reserve(dock.controls.size());
    for (const auto& control : dock.controls) {
        if (!controlsById.emplace(control.id, &control).second) {
            error = "dock '" + dock.id + "' 的 controls 中存在重复 id: " + control.id;
            return std::nullopt;
        }
    }

    DockLayoutDescriptor layout;
    layout.kind = DockLayoutKind::Table;
    layout.table.columns = static_cast<std::size_t>(columnsValue);
    layout.table.borders = layoutTable.get_or("borders", false);
    layout.table.resizable = layoutTable.get_or("resizable", true);
    layout.table.rowBg = layoutTable.get_or("row_bg", false);
    layout.table.sizing = layoutTable.get_or("sizing", std::string("stretch"));
    if (layout.table.sizing != "stretch") {
        error = "dock '" + dock.id + "' 的 layout.sizing 仅支持 'stretch'";
        return std::nullopt;
    }

    std::unordered_set<std::string> usedControls;
    usedControls.reserve(dock.controls.size());
    for (std::size_t rowIndex = 1; rowIndex <= rowsTable.size(); ++rowIndex) {
        const sol::object rowObject = rowsTable[rowIndex];
        if (!rowObject.is<sol::table>()) {
            error = "dock '" + dock.id + "' 的 layout.rows[" + std::to_string(rowIndex) + "] 必须是数组";
            return std::nullopt;
        }

        const auto rowTable = rowObject.as<sol::table>();
        if (rowTable.size() > layout.table.columns) {
            error = "dock '" + dock.id + "' 的 layout.rows[" + std::to_string(rowIndex)
                + "] 单元格数量不能超过 columns";
            return std::nullopt;
        }

        TableRowDescriptor row;
        row.cells.reserve(rowTable.size());
        for (std::size_t cellIndex = 1; cellIndex <= rowTable.size(); ++cellIndex) {
            const sol::object cellObject = rowTable[cellIndex];
            if (!cellObject.is<sol::table>()) {
                error = "dock '" + dock.id + "' 的 layout.rows[" + std::to_string(rowIndex)
                    + "][" + std::to_string(cellIndex) + "] 必须是 table";
                return std::nullopt;
            }

            const auto cellTable = cellObject.as<sol::table>();
            const sol::object controlObject = cellTable["control"];
            const sol::object spacerObject = cellTable["spacer"];
            const bool hasControl = controlObject.valid() && controlObject.get_type() != sol::type::lua_nil;
            const bool hasSpacer = spacerObject.valid() && spacerObject.get_type() != sol::type::lua_nil;
            if (hasControl == hasSpacer) {
                error = "dock '" + dock.id + "' 的 layout.rows[" + std::to_string(rowIndex)
                    + "][" + std::to_string(cellIndex) + "] 必须二选一：control 或 spacer";
                return std::nullopt;
            }

            TableCellDescriptor cell;
            if (hasSpacer) {
                if (!spacerObject.is<bool>() || !spacerObject.as<bool>()) {
                    error = "dock '" + dock.id + "' 的 layout.rows[" + std::to_string(rowIndex)
                        + "][" + std::to_string(cellIndex) + "].spacer 必须为 true";
                    return std::nullopt;
                }
                cell.spacer = true;
            } else {
                if (!controlObject.is<std::string>()) {
                    error = "dock '" + dock.id + "' 的 layout.rows[" + std::to_string(rowIndex)
                        + "][" + std::to_string(cellIndex) + "].control 必须是字符串";
                    return std::nullopt;
                }
                cell.controlId = controlObject.as<std::string>();
                if (cell.controlId.empty()) {
                    error = "dock '" + dock.id + "' 的 layout.rows[" + std::to_string(rowIndex)
                        + "][" + std::to_string(cellIndex) + "].control 不能为空";
                    return std::nullopt;
                }
                if (!controlsById.contains(cell.controlId)) {
                    error = "dock '" + dock.id + "' 的 layout.rows[" + std::to_string(rowIndex)
                        + "][" + std::to_string(cellIndex) + "] 引用了未声明控件: " + cell.controlId;
                    return std::nullopt;
                }
                if (!usedControls.emplace(cell.controlId).second) {
                    error = "dock '" + dock.id + "' 的 table layout 重复引用控件: " + cell.controlId;
                    return std::nullopt;
                }
            }
            row.cells.push_back(std::move(cell));
        }
        layout.table.rows.push_back(std::move(row));
    }

    if (usedControls.size() != dock.controls.size()) {
        std::vector<std::string> missingControls;
        missingControls.reserve(dock.controls.size() - usedControls.size());
        for (const auto& control : dock.controls) {
            if (!usedControls.contains(control.id)) {
                missingControls.push_back(control.id);
            }
        }
        std::ostringstream stream;
        stream << "dock '" << dock.id << "' 的 table layout 缺少控件:";
        for (const auto& controlId : missingControls) {
            stream << ' ' << controlId;
        }
        error = stream.str();
        return std::nullopt;
    }

    return layout;
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
            dock.anchor = dockEntry.get_or("anchor", std::string("left_bottom"));
            dock.tabGroup = dockEntry.get_or("tab_group", std::string());
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

sol::table makeTxEventTable(sol::state_view lua, const TxEvent& event) {
    sol::table table = lua.create_table();
    table["id"] = event.id;
    table["kind"] = txKindName(event.kind);
    table["state"] = txEventStateName(event.state);
    table["tag"] = event.tag;
    table["bytes"] = static_cast<int>(event.bytes);
    table["queued_ms"] = event.queuedMs;
    table["finished_ms"] = event.finishedMs;
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

sol::table makeDialogEventTable(sol::state_view lua, const DialogEvent& event) {
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

sol::table makeFileDialogEventTable(sol::state_view lua, const FileDialogEvent& event) {
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

std::filesystem::path canonicalPath(const std::filesystem::path& path) {
    std::error_code errorCode;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::absolute(path), errorCode);
    if (errorCode) {
        canonical = std::filesystem::absolute(path).lexically_normal();
    }
    return canonical;
}

std::string comparablePath(std::filesystem::path path) {
    path = path.lexically_normal();
    auto text = path.generic_string();
#if defined(_WIN32)
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
#endif
    return text;
}

bool isSameOrChildPath(const std::filesystem::path& root, const std::filesystem::path& path, bool recursive) {
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

std::size_t positiveSizeOrDefault(std::size_t value, std::size_t fallback) {
    return value == 0 ? fallback : value;
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

std::optional<std::int64_t> luaIntegerValue(const sol::object& object) {
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

std::optional<StreamValueType> parseStreamValueType(const std::string& text) {
    if (text == "u8") return StreamValueType::U8;
    if (text == "i8") return StreamValueType::I8;
    if (text == "u16_be") return StreamValueType::U16Be;
    if (text == "u16_le") return StreamValueType::U16Le;
    if (text == "i16_be") return StreamValueType::I16Be;
    if (text == "i16_le") return StreamValueType::I16Le;
    if (text == "u32_be") return StreamValueType::U32Be;
    if (text == "u32_le") return StreamValueType::U32Le;
    if (text == "i32_be") return StreamValueType::I32Be;
    if (text == "i32_le") return StreamValueType::I32Le;
    if (text == "f32_be") return StreamValueType::F32Be;
    if (text == "f32_le") return StreamValueType::F32Le;
    if (text == "bytes") return StreamValueType::Bytes;
    return std::nullopt;
}

bool streamValueTypeCanBeLength(StreamValueType type) {
    return type != StreamValueType::Bytes && !streamValueTypeIsFloat(type);
}

std::optional<StreamLengthMeans> parseStreamLengthMeans(const std::string& text) {
    if (text == "payload") {
        return StreamLengthMeans::Payload;
    }
    if (text == "frame") {
        return StreamLengthMeans::Frame;
    }
    return std::nullopt;
}

std::optional<StreamCrcType> parseStreamCrcType(const std::string& text) {
    if (text == "crc16_modbus") {
        return StreamCrcType::Crc16Modbus;
    }
    if (text == "crc16_ccitt_false") {
        return StreamCrcType::Crc16CcittFalse;
    }
    if (text == "crc32_ieee") {
        return StreamCrcType::Crc32Ieee;
    }
    return std::nullopt;
}

std::optional<StreamCrcOrder> parseStreamCrcOrder(const std::string& text) {
    if (text == "hi_lo") {
        return StreamCrcOrder::HiLo;
    }
    if (text == "lo_hi") {
        return StreamCrcOrder::LoHi;
    }
    return std::nullopt;
}

sol::object streamFieldValueToLua(sol::state_view lua, const StreamFieldValue& value) {
    if (std::holds_alternative<std::int64_t>(value.value)) {
        return sol::make_object(lua, std::get<std::int64_t>(value.value));
    }
    if (std::holds_alternative<double>(value.value)) {
        return sol::make_object(lua, std::get<double>(value.value));
    }
    if (std::holds_alternative<std::vector<std::uint8_t>>(value.value)) {
        return sol::make_object(lua, makeBytesTable(lua, std::get<std::vector<std::uint8_t>>(value.value)));
    }
    if (std::holds_alternative<std::vector<std::int64_t>>(value.value)) {
        const auto& items = std::get<std::vector<std::int64_t>>(value.value);
        sol::table table = lua.create_table(static_cast<int>(items.size()), 0);
        for (std::size_t index = 0; index < items.size(); ++index) {
            table[index + 1] = items[index];
        }
        return sol::make_object(lua, table);
    }

    const auto& items = std::get<std::vector<double>>(value.value);
    sol::table table = lua.create_table(static_cast<int>(items.size()), 0);
    for (std::size_t index = 0; index < items.size(); ++index) {
        table[index + 1] = items[index];
    }
    return sol::make_object(lua, table);
}

sol::table makeStreamFieldsTable(sol::state_view lua, const StreamFieldMap& fields) {
    sol::table table = lua.create_table();
    for (const auto& [name, value] : fields) {
        table[name] = streamFieldValueToLua(lua, value);
    }
    return table;
}

sol::table makeStreamFrameTable(sol::state_view lua, const StreamParsedFrame& frame) {
    sol::table table = lua.create_table();
    table["name"] = frame.name;
    table["raw"] = makeBytesTable(lua, frame.raw);
    table["crc_ok"] = frame.crcOk;
    const auto fields = makeStreamFieldsTable(lua, frame.fields);
    table["fields"] = fields;
    for (const auto& [name, value] : frame.fields) {
        table[name] = streamFieldValueToLua(lua, value);
    }
    return table;
}

sol::table makeStreamErrorTable(sol::state_view lua, const StreamParseError& error) {
    sol::table table = lua.create_table();
    table["code"] = std::string(streamParseErrorCodeName(error.code));
    table["message"] = error.message;
    table["dropped_bytes"] = static_cast<std::int64_t>(error.droppedBytes);
    if (error.frameName.has_value()) {
        table["frame_name"] = *error.frameName;
    }
    if (!error.raw.empty()) {
        table["raw"] = makeBytesTable(lua, error.raw);
    }
    return table;
}

struct LoadedStreamSchema {
    explicit LoadedStreamSchema(StreamBufferDefinition buffer, std::vector<StreamFrameDefinition> frames)
        : parser(std::move(buffer), std::move(frames)) {}

    FrameStreamParser parser;
    std::unordered_map<std::string, sol::protected_function> frameCallbacks;
    sol::protected_function onError;
};

std::unique_ptr<LoadedStreamSchema> parseLoadedStreamSchema(sol::state_view lua, std::string& error) {
    const sol::object streamObject = lua["stream"];
    if (!streamObject.valid() || streamObject.get_type() == sol::type::lua_nil) {
        return nullptr;
    }
    if (!streamObject.is<sol::protected_function>()) {
        error = "stream 必须是 function";
        return nullptr;
    }

    auto streamFunction = streamObject.as<sol::protected_function>();
    auto streamResult = streamFunction();
    if (!streamResult.valid()) {
        error = "stream() 执行失败: " + protectedCallError(streamResult);
        return nullptr;
    }

    const sol::object schemaObject = streamResult.get<sol::object>();
    if (!schemaObject.valid() || schemaObject.get_type() == sol::type::lua_nil) {
        return nullptr;
    }
    if (!schemaObject.is<sol::table>()) {
        error = "stream() 必须返回 table";
        return nullptr;
    }

    const auto schemaTable = schemaObject.as<sol::table>();
    StreamBufferDefinition bufferDefinition;
    if (const sol::object bufferObject = schemaTable["buffer"]; bufferObject.valid() && bufferObject.get_type() != sol::type::lua_nil) {
        if (!bufferObject.is<sol::table>()) {
            error = "stream.buffer 必须是 table";
            return nullptr;
        }
        const auto bufferTable = bufferObject.as<sol::table>();
        if (const auto capacity = luaIntegerValue(bufferTable["capacity"]); capacity.has_value()) {
            if (*capacity <= 0) {
                error = "stream.buffer.capacity 必须大于 0";
                return nullptr;
            }
            bufferDefinition.capacity = static_cast<std::size_t>(*capacity);
        }
        if (const auto overflow = luaStringField(bufferTable, "overflow"); overflow.has_value()) {
            if (*overflow != "drop_oldest") {
                error = "stream.buffer.overflow 目前仅支持 drop_oldest";
                return nullptr;
            }
            bufferDefinition.dropOldest = true;
        }
    }

    const sol::object framesObject = schemaTable["frames"];
    if (!framesObject.valid() || !framesObject.is<sol::table>()) {
        error = "stream.frames 必须是非空数组";
        return nullptr;
    }

    const auto framesTable = framesObject.as<sol::table>();
    if (framesTable.size() == 0) {
        error = "stream.frames 不能为空";
        return nullptr;
    }

    std::vector<StreamFrameDefinition> frames;
    frames.reserve(framesTable.size());
    auto loaded = std::make_unique<LoadedStreamSchema>(bufferDefinition, std::vector<StreamFrameDefinition>{});
    std::unordered_set<std::string> frameNames;

    for (std::size_t index = 1; index <= framesTable.size(); ++index) {
        const sol::object frameObject = framesTable[index];
        if (!frameObject.valid() || !frameObject.is<sol::table>()) {
            error = "stream.frames 元素必须是 table";
            return nullptr;
        }
        const auto frameTable = frameObject.as<sol::table>();

        StreamFrameDefinition frame;
        frame.name = frameTable.get_or("name", std::string());
        if (frame.name.empty()) {
            error = "stream.frames[].name 不能为空";
            return nullptr;
        }
        if (!frameNames.insert(frame.name).second) {
            error = "stream.frames[].name 不能重复: " + frame.name;
            return nullptr;
        }

        std::string bytesError;
        const auto header = bytesFromLuaObject(frameTable["header"], bytesError);
        if (!header.has_value() || header->empty()) {
            error = "frame.header 必须是非空 byte[]";
            return nullptr;
        }
        frame.header = *header;

        const auto fixedSize = luaIntegerValue(frameTable["size"]);
        const sol::object lenObject = frameTable["len"];
        const bool hasLen = lenObject.valid() && lenObject.get_type() != sol::type::lua_nil;
        if (fixedSize.has_value() == hasLen) {
            error = "frame.size 与 frame.len 必须二选一";
            return nullptr;
        }
        if (fixedSize.has_value()) {
            if (*fixedSize <= 0) {
                error = "frame.size 必须大于 0";
                return nullptr;
            }
            frame.size = static_cast<std::size_t>(*fixedSize);
        } else {
            if (!lenObject.is<sol::table>()) {
                error = "frame.len 必须是 table";
                return nullptr;
            }
            const auto lenTable = lenObject.as<sol::table>();
            StreamLengthDefinition lenDefinition;
            const auto offset = luaIntegerValue(lenTable["offset"]);
            if (!offset.has_value() || *offset <= 0) {
                error = "frame.len.offset 必须是从 1 开始的正整数";
                return nullptr;
            }
            lenDefinition.offset = static_cast<std::size_t>(*offset - 1);

            const auto typeText = luaStringField(lenTable, "type");
            if (!typeText.has_value()) {
                error = "frame.len.type 不能为空";
                return nullptr;
            }
            const auto valueType = parseStreamValueType(*typeText);
            if (!valueType.has_value() || !streamValueTypeCanBeLength(*valueType)) {
                error = "frame.len.type 必须是整数类型";
                return nullptr;
            }
            lenDefinition.type = *valueType;

            const auto meansText = luaStringField(lenTable, "means").value_or("payload");
            const auto means = parseStreamLengthMeans(meansText);
            if (!means.has_value()) {
                error = "frame.len.means 仅支持 payload 或 frame";
                return nullptr;
            }
            lenDefinition.means = *means;
            if (const auto extra = luaIntegerValue(lenTable["extra"]); extra.has_value()) {
                if (*extra < 0) {
                    error = "frame.len.extra 不能为负数";
                    return nullptr;
                }
                lenDefinition.extra = static_cast<std::size_t>(*extra);
            }
            frame.len = lenDefinition;
        }

        if (const sol::object crcObject = frameTable["crc"]; crcObject.valid() && crcObject.get_type() != sol::type::lua_nil) {
            if (crcObject.is<bool>() && !crcObject.as<bool>()) {
                frame.crc.type = StreamCrcType::None;
            } else if (crcObject.is<sol::table>()) {
                const auto crcTable = crcObject.as<sol::table>();
                const auto crcTypeText = luaStringField(crcTable, "type");
                if (!crcTypeText.has_value()) {
                    error = "frame.crc.type 不能为空";
                    return nullptr;
                }
                const auto crcType = parseStreamCrcType(*crcTypeText);
                if (!crcType.has_value()) {
                    error = "未知 frame.crc.type: " + *crcTypeText;
                    return nullptr;
                }
                frame.crc.type = *crcType;
                const auto orderText = luaStringField(crcTable, "order").value_or("lo_hi");
                const auto order = parseStreamCrcOrder(orderText);
                if (!order.has_value()) {
                    error = "frame.crc.order 仅支持 lo_hi 或 hi_lo";
                    return nullptr;
                }
                frame.crc.order = *order;
            } else {
                error = "frame.crc 必须是 table 或 false";
                return nullptr;
            }
        }

        if (const sol::object fieldsObject = frameTable["fields"]; fieldsObject.valid() && fieldsObject.get_type() != sol::type::lua_nil) {
            if (!fieldsObject.is<sol::table>()) {
                error = "frame.fields 必须是数组";
                return nullptr;
            }
            const auto fieldsTable = fieldsObject.as<sol::table>();
            frame.fields.reserve(fieldsTable.size());
            for (std::size_t fieldIndex = 1; fieldIndex <= fieldsTable.size(); ++fieldIndex) {
                const sol::object fieldObject = fieldsTable[fieldIndex];
                if (!fieldObject.valid() || !fieldObject.is<sol::table>()) {
                    error = "frame.fields 元素必须是 table";
                    return nullptr;
                }
                const auto fieldTable = fieldObject.as<sol::table>();
                StreamFieldDefinition field;
                field.name = fieldTable.get_or("name", std::string());
                if (field.name.empty()) {
                    error = "field.name 不能为空";
                    return nullptr;
                }

                const auto typeText = luaStringField(fieldTable, "type");
                if (!typeText.has_value()) {
                    error = "field.type 不能为空";
                    return nullptr;
                }
                const auto type = parseStreamValueType(*typeText);
                if (!type.has_value()) {
                    error = "未知字段类型: " + *typeText;
                    return nullptr;
                }
                field.type = *type;

                if (const auto offset = luaIntegerValue(fieldTable["offset"]); offset.has_value()) {
                    if (*offset <= 0) {
                        error = "field.offset 必须是从 1 开始的正整数";
                        return nullptr;
                    }
                    field.offset = static_cast<std::size_t>(*offset - 1);
                }

                const sol::object countObject = fieldTable["count"];
                if (countObject.valid() && countObject.get_type() != sol::type::lua_nil) {
                    if (const auto count = luaIntegerValue(countObject); count.has_value()) {
                        if (*count < 0) {
                            error = "field.count 不能为负数";
                            return nullptr;
                        }
                        field.count.fixed = static_cast<std::size_t>(*count);
                    } else if (countObject.is<std::string>()) {
                        field.count.fieldName = countObject.as<std::string>();
                    } else if (countObject.is<sol::protected_function>()) {
                        auto countCallback = countObject.as<sol::protected_function>();
                        field.count.callback = [countCallback](const StreamFieldMap& parsed,
                                                              std::size_t frameLength,
                                                              const std::vector<std::uint8_t>& frameBytes,
                                                              const std::string& fieldName,
                                                              std::string& callbackError) -> std::optional<std::size_t> {
                            sol::state_view callbackLua(countCallback.lua_state());
                            sol::table fieldInfo = callbackLua.create_table();
                            fieldInfo["name"] = fieldName;
                            auto result = countCallback(makeStreamFieldsTable(callbackLua, parsed),
                                                       static_cast<std::int64_t>(frameLength),
                                                       makeBytesTable(callbackLua, frameBytes),
                                                       fieldInfo);
                            if (!result.valid()) {
                                callbackError = protectedCallError(result);
                                return std::nullopt;
                            }
                            const sol::object countResult = result.get<sol::object>();
                            const auto count = luaIntegerValue(countResult);
                            if (!count.has_value() || *count < 0) {
                                callbackError = "回调未返回非负整数";
                                return std::nullopt;
                            }
                            return static_cast<std::size_t>(*count);
                        };
                    } else {
                        error = "field.count 仅支持整数、字段名或 function";
                        return nullptr;
                    }
                }

                frame.fields.push_back(std::move(field));
            }
        }

        const sol::object onFrameObject = frameTable["on_frame"];
        if (!onFrameObject.valid() || onFrameObject.get_type() == sol::type::lua_nil || !onFrameObject.is<sol::protected_function>()) {
            error = "frame.on_frame 必须是 function";
            return nullptr;
        }
        loaded->frameCallbacks.emplace(frame.name, onFrameObject.as<sol::protected_function>());
        frames.push_back(std::move(frame));
    }

    if (const sol::object onErrorObject = schemaTable["on_error"]; onErrorObject.valid() && onErrorObject.get_type() != sol::type::lua_nil) {
        if (!onErrorObject.is<sol::protected_function>()) {
            error = "stream.on_error 必须是 function";
            return nullptr;
        }
        loaded->onError = onErrorObject.as<sol::protected_function>();
    }

    loaded->parser = FrameStreamParser(bufferDefinition, std::move(frames));
    return loaded;
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
        descriptor.ratio = finiteOrDefault(luaNumberField(channelTable, "ratio").value_or(1.0), 1.0);
        descriptor.scale = finiteOrDefault(luaNumberField(channelTable, "scale").value_or(1.0), 1.0);
        descriptor.offset = finiteOrDefault(luaNumberField(channelTable, "offset").value_or(0.0), 0.0);
        if (const auto colorText = luaStringField(channelTable, "color"); colorText.has_value()) {
            descriptor.color = parseColorText(*colorText);
            if (!descriptor.color.has_value()) {
                error = "plot.setup.channels[" + std::to_string(index)
                    + "].color 必须是 #RRGGBB 或 #RRGGBBAA";
                return std::nullopt;
            }
        }
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

const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls, const std::string& id) {
    for (const auto& control : controls) {
        if (control.id == id) {
            return &control;
        }
    }
    return nullptr;
}

namespace script_host_lua {

std::string serializeLuaObject(const sol::object& object, int depth) {
    return protoscope::scripting::serializeLuaObject(object, depth);
}

const ControlDescriptor* findControlDescriptor(const std::vector<ControlDescriptor>& controls, const std::string& id) {
    return protoscope::scripting::findControlDescriptor(controls, id);
}

std::optional<ControlValue> controlValueFromLua(const ControlDescriptor& descriptor,
                                                const sol::object& object,
                                                std::string& error) {
    return protoscope::scripting::controlValueFromLua(descriptor, object, error);
}

sol::object controlValueToLua(sol::state_view lua,
                              const ControlDescriptor* descriptor,
                              const ControlValue& value) {
    return protoscope::scripting::controlValueToLua(lua, descriptor, value);
}

std::optional<std::vector<std::uint8_t>> bytesFromLuaObject(const sol::object& object, std::string& error) {
    return protoscope::scripting::bytesFromLuaObject(object, error);
}

std::optional<PlotSetup> parsePlotSetup(const sol::object& object, std::string& error) {
    return protoscope::scripting::parsePlotSetup(object, error);
}

std::optional<plot::WaveAppendRequest> parsePlotAppend(const sol::object& object, std::string& error) {
    return protoscope::scripting::parsePlotAppend(object, error);
}

} // namespace script_host_lua

struct ScriptHost::Runtime {
    sol::state lua;
    std::unique_ptr<LoadedStreamSchema> stream;
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

ScriptHost::ScriptHost()
    : runtime_(std::make_unique<Runtime>()) {}

ScriptHost::~ScriptHost() = default;

void ScriptHost::setFileIoConfig(FileIoConfig config) {
    fileIoConfig_ = std::move(config);
}

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
    lua.new_usertype<ProtoBuffer>("ProtoBuffer",
                                  "size",
                                  &ProtoBuffer::size,
                                  "slice",
                                  &ProtoBuffer::slice,
                                  "to_hex",
                                  &ProtoBuffer::toHex,
                                  "bytes",
                                  [this](const ProtoBuffer& buffer, const sol::object& maxBytes) {
                                      std::size_t limit = buffer.bytes.size();
                                      if (maxBytes.valid() && maxBytes.get_type() != sol::type::lua_nil && maxBytes.is<int>()) {
                                          limit = std::min<std::size_t>(limit, static_cast<std::size_t>(std::max(0, maxBytes.as<int>())));
                                      }
                                      limit = std::min(limit, fileIoConfig_.maxChunkBytes);
                                      sol::table table = runtime_->lua.create_table(static_cast<int>(limit), 0);
                                      for (std::size_t index = 0; index < limit; ++index) {
                                          table[index + 1] = buffer.bytes[index];
                                      }
                                      return table;
                                  });

    // 将协议脚本目录加入 Lua 模块搜索路径，使 main.lua 可 require 同目录模块。
    // 额外放开父目录，方便 protocols/<demo>/main.lua 共享 protocols/*.lua 公共脚本。
    // 使用 generic_string 格式，统一为 /，避免 Windows 反斜杠干扰 Lua package.path。
    const auto pkgPath = lua["package"]["path"].get<std::string>();
    const auto protocolParent = std::filesystem::path(protocolDirectory_).parent_path().generic_string();
    lua["package"]["path"] = protocolDirectory_ + "/?.lua;"
                           + protocolDirectory_ + "/?/init.lua;"
                           + (protocolParent.empty() ? std::string() : protocolParent + "/?.lua;")
                           + (protocolParent.empty() ? std::string() : protocolParent + "/?/init.lua;")
                           + pkgPath;
    auto proto = lua.create_named_table("proto");

    // 核心流程：所有脚本侧能力统一经由模块注册器挂到 proto.*，避免加载流程继续膨胀。
    registerLuaApi(proto);

    auto scriptResult = lua.safe_script_file(path, &sol::script_pass_on_error);
    if (!scriptResult.valid()) {
        setLastError("执行脚本失败: " + protectedCallError(scriptResult));
        protoLog("error", lastError_);
        return false;
    }

    std::string streamError;
    auto streamSchema = parseLoadedStreamSchema(lua, streamError);
    if (!streamError.empty()) {
        setLastError(streamError);
        protoLog("error", lastError_);
        return false;
    }
    runtime_->stream = std::move(streamSchema);

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

void ScriptHost::onTransportOpen(const transport::TransportOpenEvent& event) {
    activeConnection_ = event.context;
    if (runtime_->stream) {
        runtime_->stream->parser.reset();
    }
    callbackOnOpen(ScriptHostContext{event.context});
}

void ScriptHost::onTransportClose(const transport::TransportCloseEvent& event) {
    callbackOnClose(ScriptHostContext{event.context});
    if (activeConnection_.has_value() && activeConnection_->connectionId == event.context.connectionId) {
        activeConnection_.reset();
    }
    if (runtime_->stream) {
        runtime_->stream->parser.reset();
    }
}

void ScriptHost::onTransportError(const transport::TransportErrorEvent& event) {
    callbackOnError(ScriptHostContext{event.context}, event.message);
}

void ScriptHost::onTransportBytes(const transport::TransportBytesEvent& event) {
    if (event.context.readyForIo) {
        activeConnection_ = event.context;
    }
    if (runtime_->stream) {
        const auto batch = runtime_->stream->parser.pushBytes(event.bytes);
        for (const auto& error : batch.errors) {
            callbackOnStreamError(ScriptHostContext{event.context}, error);
        }
        for (const auto& frame : batch.frames) {
            callbackOnStreamFrame(ScriptHostContext{event.context}, frame);
        }
        return;
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

bool ScriptHost::setControlValue(const std::string& id, const ControlValue& value) {
    const auto* descriptor = findControlDescriptor(controls_, id);
    if (descriptor == nullptr) {
        return false;
    }
    controlValues_[id] = value;
    return true;
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

std::vector<TxRequest> ScriptHost::drainTxRequests() {
    auto drained = std::move(txRequests_);
    txRequests_.clear();
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

std::vector<RequestDoneResult> ScriptHost::drainRequestDoneResults() {
    auto drained = std::move(requestDoneResults_);
    requestDoneResults_.clear();
    return drained;
}

std::vector<StatusUpdate> ScriptHost::drainStatusUpdates() {
    auto drained = std::move(statusUpdates_);
    statusUpdates_.clear();
    return drained;
}

std::vector<DialogRequest> ScriptHost::drainDialogRequests() {
    auto drained = std::move(dialogRequests_);
    dialogRequests_.clear();
    return drained;
}

std::vector<FileDialogRequest> ScriptHost::drainFileDialogRequests() {
    auto drained = std::move(fileDialogRequests_);
    fileDialogRequests_.clear();
    return drained;
}

void ScriptHost::registerLuaApi(sol::table& proto) {
    ScriptHostQueues queues;
    ScriptHostContextInternal ctx{*this, queues, fileIoConfig_, activeConnection_};
    std::array modules{
        FunctionScriptHostApiModule{"core_api_module", [this](sol::table& table) { registerCoreApi(table); }},
        FunctionScriptHostApiModule{"tx_api_module", [this](sol::table& table) { registerTxApi(table); }},
        FunctionScriptHostApiModule{"status_api_module", [this](sol::table& table) { registerStatusApi(table); }},
        FunctionScriptHostApiModule{"ui_api_module", [this](sol::table& table) { registerUiApi(table); }},
        FunctionScriptHostApiModule{"file_api_module", [this](sol::table& table) { registerFileApi(table); }},
        FunctionScriptHostApiModule{"plot_api_module", [this](sol::table& table) { registerPlotApi(table); }},
        FunctionScriptHostApiModule{"control_api_module", [this](sol::table& table) { registerControlApi(table); }},
        FunctionScriptHostApiModule{"codec_api_module", [this](sol::table& table) { registerCodecApi(table); }},
    };

    // 核心流程：统一从 API 模块列表注册 Lua 函数，保持 wire format 不变，只收束宿主内部编排边界。
    for (auto& module : modules) {
        module.registerApi(ctx, proto);
    }
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

void ScriptHost::onTxEvent(const transport::ConnectionContext& ctx, const TxEvent& event) {
    const bool releaseFileChunk = event.state == TxEventState::Sent
        || event.state == TxEventState::Rejected
        || event.state == TxEventState::Dropped
        || event.state == TxEventState::Canceled
        || event.state == TxEventState::Timeout;
    if (event.fileJobId != 0 && releaseFileChunk) {
        const auto iter = fileSendJobs_.find(event.fileJobId);
        if (iter != fileSendJobs_.end()) {
            iter->second.inflight = iter->second.inflight == 0 ? 0 : iter->second.inflight - 1;
            pumpFileSendJob(event.fileJobId);
        }
    }
    callbackOnTx(ScriptHostContext{ctx}, event);
}

void ScriptHost::onDialogEvent(const transport::ConnectionContext& ctx, const DialogEvent& event) {
    callbackOnDialog(ScriptHostContext{ctx}, event);
}

void ScriptHost::onFileDialogEvent(const transport::ConnectionContext& ctx, const FileDialogEvent& event) {
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

void ScriptHost::setRequestAwaitingCompletion(bool active) {
    requestAwaitingCompletion_ = active;
}

sol::state& ScriptHost::luaState() {
    return runtime_->lua;
}

sol::state_view ScriptHost::luaView() {
    return sol::state_view(runtime_->lua.lua_state());
}

const std::vector<ControlDescriptor>& ScriptHost::controlDescriptors() const {
    return controls_;
}

const ControlValue* ScriptHost::findControlValue(const std::string& id) const {
    const auto iter = controlValues_.find(id);
    return iter == controlValues_.end() ? nullptr : &iter->second;
}

void ScriptHost::updateControlValue(const std::string& id, ControlValue value) {
    controlValues_[id] = std::move(value);
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

std::optional<TxRequest> ScriptHost::protoSendLike(TxRequestKind kind,
                                                   const sol::object& payload,
                                                   const sol::object& opts,
                                                   std::string& error) {
    const auto maybeBytes = bytesFromLuaObject(payload, error);
    if (!maybeBytes.has_value()) {
        protoLog("error", std::string(kind == TxRequestKind::Request ? "proto.request" : "proto.send") + " 调用失败: " + error);
        return std::nullopt;
    }

    TxRequest request{};
    request.id = nextTxRequestId();
    request.kind = kind;
    request.payload = *maybeBytes;
    request.timeoutMs = 0;
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
            protoLog("error", std::string(kind == TxRequestKind::Request ? "proto.request" : "proto.send") + " 调用失败: " + error);
            return std::nullopt;
        }
        const sol::table options = opts.as<sol::table>();
        if (const auto timeoutMs = luaNumberField(options, "timeout_ms"); timeoutMs.has_value()) {
            request.timeoutMs = static_cast<std::uint64_t>(std::max(0.0, *timeoutMs));
        }
        request.tag = luaStringField(options, "tag").value_or("");
    }

    txRequests_.push_back(request);
    return request;
}

bool ScriptHost::protoRequestDone(const sol::object& result, std::string& error) {
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

void ScriptHost::protoStatusSet(const std::string& text, const sol::object& opts) {
    StatusUpdate update{};
    update.text = text;
    update.level = "info";
    update.timestampMs = nowMs();
    if (opts.valid() && opts.get_type() != sol::type::lua_nil && opts.is<sol::table>()) {
        update.level = luaStringField(opts.as<sol::table>(), "level").value_or(update.level);
    }
    statusUpdates_.push_back(std::move(update));
}

void ScriptHost::protoStatusClear() {
    StatusUpdate update{};
    update.text.clear();
    update.level = "info";
    update.clear = true;
    update.timestampMs = nowMs();
    statusUpdates_.push_back(std::move(update));
}

std::optional<DialogRequest> ScriptHost::protoDialog(DialogKind kind, const sol::object& opts, std::string& error) {
    if (!opts.is<sol::table>()) {
        error = "opts 必须是 table";
        protoLog("error", std::string(kind == DialogKind::Alert ? "proto.ui.alert" : "proto.ui.confirm") + " 调用失败: " + error);
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
    const DialogRequest request{
        .id = nextDialogId(),
        .kind = kind,
        .connection = connection,
        .title = luaStringField(table, "title").value_or(""),
        .message = luaStringField(table, "message").value_or(""),
        .level = luaStringField(table, "level").value_or("info"),
        .dedupeKey = luaStringField(table, "dedupe_key").value_or(""),
        .createdAtMs = createdAtMs,
    };
    if (request.title.empty() || request.message.empty()) {
        error = "title 和 message 不能为空";
        protoLog("error", std::string(kind == DialogKind::Alert ? "proto.ui.alert" : "proto.ui.confirm") + " 调用失败: " + error);
        return std::nullopt;
    }
    dialogRequests_.push_back(request);
    return request;
}

std::optional<FileDialogRequest> ScriptHost::protoFileDialog(FileDialogKind kind, const sol::object& opts, std::string& error) {
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
    if (kind == FileDialogKind::OpenFile) {
        const auto mode = luaStringField(table, "mode").value_or("open");
        if (mode == "save") {
            kind = FileDialogKind::SaveFile;
        } else if (mode != "open") {
            error = "mode 必须是 open 或 save";
            return std::nullopt;
        }
    }

    const auto createdAtMs = nowMs();
    transport::ConnectionContext connection{};
    if (activeConnection_.has_value()) {
        connection = *activeConnection_;
    } else {
        connection.timestampMs = createdAtMs;
        connection.endpoint = "detached";
    }
    FileDialogRequest request{
        .id = nextFileDialogId(),
        .kind = kind,
        .connection = connection,
        .title = luaStringField(table, "title").value_or(kind == FileDialogKind::OpenDir ? "选择目录" : "选择文件"),
        .defaultPath = luaStringField(table, "default_path").value_or("."),
        .filters = {},
        .createdAtMs = createdAtMs,
    };

    const sol::object filtersObject = table["filters"];
    if (filtersObject.valid() && filtersObject.get_type() != sol::type::lua_nil) {
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
            const auto filterTable = filterObject.as<sol::table>();
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
            request.filters.push_back(std::move(filter));
        }
    }

    fileDialogRequests_.push_back(request);
    return request;
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsOpen(const std::string& pathText, const sol::object& opts) {
    auto fail = [this](const std::string& error) {
        return std::make_tuple(sol::make_object(runtime_->lua, sol::lua_nil), sol::make_object(runtime_->lua, error));
    };
    if (!fileIoConfig_.enabled) {
        return fail("scripting.file_io 已禁用");
    }
    if (fileHandles_.size() >= fileIoConfig_.maxOpenFiles) {
        return fail("打开文件数超过 max_open_files");
    }

    std::string mode = "read";
    bool createDirs = false;
    bool overwrite = false;
    if (opts.valid() && opts.get_type() != sol::type::lua_nil) {
        if (!opts.is<sol::table>()) {
            return fail("opts 必须是 table");
        }
        const auto table = opts.as<sol::table>();
        mode = luaStringField(table, "mode").value_or(mode);
        createDirs = luaBoolField(table, "create_dirs").value_or(false);
        overwrite = luaBoolField(table, "overwrite").value_or(false);
    }
    const bool writeMode = mode == "write" || mode == "append";
    const bool readMode = mode == "read";
    if (!readMode && !writeMode) {
        return fail("mode 必须是 read/write/append");
    }

    auto path = std::filesystem::path(pathText);
    if (path.is_relative() && !protocolDirectory_.empty()) {
        path = std::filesystem::path(protocolDirectory_) / path;
    }
    path = canonicalPath(path);

    auto authorized = [&](bool writeAccess) {
        if (fileIoConfig_.allowProtocolDir && !protocolDirectory_.empty()
            && isSameOrChildPath(canonicalPath(protocolDirectory_), path, true)) {
            return true;
        }
        for (const auto& root : fileIoConfig_.extraAllowedRoots) {
            if (!root.empty() && isSameOrChildPath(canonicalPath(root), path, true)) {
                return true;
            }
        }
        for (const auto& root : dialogAuthorizedPaths_) {
            if ((!writeAccess || root.writable) && (writeAccess || root.readable)
                && isSameOrChildPath(root.path, path, root.recursive)) {
                return true;
            }
        }
        return false;
    };
    if (!authorized(writeMode)) {
        return fail("路径未授权: " + path.generic_string());
    }

    std::error_code errorCode;
    if (readMode) {
        if (!std::filesystem::is_regular_file(path, errorCode)) {
            return fail("文件不存在或不是普通文件: " + path.generic_string());
        }
        if (std::filesystem::file_size(path, errorCode) > fileIoConfig_.maxFileSizeBytes) {
            return fail("文件大小超过 max_file_size_bytes");
        }
    } else {
        if (createDirs) {
            std::filesystem::create_directories(path.parent_path(), errorCode);
            if (errorCode) {
                return fail("创建目录失败: " + errorCode.message());
            }
        }
        if (mode == "write" && std::filesystem::exists(path, errorCode) && !overwrite) {
            return fail("文件已存在，需设置 overwrite=true");
        }
    }

    auto handle = std::make_unique<FileHandle>();
    handle->id = nextFileHandleId();
    handle->path = path;
    handle->readable = readMode;
    handle->writable = writeMode;
    auto openMode = std::ios::binary;
    if (readMode) {
        openMode |= std::ios::in;
    } else {
        openMode |= std::ios::out;
        openMode |= mode == "append" ? std::ios::app : std::ios::trunc;
    }
    handle->stream.open(path, openMode);
    if (!handle->stream.is_open()) {
        return fail("打开文件失败: " + path.generic_string());
    }

    const auto id = handle->id;
    fileHandles_[id] = std::move(handle);
    return std::make_tuple(sol::make_object(runtime_->lua, id), sol::make_object(runtime_->lua, sol::lua_nil));
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsRead(std::uint64_t handleId, const sol::object& opts) {
    auto fail = [this](const std::string& error) {
        return std::make_tuple(sol::make_object(runtime_->lua, sol::lua_nil), sol::make_object(runtime_->lua, error));
    };
    const auto iter = fileHandles_.find(handleId);
    if (iter == fileHandles_.end() || !iter->second->readable) {
        return fail("文件句柄不可读或已关闭");
    }

    std::size_t maxBytes = fileIoConfig_.defaultChunkBytes;
    if (opts.valid() && opts.get_type() != sol::type::lua_nil) {
        if (!opts.is<sol::table>()) {
            return fail("opts 必须是 table");
        }
        const sol::object value = opts.as<sol::table>()["max_bytes"];
        if (value.valid() && value.is<int>()) {
            maxBytes = static_cast<std::size_t>(std::max(1, value.as<int>()));
        }
    }
    maxBytes = std::min(positiveSizeOrDefault(maxBytes, fileIoConfig_.defaultChunkBytes), fileIoConfig_.maxChunkBytes);

    ProtoBuffer buffer;
    buffer.bytes.resize(maxBytes);
    auto& stream = iter->second->stream;
    stream.read(reinterpret_cast<char*>(buffer.bytes.data()), static_cast<std::streamsize>(buffer.bytes.size()));
    const auto readCount = stream.gcount();
    if (readCount <= 0) {
        return fail("eof");
    }
    buffer.bytes.resize(static_cast<std::size_t>(readCount));
    return std::make_tuple(sol::make_object(runtime_->lua, std::move(buffer)), sol::make_object(runtime_->lua, sol::lua_nil));
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsWrite(std::uint64_t handleId, const sol::object& payload) {
    auto fail = [this](const std::string& error) {
        return std::make_tuple(sol::make_object(runtime_->lua, false), sol::make_object(runtime_->lua, error));
    };
    const auto iter = fileHandles_.find(handleId);
    if (iter == fileHandles_.end() || !iter->second->writable) {
        return fail("文件句柄不可写或已关闭");
    }
    std::string error;
    const auto bytes = bytesFromLuaObject(payload, error);
    if (!bytes.has_value()) {
        return fail(error);
    }
    auto& handle = *iter->second;
    if (handle.bytesWritten + bytes->size() > fileIoConfig_.maxWriteFileSizeBytes) {
        return fail("写入大小超过 max_write_file_size_bytes");
    }
    handle.stream.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    if (!handle.stream.good()) {
        return fail("写入文件失败");
    }
    handle.bytesWritten += bytes->size();
    return std::make_tuple(sol::make_object(runtime_->lua, true), sol::make_object(runtime_->lua, sol::lua_nil));
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsClose(std::uint64_t handleId) {
    const auto iter = fileHandles_.find(handleId);
    if (iter == fileHandles_.end()) {
        return std::make_tuple(sol::make_object(runtime_->lua, false), sol::make_object(runtime_->lua, "文件句柄已关闭"));
    }
    fileHandles_.erase(iter);
    return std::make_tuple(sol::make_object(runtime_->lua, true), sol::make_object(runtime_->lua, sol::lua_nil));
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsStat(const std::string& pathText) {
    auto fail = [this](const std::string& error) {
        return std::make_tuple(sol::make_object(runtime_->lua, sol::lua_nil), sol::make_object(runtime_->lua, error));
    };
    if (!fileIoConfig_.enabled) {
        return fail("scripting.file_io 已禁用");
    }
    auto path = std::filesystem::path(pathText);
    if (path.is_relative() && !protocolDirectory_.empty()) {
        path = std::filesystem::path(protocolDirectory_) / path;
    }
    path = canonicalPath(path);
    const bool authorized = (fileIoConfig_.allowProtocolDir && !protocolDirectory_.empty()
                             && isSameOrChildPath(canonicalPath(protocolDirectory_), path, true))
        || std::any_of(fileIoConfig_.extraAllowedRoots.begin(),
                       fileIoConfig_.extraAllowedRoots.end(),
                       [&](const std::string& root) {
                           return !root.empty() && isSameOrChildPath(canonicalPath(root), path, true);
                       })
        || std::any_of(dialogAuthorizedPaths_.begin(),
                       dialogAuthorizedPaths_.end(),
                       [&](const AuthorizedPath& root) {
                           return root.readable && isSameOrChildPath(root.path, path, root.recursive);
                       });
    if (!authorized) {
        return fail("路径未授权: " + path.generic_string());
    }

    std::error_code errorCode;
    if (!std::filesystem::exists(path, errorCode)) {
        return fail("路径不存在: " + path.generic_string());
    }
    sol::table table = runtime_->lua.create_table();
    table["size"] = std::filesystem::is_regular_file(path, errorCode)
        ? static_cast<std::uint64_t>(std::filesystem::file_size(path, errorCode))
        : 0U;
    table["mtime_ms"] = 0;
    table["is_file"] = std::filesystem::is_regular_file(path, errorCode);
    table["is_dir"] = std::filesystem::is_directory(path, errorCode);
    return std::make_tuple(sol::make_object(runtime_->lua, table), sol::make_object(runtime_->lua, sol::lua_nil));
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsSendFile(const std::string& pathText, const sol::object& opts) {
    std::string kind = "send";
    std::string tag = "file";
    std::size_t chunkSize = fileIoConfig_.sendFile.defaultChunkBytes;
    if (opts.valid() && opts.get_type() != sol::type::lua_nil) {
        if (!opts.is<sol::table>()) {
            return std::make_tuple(sol::make_object(runtime_->lua, sol::lua_nil), sol::make_object(runtime_->lua, "opts 必须是 table"));
        }
        const auto table = opts.as<sol::table>();
        kind = luaStringField(table, "kind").value_or(kind);
        tag = luaStringField(table, "tag").value_or(tag);
        const sol::object chunk = table["chunk_size"];
        if (chunk.valid() && chunk.is<int>()) {
            chunkSize = static_cast<std::size_t>(std::max(1, chunk.as<int>()));
        }
    }
    chunkSize = std::min(chunkSize, fileIoConfig_.maxChunkBytes);
    const auto [handleObject, openError] = protoFsOpen(pathText, sol::make_object(runtime_->lua, sol::lua_nil));
    if (!handleObject.valid() || handleObject.get_type() == sol::type::lua_nil) {
        return std::make_tuple(sol::make_object(runtime_->lua, sol::lua_nil), openError);
    }

    const auto handleId = handleObject.as<std::uint64_t>();
    const auto total = std::filesystem::file_size(fileHandles_[handleId]->path);
    const auto jobId = nextFileJobId();
    fileSendJobs_[jobId] = FileSendJob{
        .id = jobId,
        .handleId = handleId,
        .kind = kind == "request" ? TxRequestKind::Request : TxRequestKind::Send,
        .tag = tag,
        .chunkSize = chunkSize,
        .total = total,
    };
    pumpFileSendJob(jobId);
    return std::make_tuple(sol::make_object(runtime_->lua, jobId), sol::make_object(runtime_->lua, sol::lua_nil));
}

void ScriptHost::pumpFileSendJob(std::uint64_t jobId) {
    auto jobIter = fileSendJobs_.find(jobId);
    if (jobIter == fileSendJobs_.end()) {
        return;
    }
    auto& job = jobIter->second;
    const std::size_t maxInflight = std::max<std::size_t>(1, fileIoConfig_.sendFile.maxInflightChunks);
    while (!job.eof && job.inflight < maxInflight) {
        sol::table readOpts = runtime_->lua.create_table();
        readOpts["max_bytes"] = static_cast<int>(job.chunkSize);
        const auto offset = job.nextOffset;
        const auto [bufferObject, readError] = protoFsRead(job.handleId, sol::make_object(runtime_->lua, readOpts));
        if (!bufferObject.valid() || bufferObject.get_type() == sol::type::lua_nil) {
            job.eof = true;
            break;
        }

        std::string error;
        const auto nilObject = sol::make_object(runtime_->lua, sol::lua_nil);
        const auto request = protoSendLike(job.kind, bufferObject, nilObject, error);
        if (!request.has_value()) {
            protoLog("error", "proto.fs.send_file 发送分块失败: " + error);
            job.eof = true;
            break;
        }
        if (!job.tag.empty()) {
            txRequests_.back().tag = job.tag;
        }
        txRequests_.back().fileJobId = job.id;
        txRequests_.back().fileOffset = offset;
        txRequests_.back().fileTotal = job.total;
        job.nextOffset += txRequests_.back().payload.size();
        ++job.inflight;
    }

    if (job.eof && job.inflight == 0) {
        const auto handleId = job.handleId;
        fileSendJobs_.erase(jobIter);
        protoFsClose(handleId);
    }
}

std::uint64_t ScriptHost::nextTxRequestId() {
    return nextTxRequestId_++;
}

std::uint64_t ScriptHost::nextDialogId() {
    return nextDialogId_++;
}

std::uint64_t ScriptHost::nextFileDialogId() {
    return nextFileDialogId_++;
}

std::uint64_t ScriptHost::nextFileHandleId() {
    return nextFileHandleId_++;
}

std::uint64_t ScriptHost::nextFileJobId() {
    return nextFileJobId_++;
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

void ScriptHost::setLastError(std::string message) {
    lastError_ = std::move(message);
}

} // namespace protoscope::scripting
