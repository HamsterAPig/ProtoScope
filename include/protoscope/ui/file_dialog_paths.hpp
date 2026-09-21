#pragma once

#include <filesystem>
#include <string>

namespace protoscope::ui {

inline std::string fileDialogPathText(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline std::filesystem::path existingDialogDirectory(std::filesystem::path candidate)
{
    // 目录可能被删除、拔盘或无权访问，逐级回退且不创建目录。
    while (!candidate.empty()) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error) && !error) return candidate;
        const auto parent = candidate.parent_path();
        if (parent == candidate) break;
        candidate = parent;
    }
    return {};
}

inline std::filesystem::path resolveFileDialogDirectory(
    const std::string& history, const std::filesystem::path& fallback,
    const std::filesystem::path& executableDirectory)
{
    if (const auto dir = existingDialogDirectory(std::filesystem::u8path(history)); !dir.empty()) return dir;
    if (const auto dir = existingDialogDirectory(fallback); !dir.empty()) return dir;
    return existingDialogDirectory(executableDirectory);
}

} // namespace protoscope::ui
