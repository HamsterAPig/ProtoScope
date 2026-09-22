#pragma once

#include "record_import.hpp"
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
};
class PinnedVolumes {
public:
    const std::vector<CatalogVolume>& volumes() const {return volumes_;}
private:
    std::vector<CatalogVolume> volumes_;
    std::vector<std::shared_ptr<int>> pins_;
    friend class VolumeCatalog;
};

// 索引只登记已完整校验的自有卷；暂存目录和未索引文件不能自动成为历史数据。
class VolumeCatalog {
public:
    VolumeCatalog(std::filesystem::path recordsRoot,std::string protocol);
    ~VolumeCatalog();
    CatalogVolume adopt(StagedRecordImport& staged,std::int64_t sealedAtUs);
    std::shared_ptr<const PinnedVolumes> pinAll();
    bool isPinned(std::uint64_t id) const;
    std::map<std::uint64_t,data::Schema> schemas() const;
    std::map<std::uint64_t,std::uint64_t> registerSchemas(const std::map<std::uint64_t,data::Schema>& schemas);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace protoscope::storage
