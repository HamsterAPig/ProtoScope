#pragma once

#include "protoscope/data/model.hpp"
#include <filesystem>
#include <memory>

namespace protoscope::storage {
struct RecordVolumeInfo {
    std::filesystem::path path;
    std::string identity;
    std::uint64_t records{0};
    std::int64_t lastId{0};
    std::int64_t openedAtUs{0};
    std::optional<std::int64_t> fromUs,toUs;
    bool sealed{false};
};

// 单写线程拥有活动卷；封存不改路径，允许已有 SQLite 读事务继续持有原文件。
class RecordVolume {
public:
    static std::unique_ptr<RecordVolume> create(const std::filesystem::path& root,const std::string& protocol,
        const std::map<std::uint64_t,data::Schema>& schemas,std::int64_t firstId,std::int64_t openedAtUs,
        std::string reservedIdentity={});
    static std::string newIdentity();
    static std::unique_ptr<RecordVolume> reopen(const std::filesystem::path& path,const std::string& protocol);
    ~RecordVolume();
    const RecordVolumeInfo& info() const;
    void append(const std::vector<data::Record>& records,std::int64_t idLimit=INT64_MAX);
    void seal(std::int64_t sealedAtUs);
    std::uint64_t diskBytes() const;
    bool rotationDue(std::int64_t nowUs,std::uint64_t maxBytes=256ULL*1024*1024) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit RecordVolume(std::unique_ptr<Impl> impl);
};
} // namespace protoscope::storage
