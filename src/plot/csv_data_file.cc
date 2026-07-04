#include "protoscope/plot/csv_data_file.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>

namespace protoscope::plot {
namespace {

    struct CsvRow {
        std::size_t line{1};
        std::vector<std::string> fields;
    };

    struct ParsedCsv {
        std::map<std::string, std::string> metadata;
        std::vector<CsvRow> rows;
    };

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

    bool parseDouble(std::string_view text, double& value)
    {
        const auto cleaned = trim(text);
        if (cleaned.empty()) {
            return false;
        }
        try {
            std::size_t consumed = 0;
            value = std::stod(cleaned, &consumed);
            return consumed == cleaned.size() && std::isfinite(value);
        } catch (...) {
            return false;
        }
    }

    bool parseUnsigned(std::string_view text, std::uint64_t& value)
    {
        const auto cleaned = trim(text);
        if (cleaned.empty()) {
            return false;
        }
        const auto* begin = cleaned.data();
        const auto* end = cleaned.data() + cleaned.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        return ec == std::errc{} && ptr == end;
    }

    char hexDigit(std::uint8_t value)
    {
        return static_cast<char>(value < 10 ? ('0' + value) : ('A' + (value - 10)));
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

    std::string encodeHex(std::span<const std::uint8_t> bytes)
    {
        std::string out;
        out.reserve(bytes.size() * 2);
        for (const auto byte : bytes) {
            out.push_back(hexDigit(static_cast<std::uint8_t>((byte >> 4U) & 0x0FU)));
            out.push_back(hexDigit(static_cast<std::uint8_t>(byte & 0x0FU)));
        }
        return out;
    }

    bool decodeHexBytes(std::string_view text, std::vector<std::uint8_t>& bytes, std::size_t line, std::string& error)
    {
        std::string cleaned;
        cleaned.reserve(text.size());
        for (const char ch : text) {
            if (std::isspace(static_cast<unsigned char>(ch)) == 0) {
                cleaned.push_back(ch);
            }
        }
        if (cleaned.size() % 2 != 0) {
            error = "CSV 第 " + std::to_string(line) + " 行 HEX 字段长度不是偶数";
            return false;
        }
        bytes.clear();
        bytes.reserve(cleaned.size() / 2);
        for (std::size_t index = 0; index < cleaned.size(); index += 2) {
            const int high = decodeHexNibble(cleaned[index]);
            const int low = decodeHexNibble(cleaned[index + 1]);
            if (high < 0 || low < 0) {
                error = "CSV 第 " + std::to_string(line) + " 行 HEX 字段包含非法字符";
                return false;
            }
            bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
        }
        return true;
    }

    std::string csvEscape(std::string_view value)
    {
        bool quoted = false;
        for (const char ch : value) {
            quoted = quoted || ch == ',' || ch == '"' || ch == '\n' || ch == '\r' || ch == '#';
        }
        if (!quoted) {
            return std::string(value);
        }
        std::string out;
        out.reserve(value.size() + 2);
        out.push_back('"');
        for (const char ch : value) {
            if (ch == '"') {
                out.push_back('"');
            }
            out.push_back(ch);
        }
        out.push_back('"');
        return out;
    }

    void writeCsvRow(std::ostream& out, const std::vector<std::string>& fields)
    {
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (index > 0) {
                out << ',';
            }
            out << csvEscape(fields[index]);
        }
        out << '\n';
    }

    std::string formatDouble(double value)
    {
        std::ostringstream out;
        out << std::setprecision(17) << value;
        return out.str();
    }

    bool parseCsvText(std::string_view text, ParsedCsv& csv, std::string& error)
    {
        csv = {};
        std::vector<std::string> fields;
        std::string field;
        std::size_t line = 1;
        std::size_t rowLine = 1;
        bool inQuotes = false;
        bool atRowStart = true;
        bool commentLine = false;
        std::string comment;

        const auto finishComment = [&]() {
            const auto content = trim(comment);
            const auto separator = content.find('=');
            if (separator != std::string::npos) {
                csv.metadata[trim(std::string_view(content).substr(0, separator))] =
                    trim(std::string_view(content).substr(separator + 1));
            }
            comment.clear();
            commentLine = false;
        };

        const auto finishRow = [&]() {
            if (!field.empty() || !fields.empty()) {
                fields.push_back(std::move(field));
                csv.rows.push_back(CsvRow{.line = rowLine, .fields = std::move(fields)});
            }
            field.clear();
            fields.clear();
            atRowStart = true;
            rowLine = line + 1;
        };

        for (std::size_t index = 0; index <= text.size(); ++index) {
            const char ch = index < text.size() ? text[index] : '\n';
            if (commentLine) {
                if (ch == '\n' || ch == '\r') {
                    finishComment();
                    if (ch == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
                        ++index;
                    }
                    ++line;
                    rowLine = line;
                } else {
                    comment.push_back(ch);
                }
                continue;
            }
            if (atRowStart && fields.empty() && field.empty() && ch == '#') {
                commentLine = true;
                continue;
            }
            atRowStart = false;
            if (inQuotes) {
                if (ch == '"') {
                    if (index + 1 < text.size() && text[index + 1] == '"') {
                        field.push_back('"');
                        ++index;
                    } else {
                        inQuotes = false;
                    }
                } else {
                    if (ch == '\n') {
                        ++line;
                    }
                    field.push_back(ch);
                }
                continue;
            }
            if (ch == '"') {
                inQuotes = true;
            } else if (ch == ',') {
                fields.push_back(std::move(field));
                field.clear();
            } else if (ch == '\n' || ch == '\r') {
                finishRow();
                if (ch == '\r' && index + 1 < text.size() && text[index + 1] == '\n') {
                    ++index;
                }
                ++line;
                rowLine = line;
            } else {
                field.push_back(ch);
            }
        }
        if (inQuotes) {
            error = "CSV 引号未闭合";
            return false;
        }
        return true;
    }

    std::optional<ParsedCsv> readParsedCsvFile(const std::filesystem::path& path, std::string& error)
    {
        try {
            std::ifstream in(path, std::ios::binary);
            if (!in.good()) {
                error = "无法打开 CSV 文件";
                return std::nullopt;
            }
            const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            ParsedCsv parsed;
            if (!parseCsvText(contents, parsed, error)) {
                return std::nullopt;
            }
            return parsed;
        } catch (const std::exception& ex) {
            error = ex.what();
            return std::nullopt;
        }
    }

    std::map<std::string, std::size_t> headerIndex(const CsvRow& header)
    {
        std::map<std::string, std::size_t> columns;
        for (std::size_t index = 0; index < header.fields.size(); ++index) {
            columns[trim(header.fields[index])] = index;
        }
        return columns;
    }

    std::string cell(const CsvRow& row, std::size_t index)
    {
        return index < row.fields.size() ? row.fields[index] : std::string{};
    }

    bool timeInRange(double time, const std::optional<std::pair<double, double>>& range)
    {
        return !range.has_value() || (time >= range->first && time <= range->second);
    }

    WaveCsvShape metadataShape(const std::map<std::string, std::string>& metadata)
    {
        const auto iter = metadata.find("shape");
        if (iter != metadata.end() && iter->second == "long") {
            return WaveCsvShape::Long;
        }
        return WaveCsvShape::Wide;
    }

    std::string metadataValue(const std::map<std::string, std::string>& metadata, std::string_view key)
    {
        const auto iter = metadata.find(std::string(key));
        return iter == metadata.end() ? std::string{} : iter->second;
    }

    std::string serializeChannelMapSemicolon(const std::vector<std::size_t>& map)
    {
        std::ostringstream out;
        for (std::size_t index = 0; index < map.size(); ++index) {
            if (index > 0) {
                out << ';';
            }
            out << map[index];
        }
        return out.str();
    }

    bool parseChannelMapSemicolon(std::string_view text,
                                  std::vector<std::size_t>& map,
                                  std::size_t line,
                                  std::string& error)
    {
        map.clear();
        const auto cleaned = trim(text);
        if (cleaned.empty()) {
            return true;
        }
        std::size_t begin = 0;
        while (begin <= cleaned.size()) {
            std::size_t end = cleaned.find(';', begin);
            if (end == std::string::npos) {
                end = cleaned.size();
            }
            std::uint64_t value = 0;
            if (!parseUnsigned(std::string_view(cleaned).substr(begin, end - begin), value) ||
                value > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                error = "CSV 第 " + std::to_string(line) + " 行 profile_channel_map 格式错误";
                return false;
            }
            map.push_back(static_cast<std::size_t>(value));
            if (end == cleaned.size()) {
                break;
            }
            begin = end + 1;
        }
        return true;
    }

    const char* rawEventTypeName(RawCaptureEventType type)
    {
        switch (type) {
            case RawCaptureEventType::RxBytes:
                return "rx_bytes";
            case RawCaptureEventType::ProfileSet:
                return "profile_set";
            case RawCaptureEventType::ProfileClear:
                return "profile_clear";
            case RawCaptureEventType::PlotSetup:
                return "plot_setup";
        }
        return "rx_bytes";
    }

    bool parseRawEventType(std::string_view text, RawCaptureEventType& type)
    {
        const auto cleaned = trim(text);
        if (cleaned == "rx_bytes") {
            type = RawCaptureEventType::RxBytes;
            return true;
        }
        if (cleaned == "profile_set") {
            type = RawCaptureEventType::ProfileSet;
            return true;
        }
        if (cleaned == "profile_clear") {
            type = RawCaptureEventType::ProfileClear;
            return true;
        }
        if (cleaned == "plot_setup") {
            type = RawCaptureEventType::PlotSetup;
            return true;
        }
        return false;
    }

    std::string encodePlotSetupRecordHex(const RawCaptureEvent& event)
    {
        const auto text = encodeRawCaptureEventRecordText(event);
        return encodeHex(
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    bool decodePlotSetupRecordHex(std::string_view text, RawCaptureEvent& event, std::size_t line, std::string& error)
    {
        std::vector<std::uint8_t> bytes;
        if (!decodeHexBytes(text, bytes, line, error)) {
            return false;
        }
        const std::string record(bytes.begin(), bytes.end());
        auto decoded = decodeRawCaptureEventRecordText(record, error);
        if (!decoded.has_value() || decoded->type != RawCaptureEventType::PlotSetup) {
            error = "CSV 第 " + std::to_string(line) + " 行 plot_setup_record_hex 不是有效 plot_setup 记录";
            return false;
        }
        event = std::move(*decoded);
        return true;
    }

    using CsvColumns = std::map<std::string, std::size_t>;

    bool readWaveCsvFrequency(const ParsedCsv& parsed, WaveCsvData& data, std::string& error)
    {
        const auto frequencyText = metadataValue(parsed.metadata, "sample_frequency_hz");
        if (frequencyText.empty()) {
            return true;
        }
        if (!parseDouble(frequencyText, data.sampleFrequencyHz)) {
            error = "波形 CSV sample_frequency_hz 格式错误";
            return false;
        }
        return true;
    }

    bool readWaveCsvLongRows(const ParsedCsv& parsed, const CsvColumns& columns, WaveCsvData& data, std::string& error)
    {
        const auto channelIndexColumn = columns.find("channel_index");
        const auto timeColumn = columns.find("time");
        const auto valueColumn = columns.find("value");
        if (channelIndexColumn == columns.end() || timeColumn == columns.end() || valueColumn == columns.end()) {
            error = "波形长表 CSV 缺少必要列";
            return false;
        }

        const auto labelColumn = columns.find("channel_label");
        const auto unitColumn = columns.find("unit");
        data.shape = WaveCsvShape::Long;
        for (std::size_t rowIndex = 1; rowIndex < parsed.rows.size(); ++rowIndex) {
            const auto& row = parsed.rows[rowIndex];
            std::uint64_t oneBasedChannel = 0;
            double time = 0.0;
            double value = 0.0;
            if (!parseUnsigned(cell(row, channelIndexColumn->second), oneBasedChannel) || oneBasedChannel == 0) {
                error = "CSV 第 " + std::to_string(row.line) + " 行 channel_index 格式错误";
                return false;
            }
            if (!parseDouble(cell(row, timeColumn->second), time) ||
                !parseDouble(cell(row, valueColumn->second), value)) {
                error = "CSV 第 " + std::to_string(row.line) + " 行 time/value 格式错误";
                return false;
            }
            const auto channelIndex = static_cast<std::size_t>(oneBasedChannel - 1);
            if (data.channels.size() <= channelIndex) {
                data.channels.resize(channelIndex + 1);
            }
            auto& channel = data.channels[channelIndex];
            if (labelColumn != columns.end() && channel.label.empty()) {
                channel.label = cell(row, labelColumn->second);
            }
            if (unitColumn != columns.end() && channel.unit.empty()) {
                channel.unit = cell(row, unitColumn->second);
            }
            channel.samples.push_back({.time = time, .value = value});
        }
        return true;
    }

    void initializeWideWaveCsvChannels(const ParsedCsv& parsed,
                                       const CsvColumns& columns,
                                       std::size_t timeColumn,
                                       WaveCsvData& data)
    {
        data.shape = WaveCsvShape::Wide;
        data.channels.resize(parsed.rows.front().fields.size() - 1);
        for (std::size_t column = 0, channelIndex = 0; column < parsed.rows.front().fields.size(); ++column) {
            if (column == timeColumn) {
                continue;
            }
            auto& channel = data.channels[channelIndex];
            channel.label = parsed.rows.front().fields[column];
            const auto metadataPrefix = "channel." + std::to_string(channelIndex + 1) + ".";
            if (const auto label = metadataValue(parsed.metadata, metadataPrefix + "label"); !label.empty()) {
                channel.label = label;
            }
            channel.unit = metadataValue(parsed.metadata, metadataPrefix + "unit");
            ++channelIndex;
        }
        static_cast<void>(columns);
    }

    bool readWaveCsvWideRows(const ParsedCsv& parsed, const CsvColumns& columns, WaveCsvData& data, std::string& error)
    {
        const auto timeColumn = columns.find("time");
        if (timeColumn == columns.end()) {
            error = "波形宽表 CSV 缺少 time 列";
            return false;
        }

        initializeWideWaveCsvChannels(parsed, columns, timeColumn->second, data);
        for (std::size_t rowIndex = 1; rowIndex < parsed.rows.size(); ++rowIndex) {
            const auto& row = parsed.rows[rowIndex];
            double time = 0.0;
            if (!parseDouble(cell(row, timeColumn->second), time)) {
                error = "CSV 第 " + std::to_string(row.line) + " 行 time 格式错误";
                return false;
            }
            for (std::size_t column = 0, channelIndex = 0; column < parsed.rows.front().fields.size(); ++column) {
                if (column == timeColumn->second) {
                    continue;
                }
                const auto text = cell(row, column);
                if (!trim(text).empty()) {
                    double value = 0.0;
                    if (!parseDouble(text, value)) {
                        error = "CSV 第 " + std::to_string(row.line) + " 行波形数值格式错误";
                        return false;
                    }
                    data.channels[channelIndex].samples.push_back({.time = time, .value = value});
                }
                ++channelIndex;
            }
        }
        return true;
    }

    void finalizeWaveCsvChannels(WaveCsvData& data)
    {
        for (std::size_t channelIndex = 0; channelIndex < data.channels.size(); ++channelIndex) {
            auto& channel = data.channels[channelIndex];
            if (channel.label.empty()) {
                channel.label = "CH" + std::to_string(channelIndex + 1);
            }
            std::sort(channel.samples.begin(),
                      channel.samples.end(),
                      [](const WaveSample& left, const WaveSample& right) { return left.time < right.time; });
        }
    }

    bool readRawCaptureCsvMetadata(const ParsedCsv& parsed, RawCaptureFileData& capture, std::string& error)
    {
        capture.protocolName = metadataValue(parsed.metadata, "protocol_name");
        capture.protocolDir = metadataValue(parsed.metadata, "protocol_dir");
        if (const auto capturedAt = metadataValue(parsed.metadata, "captured_at_ms"); !capturedAt.empty()) {
            if (!parseUnsigned(capturedAt, capture.capturedAtMs)) {
                error = "原始事件 CSV captured_at_ms 格式错误";
                return false;
            }
        }
        if (const auto frequency = metadataValue(parsed.metadata, "sample_frequency_hz"); !frequency.empty()) {
            if (!parseDouble(frequency, capture.sampleFrequencyHz)) {
                error = "原始事件 CSV sample_frequency_hz 格式错误";
                return false;
            }
        }
        return true;
    }

    bool appendRawCaptureCsvEvent(const CsvRow& row,
                                  const CsvColumns& columns,
                                  std::size_t eventTypeColumn,
                                  std::size_t timestampColumn,
                                  RawCaptureFileData& capture,
                                  std::string& error)
    {
        RawCaptureEvent event;
        if (!parseRawEventType(cell(row, eventTypeColumn), event.type)) {
            error = "CSV 第 " + std::to_string(row.line) + " 行 event_type 未知";
            return false;
        }
        if (!parseUnsigned(cell(row, timestampColumn), event.timestampMs)) {
            error = "CSV 第 " + std::to_string(row.line) + " 行 timestamp_ms 格式错误";
            return false;
        }
        if (capture.capturedAtMs == 0) {
            capture.capturedAtMs = event.timestampMs;
        }
        if (event.type == RawCaptureEventType::RxBytes) {
            const auto bytesColumn = columns.find("bytes_hex");
            if (bytesColumn == columns.end() ||
                !decodeHexBytes(cell(row, bytesColumn->second), event.bytes, row.line, error)) {
                return false;
            }
        } else if (event.type == RawCaptureEventType::ProfileSet) {
            const auto frameColumn = columns.find("profile_frame");
            const auto lengthColumn = columns.find("profile_length");
            if (frameColumn == columns.end() || lengthColumn == columns.end()) {
                error = "原始事件 CSV 缺少 profile_set 必要列";
                return false;
            }
            event.profile.frameName = cell(row, frameColumn->second);
            std::uint64_t length = 0;
            if (!parseUnsigned(cell(row, lengthColumn->second), length) || length == 0 ||
                length > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
                error = "CSV 第 " + std::to_string(row.line) + " 行 profile_length 格式错误";
                return false;
            }
            event.profile.length = static_cast<std::size_t>(length);
            if (const auto mapColumn = columns.find("profile_channel_map"); mapColumn != columns.end()) {
                if (!parseChannelMapSemicolon(
                        cell(row, mapColumn->second), event.profile.channelMap, row.line, error)) {
                    return false;
                }
            }
        } else if (event.type == RawCaptureEventType::ProfileClear) {
            const auto frameColumn = columns.find("profile_frame");
            if (frameColumn == columns.end()) {
                error = "原始事件 CSV 缺少 profile_frame 列";
                return false;
            }
            event.profile.frameName = cell(row, frameColumn->second);
        } else if (event.type == RawCaptureEventType::PlotSetup) {
            const auto recordColumn = columns.find("plot_setup_record_hex");
            if (recordColumn == columns.end() ||
                !decodePlotSetupRecordHex(cell(row, recordColumn->second), event, row.line, error)) {
                return false;
            }
            event.timestampMs = [&]() {
                std::uint64_t timestamp = 0;
                return parseUnsigned(cell(row, timestampColumn), timestamp) ? timestamp : event.timestampMs;
            }();
        }
        capture.events.push_back(std::move(event));
        return true;
    }

    void rebuildRawCapturePayload(RawCaptureFileData& capture)
    {
        capture.payload.clear();
        for (const auto& event : capture.events) {
            if (event.type == RawCaptureEventType::RxBytes) {
                capture.payload.insert(capture.payload.end(), event.bytes.begin(), event.bytes.end());
            }
        }
    }

    bool ensureParentDirectory(const std::filesystem::path& path, std::string& error)
    {
        if (!path.has_parent_path()) {
            return true;
        }
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            error = "创建 CSV 目录失败: " + ec.message();
            return false;
        }
        return true;
    }

} // namespace

std::optional<std::pair<double, double>> resolveCsvExportTimeRange(const CsvExportRange& range)
{
    switch (range.kind) {
        case CsvExportRangeKind::Full:
            return std::nullopt;
        case CsvExportRangeKind::CurrentView:
            return std::minmax(range.currentViewMinTime, range.currentViewMaxTime);
        case CsvExportRangeKind::CursorPair:
            return std::minmax(range.cursorATime, range.cursorBTime);
        case CsvExportRangeKind::Manual:
            return std::minmax(range.manualMinTime, range.manualMaxTime);
    }
    return std::nullopt;
}

CsvKind detectCsvKind(const std::filesystem::path& path, std::string& error)
{
    const auto parsed = readParsedCsvFile(path, error);
    if (!parsed.has_value()) {
        return CsvKind::Unknown;
    }
    const auto kind = metadataValue(parsed->metadata, "kind");
    if (kind == "wave") {
        return CsvKind::Wave;
    }
    if (kind == "raw_events") {
        return CsvKind::RawEvents;
    }
    if (parsed->rows.empty()) {
        error = "CSV 缺少表头";
        return CsvKind::Unknown;
    }
    const auto columns = headerIndex(parsed->rows.front());
    if (columns.contains("event_type") && columns.contains("timestamp_ms")) {
        return CsvKind::RawEvents;
    }
    if (columns.contains("time") ||
        (columns.contains("channel_index") && columns.contains("time") && columns.contains("value"))) {
        return CsvKind::Wave;
    }
    error = "未知 CSV 类型";
    return CsvKind::Unknown;
}

bool writeWaveCsvFile(const std::filesystem::path& path,
                      const WaveCsvData& data,
                      WaveCsvShape shape,
                      const CsvExportRange& range,
                      std::string& error)
{
    try {
        if (!ensureParentDirectory(path, error)) {
            return false;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            error = "无法打开波形 CSV 文件";
            return false;
        }
        out << "# protoscope_csv_version=1\n";
        out << "# kind=wave\n";
        out << "# shape=" << (shape == WaveCsvShape::Long ? "long" : "wide") << '\n';
        out << "# sample_frequency_hz=" << formatDouble(data.sampleFrequencyHz) << '\n';
        for (std::size_t index = 0; index < data.channels.size(); ++index) {
            out << "# channel." << (index + 1) << ".label=" << data.channels[index].label << '\n';
            out << "# channel." << (index + 1) << ".unit=" << data.channels[index].unit << '\n';
        }
        const auto resolvedRange = resolveCsvExportTimeRange(range);
        if (shape == WaveCsvShape::Long) {
            writeCsvRow(out, {"channel_index", "channel_label", "unit", "time", "value"});
            for (std::size_t channelIndex = 0; channelIndex < data.channels.size(); ++channelIndex) {
                const auto& channel = data.channels[channelIndex];
                for (const auto& sample : channel.samples) {
                    if (!timeInRange(sample.time, resolvedRange)) {
                        continue;
                    }
                    writeCsvRow(out,
                                {std::to_string(channelIndex + 1),
                                 channel.label,
                                 channel.unit,
                                 formatDouble(sample.time),
                                 formatDouble(sample.value)});
                }
            }
            return out.good();
        }

        std::vector<double> times;
        for (const auto& channel : data.channels) {
            for (const auto& sample : channel.samples) {
                if (timeInRange(sample.time, resolvedRange)) {
                    times.push_back(sample.time);
                }
            }
        }
        std::sort(times.begin(), times.end());
        times.erase(
            std::unique(
                times.begin(), times.end(), [](double left, double right) { return std::abs(left - right) < 1e-12; }),
            times.end());

        std::vector<std::string> header{"time"};
        for (std::size_t channelIndex = 0; channelIndex < data.channels.size(); ++channelIndex) {
            header.push_back(data.channels[channelIndex].label.empty() ? "CH" + std::to_string(channelIndex + 1)
                                                                       : data.channels[channelIndex].label);
        }
        writeCsvRow(out, header);
        std::vector<std::size_t> cursors(data.channels.size(), 0);
        for (const double time : times) {
            std::vector<std::string> row{formatDouble(time)};
            for (std::size_t channelIndex = 0; channelIndex < data.channels.size(); ++channelIndex) {
                const auto& samples = data.channels[channelIndex].samples;
                auto& cursor = cursors[channelIndex];
                while (cursor < samples.size() && samples[cursor].time < time - 1e-12) {
                    ++cursor;
                }
                if (cursor < samples.size() && std::abs(samples[cursor].time - time) < 1e-12) {
                    row.push_back(formatDouble(samples[cursor].value));
                } else {
                    row.emplace_back();
                }
            }
            writeCsvRow(out, row);
        }
        return out.good();
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::optional<WaveCsvData> readWaveCsvFile(const std::filesystem::path& path, std::string& error)
{
    const auto parsed = readParsedCsvFile(path, error);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    if (detectCsvKind(path, error) != CsvKind::Wave) {
        error = "CSV 不是波形数据";
        return std::nullopt;
    }
    if (parsed->rows.empty()) {
        error = "波形 CSV 缺少表头";
        return std::nullopt;
    }
    WaveCsvData data;
    data.shape = metadataShape(parsed->metadata);
    if (!readWaveCsvFrequency(*parsed, data, error)) {
        return std::nullopt;
    }

    const auto columns = headerIndex(parsed->rows.front());
    if (data.shape == WaveCsvShape::Long || columns.contains("channel_index")) {
        if (!readWaveCsvLongRows(*parsed, columns, data, error)) {
            return std::nullopt;
        }
    } else {
        if (!readWaveCsvWideRows(*parsed, columns, data, error)) {
            return std::nullopt;
        }
    }

    finalizeWaveCsvChannels(data);
    return data;
}

bool writeRawCaptureCsvFile(const std::filesystem::path& path,
                            const RawCaptureFileData& capture,
                            const CsvExportRange& range,
                            std::string& error)
{
    try {
        if (!ensureParentDirectory(path, error)) {
            return false;
        }
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            error = "无法打开原始事件 CSV 文件";
            return false;
        }
        out << "# protoscope_csv_version=1\n";
        out << "# kind=raw_events\n";
        out << "# protocol_name=" << capture.protocolName << '\n';
        out << "# protocol_dir=" << capture.protocolDir << '\n';
        out << "# sample_frequency_hz=" << formatDouble(capture.sampleFrequencyHz) << '\n';
        out << "# captured_at_ms=" << capture.capturedAtMs << '\n';
        writeCsvRow(out,
                    {"event_type",
                     "timestamp_ms",
                     "elapsed_ms",
                     "bytes_hex",
                     "profile_frame",
                     "profile_length",
                     "profile_channel_map",
                     "plot_setup_record_hex"});

        const auto resolvedRange = resolveCsvExportTimeRange(range);
        const auto events = capture.events.empty() ? std::vector<RawCaptureEvent>{RawCaptureEvent{
                                                         .type = RawCaptureEventType::RxBytes,
                                                         .timestampMs = capture.capturedAtMs,
                                                         .bytes = capture.payload,
                                                     }}
                                                   : capture.events;
        for (const auto& event : events) {
            const double elapsedSeconds =
                (static_cast<double>(event.timestampMs) - static_cast<double>(capture.capturedAtMs)) / 1000.0;
            if (!timeInRange(elapsedSeconds, resolvedRange)) {
                continue;
            }
            std::vector<std::string> row{
                rawEventTypeName(event.type),
                std::to_string(event.timestampMs),
                std::to_string(event.timestampMs >= capture.capturedAtMs ? event.timestampMs - capture.capturedAtMs
                                                                         : 0),
                {},
                {},
                {},
                {},
                {},
            };
            if (event.type == RawCaptureEventType::RxBytes) {
                row[3] = encodeHex(event.bytes);
            } else if (event.type == RawCaptureEventType::ProfileSet) {
                row[4] = event.profile.frameName;
                row[5] = std::to_string(event.profile.length);
                row[6] = serializeChannelMapSemicolon(event.profile.channelMap);
            } else if (event.type == RawCaptureEventType::ProfileClear) {
                row[4] = event.profile.frameName;
            } else if (event.type == RawCaptureEventType::PlotSetup) {
                row[7] = encodePlotSetupRecordHex(event);
            }
            writeCsvRow(out, row);
        }
        return out.good();
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::optional<RawCaptureFileData> readRawCaptureCsvFile(const std::filesystem::path& path, std::string& error)
{
    const auto parsed = readParsedCsvFile(path, error);
    if (!parsed.has_value()) {
        return std::nullopt;
    }
    if (detectCsvKind(path, error) != CsvKind::RawEvents) {
        error = "CSV 不是原始事件数据";
        return std::nullopt;
    }
    if (parsed->rows.empty()) {
        error = "原始事件 CSV 缺少表头";
        return std::nullopt;
    }
    const auto columns = headerIndex(parsed->rows.front());
    const auto eventTypeColumn = columns.find("event_type");
    const auto timestampColumn = columns.find("timestamp_ms");
    if (eventTypeColumn == columns.end() || timestampColumn == columns.end()) {
        error = "原始事件 CSV 缺少必要列";
        return std::nullopt;
    }

    RawCaptureFileData capture;
    if (!readRawCaptureCsvMetadata(*parsed, capture, error)) {
        return std::nullopt;
    }

    for (std::size_t rowIndex = 1; rowIndex < parsed->rows.size(); ++rowIndex) {
        const auto& row = parsed->rows[rowIndex];
        if (!appendRawCaptureCsvEvent(row, columns, eventTypeColumn->second, timestampColumn->second, capture, error)) {
            return std::nullopt;
        }
    }
    rebuildRawCapturePayload(capture);
    return capture;
}

} // namespace protoscope::plot
