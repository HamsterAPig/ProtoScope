#pragma once

#include "protoscope/storage/store.hpp"
#include <functional>
#include <stop_token>

namespace protoscope::storage {
std::uint64_t exportRecordFile(const std::filesystem::path& path,ExportFormat format,
    const std::map<std::uint64_t,data::Schema>& schemas,
    const std::function<std::optional<data::Record>()>& next,std::stop_token stop,ExportOptions options);
}
