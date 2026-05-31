#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::plot {

struct RawCaptureFileData {
    std::string protocolName;
    std::string protocolDir;
    double sampleFrequencyHz{0.0};
    std::uint64_t capturedAtMs{0};
    bool truncated{false};
    std::vector<std::uint8_t> payload;
};

std::string encodeRawCaptureHeader(const RawCaptureFileData& capture);
std::optional<RawCaptureFileData> decodeRawCaptureFile(std::string_view bytes, std::string& error);
bool writeRawCaptureFile(const std::filesystem::path& path, const RawCaptureFileData& capture, std::string& error);
std::optional<RawCaptureFileData> readRawCaptureFile(const std::filesystem::path& path, std::string& error);

} // namespace protoscope::plot
