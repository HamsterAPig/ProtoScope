#pragma once

#include <filesystem>
#include <string>
#include <map>

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

inline std::string fileDialogHistory(const std::map<std::string, std::string>& directories,
                                    const std::string& purpose, const std::string& legacy = {})
{
    const auto found = directories.find(purpose);
    return found == directories.end() ? legacy : found->second;
}

inline std::string luaFileDialogHistoryKey(const std::string& protocolDirectory, int kind,
                                          const std::string& title, const std::string& historyKey)
{
    // 长度前缀避免目录、标题中的分隔符造成不同用途碰撞。
    const auto protocol = fileDialogPathText(std::filesystem::u8path(protocolDirectory).lexically_normal());
    const auto& group = historyKey.empty() ? title : historyKey;
    return "lua:" + std::to_string(protocol.size()) + ":" + protocol + ":" + std::to_string(kind) +
        (historyKey.empty() ? ":title:" : ":key:") + std::to_string(group.size()) + ":" + group;
}

inline std::filesystem::path luaFileDialogInitialPath(const std::string& history,
    const std::string& defaultPath, bool pickDirectory, const std::filesystem::path& executableDirectory)
{
    const auto requested = std::filesystem::u8path(defaultPath);
    std::error_code ec;
    const bool directory = pickDirectory || std::filesystem::is_directory(requested, ec);
    const auto folder = resolveFileDialogDirectory(history, directory ? requested : requested.parent_path(),
                                                   executableDirectory);
    return directory ? folder : folder / requested.filename();
}

} // namespace protoscope::ui
