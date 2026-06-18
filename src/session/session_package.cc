#include "protoscope/session/session_package.hpp"

#include <charconv>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>

namespace protoscope::session {
namespace {

    constexpr std::string_view kMagic = "ProtoScopeSessionPackage";
    constexpr std::string_view kVersion = "1";
    constexpr std::size_t kMaxSessionPackageEntries = 10000;

    bool parseUnsigned(std::string_view text, std::uint64_t& value)
    {
        const auto* begin = text.data();
        const auto* end = text.data() + text.size();
        const auto [ptr, ec] = std::from_chars(begin, end, value);
        return ec == std::errc{} && ptr == end;
    }

    bool parseSize(std::string_view text, std::size_t& value)
    {
        std::uint64_t parsed = 0;
        if (!parseUnsigned(text, parsed)) {
            return false;
        }
        value = static_cast<std::size_t>(parsed);
        return static_cast<std::uint64_t>(value) == parsed;
    }

    bool consumeLine(std::string_view bytes, std::size_t& cursor, std::string_view& line)
    {
        if (cursor > bytes.size()) {
            return false;
        }
        const auto next = bytes.find('\n', cursor);
        if (next == std::string_view::npos) {
            return false;
        }
        line = bytes.substr(cursor, next - cursor);
        cursor = next + 1;
        return true;
    }

    bool consumePrefixedLine(std::string_view bytes,
                             std::size_t& cursor,
                             std::string_view prefix,
                             std::string_view& value,
                             std::string& error)
    {
        std::string_view line;
        if (!consumeLine(bytes, cursor, line)) {
            error = "会话包头部不完整";
            return false;
        }
        if (!line.starts_with(prefix)) {
            error = "会话包字段缺失: " + std::string(prefix);
            return false;
        }
        value = line.substr(prefix.size());
        return true;
    }

    std::vector<std::uint8_t> toBytes(std::string_view text)
    {
        return {text.begin(), text.end()};
    }

} // namespace

std::string encodeSessionPackage(const SessionPackageData& package)
{
    std::ostringstream out;
    out << kMagic << '\n';
    out << "version: " << kVersion << '\n';
    out << "created_at_ms: " << package.createdAtMs << '\n';
    out << "entries: " << package.entries.size() << '\n';

    // 核心流程：条目内容按字节长度读取，允许嵌入 YAML、Lua、psraw 等任意文本或二进制数据。
    for (const auto& entry : package.entries) {
        out << "entry: " << entry.name << '\n';
        out << "size: " << entry.bytes.size() << '\n';
        out << '\n';
        out.write(reinterpret_cast<const char*>(entry.bytes.data()), static_cast<std::streamsize>(entry.bytes.size()));
        out << "\nendentry\n";
    }
    return out.str();
}

std::optional<SessionPackageData> decodeSessionPackage(std::string_view bytes, std::string& error)
{
    std::size_t cursor = 0;
    std::string_view line;
    if (!consumeLine(bytes, cursor, line) || line != kMagic) {
        error = "不是 ProtoScope 会话包";
        return std::nullopt;
    }

    std::string_view value;
    if (!consumePrefixedLine(bytes, cursor, "version: ", value, error)) {
        return std::nullopt;
    }
    if (value != kVersion) {
        error = "不支持的会话包版本: " + std::string(value);
        return std::nullopt;
    }

    SessionPackageData package;
    if (!consumePrefixedLine(bytes, cursor, "created_at_ms: ", value, error) ||
        !parseUnsigned(value, package.createdAtMs)) {
        error = "会话包 created_at_ms 无效";
        return std::nullopt;
    }

    std::size_t entryCount = 0;
    if (!consumePrefixedLine(bytes, cursor, "entries: ", value, error) || !parseSize(value, entryCount)) {
        error = "会话包 entries 无效";
        return std::nullopt;
    }
    if (entryCount > kMaxSessionPackageEntries || entryCount > bytes.size()) {
        error = "会话包 entries 超出限制";
        return std::nullopt;
    }

    package.entries.reserve(entryCount);
    for (std::size_t index = 0; index < entryCount; ++index) {
        if (!consumePrefixedLine(bytes, cursor, "entry: ", value, error)) {
            return std::nullopt;
        }
        SessionPackageEntry entry;
        entry.name = std::string(value);
        if (entry.name.empty()) {
            error = "会话包条目名称不能为空";
            return std::nullopt;
        }

        std::size_t entrySize = 0;
        if (!consumePrefixedLine(bytes, cursor, "size: ", value, error) || !parseSize(value, entrySize)) {
            error = "会话包条目大小无效: " + entry.name;
            return std::nullopt;
        }
        if (!consumeLine(bytes, cursor, line) || !line.empty()) {
            error = "会话包条目头部缺少空行: " + entry.name;
            return std::nullopt;
        }
        if (entrySize > bytes.size() - cursor) {
            error = "会话包条目内容被截断: " + entry.name;
            return std::nullopt;
        }
        const auto payload = bytes.substr(cursor, entrySize);
        entry.bytes = toBytes(payload);
        cursor += entrySize;
        if (!consumeLine(bytes, cursor, line) || !line.empty()) {
            error = "会话包条目结束换行缺失: " + entry.name;
            return std::nullopt;
        }
        if (!consumeLine(bytes, cursor, line) || line != "endentry") {
            error = "会话包条目结束标记缺失: " + entry.name;
            return std::nullopt;
        }
        package.entries.push_back(std::move(entry));
    }

    if (cursor != bytes.size()) {
        error = "会话包末尾存在未知数据";
        return std::nullopt;
    }
    return package;
}

bool writeSessionPackage(const std::filesystem::path& path, const SessionPackageData& package, std::string& error)
{
    std::error_code directoryError;
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, directoryError);
        if (directoryError) {
            error = "创建会话包目录失败: " + directoryError.message();
            return false;
        }
    }

    std::ofstream out(path, std::ios::binary);
    if (!out.good()) {
        error = "无法写入会话包";
        return false;
    }
    const auto encoded = encodeSessionPackage(package);
    out.write(encoded.data(), static_cast<std::streamsize>(encoded.size()));
    if (!out.good()) {
        error = "写入会话包失败";
        return false;
    }
    return true;
}

std::optional<SessionPackageData> readSessionPackage(const std::filesystem::path& path, std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in.good()) {
        error = "无法读取会话包";
        return std::nullopt;
    }
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decodeSessionPackage(bytes, error);
}

const SessionPackageEntry* findSessionPackageEntry(const SessionPackageData& package, std::string_view name)
{
    for (const auto& entry : package.entries) {
        if (entry.name == name) {
            return &entry;
        }
    }
    return nullptr;
}

} // namespace protoscope::session
