#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/plot/csv_data_file.hpp"
#include "protoscope/plot/data_file_output.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <ostream>
#include <sstream>
#include <system_error>
#include <utility>

namespace protoscope::plot {
namespace {

    constexpr std::string_view kFileMagic = "ProtoScopeRawCapture";
    constexpr std::string_view kVersionEvents = "4";
    constexpr std::string_view kVersionLegacyEvents = "2";
    constexpr std::size_t kStreamHeaderBytes = 4096;

    std::string_view trimView(std::string_view text)
    {
        std::size_t begin = 0;
        while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
            ++begin;
        }
        std::size_t end = text.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
            --end;
        }
        return text.substr(begin, end - begin);
    }

    std::string trim(std::string_view text)
    {
        return std::string(trimView(text));
    }

    bool parseUnsigned(std::string_view text, std::uint64_t& value)
    {
        const auto cleaned = trimView(text);
        const auto* begin = cleaned.data();
        const auto* end = cleaned.data() + cleaned.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        return ec == std::errc{} && ptr == end;
    }

    bool parseDouble(std::string_view text, double& value)
    {
        const auto cleaned = trim(text);
        if (cleaned.empty()) {
            return false;
        }
        try {
            std::size_t consumed = 0;
            value = std::stod(cleaned, &consumed);
            return consumed == cleaned.size();
        } catch (...) {
            return false;
        }
    }

    bool parseBool(std::string_view text, bool& value)
    {
        const auto cleaned = trimView(text);
        if (cleaned == "true" || cleaned == "1") {
            value = true;
            return true;
        }
        if (cleaned == "false" || cleaned == "0") {
            value = false;
            return true;
        }
        return false;
    }

    char hexDigit(std::uint8_t value)
    {
        return static_cast<char>(value < 10 ? ('0' + value) : ('a' + (value - 10)));
    }

    std::string encodeStringHex(std::string_view text)
    {
        std::string encoded;
        encoded.reserve(text.size() * 2);
        for (const unsigned char ch : text) {
            encoded.push_back(hexDigit(static_cast<std::uint8_t>((ch >> 4U) & 0x0FU)));
            encoded.push_back(hexDigit(static_cast<std::uint8_t>(ch & 0x0FU)));
        }
        return encoded;
    }

    int decodeHexNibble(char ch)
    {
        if (ch >= '0' && ch <= '9') {
            return ch - '0';
        }
        if (ch >= 'a' && ch <= 'f') {
            return 10 + (ch - 'a');
        }
        if (ch >= 'A' && ch <= 'F') {
            return 10 + (ch - 'A');
        }
        return -1;
    }

    bool decodeStringHex(std::string_view text, std::string& value)
    {
        const auto cleaned = trimView(text);
        if (cleaned.size() % 2 != 0) {
            return false;
        }
        value.clear();
        value.reserve(cleaned.size() / 2);
        for (std::size_t index = 0; index < cleaned.size(); index += 2) {
            const int high = decodeHexNibble(cleaned[index]);
            const int low = decodeHexNibble(cleaned[index + 1]);
            if (high < 0 || low < 0) {
                return false;
            }
            value.push_back(static_cast<char>((high << 4) | low));
        }
        return true;
    }

    std::string serializeChannelMap(const std::vector<std::size_t>& channelMap)
    {
        std::ostringstream out;
        for (std::size_t index = 0; index < channelMap.size(); ++index) {
            if (index > 0) {
                out << ',';
            }
            out << channelMap[index];
        }
        return out.str();
    }

    bool parseChannelMap(std::string_view text, std::vector<std::size_t>& outMap)
    {
        outMap.clear();
        const auto cleaned = trimView(text);
        if (cleaned.empty()) {
            return true;
        }
        std::size_t begin = 0;
        while (begin < cleaned.size()) {
            std::size_t end = cleaned.find(',', begin);
            if (end == std::string::npos) {
                end = cleaned.size();
            }
            std::uint64_t value = 0;
            if (!parseUnsigned(cleaned.substr(begin, end - begin), value)) {
                return false;
            }
            if (value > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                return false;
            }
            outMap.push_back(static_cast<std::size_t>(value));
            begin = end + 1;
        }
        return true;
    }

    std::string serializeColor(const std::optional<std::array<float, 4>>& color)
    {
        if (!color.has_value()) {
            return "none";
        }
        std::ostringstream out;
        out << std::setprecision(9) << (*color)[0] << ',' << (*color)[1] << ',' << (*color)[2] << ',' << (*color)[3];
        return out.str();
    }

    bool parseColor(std::string_view text, std::optional<std::array<float, 4>>& color)
    {
        const auto cleaned = trimView(text);
        if (cleaned == "none" || cleaned.empty()) {
            color = std::nullopt;
            return true;
        }
        std::array<float, 4> parsed{};
        std::size_t begin = 0;
        for (std::size_t index = 0; index < parsed.size(); ++index) {
            std::size_t end = cleaned.find(',', begin);
            if (index + 1 == parsed.size()) {
                end = cleaned.size();
            } else if (end == std::string::npos) {
                return false;
            }
            double value = 0.0;
            if (!parseDouble(cleaned.substr(begin, end - begin), value) || !std::isfinite(value)) {
                return false;
            }
            parsed[index] = static_cast<float>(value);
            begin = end + 1;
        }
        color = parsed;
        return true;
    }

    std::string serializeLineWidth(const std::optional<float>& lineWidth)
    {
        if (!lineWidth.has_value()) {
            return "none";
        }
        std::ostringstream out;
        out << std::setprecision(9) << resolveChannelLineWidth(lineWidth);
        return out.str();
    }

    bool parseLineWidth(std::string_view text, std::optional<float>& lineWidth)
    {
        const auto cleaned = trimView(text);
        if (cleaned == "none" || cleaned.empty()) {
            lineWidth = std::nullopt;
            return true;
        }
        double parsed = 0.0;
        if (!parseDouble(cleaned, parsed) || !std::isfinite(parsed)) {
            return false;
        }
        lineWidth = sanitizeChannelLineWidth(parsed);
        return true;
    }

    std::string encodePlotSetupRecord(const RawCaptureEvent& event)
    {
        std::ostringstream out;
        out << "event: plot_setup\n"
            << "sequence: " << event.sequence << '\n'
            << "timestamp_ms: " << event.timestampMs << '\n'
            << "source: " << encodeStringHex(event.plotSetup.source) << '\n'
            << "reset_history: " << (event.plotSetup.resetHistory ? "true" : "false") << '\n'
            << "channel_count: " << event.plotSetup.channels.size() << '\n';
        for (std::size_t index = 0; index < event.plotSetup.channels.size(); ++index) {
            const auto& channel = event.plotSetup.channels[index];
            out << "channel." << index << ".label: " << encodeStringHex(channel.label) << '\n'
                << "channel." << index << ".unit: " << encodeStringHex(channel.unit) << '\n'
                << "channel." << index << ".ratio: " << std::setprecision(17) << channel.ratio << '\n'
                << "channel." << index << ".scale: " << std::setprecision(17) << channel.scale << '\n'
                << "channel." << index << ".offset: " << std::setprecision(17) << channel.offset << '\n'
                << "channel." << index << ".color: " << serializeColor(channel.color) << '\n'
                << "channel." << index << ".line_width: " << serializeLineWidth(channel.lineWidth) << '\n'
                << "channel." << index << ".bit_display.enabled: " << (channel.bitDisplay.enabled ? "true" : "false")
                << '\n'
                << "channel." << index << ".bit_display.first_bit: " << channel.bitDisplay.firstBit << '\n'
                << "channel." << index << ".bit_display.bit_count: " << channel.bitDisplay.bitCount << '\n'
                << "channel." << index << ".bit_display.y_offset: " << std::setprecision(17)
                << channel.bitDisplay.yOffset << '\n'
                << "channel." << index << ".bit_display.hover_readout: "
                << (channel.bitDisplay.hoverReadout ? "true" : "false") << '\n';
        }
        out << "view.time_scale: " << std::setprecision(17) << event.plotSetup.view.timeScale << '\n'
            << "view.time_unit: " << encodeStringHex(event.plotSetup.view.timeUnit) << '\n'
            << "view.vertical_min: " << std::setprecision(17) << event.plotSetup.view.verticalMin << '\n'
            << "view.vertical_max: " << std::setprecision(17) << event.plotSetup.view.verticalMax << '\n'
            << "view.vertical_unit: " << encodeStringHex(event.plotSetup.view.verticalUnit) << '\n'
            << "view.history_limit: " << event.plotSetup.view.historyLimit << '\n'
            << "view.display_formula: " << static_cast<int>(event.plotSetup.view.displayFormula) << '\n'
            << '\n';
        return out.str();
    }

    std::string encodeEventRecord(const RawCaptureEvent& event);

    bool isBytesEvent(const RawCaptureEvent& event)
    {
        return event.type == RawCaptureEventType::RxBytes || event.type == RawCaptureEventType::TxBytes;
    }

    std::uint64_t totalEventBytes(const RawCaptureFileData& capture)
    {
        std::uint64_t total = 0;
        for (const auto& event : capture.events) {
            total += encodeEventRecord(event).size();
            if (isBytesEvent(event)) total += event.bytes.size();
        }
        return total;
    }

    std::vector<RawCaptureEvent> normalizedEvents(const RawCaptureFileData& capture)
    {
        if (!capture.events.empty()) {
            return capture.events;
        }
        if (capture.payload.empty()) {
            return {};
        }
        return {RawCaptureEvent{
            .type = RawCaptureEventType::RxBytes,
            .timestampMs = capture.capturedAtMs,
            .bytes = capture.payload,
            .profile = {},
            .plotSetup = {},
        }};
    }

    std::string encodeRawCaptureHeaderWithSize(const RawCaptureFileData& capture,
                                               std::uint64_t rawSize,
                                               bool eventsMode,
                                               std::uint64_t waveSize = 0)
    {
        std::ostringstream header;
        header << kFileMagic << '\n'
               << "version: " << kVersionEvents << '\n'
               << "protocol_name: " << capture.protocolName << '\n'
               << "protocol_dir: " << capture.protocolDir << '\n'
               << "sample_frequency_hz: " << std::setprecision(17) << capture.sampleFrequencyHz << '\n'
               << "captured_at_ms: " << capture.capturedAtMs << '\n'
               << "truncated: " << (capture.truncated ? "true" : "false") << '\n'
               << "payload_size: " << rawSize << '\n'
               << "event_stream: " << (eventsMode ? "true" : "false") << '\n'
               << "wave_size: " << waveSize << '\n'
               << "source: " << encodeStringHex(capture.source) << '\n'
               << "incomplete: " << (capture.incomplete ? "true" : "false") << '\n'
               << "filtered: " << (capture.filtered ? "true" : "false") << '\n'
               << "rx_only: " << (capture.rxOnly ? "true" : "false") << '\n'
               << "range: " << encodeStringHex(capture.rangeDescription) << '\n'
               << '\n';
        return header.str();
    }

    bool encodeFixedRawCaptureHeader(const RawCaptureFileData& capture,
                                     std::uint64_t rawSize,
                                     bool eventsMode,
                                     std::string& header,
                                     std::string& error,
                                     std::uint64_t waveSize = 0)
    {
        const std::string base = encodeRawCaptureHeaderWithSize(capture, rawSize, eventsMode, waveSize);
        if (base.size() > kStreamHeaderBytes) {
            error = "psraw 文件头超出固定长度限制";
            return false;
        }
        header = base;
        header.resize(kStreamHeaderBytes, '\0');
        return true;
    }

    std::string encodeEventRecord(const RawCaptureEvent& event)
    {
        std::ostringstream out;
        switch (event.type) {
            case RawCaptureEventType::RxBytes:
            case RawCaptureEventType::TxBytes:
                out << "event: " << (event.type == RawCaptureEventType::TxBytes ? "tx_bytes" : "rx_bytes") << '\n'
                    << "timestamp_ms: " << event.timestampMs << '\n'
                    << "endpoint: " << encodeStringHex(event.endpoint) << '\n'
                    << "sequence: " << event.sequence << '\n'
                    << "write_status: " << encodeStringHex(event.writeStatus) << '\n'
                    << "size: " << event.bytes.size() << '\n'
                    << '\n';
                break;
            case RawCaptureEventType::ProfileSet:
                out << "event: profile_set\n"
                    << "sequence: " << event.sequence << '\n'
                    << "timestamp_ms: " << event.timestampMs << '\n'
                    << "frame: " << event.profile.frameName << '\n'
                    << "length: " << event.profile.length << '\n'
                    << "channel_map: " << serializeChannelMap(event.profile.channelMap) << '\n'
                    << '\n';
                break;
            case RawCaptureEventType::ProfileClear:
                out << "event: profile_clear\n"
                    << "sequence: " << event.sequence << '\n'
                    << "timestamp_ms: " << event.timestampMs << '\n'
                    << "frame: " << event.profile.frameName << '\n'
                    << '\n';
                break;
            case RawCaptureEventType::PlotSetup:
                out << encodePlotSetupRecord(event);
                break;
        }
        return out.str();
    }

    struct DecodedEventState {
        std::uint64_t rxSize{0};
        bool rxSizeSeen{false};
        bool profileLengthSeen{false};
        bool plotChannelCountSeen{false};
        std::uint64_t plotChannelCount{0};
    };

    enum class EventFieldParseResult {
        Handled,
        Ignored,
        Failed,
    };

    bool decodeEventType(std::string_view firstLine, RawCaptureEvent& event, std::string& error)
    {
        if (firstLine == "event: rx_bytes") {
            event.type = RawCaptureEventType::RxBytes;
            return true;
        }
        if (firstLine == "event: tx_bytes") {
            event.type = RawCaptureEventType::TxBytes;
            return true;
        }
        if (firstLine == "event: profile_set") {
            event.type = RawCaptureEventType::ProfileSet;
            return true;
        }
        if (firstLine == "event: profile_clear") {
            event.type = RawCaptureEventType::ProfileClear;
            return true;
        }
        if (firstLine == "event: plot_setup") {
            event.type = RawCaptureEventType::PlotSetup;
            return true;
        }

        error = "psraw 事件记录缺少 event 类型";
        return false;
    }

    EventFieldParseResult parseStringHexField(std::string_view value,
                                              std::string& target,
                                              std::string_view errorMessage,
                                              std::string& error)
    {
        if (!decodeStringHex(value, target)) {
            error = std::string(errorMessage);
            return EventFieldParseResult::Failed;
        }
        return EventFieldParseResult::Handled;
    }

    EventFieldParseResult parseFiniteDoubleField(std::string_view value,
                                                 double& target,
                                                 std::string_view errorMessage,
                                                 std::string& error)
    {
        if (!parseDouble(value, target) || !std::isfinite(target)) {
            error = std::string(errorMessage);
            return EventFieldParseResult::Failed;
        }
        return EventFieldParseResult::Handled;
    }

    EventFieldParseResult parseColorField(std::string_view value,
                                          std::optional<std::array<float, 4>>& target,
                                          std::string_view errorMessage,
                                          std::string& error)
    {
        if (!parseColor(value, target)) {
            error = std::string(errorMessage);
            return EventFieldParseResult::Failed;
        }
        return EventFieldParseResult::Handled;
    }

    EventFieldParseResult parseBaseEventField(std::string_view key,
                                              std::string_view value,
                                              RawCaptureEvent& event,
                                              DecodedEventState& state,
                                              std::string& error)
    {
        if (key == "endpoint")
            return parseStringHexField(value, event.endpoint, "psraw endpoint 格式错误", error);
        if (key == "write_status")
            return parseStringHexField(value, event.writeStatus, "psraw write_status 格式错误", error);
        if (key == "sequence") {
            if (!parseUnsigned(value, event.sequence)) {
                error = "psraw sequence 格式错误";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        }
        if (key == "timestamp_ms") {
            if (!parseUnsigned(value, event.timestampMs)) {
                error = "psraw 事件时间戳格式错误";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        }
        if (key == "size") {
            state.rxSizeSeen = parseUnsigned(value, state.rxSize);
            if (!state.rxSizeSeen) {
                error = "psraw rx_bytes size 格式错误";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        }
        if (key == "frame") {
            event.profile.frameName = std::string(value);
            return EventFieldParseResult::Handled;
        }
        if (key == "length") {
            std::uint64_t lengthValue = 0;
            if (!parseUnsigned(value, lengthValue) || lengthValue == 0) {
                error = "psraw profile_set length 格式错误";
                return EventFieldParseResult::Failed;
            }
            if (lengthValue > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                error = "psraw profile_set length 过大";
                return EventFieldParseResult::Failed;
            }
            event.profile.length = static_cast<std::size_t>(lengthValue);
            state.profileLengthSeen = true;
            return EventFieldParseResult::Handled;
        }
        if (key == "channel_map") {
            if (!parseChannelMap(value, event.profile.channelMap)) {
                error = "psraw profile_set channel_map 格式错误";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        }
        return EventFieldParseResult::Ignored;
    }

    EventFieldParseResult parsePlotSetupChannelField(std::string_view key,
                                                     std::string_view value,
                                                     RawCaptureEvent& event,
                                                     const DecodedEventState& state,
                                                     std::string& error)
    {
        constexpr std::string_view kChannelPrefix = "channel.";
        if (key.rfind(kChannelPrefix, 0) != 0) {
            return EventFieldParseResult::Ignored;
        }

        if (!state.plotChannelCountSeen) {
            error = "psraw plot_setup channel 字段早于 channel_count";
            return EventFieldParseResult::Failed;
        }
        const auto indexEnd = key.find('.', kChannelPrefix.size());
        if (indexEnd == std::string_view::npos) {
            error = "psraw plot_setup channel 字段格式错误";
            return EventFieldParseResult::Failed;
        }
        std::uint64_t channelIndexValue = 0;
        if (!parseUnsigned(key.substr(kChannelPrefix.size(), indexEnd - kChannelPrefix.size()), channelIndexValue) ||
            channelIndexValue >= state.plotChannelCount) {
            error = "psraw plot_setup channel 索引格式错误";
            return EventFieldParseResult::Failed;
        }

        auto& channel = event.plotSetup.channels[static_cast<std::size_t>(channelIndexValue)];
        const auto field = key.substr(indexEnd + 1);
        if (field == "label") {
            return parseStringHexField(value, channel.label, "psraw plot_setup channel label hex 格式错误", error);
        } else if (field == "unit") {
            return parseStringHexField(value, channel.unit, "psraw plot_setup channel unit hex 格式错误", error);
        } else if (field == "ratio") {
            return parseFiniteDoubleField(value, channel.ratio, "psraw plot_setup channel ratio 格式错误", error);
        } else if (field == "scale") {
            return parseFiniteDoubleField(value, channel.scale, "psraw plot_setup channel scale 格式错误", error);
        } else if (field == "offset") {
            return parseFiniteDoubleField(value, channel.offset, "psraw plot_setup channel offset 格式错误", error);
        } else if (field == "color") {
            return parseColorField(value, channel.color, "psraw plot_setup channel color 格式错误", error);
        } else if (field == "line_width") {
            if (!parseLineWidth(value, channel.lineWidth)) {
                error = "psraw plot_setup channel line_width 格式错误";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        } else if (field == "bit_display.enabled") {
            if (!parseBool(value, channel.bitDisplay.enabled)) {
                error = "psraw plot_setup channel bit_display.enabled 格式错误";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        } else if (field == "bit_display.hover_readout") {
            if (!parseBool(value, channel.bitDisplay.hoverReadout)) {
                error = "psraw plot_setup channel bit_display.hover_readout 格式错误";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        } else if (field == "bit_display.first_bit") {
            std::uint64_t firstBit = 0;
            if (!parseUnsigned(value, firstBit) || firstBit >= kMaxBitDisplayCount) {
                error = "psraw plot_setup channel bit_display.first_bit 格式错误";
                return EventFieldParseResult::Failed;
            }
            channel.bitDisplay.firstBit = static_cast<std::size_t>(firstBit);
            if (channel.bitDisplay.firstBit + channel.bitDisplay.bitCount > kMaxBitDisplayCount) {
                error = "psraw plot_setup channel bit_display 范围超过 64 bit";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        } else if (field == "bit_display.bit_count") {
            std::uint64_t bitCount = 0;
            if (!parseUnsigned(value, bitCount) || bitCount == 0 || bitCount > kMaxBitDisplayCount) {
                error = "psraw plot_setup channel bit_display.bit_count 格式错误";
                return EventFieldParseResult::Failed;
            }
            channel.bitDisplay.bitCount = static_cast<std::size_t>(bitCount);
            if (channel.bitDisplay.firstBit + channel.bitDisplay.bitCount > kMaxBitDisplayCount) {
                error = "psraw plot_setup channel bit_display 范围超过 64 bit";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        } else if (field == "bit_display.y_offset") {
            return parseFiniteDoubleField(
                value, channel.bitDisplay.yOffset, "psraw plot_setup channel bit_display.y_offset 格式错误", error);
        }
        return EventFieldParseResult::Handled;
    }

    EventFieldParseResult parsePlotSetupViewField(std::string_view key,
                                                  std::string_view value,
                                                  RawCaptureEvent& event,
                                                  std::string& error)
    {
        if (key == "view.time_scale") {
            return parseFiniteDoubleField(
                value, event.plotSetup.view.timeScale, "psraw plot_setup view.time_scale 格式错误", error);
        }
        if (key == "view.time_unit") {
            return parseStringHexField(
                value, event.plotSetup.view.timeUnit, "psraw plot_setup view.time_unit hex 格式错误", error);
        }
        if (key == "view.vertical_min") {
            return parseFiniteDoubleField(
                value, event.plotSetup.view.verticalMin, "psraw plot_setup view.vertical_min 格式错误", error);
        }
        if (key == "view.vertical_max") {
            return parseFiniteDoubleField(
                value, event.plotSetup.view.verticalMax, "psraw plot_setup view.vertical_max 格式错误", error);
        }
        if (key == "view.vertical_unit") {
            return parseStringHexField(
                value, event.plotSetup.view.verticalUnit, "psraw plot_setup view.vertical_unit hex 格式错误", error);
        }
        if (key == "view.history_limit") {
            std::uint64_t historyLimit = 0;
            if (!parseUnsigned(value, historyLimit) ||
                historyLimit > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                error = "psraw plot_setup view.history_limit 格式错误";
                return EventFieldParseResult::Failed;
            }
            event.plotSetup.view.historyLimit = static_cast<std::size_t>(historyLimit);
            return EventFieldParseResult::Handled;
        }
        return EventFieldParseResult::Ignored;
    }

    EventFieldParseResult parsePlotSetupField(std::string_view key,
                                              std::string_view value,
                                              RawCaptureEvent& event,
                                              DecodedEventState& state,
                                              std::string& error)
    {
        if (event.type != RawCaptureEventType::PlotSetup) {
            return EventFieldParseResult::Ignored;
        }

        if (key == "source") {
            return parseStringHexField(value, event.plotSetup.source, "psraw plot_setup source hex 格式错误", error);
        }
        if (key == "reset_history") {
            if (!parseBool(value, event.plotSetup.resetHistory)) {
                error = "psraw plot_setup reset_history 格式错误";
                return EventFieldParseResult::Failed;
            }
            return EventFieldParseResult::Handled;
        }
        if (key == "channel_count") {
            if (!parseUnsigned(value, state.plotChannelCount) || state.plotChannelCount == 0) {
                error = "psraw plot_setup channel_count 格式错误";
                return EventFieldParseResult::Failed;
            }
            if (state.plotChannelCount > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                error = "psraw plot_setup channel_count 过大";
                return EventFieldParseResult::Failed;
            }
            event.plotSetup.channels.resize(static_cast<std::size_t>(state.plotChannelCount));
            state.plotChannelCountSeen = true;
            return EventFieldParseResult::Handled;
        }

        auto result = parsePlotSetupChannelField(key, value, event, state, error);
        if (result != EventFieldParseResult::Ignored) {
            return result;
        }
        return parsePlotSetupViewField(key, value, event, error);
    }

    bool parseDecodedEventField(std::string_view key,
                                std::string_view value,
                                RawCaptureEvent& event,
                                DecodedEventState& state,
                                std::string& error)
    {
        if (key == "view.display_formula") {
            if (value != "0" && value != "1") {
                error = "psraw display_formula 格式错误";
                return false;
            }
            event.plotSetup.view.displayFormula = value == "1" ? WaveDisplayFormula::ScaleThenOffset :
                WaveDisplayFormula::OffsetThenScale;
            return true;
        }
        auto result = parseBaseEventField(key, value, event, state, error);
        if (result == EventFieldParseResult::Failed) {
            return false;
        }
        if (result == EventFieldParseResult::Handled) {
            return true;
        }

        result = parsePlotSetupField(key, value, event, state, error);
        return result != EventFieldParseResult::Failed;
    }

    bool finalizeDecodedEvent(std::string_view bytes,
                              std::size_t& cursor,
                              RawCaptureEvent& event,
                              const DecodedEventState& state,
                              std::string& error)
    {
        if (isBytesEvent(event)) {
            if (!state.rxSizeSeen) {
                error = "psraw rx_bytes 事件缺少 size";
                return false;
            }
            if (state.rxSize > static_cast<std::uint64_t>(bytes.size() - cursor)) {
                error = "psraw rx_bytes 事件 payload 超出文件范围";
                return false;
            }
            const auto rxSize = static_cast<std::size_t>(state.rxSize);
            event.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
                               bytes.begin() + static_cast<std::ptrdiff_t>(cursor + rxSize));
            cursor += rxSize;
            return true;
        }

        if (event.type == RawCaptureEventType::PlotSetup) {
            if (!state.plotChannelCountSeen) {
                error = "psraw plot_setup 事件缺少 channel_count";
                return false;
            }
            return true;
        }

        if (event.profile.frameName.empty()) {
            error = "psraw profile 事件缺少 frame";
            return false;
        }
        if (event.type == RawCaptureEventType::ProfileSet && !state.profileLengthSeen) {
            error = "psraw profile_set 事件缺少 length";
            return false;
        }
        return true;
    }

    bool decodeEventStream(std::string_view bytes, std::vector<RawCaptureEvent>& events, std::string& error)
    {
        events.clear();
        std::size_t cursor = 0;
        while (cursor < bytes.size()) {
            if (dataFileStopToken().stop_requested()) { error = "读取已取消"; return false; }
            std::size_t lineEnd = bytes.find('\n', cursor);
            if (lineEnd == std::string::npos) {
                lineEnd = bytes.size();
            }
            const auto firstLine = trimView(bytes.substr(cursor, lineEnd - cursor));
            if (firstLine.empty()) {
                cursor = lineEnd + 1;
                continue;
            }
            RawCaptureEvent event;
            if (!decodeEventType(firstLine, event, error)) {
                return false;
            }

            cursor = lineEnd + 1;
            DecodedEventState state;

            // 每条记录先解析文本字段，遇到空行后再按事件类型消费后续二进制 payload。
            while (cursor < bytes.size()) {
                lineEnd = bytes.find('\n', cursor);
                if (lineEnd == std::string::npos) {
                    lineEnd = bytes.size();
                }
                const auto line = trimView(bytes.substr(cursor, lineEnd - cursor));
                cursor = lineEnd + 1;
                if (line.empty()) {
                    break;
                }
                const auto pos = line.find(':');
                if (pos == std::string::npos) {
                    error = "psraw 事件字段格式错误";
                    return false;
                }
                const auto key = trimView(line.substr(0, pos));
                const auto value = trimView(line.substr(pos + 1));
                if (!parseDecodedEventField(key, value, event, state, error)) {
                    return false;
                }
            }

            if (!finalizeDecodedEvent(bytes, cursor, event, state, error)) {
                return false;
            }
            events.push_back(std::move(event));
        }
        return true;
    }

    struct DecodedRawCaptureHeader {
        RawCaptureFileData capture;
        std::uint64_t payloadSize{0};
        std::uint64_t waveSize{0};
    };

    struct RawCaptureHeaderState {
        bool separatorSeen{false};
        bool versionSeen{false};
        bool protocolNameSeen{false};
        bool protocolDirSeen{false};
        bool sampleFrequencySeen{false};
        bool rawSizeSeen{false};
        bool capturedAtSeen{false};
    };

    bool parseRawCaptureHeaderField(std::string_view key,
                                    std::string_view value,
                                    DecodedRawCaptureHeader& header,
                                    RawCaptureHeaderState& state,
                                    std::string& error)
    {
        if (key == "version") {
            state.versionSeen = (value == kVersionEvents || value == "3" || value == kVersionLegacyEvents);
        } else if (key == "wave_size") {
            if (!parseUnsigned(value, header.waveSize)) { error = "psraw wave_size 无效"; return false; }
        } else if (key == "source") {
            if (!decodeStringHex(value, header.capture.source)) { error = "psraw source 无效"; return false; }
        } else if (key == "range") {
            if (!decodeStringHex(value, header.capture.rangeDescription)) { error = "psraw range 无效"; return false; }
        } else if (key == "incomplete" || key == "filtered" || key == "rx_only") {
            bool flag = false;
            if (!parseBool(value, flag)) { error = "psraw 完整性字段无效"; return false; }
            if (key == "incomplete") header.capture.incomplete = flag;
            if (key == "filtered") header.capture.filtered = flag;
            if (key == "rx_only") header.capture.rxOnly = flag;
        } else if (key == "protocol_name") {
            state.protocolNameSeen = true;
            header.capture.protocolName = std::string(value);
        } else if (key == "protocol_dir") {
            state.protocolDirSeen = true;
            header.capture.protocolDir = std::string(value);
        } else if (key == "sample_frequency_hz") {
            state.sampleFrequencySeen = parseDouble(value, header.capture.sampleFrequencyHz);
        } else if (key == "payload_size" || key == "raw_size") {
            state.rawSizeSeen = parseUnsigned(value, header.payloadSize);
        } else if (key == "captured_at_ms") {
            state.capturedAtSeen = parseUnsigned(value, header.capture.capturedAtMs);
        } else if (key == "truncated") {
            if (!parseBool(value, header.capture.truncated)) {
                error = "psraw 文件头 truncated 字段格式错误";
                return false;
            }
        } else if (key == "event_stream") {
            bool eventsMode = false;
            if (!parseBool(value, eventsMode)) {
                error = "psraw 文件头 event_stream 字段格式错误";
                return false;
            }
        }
        return true;
    }

    bool validateRawCaptureHeaderState(const RawCaptureHeaderState& state, std::string& error)
    {
        if (!state.separatorSeen) {
            error = "psraw 文件头缺少空行分隔";
            return false;
        }
        if (!state.versionSeen || !state.protocolNameSeen || !state.protocolDirSeen || !state.sampleFrequencySeen ||
            !state.rawSizeSeen || !state.capturedAtSeen) {
            error = "psraw 文件头缺少必要字段";
            return false;
        }
        return true;
    }

    bool parseRawCaptureHeader(std::string_view headerText, DecodedRawCaptureHeader& header, std::string& error)
    {
        RawCaptureHeaderState state;
        std::size_t lineBegin = 0;
        for (;;) {
            std::size_t lineEnd = headerText.find('\n', lineBegin);
            if (lineEnd == std::string::npos) {
                lineEnd = headerText.size();
            }
            const auto line = trimView(headerText.substr(lineBegin, lineEnd - lineBegin));
            lineBegin = lineEnd + 1;
            if (line.empty()) {
                state.separatorSeen = true;
                break;
            }
            if (line == kFileMagic) {
                continue;
            }
            const auto separator = line.find(':');
            if (separator == std::string::npos) {
                error = "psraw 文件头字段格式错误";
                return false;
            }
            const auto key = trimView(line.substr(0, separator));
            const auto value = trimView(line.substr(separator + 1));
            if (!parseRawCaptureHeaderField(key, value, header, state, error)) {
                return false;
            }
            if (lineEnd >= headerText.size()) {
                break;
            }
        }
        return validateRawCaptureHeaderState(state, error);
    }

    bool sliceRawCapturePayload(std::string_view bytes,
                                std::uint64_t payloadSize,
                                std::string_view& payloadBytes,
                                std::string& error)
    {
        if (payloadSize >
            (std::numeric_limits<std::uint64_t>::max)() - static_cast<std::uint64_t>(kStreamHeaderBytes)) {
            error = "psraw payload 长度溢出";
            return false;
        }

        const auto expectedFileSize = static_cast<std::uint64_t>(kStreamHeaderBytes) + payloadSize;
        if (expectedFileSize > static_cast<std::uint64_t>(bytes.size())) {
            error = "psraw payload 长度超出文件大小";
            return false;
        }
        if (expectedFileSize != static_cast<std::uint64_t>(bytes.size())) {
            error = "psraw payload 后存在尾随脏字节";
            return false;
        }

        payloadBytes = std::string_view(bytes.data() + static_cast<std::ptrdiff_t>(kStreamHeaderBytes),
                                        static_cast<std::size_t>(payloadSize));
        return true;
    }

    void rebuildRawCapturePayloadFromEvents(RawCaptureFileData& capture)
    {
        std::size_t payloadSize = 0;
        for (const auto& event : capture.events) {
            if (event.type == RawCaptureEventType::RxBytes) {
                payloadSize += event.bytes.size();
            }
        }
        capture.payload.clear();
        capture.payload.reserve(payloadSize);
        for (const auto& event : capture.events) {
            if (event.type == RawCaptureEventType::RxBytes) {
                capture.payload.insert(capture.payload.end(), event.bytes.begin(), event.bytes.end());
            }
        }
    }

    struct PreparedRawCaptureEncoding {
        RawCaptureFileData normalized;
        std::string header;
        std::uint64_t rawSize{0};
        std::string waveform;
    };

    bool prepareRawCaptureEncoding(const RawCaptureFileData& capture,
                                   PreparedRawCaptureEncoding& prepared,
                                   std::string& error)
    {
        prepared.normalized = capture;
        prepared.normalized.events = normalizedEvents(capture);
        prepared.rawSize = totalEventBytes(prepared.normalized);
        if (capture.waveform) {
            std::ostringstream out;
            if (!encodeWaveCsv(out, *capture.waveform, WaveCsvShape::Long, {}, error)) return false;
            prepared.waveform = out.str();
        }
        return encodeFixedRawCaptureHeader(prepared.normalized, prepared.rawSize + prepared.waveform.size(),
                                           true, prepared.header, error, prepared.waveform.size());
    }

    void appendEncodedEventStream(const std::vector<RawCaptureEvent>& events, std::vector<std::uint8_t>& bytes)
    {
        for (const auto& event : events) {
            if (dataFileStopToken().stop_requested()) throw std::runtime_error("导出已取消");
            const auto record = encodeEventRecord(event);
            bytes.insert(bytes.end(), record.begin(), record.end());
            if (isBytesEvent(event) && !event.bytes.empty()) {
                bytes.insert(bytes.end(), event.bytes.begin(), event.bytes.end());
            }
            reportDataFileProgress(1);
        }
    }

    void writeEncodedEventStream(std::ostream& out, const std::vector<RawCaptureEvent>& events)
    {
        for (const auto& event : events) {
            if (dataFileStopToken().stop_requested()) throw std::runtime_error("导出已取消");
            const auto record = encodeEventRecord(event);
            out.write(record.data(), static_cast<std::streamsize>(record.size()));
            if (isBytesEvent(event) && !event.bytes.empty()) {
                out.write(reinterpret_cast<const char*>(event.bytes.data()),
                          static_cast<std::streamsize>(event.bytes.size()));
            }
            reportDataFileProgress(1);
        }
    }

} // namespace

std::string encodeRawCaptureHeader(const RawCaptureFileData& capture)
{
    RawCaptureFileData normalized = capture;
    normalized.events = normalizedEvents(capture);
    const auto rawSize = totalEventBytes(normalized);
    return encodeRawCaptureHeaderWithSize(normalized, rawSize, true);
}

std::string encodeRawCaptureEventRecordText(const RawCaptureEvent& event)
{
    return encodeEventRecord(event);
}

std::optional<RawCaptureEvent> decodeRawCaptureEventRecordText(std::string_view text, std::string& error)
{
    std::vector<RawCaptureEvent> events;
    if (!decodeEventStream(text, events, error) || events.size() != 1) {
        if (error.empty()) {
            error = "psraw 事件记录数量错误";
        }
        return std::nullopt;
    }
    return std::move(events.front());
}

bool encodeRawCaptureFile(const RawCaptureFileData& capture, std::vector<std::uint8_t>& bytes, std::string& error)
{
    PreparedRawCaptureEncoding prepared;
    if (!prepareRawCaptureEncoding(capture, prepared, error)) {
        return false;
    }

    bytes.clear();
    bytes.reserve(prepared.header.size() + static_cast<std::size_t>(prepared.rawSize));
    bytes.insert(bytes.end(), prepared.header.begin(), prepared.header.end());
    appendEncodedEventStream(prepared.normalized.events, bytes);
    bytes.insert(bytes.end(), prepared.waveform.begin(), prepared.waveform.end());
    return true;
}

std::optional<RawCaptureFileData> decodeRawCaptureFile(std::string_view bytes, std::string& error,
                                                     const DataFileReadCallbacks* callbacks)
{
    if (callbacks) {
        // 内存包使用流视图，不重复构造文本或完整波形模型。
        struct ViewBuffer : std::streambuf {
            explicit ViewBuffer(std::string_view bytes) {
                auto* begin = const_cast<char*>(bytes.data());
                setg(begin, begin, begin + bytes.size());
            }
            pos_type seekoff(off_type off, std::ios_base::seekdir direction,
                             std::ios_base::openmode) override {
                const auto base = direction == std::ios_base::beg ? 0 :
                    direction == std::ios_base::cur ? gptr() - eback() : egptr() - eback();
                const auto target = base + off;
                if (target < 0 || target > egptr() - eback()) return pos_type(off_type(-1));
                setg(eback(), eback() + target, egptr());
                return pos_type(target);
            }
            pos_type seekpos(pos_type pos, std::ios_base::openmode mode) override {
                return seekoff(off_type(pos), std::ios_base::beg, mode);
            }
        } buffer(bytes);
        std::istream input(&buffer);
        return readRawCaptureStream(input, bytes.size(), error, callbacks);
    }
    if (bytes.size() < kStreamHeaderBytes) {
        error = "psraw 文件长度不足";
        return std::nullopt;
    }

    DecodedRawCaptureHeader header;
    const auto headerText = std::string_view(bytes.data(), kStreamHeaderBytes);
    if (!parseRawCaptureHeader(headerText, header, error)) {
        return std::nullopt;
    }

    std::string_view payloadBytes;
    if (!sliceRawCapturePayload(bytes, header.payloadSize, payloadBytes, error)) {
        return std::nullopt;
    }

    if (header.waveSize > payloadBytes.size()) {
        error = "psraw 波形快照超出文件范围";
        return std::nullopt;
    }
    const auto eventSize = payloadBytes.size() - static_cast<std::size_t>(header.waveSize);
    if (header.waveSize > 0) {
        header.capture.waveform = decodeWaveCsv(payloadBytes.substr(eventSize), error);
        if (!header.capture.waveform) return std::nullopt;
    }
    if (!decodeEventStream(payloadBytes.substr(0, eventSize), header.capture.events, error)) {
        return std::nullopt;
    }
    rebuildRawCapturePayloadFromEvents(header.capture);
    for (const auto& event : header.capture.events)
        if (event.type == RawCaptureEventType::TxBytes) header.capture.rxOnly = false;
    return header.capture;
}

bool writeRawCaptureFile(const std::filesystem::path& path, const RawCaptureFileData& capture, std::string& error)
{
    PreparedRawCaptureEncoding prepared;
    if (!prepareRawCaptureEncoding(capture, prepared, error)) {
        return false;
    }

    try {
        if (path.has_parent_path()) {
            std::error_code directoryError;
            std::filesystem::create_directories(path.parent_path(), directoryError);
            if (directoryError) {
                error = "创建 psraw 目录失败: " + directoryError.message();
                return false;
            }
        }
        DataFileOutput output(path);
        auto& out = output.stream;
        if (!out.good()) {
            error = "无法打开 psraw 文件";
            return false;
        }
        out.write(prepared.header.data(), static_cast<std::streamsize>(prepared.header.size()));
        writeEncodedEventStream(out, prepared.normalized.events);
        out.write(prepared.waveform.data(), static_cast<std::streamsize>(prepared.waveform.size()));
        if (!out.good()) {
            error = "写入 psraw 文件失败";
            return false;
        }
        return output.commit(error);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::optional<RawCaptureFileData> readRawCaptureFile(const std::filesystem::path& path, std::string& error,
                                                   const DataFileReadCallbacks* callbacks)
{
    try {
        std::error_code sizeError;
        const auto fileSize = std::filesystem::file_size(path, sizeError);
        if (sizeError) {
            error = "无法获取 psraw 文件大小: " + sizeError.message();
            return std::nullopt;
        }
        if (fileSize > static_cast<std::uint64_t>((std::numeric_limits<std::streamsize>::max)())) {
            error = "psraw 文件过大，无法读取";
            return std::nullopt;
        }

        std::array<char, 65536> buffer{};
        std::ifstream in;
        in.rdbuf()->pubsetbuf(buffer.data(), buffer.size());
        in.open(path, std::ios::binary);
        if (!in.good()) {
            error = "无法打开 psraw 文件";
            return std::nullopt;
        }
        return readRawCaptureStream(in, fileSize, error, callbacks);
    } catch (const std::exception& ex) {
        error = ex.what();
        return std::nullopt;
    }
}

std::optional<RawCaptureFileData> readRawCaptureStream(std::istream& in, std::uint64_t fileSize, std::string& error,
                                                      const DataFileReadCallbacks* callbacks)
{
    try {
        std::array<char, kStreamHeaderBytes> headerBytes{};
        in.read(headerBytes.data(), headerBytes.size());
        if (in.gcount() != static_cast<std::streamsize>(headerBytes.size())) {
            error = "psraw 文件头读取不完整"; return std::nullopt;
        }
        DecodedRawCaptureHeader header;
        if (!parseRawCaptureHeader({headerBytes.data(), headerBytes.size()}, header, error)) return std::nullopt;
        if (fileSize < kStreamHeaderBytes || header.payloadSize != fileSize - kStreamHeaderBytes ||
            header.waveSize > header.payloadSize) {
            error = "psraw 文件长度与文件头不一致"; return std::nullopt;
        }
        const auto eventEnd = fileSize - header.waveSize;
        if (callbacks) {
            if (header.waveSize) {
                // 快照在事件区之后；先读取其表头确认替换，再流式提交样本，最后恢复事件。
                in.seekg(static_cast<std::streamoff>(eventEnd));
                auto waveCallbacks = *callbacks;
                waveCallbacks.metadata = [&](const RawCaptureFileData& wave, bool) {
                    header.capture.waveform = wave.waveform;
                    return callbacks->metadata(header.capture, eventEnd > kStreamHeaderBytes);
                };
                header.capture.waveform = readWaveCsvStream(in, error, &waveCallbacks);
                if (!header.capture.waveform) return std::nullopt;
                in.clear();
                in.seekg(kStreamHeaderBytes);
            } else if (!callbacks->metadata(header.capture, true)) return std::nullopt;
        }
        while (static_cast<std::uint64_t>(in.tellg()) < eventEnd) {
            if (dataFileStopToken().stop_requested()) { error = "读取已取消"; return std::nullopt; }
            std::string line;
            if (!std::getline(in, line)) { error = "psraw 事件读取不完整"; return std::nullopt; }
            if (trimView(line).empty()) continue;
            RawCaptureEvent event;
            if (!decodeEventType(trimView(line), event, error)) return std::nullopt;
            DecodedEventState state;
            bool separator = false;
            while (std::getline(in, line)) {
                const auto view = trimView(line);
                if (view.empty()) { separator = true; break; }
                const auto pos = view.find(':');
                if (pos == view.npos || !parseDecodedEventField(trimView(view.substr(0, pos)),
                    trimView(view.substr(pos + 1)), event, state, error)) {
                    if (error.empty()) error = "psraw 事件字段格式错误";
                    return std::nullopt;
                }
            }
            const auto position = in.tellg();
            if (!separator || position < 0 || static_cast<std::uint64_t>(position) > eventEnd) {
                error = "psraw 事件头超出事件区域"; return std::nullopt;
            }
            if (isBytesEvent(event)) {
                if (!state.rxSizeSeen || state.rxSize > eventEnd - static_cast<std::uint64_t>(position)) {
                    error = "psraw 事件字节长度无效"; return std::nullopt;
                }
                if (!callbacks) event.bytes.resize(static_cast<std::size_t>(state.rxSize));
                for (std::size_t offset = 0; offset < state.rxSize; offset += 65536) {
                    if (dataFileStopToken().stop_requested()) { error = "读取已取消"; return std::nullopt; }
                    const auto count = (std::min<std::size_t>)(65536, state.rxSize - offset);
                    if (callbacks) event.bytes.resize(count);
                    in.read(reinterpret_cast<char*>(event.bytes.data() + (callbacks ? 0 : offset)), count);
                    if (in.gcount() != static_cast<std::streamsize>(count)) {
                        error = "psraw 字节读取不完整"; return std::nullopt;
                    }
                    if (callbacks && !callbacks->event(event, offset != 0)) return std::nullopt;
                }
            } else {
                std::size_t cursor = 0;
                if (!finalizeDecodedEvent({}, cursor, event, state, error)) return std::nullopt;
            }
            if (event.type == RawCaptureEventType::TxBytes) header.capture.rxOnly = false;
            if (callbacks) {
                if ((!isBytesEvent(event) || state.rxSize == 0) && !callbacks->event(std::move(event), false))
                    return std::nullopt;
            } else header.capture.events.push_back(std::move(event));
        }
        if (header.waveSize && !callbacks) {
            header.capture.waveform = readWaveCsvStream(in, error);
            if (!header.capture.waveform) return std::nullopt;
        }
        rebuildRawCapturePayloadFromEvents(header.capture);
        return std::move(header.capture);
    } catch (const std::exception& ex) {
        error = ex.what();
        return std::nullopt;
    }
}

std::optional<RawCaptureFileData> readRawCaptureFileRegion(const std::filesystem::path& path,
    std::uint64_t offset, std::uint64_t size, std::string& error, const DataFileReadCallbacks& callbacks)
{
    // 把现场包中的一个条目暴露为独立的有界流；CSV 不得越界读到下一个附件。
    class RegionBuffer final : public std::streambuf {
    public:
        RegionBuffer(const std::filesystem::path& path, std::uint64_t offset, std::uint64_t size)
            : file_(path, std::ios::binary), offset_(offset), size_(size) { setg(buffer_.data(), buffer_.data(), buffer_.data()); }
    protected:
        int_type underflow() override {
            if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
            if (next_ >= size_) return traits_type::eof();
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(offset_ + next_));
            const auto count = (std::min<std::uint64_t>)(buffer_.size(), size_ - next_);
            file_.read(buffer_.data(), static_cast<std::streamsize>(count));
            if (file_.gcount() != static_cast<std::streamsize>(count))
                throw std::runtime_error("现场包原始数据读取不完整");
            next_ += count;
            setg(buffer_.data(), buffer_.data(), buffer_.data() + count);
            return traits_type::to_int_type(*gptr());
        }
        pos_type seekoff(off_type off, std::ios_base::seekdir direction, std::ios_base::openmode) override {
            const auto current = next_ - static_cast<std::uint64_t>(egptr() - gptr());
            if (direction == std::ios_base::cur && off == 0) return pos_type(current);
            const auto base = direction == std::ios_base::beg ? 0 :
                direction == std::ios_base::cur ? current : size_;
            const auto target = static_cast<off_type>(base) + off;
            if (target < 0 || static_cast<std::uint64_t>(target) > size_) return pos_type(off_type(-1));
            next_ = static_cast<std::uint64_t>(target);
            setg(buffer_.data(), buffer_.data(), buffer_.data());
            return pos_type(target);
        }
        pos_type seekpos(pos_type pos, std::ios_base::openmode mode) override {
            return seekoff(off_type(pos), std::ios_base::beg, mode);
        }
    private:
        std::ifstream file_;
        std::array<char, 65536> buffer_{};
        std::uint64_t offset_, size_, next_{0};
    } buffer(path, offset, size);
    std::istream input(&buffer);
    return readRawCaptureStream(input, size, error, &callbacks);
}

RawCaptureStreamWriter::~RawCaptureStreamWriter()
{
    if (!out_.is_open()) {
        return;
    }
    std::string ignored;
    static_cast<void>(close(ignored));
}

bool RawCaptureStreamWriter::isOpen() const
{
    return out_.is_open();
}

const std::filesystem::path& RawCaptureStreamWriter::path() const
{
    return path_;
}

std::uint64_t RawCaptureStreamWriter::bytesWritten() const
{
    return rxBytesWritten_;
}

bool RawCaptureStreamWriter::open(const std::filesystem::path& path,
                                  const RawCaptureFileData& metadata,
                                  std::string& error)
{
    if (out_.is_open()) {
        error = "已有完整原始数据录制正在进行";
        return false;
    }
    RawCaptureFileData cleanMetadata = metadata;
    cleanMetadata.payload.clear();
    cleanMetadata.events.clear();
    cleanMetadata.waveform.reset();
    cleanMetadata.truncated = false;
    std::string header;
    if (!encodeFixedRawCaptureHeader(cleanMetadata, 0, true, header, error)) {
        return false;
    }

    try {
        if (path.has_parent_path()) {
            std::error_code directoryError;
            std::filesystem::create_directories(path.parent_path(), directoryError);
            if (directoryError) {
                error = "创建 psraw 录制目录失败: " + directoryError.message();
                return false;
            }
        }
        out_.open(path, std::ios::binary | std::ios::trunc);
        if (!out_.good()) {
            error = "无法打开 psraw 录制文件";
            return false;
        }
        out_.write(header.data(), static_cast<std::streamsize>(header.size()));
        if (!out_.good()) {
            error = "无法写入 psraw 录制文件头";
            out_.close();
            return false;
        }
    } catch (const std::exception& ex) {
        error = ex.what();
        if (out_.is_open()) {
            out_.close();
        }
        return false;
    }

    path_ = path;
    metadata_ = std::move(cleanMetadata);
    bytesWritten_ = 0;
    rxBytesWritten_ = 0;
    return true;
}

bool RawCaptureStreamWriter::append(std::span<const std::uint8_t> bytes, std::string& error)
{
    RawCaptureEvent event;
    event.type = RawCaptureEventType::RxBytes;
    event.bytes.assign(bytes.begin(), bytes.end());
    return appendEvent(event, error);
}

bool RawCaptureStreamWriter::appendEvent(const RawCaptureEvent& event, std::string& error)
{
    if (!out_.is_open()) {
        error = "完整原始数据录制尚未开始";
        return false;
    }
    const auto record = encodeEventRecord(event);
    out_.write(record.data(), static_cast<std::streamsize>(record.size()));
    if (!out_.good()) {
        error = "写入 psraw 事件记录失败";
        return false;
    }
    bytesWritten_ += static_cast<std::uint64_t>(record.size());
    if (event.type == RawCaptureEventType::TxBytes) metadata_.rxOnly = false;
    if (isBytesEvent(event) && !event.bytes.empty()) {
        out_.write(reinterpret_cast<const char*>(event.bytes.data()), static_cast<std::streamsize>(event.bytes.size()));
        if (!out_.good()) {
            error = "写入 psraw 事件数据失败";
            return false;
        }
        bytesWritten_ += static_cast<std::uint64_t>(event.bytes.size());
        rxBytesWritten_ += static_cast<std::uint64_t>(event.bytes.size());
    }
    return true;
}

bool RawCaptureStreamWriter::close(std::string& error)
{
    if (!out_.is_open()) {
        return true;
    }

    std::string header;
    if (!encodeFixedRawCaptureHeader(metadata_, bytesWritten_, true, header, error)) {
        out_.close();
        return false;
    }
    out_.flush();
    if (!out_.good()) {
        error = "刷新 psraw 录制文件失败";
        out_.close();
        return false;
    }
    out_.seekp(0, std::ios::beg);
    if (!out_.good()) {
        error = "回写 psraw 录制文件头失败";
        out_.close();
        return false;
    }
    out_.write(header.data(), static_cast<std::streamsize>(header.size()));
    if (!out_.good()) {
        error = "更新 psraw 录制文件头失败";
        out_.close();
        return false;
    }
    out_.close();
    return true;
}

} // namespace protoscope::plot
