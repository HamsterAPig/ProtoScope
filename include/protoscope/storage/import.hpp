#pragma once

#include "protoscope/data/record_csv.hpp"

namespace protoscope::storage {
enum class ImportFormat { Psrec, Csv, MappedCsv };
struct ImportLimits {
    std::uint64_t sourceBytes{10ULL*1024*1024*1024};
    std::uint64_t stagedBytes{10ULL*1024*1024*1024};
};
} // namespace protoscope::storage
