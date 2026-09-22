#include "industrial_control.hpp"
#include "control_data_binding.hpp"
#include "data_table_session.hpp"

#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace protoscope::scripting {
std::optional<std::string> formatReadout(const sol::object& value, int precision, std::string& error)
{
    if (value.get_type() == sol::type::string) return value.as<std::string>();
    if (value.get_type() != sol::type::number) {
        error = "readout requires a number or display string"; return {};
    }
    // 整数直接格式化，避免 int64 先经 double 丢失有效位。
    sol::stack::push(value.lua_state(), value);
    const bool integer = lua_isinteger(value.lua_state(), -1);
    lua_pop(value.lua_state(), 1);
    if (integer) {
        auto result = std::to_string(value.as<std::int64_t>());
        if (precision > 0) result += "." + std::string(static_cast<std::size_t>(precision), '0');
        return result;
    }
    const double number = value.as<double>();
    if (!std::isfinite(number)) { error = "readout requires a finite number"; return {}; }
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(precision) << number;
    return out.str();
}

bool applyIndustrialControlConfig(ControlDescriptor& descriptor, const sol::table& table, std::string& error)
{
    try {
        const bool slider = descriptor.type == ControlType::SliderInt || descriptor.type == ControlType::SliderFloat;
        if (slider) {
            descriptor.minimum = 0; descriptor.maximum = 100;
            descriptor.commitMode = ControlCommitMode::Commit;
        }
        if (descriptor.type == ControlType::Progress) {
            descriptor.minimum = 0; descriptor.maximum = 1;
        }
        if (descriptor.type == ControlType::TextArea || descriptor.type == ControlType::Label ||
            descriptor.type == ControlType::Readout) descriptor.maxLength = 4096;
        if (descriptor.type == ControlType::TextArea) descriptor.commitMode = ControlCommitMode::Commit;
        const sol::object mode = table["commit_mode"];
        if (mode.valid() && mode.get_type() != sol::type::lua_nil) {
            if (mode.get_type() != sol::type::string) throw std::invalid_argument("commit_mode must be string");
            const auto text = mode.as<std::string>();
            if (text != "change" && text != "commit") throw std::invalid_argument("invalid commit_mode");
            descriptor.commitMode = text == "commit" ? ControlCommitMode::Commit : ControlCommitMode::Change;
        }
        auto text = [&](const char* key, std::string& target) {
            const sol::object value = table[key];
            if (!value.valid() || value.get_type() == sol::type::lua_nil) return;
            if (value.get_type() != sol::type::string) throw std::invalid_argument(std::string(key) + " must be string");
            target = value.as<std::string>();
            if (target.size() > 4096 || target.find('\0') != target.npos)
                throw std::invalid_argument("display text exceeds limit or contains NUL");
        };
        auto integer = [&](const char* key, std::int64_t fallback, std::int64_t min, std::int64_t max) {
            const sol::object value = table[key];
            if (!value.valid() || value.get_type() == sol::type::lua_nil) return fallback;
            if (value.get_type() != sol::type::number || !value.is<std::int64_t>())
                throw std::invalid_argument(std::string(key) + " must be integer");
            const auto result = value.as<std::int64_t>();
            if (result < min || result > max) throw std::invalid_argument(std::string(key) + " outside range");
            return result;
        };
        auto boolean = [&](const char* key, bool& target) {
            const sol::object value = table[key];
            if (!value.valid() || value.get_type() == sol::type::lua_nil) return;
            if (value.get_type() != sol::type::boolean) throw std::invalid_argument(std::string(key) + " must be boolean");
            target = value.as<bool>();
        };
        // 仅解释所属控件的新属性，不能把旧 value_table 的 rows 数组当作文本行数。
        if (descriptor.type == ControlType::Readout) {
            text("unit", descriptor.unit);
            descriptor.staleAfterMs = static_cast<std::uint64_t>(integer("stale_after_ms", 0, 0, 86400000));
            boolean("show_update_time", descriptor.showUpdateTime);
        }
        if (descriptor.type == ControlType::Readout || descriptor.type == ControlType::SliderFloat)
            descriptor.precision = static_cast<int>(integer("precision", 2, 0, 12));
        if (descriptor.type == ControlType::Indicator) {
            text("on_text", descriptor.onText); text("off_text", descriptor.offText);
        }
        if (controlValueKind(descriptor.type) == ControlType::InputText)
            descriptor.maxLength = static_cast<std::size_t>(integer("max_length", descriptor.maxLength, 1, 262144));
        if (descriptor.type == ControlType::TextArea) {
            descriptor.rows = static_cast<int>(integer("rows", 5, 2, 40));
            boolean("wrap", descriptor.wrap);
        }
        if (descriptor.type == ControlType::Progress) boolean("indeterminate", descriptor.indeterminate);
        if (descriptor.type == ControlType::Label && descriptor.textDefault.empty())
            descriptor.textDefault = descriptor.label;
        if (descriptor.type == ControlType::Readout) {
            const sol::object value = table["default"];
            if (value.valid() && value.get_type() != sol::type::lua_nil) {
                auto formatted = formatReadout(value, descriptor.precision, error);
                if (!formatted) return false;
                descriptor.textDefault = std::move(*formatted);
            }
        }
        return parseControlBinding(descriptor,table,error) && parseDataTableConfig(descriptor,table,error);
    } catch (const std::exception& exception) {
        error = exception.what(); return false;
    }
}
} // namespace protoscope::scripting
