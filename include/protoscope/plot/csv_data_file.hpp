#pragma once

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/raw_capture_file.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::plot {

enum class CsvKind {
    Unknown,
    Wave,
    RawEvents,
};

enum class CsvExportRangeKind {
    Full,
    CurrentView,
    CursorPair,
    Manual,
};

struct CsvExportRange {
    CsvExportRangeKind kind{CsvExportRangeKind::Full};
    double currentViewMinTime{0.0};
    double currentViewMaxTime{0.0};
    double cursorATime{0.0};
    double cursorBTime{0.0};
    double manualMinTime{0.0};
    double manualMaxTime{0.0};
};

std::optional<std::pair<double, double>> resolveCsvExportTimeRange(const CsvExportRange& range);
bool validateCsvExportRange(const CsvExportRange& range, std::string& error);
bool encodeWaveCsv(std::ostream& out, const WaveCsvData& data, WaveCsvShape shape,
                   const CsvExportRange& range, std::string& error);
std::optional<WaveCsvData> decodeWaveCsv(std::string_view text, std::string& error);
std::optional<WaveCsvData> readWaveCsvStream(std::istream& input, std::string& error,
                                          const DataFileReadCallbacks* callbacks = nullptr);

CsvKind detectCsvKind(const std::filesystem::path& path, std::string& error);
bool writeWaveCsvFile(const std::filesystem::path& path,
                      const WaveCsvData& data,
                      WaveCsvShape shape,
                      const CsvExportRange& range,
                      std::string& error);
std::optional<WaveCsvData> readWaveCsvFile(const std::filesystem::path& path, std::string& error,
                                        const DataFileReadCallbacks* callbacks = nullptr);
bool writeRawCaptureCsvFile(const std::filesystem::path& path,
                            const RawCaptureFileData& capture,
                            const CsvExportRange& range,
                            std::string& error);
std::optional<RawCaptureFileData> readRawCaptureCsvFile(const std::filesystem::path& path, std::string& error,
                                                      const DataFileReadCallbacks* callbacks = nullptr);

} // namespace protoscope::plot
