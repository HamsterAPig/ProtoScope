#include "protoscope/config/embedded_protocols.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include <cmrc/cmrc.hpp>
#include <protoscope/config/embedded_protocols_manifest.hpp>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <limits.h>

#include <mach-o/dyld.h>
#else
#include <limits.h>
#include <unistd.h>
#endif

CMRC_DECLARE(proto_resources);

namespace protoscope::config::embedded {

namespace fs = std::filesystem;

namespace {

    bool writeFileFromResource(const cmrc::file& resourceFile, const fs::path& outputPath, std::string& error)
    {
        std::error_code ec;

        const auto parent = outputPath.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) {
                error = "创建目录失败: " + parent.string() + ", " + ec.message();
                return false;
            }
        }

        std::ofstream out(outputPath, std::ios::binary);
        if (!out.good()) {
            error = "打开文件失败: " + outputPath.string();
            return false;
        }

        out.write(resourceFile.begin(), static_cast<std::streamsize>(resourceFile.size()));
        if (!out.good()) {
            error = "写入文件失败: " + outputPath.string();
            return false;
        }

        return true;
    }

    std::optional<bool> fileMatchesResource(const fs::path& outputPath,
                                            const cmrc::file& resourceFile,
                                            std::string& error)
    {
        std::ifstream in(outputPath, std::ios::binary);
        if (!in.good()) {
            error = "打开文件失败: " + outputPath.string();
            return std::nullopt;
        }

        in.seekg(0, std::ios::end);
        const auto fileSize = in.tellg();
        if (fileSize < 0) {
            error = "读取文件大小失败: " + outputPath.string();
            return std::nullopt;
        }
        if (static_cast<std::uintmax_t>(fileSize) != static_cast<std::uintmax_t>(resourceFile.size())) {
            return false;
        }

        in.seekg(0, std::ios::beg);
        std::vector<char> buffer(resourceFile.size());
        if (!buffer.empty()) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (!in.good()) {
                error = "读取文件失败: " + outputPath.string();
                return std::nullopt;
            }
        }

        return std::equal(buffer.begin(), buffer.end(), resourceFile.begin(), resourceFile.end());
    }

    bool extractOneResource(const char* resourcePath, const fs::path& outputPath, std::string& error)
    {
        auto resourceFs = cmrc::proto_resources::get_filesystem();

        try {
            const auto resourceFile = resourceFs.open(resourcePath);
            return writeFileFromResource(resourceFile, outputPath, error);
        } catch (const std::exception& ex) {
            error = "读取内嵌资源失败: ";
            error += resourcePath;
            error += ", ";
            error += ex.what();
            return false;
        }
    }

    bool extractResourceIfChanged(const char* resourcePath, const fs::path& outputPath, std::string& error)
    {
        auto resourceFs = cmrc::proto_resources::get_filesystem();

        try {
            const auto resourceFile = resourceFs.open(resourcePath);
            std::error_code ec;
            const bool outputExists = fs::exists(outputPath, ec);
            if (ec) {
                error = "检查文件失败: " + outputPath.string() + ", " + ec.message();
                return false;
            }
            if (outputExists) {
                const auto matches = fileMatchesResource(outputPath, resourceFile, error);
                if (!matches.has_value()) {
                    return false;
                }
                if (*matches) {
                    return true;
                }
            }
            return writeFileFromResource(resourceFile, outputPath, error);
        } catch (const std::exception& ex) {
            error = "读取内嵌协议资源失败: " + std::string(resourcePath) + ", " + ex.what();
            return false;
        }
    }
} // namespace

bool extractResourceToFile(const char* resourcePath, const fs::path& outputPath, std::string& error)
{
    return extractOneResource(resourcePath, outputPath, error);
}

fs::path executableDirectory()
{
#if defined(_WIN32)

    std::wstring buffer;
    buffer.resize(MAX_PATH);

    while (true) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));

        if (length == 0) {
            return fs::current_path();
        }

        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }

        buffer.resize(buffer.size() * 2);
    }

#elif defined(__APPLE__)

    std::vector<char> buffer(PATH_MAX);
    uint32_t size = static_cast<uint32_t>(buffer.size());

    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        buffer.resize(size);
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return fs::current_path();
        }
    }

    std::error_code ec;
    const auto canonicalPath = fs::weakly_canonical(fs::path(buffer.data()), ec);
    if (ec) {
        return fs::path(buffer.data()).parent_path();
    }

    return canonicalPath.parent_path();

#else

    std::array<char, PATH_MAX> buffer{};
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);

    if (length <= 0) {
        return fs::current_path();
    }

    buffer[static_cast<std::size_t>(length)] = '\0';

    std::error_code ec;
    const auto canonicalPath = fs::weakly_canonical(fs::path(buffer.data()), ec);
    if (ec) {
        return fs::path(buffer.data()).parent_path();
    }

    return canonicalPath.parent_path();

#endif
}

bool ensureProtocolWorkspace(const fs::path& rootDir, std::string& error)
{
    std::error_code ec;

    if (fs::exists(rootDir, ec)) {
        if (ec) {
            error = "检查协议目录失败: " + rootDir.string() + ", " + ec.message();
            return false;
        }

        if (!fs::is_directory(rootDir, ec)) {
            error = "protocols 路径已存在，但不是目录: " + rootDir.string();
            return false;
        }

        // 核心策略：允许已有 protocols 根目录存在，但只补齐缺失的内置资源。
        // 这样升级后能拿到 templates 和注解文件，同时不覆盖用户改过的脚本。
    } else {
        fs::create_directories(rootDir, ec);
        if (ec) {
            error = "创建协议目录失败: " + rootDir.string() + ", " + ec.message();
            return false;
        }
    }

    for (const auto& entry : kProtocolResources) {
        const auto outputPath = rootDir / fs::path(entry.output_path);
        // 核心流程：内嵌模板升级后要刷新旧副本，否则测试/运行时会继续加载过期 Lua 布局。
        if (!extractResourceIfChanged(entry.resource_path, outputPath, error)) {
            return false;
        }
    }

    return true;
}

bool ensureDefaultProtocolScript(const fs::path& protocolDir, std::string& error)
{
    std::error_code ec;

    fs::create_directories(protocolDir, ec);
    if (ec) {
        error = "创建默认协议目录失败: " + protocolDir.string() + ", " + ec.message();
        return false;
    }

    const auto mainLuaPath = protocolDir / "main.lua";

    if (fs::exists(mainLuaPath, ec)) {
        return true;
    }

    return extractOneResource("protocols/default_protocol/main.lua", mainLuaPath, error);
}

} // namespace protoscope::config::embedded
