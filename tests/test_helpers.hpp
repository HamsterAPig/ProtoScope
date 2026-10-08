#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32) && defined(__MINGW32__)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace protoscope::tests {

inline void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

inline std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count());
}

inline std::filesystem::path makeUniqueTempDir(std::string_view prefix)
{
    const auto path = std::filesystem::temp_directory_path() / (std::string(prefix) + "-" + std::to_string(nowMs()));
    std::filesystem::create_directories(path);
    return path;
}

inline std::filesystem::path makeUniqueTempFile(std::string_view prefix, std::string_view extension = ".tmp")
{
    return std::filesystem::temp_directory_path() /
           (std::string(prefix) + "-" + std::to_string(nowMs()) + std::string(extension));
}

class ScopedTempPath {
public:
    explicit ScopedTempPath(std::filesystem::path path) : path_(std::move(path)) {}

    ~ScopedTempPath()
    {
#if defined(_WIN32) && defined(__MINGW32__)
        // MinGW 的 remove_all/remove 会经 CRT stat 打开文件，清理时可能阻塞；使用原生枚举及删除。
        try {
            const auto attributes = GetFileAttributesW(path_.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES) return;
            std::vector<std::pair<std::filesystem::path, DWORD>> paths{{path_, attributes}};
            for (std::size_t index = 0; index < paths.size(); ++index) {
                const auto [directory, flags] = paths[index];
                // 重解析点只删除链接本身，不能遍历到临时目录之外。
                if (!(flags & FILE_ATTRIBUTE_DIRECTORY) || (flags & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
                WIN32_FIND_DATAW entry{};
                const auto pattern = directory / L"*";
                const auto handle = FindFirstFileW(pattern.c_str(), &entry);
                if (handle == INVALID_HANDLE_VALUE) continue;
                struct Search {
                    HANDLE handle;
                    ~Search() { FindClose(handle); }
                } search{handle};
                do {
                    const std::wstring_view name{entry.cFileName};
                    if (name != L"." && name != L"..")
                        paths.emplace_back(directory / entry.cFileName, entry.dwFileAttributes);
                } while (FindNextFileW(handle, &entry));
            }
            // 全部枚举句柄关闭后再删除子节点，最后删除根目录。
            for (auto entry = paths.rbegin(); entry != paths.rend(); ++entry) {
                if (entry->second & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(entry->first.c_str());
                else DeleteFileW(entry->first.c_str());
            }
        } catch (...) {
            // 临时文件清理失败不能从析构函数抛出异常。
        }
#else
        std::error_code error;
        std::filesystem::remove_all(path_, error);
#endif
    }

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

template <typename Predicate>
bool waitUntil(Predicate predicate,
               int attempts = 50,
               std::chrono::milliseconds interval = std::chrono::milliseconds(10))
{
    for (int i = 0; i < attempts; ++i) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(interval);
    }
    return false;
}

} // namespace protoscope::tests
