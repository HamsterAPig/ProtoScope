#pragma once

#include "protoscope/plot/oscilloscope.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace protoscope::plot {

enum class RawCaptureEventType {
    RxBytes,
    ProfileSet,
    ProfileClear,
    PlotSetup,
    TxBytes,
};

struct RawCaptureProfileEventData {
    std::string frameName;
    std::size_t length{0};
    std::vector<std::size_t> channelMap;
};

struct RawCapturePlotSetupEventData {
    std::string source;
    std::vector<ChannelSpec> channels;
    ViewConfig view{};
    bool resetHistory{false};
};

struct RawCaptureEvent {
    RawCaptureEventType type{RawCaptureEventType::RxBytes};
    std::uint64_t timestampMs{0};
    std::vector<std::uint8_t> bytes;
    RawCaptureProfileEventData profile;
    RawCapturePlotSetupEventData plotSetup;
    std::string endpoint;
    std::uint64_t sequence{0};
    std::string writeStatus;
};

enum class WaveCsvShape { Wide, Long };

struct WaveCsvChannel {
    std::string label;
    std::string unit;
    std::vector<WaveSample> samples;
    ChannelSpec spec{};
    std::size_t sampleIndexOffset{0};
};

// 快照拥有原始样本，异步导出不依赖缓冲区或绘图缓存的生命周期。
struct WaveCsvData {
    WaveCsvShape shape{WaveCsvShape::Wide};
    double sampleFrequencyHz{0.0};
    ViewConfig view{};
    std::vector<WaveCsvChannel> channels;
    std::string source;
    std::string timeAxis{"script"};
    bool incomplete{false};
    std::string rangeDescription{"full"};
};

struct RawCaptureFileData {
    std::string protocolName;
    std::string protocolDir;
    double sampleFrequencyHz{0.0};
    std::uint64_t capturedAtMs{0};
    bool truncated{false};
    std::vector<std::uint8_t> payload;
    std::vector<RawCaptureEvent> events;
    std::string source;
    bool incomplete{false};
    bool filtered{false};
    bool rxOnly{true};
    std::optional<WaveCsvData> waveform;
    std::string rangeDescription{"full"};
};

// 返回 false 表示停止读取；回调在读取线程执行，并通过有界队列施加背压。
struct DataFileReadCallbacks {
    std::function<bool(const RawCaptureFileData&, bool)> metadata;
    std::function<bool(std::size_t, std::size_t, std::vector<WaveSample>)> samples;
    std::function<bool(RawCaptureEvent, bool)> event;
};

std::string encodeRawCaptureHeader(const RawCaptureFileData& capture);
std::string encodeRawCaptureEventRecordText(const RawCaptureEvent& event);
std::optional<RawCaptureEvent> decodeRawCaptureEventRecordText(std::string_view text, std::string& error);
bool encodeRawCaptureFile(const RawCaptureFileData& capture, std::vector<std::uint8_t>& bytes, std::string& error);
std::optional<RawCaptureFileData> decodeRawCaptureFile(std::string_view bytes, std::string& error,
                                                     const DataFileReadCallbacks* callbacks = nullptr);
std::optional<RawCaptureFileData> readRawCaptureStream(std::istream& input, std::uint64_t size, std::string& error,
                                                     const DataFileReadCallbacks* callbacks = nullptr);
std::optional<RawCaptureFileData> readRawCaptureFileRegion(const std::filesystem::path& path,
    std::uint64_t offset, std::uint64_t size, std::string& error, const DataFileReadCallbacks& callbacks);
bool writeRawCaptureFile(const std::filesystem::path& path, const RawCaptureFileData& capture, std::string& error);
std::optional<RawCaptureFileData> readRawCaptureFile(const std::filesystem::path& path, std::string& error,
                                                   const DataFileReadCallbacks* callbacks = nullptr);

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
