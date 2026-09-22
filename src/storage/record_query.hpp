#pragma once

#include "protoscope/storage/store.hpp"
#include "volume_catalog.hpp"

namespace protoscope::storage {
struct RecordSource {
    std::filesystem::path path;
    std::int64_t cutoff{0};
    std::int64_t idBase{0};
    std::map<std::uint64_t,std::uint64_t> schemaIds;
};
struct RecordSnapshot {
    std::int64_t token{0};
    std::vector<RecordSource> sources;
    std::map<std::uint64_t,data::Schema> schemas;
    std::shared_ptr<const PinnedVolumes> volumes;
    std::size_t memoryBytes{0};
};

class RecordQueryService {
public:
    RecordQueryService(std::filesystem::path active,VolumeCatalog& catalog,std::size_t memoryBudget);
    Completion query(const Query& query,std::stop_token stop);
    Completion exportRecords(const std::filesystem::path& path,ExportFormat format,
                             const Query& query,ExportOptions options,std::stop_token stop);
    void expireUnpinned();
private:
    std::shared_ptr<const RecordSnapshot> snapshot(std::optional<std::int64_t> token);
    std::filesystem::path active_;
    VolumeCatalog& catalog_;
    std::size_t memoryBudget_;
    std::map<std::uint64_t,std::uint64_t> activeSchemas_;
    std::mutex mutex_;
    std::map<std::int64_t,std::shared_ptr<const RecordSnapshot>> snapshots_;
};
} // namespace protoscope::storage
