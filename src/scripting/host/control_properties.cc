#include "control_properties.hpp"
#include "script_host_internal.hpp"
#include "script_host_lua_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace protoscope::scripting {
bool validateControlValue(const ControlDescriptor& descriptor, const ControlValue& value)
{
    if (value.index() != defaultValueFor(descriptor).index()) return false;
    std::optional<double> number;
    if (const auto* integer = std::get_if<int>(&value)) number = *integer;
    if (const auto* floating = std::get_if<float>(&value)) number = *floating;
    if (number && (!std::isfinite(*number) ||
        (descriptor.minimum && *number < *descriptor.minimum) ||
        (descriptor.maximum && *number > *descriptor.maximum))) return false;
    if (descriptor.type == ControlType::Combo) {
        const auto index = std::get<int>(value);
        return descriptor.comboOptions.empty() ? index == 0 :
            index >= 0 && static_cast<std::size_t>(index) < descriptor.comboOptions.size();
    }
    return true;
}

bool applyControlProperties(ControlDescriptor& descriptor, const sol::table& patch,
                            bool strict, std::string& error)
{
    static const std::set<std::string> names{
        "value", "label", "visible", "disabled", "read_only", "tooltip", "min", "max", "options"};
    try {
        if (strict) {
            for (const auto& [key, value] : patch) {
                if (key.get_type() != sol::type::string || !names.contains(key.as<std::string>()))
                    throw std::invalid_argument("unknown control property");
            }
        }
        auto text = [&](const char* key, std::string& target) {
            const sol::object value = patch[key];
            if (!value.valid() || value.get_type() == sol::type::lua_nil) return;
            if (value.get_type() != sol::type::string) throw std::invalid_argument(std::string(key) + " must be string");
            const auto view = value.as<std::string_view>();
            if (view.size() > 4096 || view.find('\0') != view.npos)
                throw std::invalid_argument("control text exceeds limit or contains NUL");
            target = std::string(view);
        };
        text("label", descriptor.label); text("tooltip", descriptor.tooltip);
        auto boolean = [&](const char* key, bool& target) {
            const sol::object value = patch[key];
            if (!value.valid() || value.get_type() == sol::type::lua_nil) return;
            if (value.get_type() != sol::type::boolean) throw std::invalid_argument(std::string(key) + " must be boolean");
            target = value.as<bool>();
        };
        boolean("visible", descriptor.visible);
        boolean("disabled", descriptor.disabled);
        boolean("read_only", descriptor.readOnly);
        auto bound = [&](const char* key, std::optional<double>& target) {
            const sol::object value = patch[key];
            if (!value.valid() || value.get_type() == sol::type::lua_nil) return;
            if (descriptor.type != ControlType::InputInt && descriptor.type != ControlType::InputFloat)
                throw std::invalid_argument("numeric constraints require numeric input");
            if (value.get_type() == sol::type::boolean && !value.as<bool>()) {
                target.reset(); return;
            }
            if (value.get_type() != sol::type::number) throw std::invalid_argument("numeric constraint must be number");
            const double number = value.as<double>();
            if (!std::isfinite(number)) throw std::invalid_argument("numeric constraint must be finite");
            if (descriptor.type == ControlType::InputInt &&
                (std::floor(number) != number || number < std::numeric_limits<int>::min() ||
                 number > std::numeric_limits<int>::max()))
                throw std::invalid_argument("integer constraint outside range");
            target = number;
        };
        bound("min", descriptor.minimum); bound("max", descriptor.maximum);
        if (descriptor.minimum && descriptor.maximum && *descriptor.minimum > *descriptor.maximum)
            throw std::invalid_argument("min exceeds max");
        const sol::object options = patch["options"];
        if (strict && options.valid() && options.get_type() != sol::type::lua_nil) {
            if (descriptor.type != ControlType::Combo || options.get_type() != sol::type::table)
                throw std::invalid_argument("options require a combo string array");
            std::map<int, std::string> entries;
            for (const auto& [key, value] : options.as<sol::table>()) {
                if (key.get_type() != sol::type::number || !key.is<int>() ||
                    key.as<int>() < 1 || key.as<int>() > 1024 || value.get_type() != sol::type::string)
                    throw std::invalid_argument("invalid combo options");
                const auto item = value.as<std::string_view>();
                if (item.size() > 4096 || item.find('\0') != item.npos)
                    throw std::invalid_argument("combo option exceeds limit or contains NUL");
                entries.emplace(key.as<int>(), std::string(item));
            }
            if (!entries.empty() && entries.rbegin()->first != static_cast<int>(entries.size()))
                throw std::invalid_argument("sparse combo options");
            descriptor.comboOptions.clear();
            for (auto& [key, value] : entries) descriptor.comboOptions.push_back(std::move(value));
        }
        return true;
    } catch (const std::exception& failure) {
        error = failure.what(); return false;
    }
}

bool ScriptHost::updateControlProperties(const sol::table& patches, std::string& error)
{
    if (!scriptLoaded_) {
        error = "control updates are unavailable during declaration";
        return false;
    }
    try {
        // 全部修改先作用于副本，任一校验失败都不改变宿主值和 Dock 描述。
        auto controls = controls_;
        auto values = controlValues_;
        std::size_t count = 0;
        for (const auto& [key, item] : patches) {
            if (++count > 4096 || key.get_type() != sol::type::string || item.get_type() != sol::type::table)
                throw std::invalid_argument("patches must map control IDs to tables");
            const auto id = key.as<std::string>();
            const auto iter = std::find_if(controls.begin(), controls.end(), [&](const auto& c) { return c.id == id; });
            if (iter == controls.end()) throw std::invalid_argument("unknown control: " + id);
            const auto patch = item.as<sol::table>();
            if (!applyControlProperties(*iter, patch, true, error)) return false;
            const sol::object input = patch["value"];
            if (input.valid() && input.get_type() != sol::type::lua_nil) {
                // 新原子 API 不沿用旧 set_control 的截断和下标钳制。
                if (iter->type == ControlType::Combo || iter->type == ControlType::InputInt) {
                    if (input.get_type() != sol::type::number || !input.is<int>())
                        throw std::invalid_argument("control value must be integer");
                    values[id] = input.as<int>();
                } else {
                    auto value = script_host_lua::controlValueFromLua(*iter, input, error);
                    if (!value) return false;
                    if (iter->type == ControlType::ValueTable) {
                        auto& current = std::get<ValueTableValue>(values.at(id)).rows;
                        const auto& changes = std::get<ValueTableValue>(*value).rows;
                        for (std::size_t i = 0; i < std::min(current.size(), changes.size()); ++i)
                            if (changes[i].set) current[i] = changes[i];
                    } else values[id] = std::move(*value);
                }
            }
            if (!validateControlValue(*iter, values.at(id))) throw std::invalid_argument("control value violates constraints");
        }
        auto docks = docks_;
        for (auto& dock : docks) {
            for (auto& control : dock.controls) {
                control = *std::find_if(controls.begin(), controls.end(),
                                       [&](const auto& c) { return c.id == control.id; });
            }
        }
        controls_.swap(controls); controlValues_.swap(values); docks_.swap(docks);
        return true;
    } catch (const std::exception& failure) {
        error = failure.what(); return false;
    }
}
} // namespace protoscope::scripting
