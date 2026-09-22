#include "record_volume_coordinator.hpp"
#include "legacy_record_migration.hpp"

namespace protoscope::storage {
RecordVolumeCoordinator::RecordVolumeCoordinator(std::filesystem::path root,std::string protocol,
    const std::vector<data::Schema>& schemas,VolumeCatalog& catalog,RecordSession& session,
    std::int64_t nowUs,bool resumeRecording)
    :root_(std::filesystem::weakly_canonical(std::filesystem::absolute(root))),protocol_(std::move(protocol)),
     catalog_(catalog),session_(session)
{
    std::map<std::uint64_t,data::Schema> local;
    for (const auto& schema:schemas) {
        data::validateSchema(schema);
        if (!schemaIds_.emplace(schema.dataset,0).second) throw std::invalid_argument("duplicate recording dataset");
        local.emplace(local.size()+1,schema);
    }
    const auto ids=catalog_.registerSchemas(local);
    for (const auto& [id,schema]:local) {
        schemas_.emplace(ids.at(id),schema);schemaIds_.at(schema.dataset)=ids.at(id);
    }
    migrateLegacyRecords(root_,protocol_,catalog_,session_,nowUs);
    const bool pending=!session_.state().pendingIdentity.empty();
    const bool existing=!session_.state().activeIdentity.empty();
    if (pending) finishPending(nowUs,nullptr);
    else if (existing)
        active_=RecordVolume::reopen(root_/("vol-"+session_.state().activeIdentity)/"records.sqlite",protocol_);
    else rotate(nowUs);
    synchronize();
    if (active_->info().sealed || active_->schemas()!=schemas_ || (resumeRecording && existing && !pending))
        rotate(nowUs);
}
void RecordVolumeCoordinator::synchronize()
{
    if (!active_) return;
    const auto& info=active_->info();
    if (info.lastId<session_.state().lastCommittedId)
        throw std::runtime_error("active volume is behind committed session position");
    if (info.lastId>session_.state().lastCommittedId) {
        if (!info.lastReceivedTimeUs) throw std::runtime_error("active volume has no record at advanced session position");
        session_.committed(info.lastId,*info.lastReceivedTimeUs);
    }
}
void RecordVolumeCoordinator::indexSealed(const RecordVolumeInfo& info,std::int64_t nowUs,std::shared_ptr<int> pin)
{
    if (!info.sealed) throw std::runtime_error("cannot index an active recording volume");
    {
        auto volumes=catalog_.pinAll();
        for (const auto& volume:volumes->volumes()) if (volume.identity==info.identity) {
            if (volume.records!=info.records || (info.records && volume.highWater!=info.lastId))
                throw std::runtime_error("sealed index differs from rotation source");
            return;
        }
    }
    catalog_.adoptRecording(info,nowUs,std::move(pin));
}
void RecordVolumeCoordinator::finishPending(std::int64_t nowUs,RecordQueryService* queries)
{
    const auto state=session_.state();
    if (state.pendingIdentity.empty()) throw std::logic_error("no pending record rotation");
    if (!state.activeIdentity.empty()) {
        const auto previous=root_/("vol-"+state.activeIdentity)/"records.sqlite";
        if (!active_ || active_->info().path!=previous) active_=RecordVolume::reopen(previous,protocol_);
        if (active_->info().lastId!=state.pendingFirstId-1)
            throw std::runtime_error("pending rotation no longer matches active record boundary");
    } else if (state.pendingFirstId-1!=state.lastCommittedId)
        throw std::runtime_error("initial volume differs from committed session boundary");
    const auto target=root_/("vol-"+state.pendingIdentity)/"records.sqlite";
    auto next=std::filesystem::exists(target) ? RecordVolume::reopen(target,protocol_):
        RecordVolume::create(root_,protocol_,schemas_,state.pendingFirstId,state.pendingOpenedUs,state.pendingIdentity);
    if (next->info().sealed || next->info().records || next->info().lastId!=state.pendingFirstId-1 ||
        next->info().openedAtUs!=state.pendingOpenedUs)
        throw std::runtime_error("pending rotation target is not an unpublished prepared volume");
    const auto commit=[&](std::shared_ptr<int> pin) {
        if (active_) {
            active_->seal(nowUs);
            indexSealed(active_->info(),nowUs,std::move(pin));
        }
        session_.commitRotation(state.pendingIdentity);
    };
    // 指针提交之前不允许往新卷发布。若中途失败，旧路径/待切换身份足以在重开时重试。
    if (queries) queries->switchActive(target,commit);
    else commit({});
    active_=std::move(next);
}
void RecordVolumeCoordinator::rotate(std::int64_t nowUs,RecordQueryService* queries)
{
    if (!session_.state().pendingIdentity.empty()) {
        finishPending(nowUs,queries);
        return;
    }
    synchronize();
    const auto last=session_.state().lastCommittedId;
    if (last==INT64_MAX) throw std::overflow_error("live record ID space exhausted");
    session_.prepareRotation(RecordVolume::newIdentity(),last+1,nowUs);
    finishPending(nowUs,queries);
}
void RecordVolumeCoordinator::append(const std::vector<data::Record>& records,std::int64_t nowUs,
    std::uint64_t maxBytes,RecordQueryService* queries)
{
    if (active_->rotationDue(nowUs,maxBytes)) rotate(nowUs,queries);
    if (active_->info().sealed || !session_.state().pendingIdentity.empty())
        throw std::runtime_error("record volume rotation requires recovery");
    if (records.size()>static_cast<std::uint64_t>(INT64_MAX-active_->info().lastId))
        throw std::overflow_error("live record ID space exhausted");
    catalog_.reserveLiveThrough(active_->info().lastId+static_cast<std::int64_t>(records.size()));
    active_->append(records);
}
} // namespace protoscope::storage
