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
    if (const auto* text = std::get_if<std::string>(&value); text && descriptor.maxLength != 0) {
        if (text->size() > descriptor.maxLength || text->find('\0') != text->npos) return false;
    }
    if (descriptor.type == ControlType::TabSelection) {
        const auto& selected = std::get<std::string>(value);
        return std::find(descriptor.comboOptions.begin(), descriptor.comboOptions.end(), selected) !=
               descriptor.comboOptions.end();
    }
    std::optional<double> number;
    if (const auto* integer = std::get_if<int>(&value)) number = *integer;
    if (const auto* floating = std::get_if<float>(&value)) number = *floating;
    if (number && (!std::isfinite(*number) ||
        (descriptor.minimum && *number < *descriptor.minimum) ||
        (descriptor.maximum && *number > *descriptor.maximum))) return false;
    if (controlValueKind(descriptor.type) == ControlType::Combo) {
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
        "value", "label", "visible", "disabled", "read_only", "tooltip", "min", "max", "options",
        "max_length", "wrap", "indeterminate"};
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
        boolean("wrap", descriptor.wrap); boolean("indeterminate", descriptor.indeterminate);
        const sol::object length = patch["max_length"];
        if (length.valid() && length.get_type() != sol::type::lua_nil) {
            if (controlValueKind(descriptor.type) != ControlType::InputText ||
                length.get_type() != sol::type::number || !length.is<int>() ||
                length.as<int>() < 1 || length.as<int>() > 262144)
                throw std::invalid_argument("max_length requires a text control and 1..262144 bytes");
            descriptor.maxLength = static_cast<std::size_t>(length.as<int>());
        }
        auto bound = [&](const char* key, std::optional<double>& target) {
            const sol::object value = patch[key];
            if (!value.valid() || value.get_type() == sol::type::lua_nil) return;
            const auto kind = controlValueKind(descriptor.type);
            if (kind != ControlType::InputInt && kind != ControlType::InputFloat)
                throw std::invalid_argument("numeric constraints require numeric input");
            if (value.get_type() == sol::type::boolean && !value.as<bool>()) {
                target.reset(); return;
            }
            if (value.get_type() != sol::type::number) throw std::invalid_argument("numeric constraint must be number");
            const double number = value.as<double>();
            if (!std::isfinite(number)) throw std::invalid_argument("numeric constraint must be finite");
            if (kind == ControlType::InputInt &&
                (std::floor(number) != number || number < std::numeric_limits<int>::min() ||
                 number > std::numeric_limits<int>::max()))
                throw std::invalid_argument("integer constraint outside range");
            target = number;
        };
        bound("min", descriptor.minimum); bound("max", descriptor.maximum);
        if (descriptor.minimum && descriptor.maximum && *descriptor.minimum > *descriptor.maximum)
            throw std::invalid_argument("min exceeds max");
        if ((descriptor.type == ControlType::SliderInt || descriptor.type == ControlType::SliderFloat) &&
            (!descriptor.minimum || !descriptor.maximum || *descriptor.minimum >= *descriptor.maximum))
            throw std::invalid_argument("slider requires min < max");
        if (descriptor.type == ControlType::SliderInt &&
            (*descriptor.minimum < std::numeric_limits<int>::min()/2 ||
             *descriptor.maximum > std::numeric_limits<int>::max()/2))
            throw std::invalid_argument("slider_int range exceeds renderer limit");
        if (descriptor.type == ControlType::SliderFloat &&
            (*descriptor.minimum < -std::numeric_limits<float>::max()/2 ||
             *descriptor.maximum > std::numeric_limits<float>::max()/2))
            throw std::invalid_argument("slider_float range exceeds renderer limit");
        const sol::object options = patch["options"];
        if (strict && options.valid() && options.get_type() != sol::type::lua_nil) {
            if (controlValueKind(descriptor.type) != ControlType::Combo || options.get_type() != sol::type::table)
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
        auto timestamps = runtime_->controlUpdatedAtMs;
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
                if (iter->binding) throw std::invalid_argument("bound output values are owned by the dataset");
                // 新原子 API 不沿用旧 set_control 的截断和下标钳制。
                const auto kind = controlValueKind(iter->type);
                if (kind == ControlType::Combo || kind == ControlType::InputInt) {
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
                timestamps[id] = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
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
        runtime_->controlUpdatedAtMs.swap(timestamps);
        return true;
    } catch (const std::exception& failure) {
        error = failure.what(); return false;
    }
}
} // namespace protoscope::scripting
