#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::session {

struct SessionPackageEntry {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

struct SessionPackageData {
    std::uint64_t createdAtMs{0};
    std::vector<SessionPackageEntry> entries;
};

std::string encodeSessionPackage(const SessionPackageData& package);
std::optional<SessionPackageData> decodeSessionPackage(std::string_view bytes, std::string& error);
bool writeSessionPackage(const std::filesystem::path& path, const SessionPackageData& package, std::string& error);
std::optional<SessionPackageData> readSessionPackage(const std::filesystem::path& path, std::string& error);
const SessionPackageEntry* findSessionPackageEntry(const SessionPackageData& package, std::string_view name);

} // namespace protoscope::session
