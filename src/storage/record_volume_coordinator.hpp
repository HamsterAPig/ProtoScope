#pragma once

#include "record_query.hpp"
#include "record_session.hpp"

namespace protoscope::storage {
class RecordVolumeCoordinator {
public:
    RecordVolumeCoordinator(std::filesystem::path root,std::string protocol,const std::vector<data::Schema>& schemas,
        VolumeCatalog& catalog,RecordSession& session,std::int64_t nowUs,bool resumeRecording=false);
    const RecordVolumeInfo& info() const {return active_->info();}
    const std::map<std::string,std::uint64_t>& schemaIds() const {return schemaIds_;}
    void append(const std::vector<data::Record>& records,std::int64_t nowUs,std::uint64_t maxBytes,
                RecordQueryService* queries=nullptr);
    void rotate(std::int64_t nowUs,RecordQueryService* queries=nullptr);
    void synchronize();
private:
    void finishPending(std::int64_t nowUs,RecordQueryService* queries);
    void indexSealed(const RecordVolumeInfo& info,std::int64_t nowUs,std::shared_ptr<int> pin={});
    std::filesystem::path root_;
    std::string protocol_;
    std::map<std::uint64_t,data::Schema> schemas_;
    std::map<std::string,std::uint64_t> schemaIds_;
    VolumeCatalog& catalog_;
    RecordSession& session_;
    std::unique_ptr<RecordVolume> active_;
};
} // namespace protoscope::storage
