#pragma once

#include <charconv>
#include <optional>
#include <string_view>

namespace protoscope::ui {

struct SemanticVersion {
    int major{0};
    int minor{0};
    int patch{0};
};

inline std::optional<SemanticVersion> parseSemanticTag(std::string_view tag) {
    if (tag.size() < 6 || tag.front() != 'v') {
        return std::nullopt;
    }

    SemanticVersion version{};
    const char* cursor = tag.data() + 1;
    const char* const end = tag.data() + tag.size();

    const auto readNumber = [&](int& value) {
        const auto result = std::from_chars(cursor, end, value);
        if (result.ec != std::errc{}) {
            return false;
        }
        cursor = result.ptr;
        return true;
    };

    if (!readNumber(version.major) || cursor == end || *cursor != '.') {
        return std::nullopt;
    }
    ++cursor;
    if (!readNumber(version.minor) || cursor == end || *cursor != '.') {
        return std::nullopt;
    }
    ++cursor;
    if (!readNumber(version.patch) || cursor != end) {
        return std::nullopt;
    }
    return version;
}

inline int compareSemanticVersion(const SemanticVersion& lhs, const SemanticVersion& rhs) {
    if (lhs.major != rhs.major) {
        return lhs.major < rhs.major ? -1 : 1;
    }
    if (lhs.minor != rhs.minor) {
        return lhs.minor < rhs.minor ? -1 : 1;
    }
    if (lhs.patch != rhs.patch) {
        return lhs.patch < rhs.patch ? -1 : 1;
    }
    return 0;
}

} // namespace protoscope::ui
