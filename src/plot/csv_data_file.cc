#include "protoscope/plot/csv_data_file.hpp"
#include "protoscope/plot/data_file_output.hpp"

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
#include <functional>
#include <queue>

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

    bool parseCsvStream(std::istream& input, ParsedCsv& csv, std::string& error,
                        const std::function<bool(CsvRow&&)>& consume = {})
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
        bool failed = false;

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
                CsvRow row{.line = rowLine, .fields = std::move(fields)};
                if (consume) failed = !consume(std::move(row));
                else csv.rows.push_back(std::move(row));
            }
            field.clear();
            fields.clear();
            atRowStart = true;
            rowLine = line + 1;
        };

        for (;;) {
            if (failed || dataFileStopToken().stop_requested()) {
                if (error.empty()) error = "CSV 读取已取消";
                return false;
            }
            const int next = input.get();
            const bool eof = next == std::char_traits<char>::eof();
            if (eof && inQuotes) { error = "CSV 引号未闭合"; return false; }
            const char ch = eof ? '\n' : static_cast<char>(next);
            if (commentLine) {
                if (ch == '\n' || ch == '\r') {
                    finishComment();
                    if (ch == '\r' && input.peek() == '\n') input.get();
                    ++line;
                    rowLine = line;
                } else {
                    comment.push_back(ch);
                }
                if (eof) break;
                continue;
            }
            if (atRowStart && fields.empty() && field.empty() && ch == '#') {
                commentLine = true;
                continue;
            }
            atRowStart = false;
            if (inQuotes) {
                if (ch == '"') {
                    if (input.peek() == '"') {
                        field.push_back('"');
                        input.get();
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
                if (ch == '\r' && input.peek() == '\n') input.get();
                ++line;
                rowLine = line;
            } else {
                field.push_back(ch);
            }
            if (eof) break;
        }
        if (inQuotes) {
            error = "CSV 引号未闭合";
            return false;
        }
        if (input.bad()) { error = "CSV 文件读取失败"; return false; }
        return !failed;
    }

    std::optional<ParsedCsv> readParsedCsvFile(const std::filesystem::path& path, std::string& error)
    {
        try {
            std::array<char, 65536> buffer{};
            std::ifstream in;
            in.rdbuf()->pubsetbuf(buffer.data(), buffer.size());
            in.open(path, std::ios::binary);
            if (!in.good()) {
                error = "无法打开 CSV 文件";
                return std::nullopt;
            }
            ParsedCsv parsed;
            // 探测仅消费文件头和第一条表头记录，避免导入时重复扫描整个文件。
            std::string probeError;
            parseCsvStream(in, parsed, probeError, [&](CsvRow&& row) {
                parsed.rows.push_back(std::move(row));
                return false;
            });
            if (parsed.rows.empty()) {
                error = probeError.empty() ? "CSV 缺少表头" : probeError;
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
            case RawCaptureEventType::TxBytes:
                return "tx_bytes";
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
        if (cleaned == "tx_bytes") {
            type = RawCaptureEventType::TxBytes;
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
            if (!parseUnsigned(cell(row, channelIndexColumn->second), oneBasedChannel) || oneBasedChannel == 0 ||
                oneBasedChannel > 65536) {
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
            std::stable_sort(channel.samples.begin(),
                      channel.samples.end(),
                      [](const WaveSample& left, const WaveSample& right) { return left.time < right.time; });
        }
    }

    bool readRawCaptureCsvMetadata(const ParsedCsv& parsed, RawCaptureFileData& capture, std::string& error)
    {
        capture.source = metadataValue(parsed.metadata, "source");
        capture.truncated = metadataValue(parsed.metadata, "truncated") == "true";
        capture.filtered = metadataValue(parsed.metadata, "filtered") == "true";
        capture.incomplete = metadataValue(parsed.metadata, "incomplete") == "true";
        capture.rxOnly = metadataValue(parsed.metadata, "rx_only") != "false";
        if (const auto range = metadataValue(parsed.metadata, "range"); !range.empty())
            capture.rangeDescription = range;
        capture.protocolName = metadataValue(parsed.metadata, "protocol_name");
        capture.protocolDir = metadataValue(parsed.metadata, "protocol_dir");
        for (auto [key, target] : {std::pair{"source_hex", &capture.source},
                                  std::pair{"protocol_name_hex", &capture.protocolName},
                                  std::pair{"protocol_dir_hex", &capture.protocolDir}}) {
            if (!parsed.metadata.contains(key)) continue;
            std::vector<std::uint8_t> bytes;
            if (!decodeHexBytes(metadataValue(parsed.metadata, key), bytes, 1, error)) return false;
            target->assign(bytes.begin(), bytes.end());
        }
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
        if (event.type == RawCaptureEventType::RxBytes || event.type == RawCaptureEventType::TxBytes) {
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
        if (const auto it = columns.find("endpoint"); it != columns.end()) event.endpoint = cell(row, it->second);
        if (const auto it = columns.find("write_status"); it != columns.end()) event.writeStatus = cell(row, it->second);
        if (const auto it = columns.find("sequence"); it != columns.end() &&
            !parseUnsigned(cell(row, it->second), event.sequence)) {
            error = "CSV 事件顺序格式错误";
            return false;
        }
        if (event.type == RawCaptureEventType::TxBytes) capture.rxOnly = false;
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

bool validateCsvExportRange(const CsvExportRange& range, std::string& error)
{
    const auto resolved = resolveCsvExportTimeRange(range);
    if (resolved && (!std::isfinite(resolved->first) || !std::isfinite(resolved->second))) {
        error = "导出范围包含无效时间";
        return false;
    }
    return true;
}

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
    if (!kind.empty()) { error = "CSV 类型不支持无损导入"; return CsvKind::Unknown; }
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
        if (!validateCsvExportRange(range, error)) return false;
        if (!ensureParentDirectory(path, error)) {
            return false;
        }
        DataFileOutput output(path);
        auto& out = output.stream;
        if (!out.good()) {
            error = "无法打开波形 CSV 文件";
            return false;
        }
        return encodeWaveCsv(out, data, shape, range, error) && output.commit(error);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

bool encodeWaveCsv(std::ostream& out, const WaveCsvData& data, WaveCsvShape shape,
                   const CsvExportRange& range, std::string& error)
{
    try {
        if (!validateCsvExportRange(range, error)) return false;
        for (const auto& channel : data.channels) {
            for (const auto& sample : channel.samples) {
                if (!std::isfinite(sample.time) || !std::isfinite(sample.value)) {
                    error = "波形样本包含非有限数值";
                    return false;
                }
            }
        }
        out << "# protoscope_csv_version=2\n";
        out << "# kind=wave\n";
        out << "# shape=" << (shape == WaveCsvShape::Long ? "long" : "wide") << '\n';
        const bool ordered = std::all_of(data.channels.begin(), data.channels.end(), [](const auto& channel) {
            return std::is_sorted(channel.samples.begin(), channel.samples.end(),
                [](const auto& a, const auto& b) { return a.time < b.time; });
        });
        if (ordered) out << "# sample_order=channel_time\n";
        out << "# sample_frequency_hz=" << formatDouble(data.sampleFrequencyHz) << '\n';
        out << "# time_axis=" << data.timeAxis << '\n';
        out << "# incomplete=" << (data.incomplete ? "true" : "false") << '\n';
        out << "# range=" << data.rangeDescription << '\n';
        RawCaptureEvent setup;
        setup.type = RawCaptureEventType::PlotSetup;
        setup.plotSetup.source = data.source;
        setup.plotSetup.view = data.view;
        for (std::size_t index = 0; index < data.channels.size(); ++index) {
            auto spec = data.channels[index].spec;
            spec.label = data.channels[index].label;
            spec.unit = data.channels[index].unit;
            setup.plotSetup.channels.push_back(std::move(spec));
            out << "# channel." << (index + 1) << ".index_offset=" << data.channels[index].sampleIndexOffset << '\n';
        }
        out << "# plot_setup_hex=" << encodePlotSetupRecordHex(setup) << '\n';
        const auto resolvedRange = resolveCsvExportTimeRange(range);
        if (shape == WaveCsvShape::Long) {
            writeCsvRow(out, {"channel_index", "channel_label", "unit", "time", "value"});
            for (std::size_t channelIndex = 0; channelIndex < data.channels.size(); ++channelIndex) {
                const auto& channel = data.channels[channelIndex];
                for (const auto& sample : channel.samples) {
                    if (dataFileStopToken().stop_requested()) { error = "导出已取消"; return false; }
                    if (!timeInRange(sample.time, resolvedRange)) {
                        continue;
                    }
                    writeCsvRow(out,
                                {std::to_string(channelIndex + 1),
                                 channel.label,
                                 channel.unit,
                                 formatDouble(sample.time),
                                 formatDouble(sample.value)});
                    reportDataFileProgress(1);
                }
            }
            return out.good();
        }

        // 同一时间的第 N 个样本占独立一行；仅按精确时间配对，绝不使用绘图容差去重。
        struct Position { double time; std::size_t channel; std::size_t sample; };
        const auto later = [](const Position& a, const Position& b) {
            return a.time > b.time || (a.time == b.time && a.channel > b.channel);
        };
        std::priority_queue<Position, std::vector<Position>, decltype(later)> rows(later);
        const auto enqueue = [&](std::size_t c, std::size_t i) {
            const auto& samples = data.channels[c].samples;
            while (i < samples.size() && !timeInRange(samples[i].time, resolvedRange)) ++i;
            if (i < samples.size()) rows.push({samples[i].time, c, i});
        };
        for (std::size_t c = 0; c < data.channels.size(); ++c) enqueue(c, 0);

        std::vector<std::string> header{"time"};
        for (std::size_t channelIndex = 0; channelIndex < data.channels.size(); ++channelIndex) {
            header.push_back("channel_" + std::to_string(channelIndex + 1));
        }
        writeCsvRow(out, header);
        while (!rows.empty()) {
            if (dataFileStopToken().stop_requested()) { error = "导出已取消"; return false; }
            const double time = rows.top().time;
            std::vector<std::string> row(data.channels.size() + 1);
            row[0] = formatDouble(time);
            std::vector<Position> consumed;
            while (!rows.empty() && rows.top().time == time) {
                const auto position = rows.top();
                rows.pop();
                row[position.channel + 1] = formatDouble(data.channels[position.channel].samples[position.sample].value);
                consumed.push_back(position);
            }
            writeCsvRow(out, row);
            reportDataFileProgress(consumed.size());
            for (const auto& position : consumed) enqueue(position.channel, position.sample + 1);
        }
        return out.good();
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

static std::optional<WaveCsvData> decodeParsedWaveCsv(const ParsedCsv& parsedData, std::string& error,
                                                    std::optional<WaveCsvData> streamed = std::nullopt)
{
    const auto* parsed = &parsedData;
    const auto kind = metadataValue(parsed->metadata, "kind");
    if (!kind.empty() && kind != "wave") {
        error = "CSV 不是波形数据";
        return std::nullopt;
    }
    if (parsed->rows.empty()) {
        error = "波形 CSV 缺少表头";
        return std::nullopt;
    }
    WaveCsvData data = streamed ? std::move(*streamed) : WaveCsvData{};
    if (const auto range = metadataValue(parsed->metadata, "range"); !range.empty()) data.rangeDescription = range;
    data.shape = metadataShape(parsed->metadata);
    if (!readWaveCsvFrequency(*parsed, data, error)) {
        return std::nullopt;
    }

    const auto columns = headerIndex(parsed->rows.front());
    if (columns.contains("channel_index")) data.shape = WaveCsvShape::Long;
    if (streamed) {
        if (!columns.contains("channel_index"))
            initializeWideWaveCsvChannels(*parsed, columns, columns.at("time"), data);
    } else if (data.shape == WaveCsvShape::Long || columns.contains("channel_index")) {
        if (!readWaveCsvLongRows(*parsed, columns, data, error)) {
            return std::nullopt;
        }
    } else {
        if (!readWaveCsvWideRows(*parsed, columns, data, error)) {
            return std::nullopt;
        }
    }

    finalizeWaveCsvChannels(data);
    if (const auto setupHex = metadataValue(parsed->metadata, "plot_setup_hex"); !setupHex.empty()) {
        RawCaptureEvent setup;
        if (!decodePlotSetupRecordHex(setupHex, setup, 1, error)) return std::nullopt;
        if (data.channels.size() > setup.plotSetup.channels.size()) {
            error = "波形 CSV 通道数量与元数据不一致";
            return std::nullopt;
        }
        data.channels.resize(setup.plotSetup.channels.size());
        data.source = setup.plotSetup.source;
        data.view = setup.plotSetup.view;
        data.timeAxis = metadataValue(parsed->metadata, "time_axis");
        data.incomplete = metadataValue(parsed->metadata, "incomplete") == "true";
        for (std::size_t i = 0; i < data.channels.size(); ++i) {
            auto& channel = data.channels[i];
            channel.spec = setup.plotSetup.channels[i];
            channel.label = channel.spec.label;
            channel.unit = channel.spec.unit;
            std::uint64_t offset = 0;
            if (!parseUnsigned(metadataValue(parsed->metadata, "channel." + std::to_string(i + 1) + ".index_offset"), offset)) {
                error = "波形 CSV 样本序号偏移无效";
                return std::nullopt;
            }
            channel.sampleIndexOffset = static_cast<std::size_t>(offset);
        }
    }
    return data;
}

std::optional<WaveCsvData> decodeWaveCsv(std::string_view text, std::string& error)
{
    struct ViewBuffer : std::streambuf {
        explicit ViewBuffer(std::string_view value) {
            auto* begin = const_cast<char*>(value.data());
            setg(begin, begin, begin + value.size());
        }
    } buffer(text);
    std::istream input(&buffer);
    return readWaveCsvStream(input, error);
}

std::optional<WaveCsvData> readWaveCsvFile(const std::filesystem::path& path, std::string& error,
                                        const DataFileReadCallbacks* callbacks)
{
    std::array<char, 65536> buffer{};
    std::ifstream input;
    input.rdbuf()->pubsetbuf(buffer.data(), buffer.size());
    input.open(path, std::ios::binary);
    if (!input) { error = "无法打开波形 CSV"; return std::nullopt; }
    return readWaveCsvStream(input, error, callbacks);
}

std::optional<WaveCsvData> readWaveCsvStream(std::istream& input, std::string& error,
                                          const DataFileReadCallbacks* callbacks)
{
    ParsedCsv parsed;
    WaveCsvData data;
    CsvColumns columns;
    CsvRow header;
    bool streaming = false;
    std::size_t pending = 0;
    WaveCsvData metadata;
    std::vector<double> lastTimes;
    const auto flush = [&]() {
        for (std::size_t c = 0; c < data.channels.size(); ++c) {
            auto& values = data.channels[c].samples;
            if (values.empty()) continue;
            if (!callbacks->samples(c, metadata.channels[c].sampleIndexOffset, std::move(values))) return false;
            values.clear();
        }
        pending = 0;
        return true;
    };
    const auto consume = [&](CsvRow&& row) {
        if (header.fields.empty()) {
            header = std::move(row);
            columns = headerIndex(header);
            if (!columns.contains("time")) { error = "波形 CSV 缺少 time 列"; return false; }
            // 原生有序文件只解析一次；旧版或乱序文件仍在后台完整排序，禁止 UI 全历史排序。
            streaming = callbacks && metadataValue(parsed.metadata, "sample_order") == "channel_time" &&
                        !metadataValue(parsed.metadata, "plot_setup_hex").empty();
            if (streaming) {
                parsed.rows.push_back(header);
                auto decoded = decodeParsedWaveCsv(parsed, error);
                parsed.rows.clear();
                if (!decoded) return false;
                metadata = std::move(*decoded);
                data = metadata;
                lastTimes.assign(metadata.channels.size(), -std::numeric_limits<double>::infinity());
                if (!callbacks->metadata(RawCaptureFileData{.waveform = metadata}, false)) return false;
            }
            return true;
        }
        ParsedCsv batch;
        batch.rows.push_back(header);
        batch.rows.push_back(std::move(row));
        if (!(columns.contains("channel_index") ? readWaveCsvLongRows(batch, columns, data, error) :
                                                 readWaveCsvWideRows(batch, columns, data, error))) return false;
        if (streaming) {
            if (data.channels.size() != metadata.channels.size()) {
                error = "波形 CSV 通道数量与元数据不一致"; return false;
            }
            pending = 0;
            for (std::size_t c = 0; c < data.channels.size(); ++c) {
                const auto& values = data.channels[c].samples;
                pending += values.size();
                if (!values.empty()) {
                    if (values.back().time < lastTimes[c]) {
                        error = "波形 CSV 与有序样本声明不一致"; return false;
                    }
                    lastTimes[c] = values.back().time;
                }
            }
            if (pending >= 8192 && !flush()) return false;
        }
        return true;
    };
    if (!parseCsvStream(input, parsed, error, consume)) return std::nullopt;
    if (streaming) {
        if (!flush()) return std::nullopt;
        return metadata;
    }
    if (!header.fields.empty()) parsed.rows.push_back(std::move(header));
    auto decoded = decodeParsedWaveCsv(parsed, error, std::move(data));
    if (!decoded || !callbacks) return decoded;
    metadata = *decoded;
    for (auto& channel : metadata.channels) channel.samples.clear();
    if (!callbacks->metadata(RawCaptureFileData{.waveform = metadata}, false)) return std::nullopt;
    for (std::size_t c = 0; c < decoded->channels.size(); ++c) {
        const auto& channel = decoded->channels[c];
        for (std::size_t i = 0; i < channel.samples.size(); i += 8192) {
            const auto end = (std::min)(i + 8192, channel.samples.size());
            if (!callbacks->samples(c, channel.sampleIndexOffset,
                {channel.samples.begin() + i, channel.samples.begin() + end})) return std::nullopt;
        }
    }
    return metadata;
}

bool writeRawCaptureCsvFile(const std::filesystem::path& path,
                            const RawCaptureFileData& capture,
                            const CsvExportRange& range,
                            std::string& error)
{
    try {
        if (!validateCsvExportRange(range, error)) return false;
        if (!ensureParentDirectory(path, error)) {
            return false;
        }
        DataFileOutput output(path);
        auto& out = output.stream;
        if (!out.good()) {
            error = "无法打开原始事件 CSV 文件";
            return false;
        }
        out << "# protoscope_csv_version=2\n";
        out << "# kind=raw_events\n";
        const auto textHex = [](const std::string& text) {
            return encodeHex({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
        };
        out << "# source_hex=" << textHex(capture.source) << '\n';
        out << "# truncated=" << (capture.truncated ? "true" : "false") << '\n';
        out << "# filtered=" << (capture.filtered || range.kind != CsvExportRangeKind::Full ? "true" : "false") << '\n';
        out << "# incomplete=" << (capture.incomplete ? "true" : "false") << '\n';
        out << "# rx_only=" << (capture.rxOnly ? "true" : "false") << '\n';
        out << "# range=" << capture.rangeDescription << '\n';
        out << "# protocol_name_hex=" << textHex(capture.protocolName) << '\n';
        out << "# protocol_dir_hex=" << textHex(capture.protocolDir) << '\n';
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
                     "plot_setup_record_hex", "endpoint", "sequence", "write_status"});

        const auto resolvedRange = resolveCsvExportTimeRange(range);
        const auto events = capture.events.empty() ? std::vector<RawCaptureEvent>{RawCaptureEvent{
                                                         .type = RawCaptureEventType::RxBytes,
                                                         .timestampMs = capture.capturedAtMs,
                                                         .bytes = capture.payload,
                                                     }}
                                                   : capture.events;
        for (const auto& event : events) {
            if (dataFileStopToken().stop_requested()) { error = "导出已取消"; return false; }
            const double elapsedSeconds =
                (static_cast<double>(event.timestampMs) - static_cast<double>(capture.capturedAtMs)) / 1000.0;
            // 配置事件保留至范围末端，保证所选字节之前的解析配置仍可恢复。
            const bool bytesEvent = event.type == RawCaptureEventType::RxBytes || event.type == RawCaptureEventType::TxBytes;
            if ((bytesEvent && !timeInRange(elapsedSeconds, resolvedRange)) ||
                (!bytesEvent && resolvedRange && elapsedSeconds > resolvedRange->second)) {
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
                event.endpoint,
                std::to_string(event.sequence),
                event.writeStatus,
            };
            if (bytesEvent) {
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
            reportDataFileProgress(1);
        }
        return output.commit(error);
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

std::optional<RawCaptureFileData> readRawCaptureCsvFile(const std::filesystem::path& path, std::string& error,
                                                      const DataFileReadCallbacks* callbacks)
{
    std::array<char, 65536> buffer{};
    std::ifstream input;
    input.rdbuf()->pubsetbuf(buffer.data(), buffer.size());
    input.open(path, std::ios::binary);
    if (!input) { error = "无法打开收发 CSV"; return std::nullopt; }
    ParsedCsv parsed;
    CsvColumns columns;
    RawCaptureFileData capture;
    bool headerSeen = false;
    const auto consume = [&](CsvRow&& row) {
        if (!headerSeen) {
            headerSeen = true;
            columns = headerIndex(row);
            if (!columns.contains("event_type") || !columns.contains("timestamp_ms")) {
                error = "原始事件 CSV 缺少必要列";
                return false;
            }
            if (const auto kind = metadataValue(parsed.metadata, "kind"); !kind.empty() && kind != "raw_events") {
                error = "CSV 不是原始事件数据"; return false;
            }
            if (!readRawCaptureCsvMetadata(parsed, capture, error)) return false;
            if (callbacks && !callbacks->metadata(capture, true)) return false;
            return true;
        }
        if (!appendRawCaptureCsvEvent(row, columns, columns.at("event_type"), columns.at("timestamp_ms"), capture, error))
            return false;
        if (callbacks) {
            auto event = std::move(capture.events.back());
            capture.events.clear();
            if (!callbacks->event(std::move(event), false)) return false;
        }
        return true;
    };
    if (!parseCsvStream(input, parsed, error, consume)) return std::nullopt;
    if (const auto kind = metadataValue(parsed.metadata, "kind"); !kind.empty() && kind != "raw_events") {
        error = "CSV 不是原始事件数据";
        return std::nullopt;
    }
    if (!headerSeen) {
        error = "原始事件 CSV 缺少表头";
        return std::nullopt;
    }
    if (!readRawCaptureCsvMetadata(parsed, capture, error)) {
        return std::nullopt;
    }

    for (const auto& event : capture.events)
        if (event.type == RawCaptureEventType::TxBytes) capture.rxOnly = false;
    rebuildRawCapturePayload(capture);
    return capture;
}

} // namespace protoscope::plot
