#pragma once

#include "record_session.hpp"
#include "volume_catalog.hpp"
#include <functional>

namespace protoscope::storage {
enum class LegacyMigrationPhase { IntentCommitted, FilePrepared, FileMoved, Indexed };
bool hasLegacyMigration(const std::filesystem::path& recordsRoot);
std::optional<CatalogVolume> migrateLegacyRecords(const std::filesystem::path& recordsRoot,
    const std::string& protocol,VolumeCatalog& catalog,RecordSession& session,std::int64_t nowUs,
    const std::function<void(LegacyMigrationPhase)>& checkpoint={});
} // namespace protoscope::storage
