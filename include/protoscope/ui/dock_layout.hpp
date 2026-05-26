#pragma once

#include "protoscope/scripting/script_host.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::ui {

enum class LuaDockAnchor {
    Left,
    LeftBottom,
    RightTop,
    RightMid,
    RightBottom,
    MainBottom,
};

struct LuaDockLayoutRequest {
    std::string windowName;
    std::string anchor;
    std::string tabGroup;
};

std::optional<LuaDockAnchor> parseLuaDockAnchor(std::string_view value);
bool isValidLuaDockAnchor(std::string_view value);
std::string luaDockLayoutKey(std::string_view protocolDir, std::string_view scriptPath);
std::string legacyLuaDockLayoutKey(std::string_view protocolDir, std::string_view scriptPath);
std::string luaDockWindowName(const scripting::DockDescriptor& dock, std::string_view layoutKey);
std::vector<LuaDockLayoutRequest> buildLuaDockLayoutRequests(
    const std::vector<scripting::DockSnapshot>& docks,
    std::string_view layoutKey);

} // namespace protoscope::ui
