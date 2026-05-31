#include "protoscope/plot/raw_capture_file.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>

namespace protoscope::plot {
namespace {

constexpr std::string_view kFileMagic = "ProtoScopeRawCapture";
constexpr std::string_view kVersion = "1";
constexpr std::size_t kStreamHeaderBytes = 4096;

std::string trim(std::string_view text) {
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

bool parseUnsigned(std::string_view text, std::uint64_t& value) {
    const auto cleaned = trim(text);
    const auto* begin = cleaned.data();
    const auto* end = cleaned.data() + cleaned.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    return ec == std::errc{} && ptr == end;
}

bool parseDouble(std::string_view text, double& value) {
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

bool parseBool(std::string_view text, bool& value) {
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

std::string encodeRawCaptureHeaderWithSize(const RawCaptureFileData& capture, std::uint64_t rawSize) {
    std::ostringstream out;
    out << kFileMagic << '\n'
        << "version: " << kVersion << '\n'
        << "protocol_name: " << capture.protocolName << '\n'
        << "protocol_dir: " << capture.protocolDir << '\n'
        << "sample_frequency_hz: " << capture.sampleFrequencyHz << '\n'
        << "raw_size: " << rawSize << '\n'
        << "captured_at_ms: " << capture.capturedAtMs << '\n'
        << "truncated: " << (capture.truncated ? "true" : "false") << '\n'
        << '\n';
    return out.str();
}

bool encodeFixedRawCaptureHeader(const RawCaptureFileData& capture,
                                 std::uint64_t rawSize,
                                 std::string& header,
                                 std::string& error) {
    const std::string base = encodeRawCaptureHeaderWithSize(capture, rawSize);
    constexpr std::string_view kPaddingPrefix = "padding: ";
    const auto minimumSize = base.size() + kPaddingPrefix.size() + 1U;
    if (minimumSize > kStreamHeaderBytes) {
        error = "psraw 文件头超过流式录制预留空间";
        return false;
    }

    header = base;
    header.pop_back();
    header += kPaddingPrefix;
    header.append(kStreamHeaderBytes - minimumSize, ' ');
    header += "\n\n";
    return true;
}

} // namespace

std::string encodeRawCaptureHeader(const RawCaptureFileData& capture) {
    return encodeRawCaptureHeaderWithSize(capture, capture.payload.size());
}

std::optional<RawCaptureFileData> decodeRawCaptureFile(std::string_view bytes, std::string& error) {
    std::size_t cursor = 0;
    auto nextLine = [&](std::string& line) -> bool {
        if (cursor >= bytes.size()) {
            return false;
        }
        const std::size_t lineStart = cursor;
        while (cursor < bytes.size() && bytes[cursor] != '\n') {
            ++cursor;
        }
        std::size_t lineEnd = cursor;
        if (cursor < bytes.size() && bytes[cursor] == '\n') {
            ++cursor;
        }
        if (lineEnd > lineStart && bytes[lineEnd - 1] == '\r') {
            --lineEnd;
        }
        line.assign(bytes.substr(lineStart, lineEnd - lineStart));
        return true;
    };

    std::string line;
    if (!nextLine(line) || trim(line) != kFileMagic) {
        error = "psraw 文件头缺少 ProtoScopeRawCapture 标识";
        return std::nullopt;
    }

    RawCaptureFileData capture;
    bool versionSeen = false;
    bool protocolNameSeen = false;
    bool protocolDirSeen = false;
    bool sampleFrequencySeen = false;
    bool rawSizeSeen = false;
    bool capturedAtSeen = false;
    bool separatorSeen = false;
    std::uint64_t rawSize = 0;

    while (nextLine(line)) {
        if (line.empty()) {
            separatorSeen = true;
            break;
        }
        const auto delimiter = line.find(':');
        if (delimiter == std::string::npos) {
            error = "psraw 文件头格式错误";
            return std::nullopt;
        }

        const auto key = trim(std::string_view(line).substr(0, delimiter));
        const auto value = trim(std::string_view(line).substr(delimiter + 1));
        if (key == "version") {
            versionSeen = true;
            if (value != kVersion) {
                error = "psraw 版本不受支持";
                return std::nullopt;
            }
        } else if (key == "protocol_name") {
            protocolNameSeen = true;
            capture.protocolName = value;
        } else if (key == "protocol_dir") {
            protocolDirSeen = true;
            capture.protocolDir = value;
        } else if (key == "sample_frequency_hz") {
            sampleFrequencySeen = parseDouble(value, capture.sampleFrequencyHz);
        } else if (key == "raw_size") {
            rawSizeSeen = parseUnsigned(value, rawSize);
        } else if (key == "captured_at_ms") {
            capturedAtSeen = parseUnsigned(value, capture.capturedAtMs);
        } else if (key == "truncated") {
            if (!parseBool(value, capture.truncated)) {
                error = "psraw 文件头 truncated 字段格式错误";
                return std::nullopt;
            }
        }
    }

    if (!separatorSeen) {
        error = "psraw 文件头缺少空行分隔";
        return std::nullopt;
    }
    if (!versionSeen || !protocolNameSeen || !protocolDirSeen || !sampleFrequencySeen || !rawSizeSeen || !capturedAtSeen) {
        error = "psraw 文件头缺少必要字段";
        return std::nullopt;
    }
    if (capture.protocolName.empty() || capture.protocolDir.empty()) {
        error = "psraw 文件头缺少协议信息";
        return std::nullopt;
    }

    capture.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end());
    if (capture.payload.size() != rawSize) {
        error = "psraw raw_size 与 payload 长度不一致";
        return std::nullopt;
    }
    return capture;
}

bool writeRawCaptureFile(const std::filesystem::path& path, const RawCaptureFileData& capture, std::string& error) {
    try {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream out(path, std::ios::binary);
        if (!out.good()) {
            error = "无法写入 psraw 文件";
            return false;
        }
        const auto header = encodeRawCaptureHeader(capture);
        out.write(header.data(), static_cast<std::streamsize>(header.size()));
        if (!capture.payload.empty()) {
            out.write(reinterpret_cast<const char*>(capture.payload.data()), static_cast<std::streamsize>(capture.payload.size()));
        }
        if (!out.good()) {
            error = "写入 psraw 文件失败";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        error = std::string("写入 psraw 文件失败: ") + ex.what();
        return false;
    }
}

std::optional<RawCaptureFileData> readRawCaptureFile(const std::filesystem::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        error = "无法读取 psraw 文件";
        return std::nullopt;
    }
    const std::string contents((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decodeRawCaptureFile(contents, error);
}

RawCaptureStreamWriter::~RawCaptureStreamWriter() {
    if (out_.is_open()) {
        std::string error;
        static_cast<void>(close(error));
    }
}

bool RawCaptureStreamWriter::isOpen() const {
    return out_.is_open();
}

const std::filesystem::path& RawCaptureStreamWriter::path() const {
    return path_;
}

std::uint64_t RawCaptureStreamWriter::bytesWritten() const {
    return bytesWritten_;
}

bool RawCaptureStreamWriter::open(const std::filesystem::path& path,
                                  const RawCaptureFileData& metadata,
                                  std::string& error) {
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
    cleanMetadata.truncated = false;

    std::string header;
    if (!encodeFixedRawCaptureHeader(cleanMetadata, 0, header, error)) {
        return false;
    }

    try {
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
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
    return true;
}

bool RawCaptureStreamWriter::append(std::span<const std::uint8_t> bytes, std::string& error) {
    if (!out_.is_open()) {
        error = "完整原始数据录制尚未开始";
        return false;
    }
    if (bytes.empty()) {
        return true;
    }
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<std::uint64_t>::max)() - bytesWritten_)) {
        error = "psraw 录制文件大小超过可记录范围";
        return false;
    }

    out_.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out_.good()) {
        error = "写入 psraw 录制数据失败";
        return false;
    }
    bytesWritten_ += static_cast<std::uint64_t>(bytes.size());
    return true;
}

bool RawCaptureStreamWriter::close(std::string& error) {
    if (!out_.is_open()) {
        return true;
    }

    std::string header;
    if (!encodeFixedRawCaptureHeader(metadata_, bytesWritten_, header, error)) {
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
