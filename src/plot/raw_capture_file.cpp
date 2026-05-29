#include "protoscope/plot/raw_capture_file.hpp"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>

namespace protoscope::plot {
namespace {

constexpr std::string_view kFileMagic = "ProtoScopeRawCapture";
constexpr std::string_view kVersion = "1";

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

} // namespace

std::string encodeRawCaptureHeader(const RawCaptureFileData& capture) {
    std::ostringstream out;
    out << kFileMagic << '\n'
        << "version: " << kVersion << '\n'
        << "protocol_name: " << capture.protocolName << '\n'
        << "protocol_dir: " << capture.protocolDir << '\n'
        << "sample_frequency_hz: " << capture.sampleFrequencyHz << '\n'
        << "raw_size: " << capture.payload.size() << '\n'
        << "captured_at_ms: " << capture.capturedAtMs << '\n'
        << '\n';
    return out.str();
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

} // namespace protoscope::plot
