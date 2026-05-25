#include "protoscope/ui/dock_layout.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <unordered_map>

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

    // 核心逻辑：ImGui ID 中避免混入空白和路径分隔符，让同一协议目录稳定映射到同一个布局 key。
    for (char& ch : text) {
        const auto code = static_cast<unsigned char>(ch);
        if (std::isspace(code) != 0 || ch == '/' || ch == ':' || ch == '*'
            || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            ch = '_';
        }
    }
    return text;
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
    if (auto key = normalizeKeyPart(protocolDir); !key.empty()) {
        return key;
    }
    if (auto key = normalizeKeyPart(scriptPath); !key.empty()) {
        return key;
    }
    return kDefaultLayoutKey;
}

std::string luaDockWindowName(const scripting::DockDescriptor& dock, std::string_view layoutKey) {
    std::ostringstream stream;
    stream << dock.title << "###LuaDock:" << layoutKey << ':' << dock.id;
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
