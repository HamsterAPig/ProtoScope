#include "protoscope/plot/raw_capture_file.hpp"

#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace protoscope::plot {
namespace {

    constexpr std::string_view kFileMagic = "ProtoScopeRawCapture";
    constexpr std::string_view kVersionEvents = "3";
    constexpr std::string_view kVersionLegacyEvents = "2";
    constexpr std::size_t kStreamHeaderBytes = 4096;

    std::string trim(std::string_view text)
    {
        std::size_t begin = 0;
        while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
            ++begin;
        }
        std::size_t end = text.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
            --end;
        }
        return std::string(text.substr(begin, end - begin));
    }

    bool parseUnsigned(std::string_view text, std::uint64_t& value)
    {
        const auto cleaned = trim(text);
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
        const auto cleaned = trim(text);
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
        const auto cleaned = trim(text);
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
        const auto cleaned = trim(text);
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
        const auto cleaned = trim(text);
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

    std::string encodePlotSetupRecord(const RawCaptureEvent& event)
    {
        std::ostringstream out;
        out << "event: plot_setup\n"
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
                << "channel." << index << ".color: " << serializeColor(channel.color) << '\n';
        }
        out << "view.time_scale: " << std::setprecision(17) << event.plotSetup.view.timeScale << '\n'
            << "view.time_unit: " << encodeStringHex(event.plotSetup.view.timeUnit) << '\n'
            << "view.vertical_min: " << std::setprecision(17) << event.plotSetup.view.verticalMin << '\n'
            << "view.vertical_max: " << std::setprecision(17) << event.plotSetup.view.verticalMax << '\n'
            << "view.vertical_unit: " << encodeStringHex(event.plotSetup.view.verticalUnit) << '\n'
            << "view.history_limit: " << event.plotSetup.view.historyLimit << '\n'
            << '\n';
        return out.str();
    }

    std::uint64_t totalEventBytes(const RawCaptureFileData& capture)
    {
        std::uint64_t total = 0;
        for (const auto& event : capture.events) {
            switch (event.type) {
                case RawCaptureEventType::RxBytes: {
                    std::ostringstream line;
                    line << "event: rx_bytes\n"
                         << "timestamp_ms: " << event.timestampMs << '\n'
                         << "size: " << event.bytes.size() << '\n'
                         << '\n';
                    total +=
                        static_cast<std::uint64_t>(line.str().size()) + static_cast<std::uint64_t>(event.bytes.size());
                    break;
                }
                case RawCaptureEventType::ProfileSet: {
                    std::ostringstream line;
                    line << "event: profile_set\n"
                         << "timestamp_ms: " << event.timestampMs << '\n'
                         << "frame: " << event.profile.frameName << '\n'
                         << "length: " << event.profile.length << '\n'
                         << "channel_map: " << serializeChannelMap(event.profile.channelMap) << '\n'
                         << '\n';
                    total += static_cast<std::uint64_t>(line.str().size());
                    break;
                }
                case RawCaptureEventType::ProfileClear: {
                    std::ostringstream line;
                    line << "event: profile_clear\n"
                         << "timestamp_ms: " << event.timestampMs << '\n'
                         << "frame: " << event.profile.frameName << '\n'
                         << '\n';
                    total += static_cast<std::uint64_t>(line.str().size());
                    break;
                }
                case RawCaptureEventType::PlotSetup: {
                    total += static_cast<std::uint64_t>(encodePlotSetupRecord(event).size());
                    break;
                }
            }
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
                                               bool eventsMode)
    {
        std::ostringstream header;
        header << kFileMagic << '\n'
               << "version: " << kVersionEvents << '\n'
               << "protocol_name: " << capture.protocolName << '\n'
               << "protocol_dir: " << capture.protocolDir << '\n'
               << "sample_frequency_hz: " << capture.sampleFrequencyHz << '\n'
               << "captured_at_ms: " << capture.capturedAtMs << '\n'
               << "truncated: " << (capture.truncated ? "true" : "false") << '\n'
               << "payload_size: " << rawSize << '\n'
               << "event_stream: " << (eventsMode ? "true" : "false") << '\n'
               << '\n';
        return header.str();
    }

    bool encodeFixedRawCaptureHeader(const RawCaptureFileData& capture,
                                     std::uint64_t rawSize,
                                     bool eventsMode,
                                     std::string& header,
                                     std::string& error)
    {
        const std::string base = encodeRawCaptureHeaderWithSize(capture, rawSize, eventsMode);
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
                out << "event: rx_bytes\n"
                    << "timestamp_ms: " << event.timestampMs << '\n'
                    << "size: " << event.bytes.size() << '\n'
                    << '\n';
                break;
            case RawCaptureEventType::ProfileSet:
                out << "event: profile_set\n"
                    << "timestamp_ms: " << event.timestampMs << '\n'
                    << "frame: " << event.profile.frameName << '\n'
                    << "length: " << event.profile.length << '\n'
                    << "channel_map: " << serializeChannelMap(event.profile.channelMap) << '\n'
                    << '\n';
                break;
            case RawCaptureEventType::ProfileClear:
                out << "event: profile_clear\n"
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
        if (event.type == RawCaptureEventType::RxBytes) {
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
            std::size_t lineEnd = bytes.find('\n', cursor);
            if (lineEnd == std::string::npos) {
                lineEnd = bytes.size();
            }
            const auto firstLine = trim(bytes.substr(cursor, lineEnd - cursor));
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
                const auto line = trim(bytes.substr(cursor, lineEnd - cursor));
                cursor = lineEnd + 1;
                if (line.empty()) {
                    break;
                }
                const auto pos = line.find(':');
                if (pos == std::string::npos) {
                    error = "psraw 事件字段格式错误";
                    return false;
                }
                const auto key = trim(line.substr(0, pos));
                const auto value = trim(line.substr(pos + 1));
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
            state.versionSeen = (value == kVersionEvents || value == kVersionLegacyEvents);
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
            auto line = trim(headerText.substr(lineBegin, lineEnd - lineBegin));
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
            const auto key = trim(line.substr(0, separator));
            const auto value = trim(line.substr(separator + 1));
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
        capture.payload.clear();
        for (const auto& event : capture.events) {
            if (event.type == RawCaptureEventType::RxBytes) {
                capture.payload.insert(capture.payload.end(), event.bytes.begin(), event.bytes.end());
            }
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

bool encodeRawCaptureFile(const RawCaptureFileData& capture, std::vector<std::uint8_t>& bytes, std::string& error)
{
    RawCaptureFileData normalized = capture;
    normalized.events = normalizedEvents(capture);
    const auto rawSize = totalEventBytes(normalized);
    std::string header;
    if (!encodeFixedRawCaptureHeader(normalized, rawSize, true, header, error)) {
        return false;
    }

    bytes.clear();
    bytes.reserve(header.size() + static_cast<std::size_t>(rawSize));
    bytes.insert(bytes.end(), header.begin(), header.end());
    for (const auto& event : normalized.events) {
        const auto record = encodeEventRecord(event);
        bytes.insert(bytes.end(), record.begin(), record.end());
        if (event.type == RawCaptureEventType::RxBytes && !event.bytes.empty()) {
            bytes.insert(bytes.end(), event.bytes.begin(), event.bytes.end());
        }
    }
    return true;
}

std::optional<RawCaptureFileData> decodeRawCaptureFile(std::string_view bytes, std::string& error)
{
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

    if (!decodeEventStream(payloadBytes, header.capture.events, error)) {
        return std::nullopt;
    }
    rebuildRawCapturePayloadFromEvents(header.capture);
    return header.capture;
}

bool writeRawCaptureFile(const std::filesystem::path& path, const RawCaptureFileData& capture, std::string& error)
{
    RawCaptureFileData normalized = capture;
    normalized.events = normalizedEvents(capture);
    const auto rawSize = totalEventBytes(normalized);
    std::string header;
    if (!encodeFixedRawCaptureHeader(normalized, rawSize, true, header, error)) {
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
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            error = "无法打开 psraw 文件";
            return false;
        }
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        for (const auto& event : normalized.events) {
            const auto record = encodeEventRecord(event);
            out.write(record.data(), static_cast<std::streamsize>(record.size()));
            if (event.type == RawCaptureEventType::RxBytes && !event.bytes.empty()) {
                out.write(reinterpret_cast<const char*>(event.bytes.data()),
                          static_cast<std::streamsize>(event.bytes.size()));
            }
        }
        if (!out.good()) {
            error = "写入 psraw 文件失败";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::optional<RawCaptureFileData> readRawCaptureFile(const std::filesystem::path& path, std::string& error)
{
    try {
        std::ifstream in(path, std::ios::binary);
        if (!in.good()) {
            error = "无法打开 psraw 文件";
            return std::nullopt;
        }
        std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return decodeRawCaptureFile(contents, error);
    } catch (const std::exception& ex) {
        error = ex.what();
        return std::nullopt;
    }
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
    if (metadata.protocolName.empty() || metadata.protocolDir.empty()) {
        error = "当前协议元数据不完整，无法开始录制";
        return false;
    }

    RawCaptureFileData cleanMetadata = metadata;
    cleanMetadata.payload.clear();
    cleanMetadata.events.clear();
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
    if (event.type == RawCaptureEventType::RxBytes && !event.bytes.empty()) {
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
