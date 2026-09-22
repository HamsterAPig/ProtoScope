#pragma once

#include "record_import.hpp"
#include "record_volume.hpp"
#include <chrono>
#include <mutex>

namespace protoscope::storage {
struct CatalogVolume {
    std::uint64_t id{0};
    std::filesystem::path path;
    std::string identity;
    std::uint64_t records{0};
    std::optional<std::int64_t> fromUs,toUs;
    std::int64_t sealedAtUs{0};
    std::int64_t idBase{0};
    std::map<std::uint64_t,std::uint64_t> schemaIds;
    std::int64_t highWater{0};
};
struct RetentionPolicy {
    std::uint64_t maxBytes{10ULL*1024*1024*1024};
    std::chrono::microseconds maxAge{std::chrono::hours(24*30)};
};
struct RetentionResult {
    std::uint64_t bytes{0};
    std::uint64_t removedVolumes{0};
    bool capacityExceeded{false};
    bool expiredPinned{false};
};
class PinnedVolumes {
public:
    const std::vector<CatalogVolume>& volumes() const {return volumes_;}
private:
    std::vector<CatalogVolume> volumes_;
    std::vector<std::shared_ptr<int>> pins_;
    std::shared_ptr<void> runtimeLease_;
    friend class VolumeCatalog;
};

// 索引只登记已完整校验的自有卷；暂存目录和未索引文件不能自动成为历史数据。
class VolumeCatalog {
public:
    VolumeCatalog(std::filesystem::path recordsRoot,std::string protocol);
    ~VolumeCatalog();
    CatalogVolume adopt(StagedRecordImport& staged,std::int64_t sealedAtUs,std::stop_token stop={});
    CatalogVolume adoptRecording(const RecordVolumeInfo& info,std::int64_t sealedAtUs,
        std::shared_ptr<int> activePin={});
    std::shared_ptr<const PinnedVolumes> pinAll();
    bool isPinned(std::uint64_t id) const;
    std::map<std::uint64_t,data::Schema> schemas() const;
    std::map<std::uint64_t,std::uint64_t> registerSchemas(const std::map<std::uint64_t,data::Schema>& schemas);
    RetentionResult retain(RetentionPolicy policy,std::int64_t nowUs,std::stop_token stop={});
    std::uint64_t diskBytes() const;
    std::shared_ptr<void> claimWriter();
    std::shared_ptr<int> trackActive(const std::filesystem::path& path,const std::shared_ptr<int>& pin);
    void reserveLiveThrough(std::int64_t id);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace protoscope::storage
