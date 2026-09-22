#pragma once

#include "protoscope/data/record_csv.hpp"
#include <filesystem>
#include <functional>
#include <memory>

namespace protoscope::storage {
enum class ImportFormat { Psrec, Csv, MappedCsv };
struct ImportLimits {
    std::uint64_t sourceBytes{10ULL*1024*1024*1024};
    std::uint64_t stagedBytes{10ULL*1024*1024*1024};
};
struct ImportedVolumeInfo {
    std::filesystem::path path;
    std::string identity;
    std::uint64_t records{0};
    std::optional<std::int64_t> fromUs,toUs;
};

// 未登记的暂存卷由对象持有；析构只清理本次创建且身份标记仍匹配的文件。
class StagedRecordImport {
public:
    ~StagedRecordImport();
    StagedRecordImport(StagedRecordImport&&) noexcept;
    StagedRecordImport& operator=(StagedRecordImport&&) noexcept;
    const ImportedVolumeInfo& info() const;
    void release();
private:
    friend class VolumeCatalog;
    void relocate(const std::filesystem::path& directory);
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit StagedRecordImport(std::unique_ptr<Impl> impl);
    friend StagedRecordImport stageRecordImport(const std::filesystem::path&,const std::string&,
        const std::filesystem::path&,ImportFormat,const std::optional<data::CsvImportMapping>&,
        ImportLimits,std::stop_token,const std::function<void(std::uint64_t)>&);
};

StagedRecordImport stageRecordImport(const std::filesystem::path& recordsRoot,const std::string& protocol,
    const std::filesystem::path& source,ImportFormat format,
    const std::optional<data::CsvImportMapping>& mapping={},ImportLimits limits={},
    std::stop_token stop={},const std::function<void(std::uint64_t)>& progress={});
} // namespace protoscope::storage
