#pragma once

#include "protoscope/scripting/script_host.hpp"

#include <filesystem>
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

struct LuaDockLayoutPaths {
    std::string protocolKey;
    std::filesystem::path layoutPath;
    std::filesystem::path legacyLayoutPath;
    std::filesystem::path metaPath;
    bool hasUserLayout{false};
    bool hasLegacyLayout{false};
    bool hasMeta{false};
    int schemaVersion{0};
    bool isLegacyLayout{false};
};

struct ProtocolWorkspaceSwitchDecision {
    bool draftChanged{false};
    std::optional<std::string> reloadProtocolDir;
};

enum class WorkspaceLayoutMode {
    NeedsDefaultBuild,
    Ready,
};

std::optional<LuaDockAnchor> parseLuaDockAnchor(std::string_view value);
bool isValidLuaDockAnchor(std::string_view value);
std::string luaDockLayoutKey(std::string_view protocolDir, std::string_view scriptPath);
std::string legacyLuaDockLayoutKey(std::string_view protocolDir, std::string_view scriptPath);
std::filesystem::path executableDirectory();
std::filesystem::path luaDockLayoutPath(const std::filesystem::path& executableDir, std::string_view layoutKey);
std::filesystem::path luaDockLayoutMetaPath(const std::filesystem::path& executableDir, std::string_view layoutKey);
void writeLuaDockLayoutMeta(const std::filesystem::path& path, int schemaVersion);
LuaDockLayoutPaths resolveLuaDockLayoutPaths(
    const std::filesystem::path& executableDir,
    std::string_view protocolDir,
    std::string_view scriptPath);
ProtocolWorkspaceSwitchDecision decideProtocolWorkspaceSwitch(
    std::string_view loadedProtocolDir,
    std::string_view draftProtocolDir,
    bool reloadClicked);
WorkspaceLayoutMode workspaceLayoutModeAfterLoad(const LuaDockLayoutPaths& layoutPaths);
bool shouldResetLuaDefaultDockStateOnProtocolSwitch(bool sameProtocol);
bool shouldRunLuaDefaultDockLayout(WorkspaceLayoutMode layoutMode, bool pendingDefaultDockLayout);
bool canResetProtocolWorkspaceLayout(bool protocolWorkspaceLoaded, std::string_view activeWorkspaceProtocolKey);
std::string luaDockStableId(const scripting::DockDescriptor& dock, std::string_view layoutKey);
std::vector<std::string> buildLuaDockStableIds(
    const std::vector<scripting::DockSnapshot>& docks,
    std::string_view layoutKey);
bool shouldKeepLuaWindowSettings(
    std::string_view stableId,
    std::string_view layoutKey);
std::string luaDockWindowName(const scripting::DockDescriptor& dock, std::string_view layoutKey);
std::vector<LuaDockLayoutRequest> buildLuaDockLayoutRequests(
    const std::vector<scripting::DockSnapshot>& docks,
    std::string_view layoutKey);

} // namespace protoscope::ui
