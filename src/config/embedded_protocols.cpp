#include "protoscope/config/embedded_protocols.hpp"

#include <cmrc/cmrc.hpp>

#include <array>
#include <cstddef>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

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
    #include <mach-o/dyld.h>
    #include <limits.h>
#else
    #include <limits.h>
    #include <unistd.h>
#endif

CMRC_DECLARE(proto_resources);

namespace protoscope::config::embedded {

namespace fs = std::filesystem;

namespace {

bool writeFileFromResource(const cmrc::file& resourceFile,
                           const fs::path& outputPath,
                           std::string& error) {
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

bool extractOneResource(const char* resourcePath,
                        const fs::path& outputPath,
                        std::string& error) {
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
} // namespace

bool extractResourceToFile(
    const char* resourcePath,
    const fs::path& outputPath,
    std::string& error
) {
    return extractOneResource(resourcePath, outputPath, error);
}

fs::path executableDirectory() {
#if defined(_WIN32)

    std::wstring buffer;
    buffer.resize(MAX_PATH);

    while (true) {
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size())
        );

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

bool ensureProtocolWorkspace(const fs::path& rootDir, std::string& error) {
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

        // 核心策略：只要 protocols 根目录存在，就认为用户已经有工作区。
        // 不覆盖，不补齐，避免误改用户脚本。
        return true;
    }

    fs::create_directories(rootDir, ec);
    if (ec) {
        error = "创建协议目录失败: " + rootDir.string() + ", " + ec.message();
        return false;
    }

    for (const auto& entry : kProtocolResources) {
        const auto outputPath = rootDir / fs::path(entry.output_path);

        if (!extractOneResource(entry.resource_path, outputPath, error)) {
            return false;
        }
    }

    return true;
}

bool ensureDefaultProtocolScript(const fs::path& protocolDir, std::string& error) {
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

    return extractOneResource(
        "protocols/default_protocol/main.lua",
        mainLuaPath,
        error
    );
}

} // namespace protoscope::config::embedded