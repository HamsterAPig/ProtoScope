#include "protoscope/ui/dock_layout.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace protoscope::ui {

namespace {

constexpr const char* kDefaultLayoutKey = "default";

std::string trimCopy(std::string_view value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string normalizeKeyPart(std::string_view value) {
    auto text = trimCopy(value);
    if (text.empty()) {
        return {};
    }

    std::replace(text.begin(), text.end(), '\\', '/');
    while (text.size() > 1 && text.ends_with('/')) {
        text.pop_back();
    }

    // 核心逻辑：ImGui ID 中避免混入空白和路径分隔符，让同一路径稳定映射到同一个布局 key。
    for (char& ch : text) {
        const auto code = static_cast<unsigned char>(ch);
        if (std::isspace(code) != 0 || ch == '/' || ch == ':' || ch == '*'
            || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    return text;
}

std::string protocolDirectoryKey(std::string_view protocolDir, std::string_view scriptPath) {
    if (auto key = normalizeKeyPart(protocolDir); !key.empty()) {
        return key;
    }

    std::filesystem::path script(scriptPath);
    if (script.has_parent_path()) {
        if (auto key = normalizeKeyPart(script.parent_path().generic_string()); !key.empty()) {
            return key;
        }
    }

    return normalizeKeyPart(scriptPath);
}

} // namespace

std::optional<LuaDockAnchor> parseLuaDockAnchor(std::string_view value) {
    if (value == "left") {
        return LuaDockAnchor::Left;
    }
    if (value == "left_bottom") {
        return LuaDockAnchor::LeftBottom;
    }
    if (value == "right_top") {
        return LuaDockAnchor::RightTop;
    }
    if (value == "right_mid") {
        return LuaDockAnchor::RightMid;
    }
    if (value == "right_bottom") {
        return LuaDockAnchor::RightBottom;
    }
    if (value == "main_bottom") {
        return LuaDockAnchor::MainBottom;
    }
    return std::nullopt;
}

bool isValidLuaDockAnchor(std::string_view value) {
    return parseLuaDockAnchor(value).has_value();
}

std::string luaDockLayoutKey(std::string_view protocolDir, std::string_view scriptPath) {
    if (auto key = protocolDirectoryKey(protocolDir, scriptPath); !key.empty()) {
        return key;
    }
    return kDefaultLayoutKey;
}

std::string legacyLuaDockLayoutKey(std::string_view protocolDir, std::string_view scriptPath) {
    const auto protocolKey = normalizeKeyPart(protocolDir);
    const auto scriptKey = normalizeKeyPart(scriptPath);
    if (!protocolKey.empty() && !scriptKey.empty()) {
        return protocolKey + "__" + scriptKey;
    }
    if (!scriptKey.empty()) {
        return scriptKey;
    }
    if (!protocolKey.empty()) {
        return protocolKey;
    }
    return kDefaultLayoutKey;
}

std::filesystem::path executableDirectory() {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = 0;
    while (true) {
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::filesystem::current_path();
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::filesystem::current_path();
    }
    return std::filesystem::weakly_canonical(std::filesystem::path(buffer.c_str())).parent_path();
#else
    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return std::filesystem::current_path();
    }
    buffer[static_cast<std::size_t>(length)] = '\0';
    return std::filesystem::path(buffer.data()).parent_path();
#endif
}

std::filesystem::path luaDockLayoutPath(const std::filesystem::path& executableDir, std::string_view layoutKey) {
    const auto key = normalizeKeyPart(layoutKey);
    return executableDir / "config" / "ui" / ((key.empty() ? std::string(kDefaultLayoutKey) : key) + ".imgui.ini");
}

LuaDockLayoutPaths resolveLuaDockLayoutPaths(
    const std::filesystem::path& executableDir,
    std::string_view protocolDir,
    std::string_view scriptPath) {
    LuaDockLayoutPaths paths;
    paths.protocolKey = luaDockLayoutKey(protocolDir, scriptPath);
    paths.layoutPath = luaDockLayoutPath(executableDir, paths.protocolKey);
    paths.legacyLayoutPath = luaDockLayoutPath(executableDir, legacyLuaDockLayoutKey(protocolDir, scriptPath));
    paths.hasUserLayout = std::filesystem::exists(paths.layoutPath);
    paths.hasLegacyLayout = paths.legacyLayoutPath != paths.layoutPath && std::filesystem::exists(paths.legacyLayoutPath);
    return paths;
}

WorkspaceLayoutMode workspaceLayoutModeAfterLoad(const LuaDockLayoutPaths& layoutPaths) {
    return layoutPaths.hasUserLayout || layoutPaths.hasLegacyLayout
        ? WorkspaceLayoutMode::NeedsLoadedLayoutApply
        : WorkspaceLayoutMode::NeedsDefaultBuild;
}

WorkspaceLayoutMode workspaceLayoutModeAfterLoadedLayoutApply(WorkspaceLayoutMode mode) {
    return mode == WorkspaceLayoutMode::NeedsLoadedLayoutApply ? WorkspaceLayoutMode::Ready : mode;
}

bool shouldResetLuaDefaultDockStateOnProtocolSwitch(bool sameProtocol) {
    return !sameProtocol;
}

std::string luaDockStableId(const scripting::DockDescriptor& dock, std::string_view layoutKey) {
    std::ostringstream stream;
    stream << "LuaDock:" << layoutKey << ':' << dock.id;
    return stream.str();
}

std::vector<std::string> buildLuaDockStableIds(
    const std::vector<scripting::DockSnapshot>& docks,
    std::string_view layoutKey) {
    std::vector<std::string> stableIds;
    stableIds.reserve(docks.size());
    for (const auto& dock : docks) {
        stableIds.push_back(luaDockStableId(dock.descriptor, layoutKey));
    }
    return stableIds;
}

bool shouldKeepLuaWindowSettings(
    std::string_view stableId,
    std::string_view layoutKey,
    const std::vector<std::string>& activeStableIds) {
    if (!stableId.starts_with("LuaDock:")) {
        return true;
    }

    const auto activePrefix = std::string("LuaDock:") + std::string(layoutKey) + ':';
    if (!stableId.starts_with(activePrefix)) {
        return false;
    }

    return std::find(activeStableIds.begin(), activeStableIds.end(), stableId) != activeStableIds.end();
}

std::string luaDockWindowName(const scripting::DockDescriptor& dock, std::string_view layoutKey) {
    std::ostringstream stream;
    stream << dock.title << "###" << luaDockStableId(dock, layoutKey);
    return stream.str();
}

std::vector<LuaDockLayoutRequest> buildLuaDockLayoutRequests(
    const std::vector<scripting::DockSnapshot>& docks,
    std::string_view layoutKey) {
    std::vector<LuaDockLayoutRequest> requests;
    requests.reserve(docks.size());
    std::unordered_map<std::string, std::string> groupedAnchors;

    for (const auto& dock : docks) {
        const auto& descriptor = dock.descriptor;
        const auto groupKey = descriptor.tabGroup.empty() ? descriptor.id : descriptor.tabGroup;
        auto anchor = descriptor.anchor.empty() ? std::string("left_bottom") : descriptor.anchor;
        if (const auto iter = groupedAnchors.find(groupKey); iter != groupedAnchors.end()) {
            anchor = iter->second;
        } else {
            groupedAnchors.emplace(groupKey, anchor);
        }

        requests.push_back({
            .windowName = luaDockWindowName(descriptor, layoutKey),
            .anchor = std::move(anchor),
            .tabGroup = groupKey,
        });
    }

    return requests;
}

} // namespace protoscope::ui
