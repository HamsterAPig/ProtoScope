#pragma once

#include "protoscope/scripting/script_host.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace protoscope::ui {

inline std::string resolveLuaControlVisibleLabel(const scripting::ControlDescriptor& descriptor,
                                                 std::optional<float> layoutWidth)
{
    if (layoutWidth.has_value() && descriptor.compactLabelBelow.has_value() &&
        *layoutWidth < *descriptor.compactLabelBelow && !descriptor.shortLabel.empty()) {
        return descriptor.shortLabel;
    }
    return descriptor.label;
}

inline bool luaControlUsesCompactLabel(const scripting::ControlDescriptor& descriptor, std::string_view visibleLabel)
{
    return !descriptor.label.empty() && visibleLabel != descriptor.label;
}

} // namespace protoscope::ui
