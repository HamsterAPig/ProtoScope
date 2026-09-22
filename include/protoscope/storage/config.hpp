#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace protoscope::storage {
struct Config {
    std::size_t queueBytes{32U * 1024U * 1024U};
    std::size_t batchRows{1000};
    std::chrono::milliseconds batchInterval{100};
    std::size_t kvValueBytes{256U * 1024U};
    std::size_t kvTotalBytes{8U * 1024U * 1024U};
    std::size_t kvDepth{16};
    std::uint64_t recordMaxBytes{10ULL*1024*1024*1024};
    std::chrono::microseconds recordMaxAge{std::chrono::hours(24*30)};
    std::chrono::milliseconds maintenanceInterval{1000};
};
} // namespace protoscope::storage
