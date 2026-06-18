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
        std::error_code error;
        std::filesystem::remove_all(path_, error);
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
