#pragma once

#include <cstdint>
#include <fstream>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace protoscope::plot {

enum class RawCaptureEventType {
    RxBytes,
    ProfileSet,
    ProfileClear,
};

struct RawCaptureProfileEventData {
    std::string frameName;
    std::size_t length{0};
    std::vector<std::size_t> channelMap;
};

struct RawCaptureEvent {
    RawCaptureEventType type{RawCaptureEventType::RxBytes};
    std::uint64_t timestampMs{0};
    std::vector<std::uint8_t> bytes;
    RawCaptureProfileEventData profile;
};

struct RawCaptureFileData {
    std::string protocolName;
    std::string protocolDir;
    double sampleFrequencyHz{0.0};
    std::uint64_t capturedAtMs{0};
    bool truncated{false};
    std::vector<std::uint8_t> payload;
    std::vector<RawCaptureEvent> events;
};

std::string encodeRawCaptureHeader(const RawCaptureFileData& capture);
std::optional<RawCaptureFileData> decodeRawCaptureFile(std::string_view bytes, std::string& error);
bool writeRawCaptureFile(const std::filesystem::path& path, const RawCaptureFileData& capture, std::string& error);
std::optional<RawCaptureFileData> readRawCaptureFile(const std::filesystem::path& path, std::string& error);

class RawCaptureStreamWriter {
public:
    RawCaptureStreamWriter() = default;
    RawCaptureStreamWriter(const RawCaptureStreamWriter&) = delete;
    RawCaptureStreamWriter& operator=(const RawCaptureStreamWriter&) = delete;
    RawCaptureStreamWriter(RawCaptureStreamWriter&&) = delete;
    RawCaptureStreamWriter& operator=(RawCaptureStreamWriter&&) = delete;
    ~RawCaptureStreamWriter();

    [[nodiscard]] bool isOpen() const;
    [[nodiscard]] const std::filesystem::path& path() const;
    [[nodiscard]] std::uint64_t bytesWritten() const;

    bool open(const std::filesystem::path& path, const RawCaptureFileData& metadata, std::string& error);
    bool append(std::span<const std::uint8_t> bytes, std::string& error);
    bool appendEvent(const RawCaptureEvent& event, std::string& error);
    bool close(std::string& error);

private:
    std::ofstream out_;
    std::filesystem::path path_;
    RawCaptureFileData metadata_{};
    std::uint64_t bytesWritten_{0};
    std::uint64_t rxBytesWritten_{0};
};

} // namespace protoscope::plot
