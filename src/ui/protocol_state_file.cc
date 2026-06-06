#include "protoscope/ui/protocol_state_file.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>

#include <sys/types.h>
#endif

namespace protoscope::ui {
namespace {

    std::uint64_t fnv1a64(std::string_view value)
    {
        std::uint64_t hash = 14695981039346656037ull;
        for (const unsigned char ch : value) {
            hash ^= ch;
            hash *= 1099511628211ull;
        }
        return hash;
    }

    std::string normalizedPathKey(const std::filesystem::path& path)
    {
        std::error_code ec;
        auto absolutePath = std::filesystem::absolute(path, ec);
        if (ec) {
            absolutePath = path;
        }
        return absolutePath.lexically_normal().generic_string();
    }

    std::uint32_t currentProcessId()
    {
#if defined(_WIN32)
        return static_cast<std::uint32_t>(GetCurrentProcessId());
#else
        return static_cast<std::uint32_t>(getpid());
#endif
    }

    std::string timestampForFileName()
    {
        const auto now = std::chrono::system_clock::now();
        const auto timeValue = std::chrono::system_clock::to_time_t(now);
        std::tm localTime{};
#if defined(_WIN32)
        localtime_s(&localTime, &timeValue);
#else
        localtime_r(&timeValue, &localTime);
#endif
        std::ostringstream out;
        out << std::put_time(&localTime, "%Y%m%d-%H%M%S");
        return out.str();
    }

    std::filesystem::path corruptBackupPath(const std::filesystem::path& statePath)
    {
        const auto parent = statePath.parent_path();
        const auto baseName = statePath.filename().string() + ".corrupt-" + timestampForFileName() + "-" +
                              std::to_string(currentProcessId()) + ".bak";
        auto candidate = parent / baseName;
        for (int index = 1; std::filesystem::exists(candidate); ++index) {
            candidate = parent / (baseName + "." + std::to_string(index));
        }
        return candidate;
    }

    YAML::Node emptyMapNode()
    {
        return YAML::Node(YAML::NodeType::Map);
    }

    std::string readFileText(const std::filesystem::path& path)
    {
        std::ifstream input(path, std::ios::binary);
        std::ostringstream buffer;
        buffer << input.rdbuf();
        return buffer.str();
    }

    std::filesystem::path temporaryStatePath(const std::filesystem::path& statePath)
    {
        static std::atomic_uint counter{0};
        const auto suffix = ".tmp." + std::to_string(currentProcessId()) + "." + std::to_string(++counter);
        return statePath.parent_path() / (statePath.filename().string() + suffix);
    }

    bool ensureParentDirectory(const std::filesystem::path& path, std::string& error)
    {
        const auto parent = path.parent_path();
        if (parent.empty()) {
            return true;
        }
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (!ec) {
            return true;
        }
        error = "无法创建目录: " + parent.generic_string() + " (" + ec.message() + ")";
        return false;
    }

    bool replaceFile(const std::filesystem::path& from, const std::filesystem::path& to, std::string& error)
    {
#if defined(_WIN32)
        if (MoveFileExW(from.wstring().c_str(),
                        to.wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            return true;
        }
        error = "替换状态文件失败: Windows error " + std::to_string(GetLastError());
        return false;
#else
        std::error_code ec;
        std::filesystem::rename(from, to, ec);
        if (!ec) {
            return true;
        }
        error = "替换状态文件失败: " + ec.message();
        return false;
#endif
    }

#if defined(_WIN32)
    std::wstring mutexNameForPath(const std::filesystem::path& statePath)
    {
        const auto hash = fnv1a64(normalizedPathKey(statePath));
        std::wostringstream out;
        out << L"Local\\ProtoScope.ProtocolState." << std::hex << hash;
        return out.str();
    }
#endif

} // namespace

struct ProtocolStateFileLock::Impl {
#if defined(_WIN32)
    HANDLE handle{nullptr};
    bool owns{false};
#else
    int fd{-1};
#endif
};

ProtocolStateFileLock::ProtocolStateFileLock() = default;

ProtocolStateFileLock::ProtocolStateFileLock(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

ProtocolStateFileLock::~ProtocolStateFileLock()
{
    if (!impl_) {
        return;
    }
#if defined(_WIN32)
    if (impl_->handle != nullptr) {
        if (impl_->owns) {
            ReleaseMutex(impl_->handle);
        }
        CloseHandle(impl_->handle);
    }
#else
    if (impl_->fd >= 0) {
        flock lock{};
        lock.l_type = F_UNLCK;
        lock.l_whence = SEEK_SET;
        (void) fcntl(impl_->fd, F_SETLK, &lock);
        close(impl_->fd);
    }
#endif
}

ProtocolStateFileLock::ProtocolStateFileLock(ProtocolStateFileLock&& other) noexcept = default;

ProtocolStateFileLock& ProtocolStateFileLock::operator=(ProtocolStateFileLock&& other) noexcept = default;

ProtocolStateFileLock::operator bool() const noexcept
{
    return static_cast<bool>(impl_);
}

std::optional<ProtocolStateFileLock> ProtocolStateFileLock::acquire(const std::filesystem::path& statePath,
                                                                    std::chrono::milliseconds timeout,
                                                                    std::string& error)
{
    error.clear();
#if defined(_WIN32)
    const auto name = mutexNameForPath(statePath);
    HANDLE handle = CreateMutexW(nullptr, FALSE, name.c_str());
    if (handle == nullptr) {
        error = "创建状态文件互斥锁失败: Windows error " + std::to_string(GetLastError());
        return std::nullopt;
    }
    const DWORD waitMs = timeout.count() < 0 ? INFINITE : static_cast<DWORD>(timeout.count());
    const DWORD result = WaitForSingleObject(handle, waitMs);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        auto impl = std::make_unique<Impl>();
        impl->handle = handle;
        impl->owns = true;
        return ProtocolStateFileLock(std::move(impl));
    }
    CloseHandle(handle);
    error = result == WAIT_TIMEOUT ? "等待状态文件互斥锁超时"
                                   : "等待状态文件互斥锁失败: Windows error " + std::to_string(GetLastError());
    return std::nullopt;
#else
    std::string parentError;
    if (!ensureParentDirectory(statePath, parentError)) {
        error = parentError;
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto lockPath = statePath.parent_path() / (statePath.filename().string() + ".lock");
    const int fd = open(lockPath.c_str(), O_RDWR | O_CREAT, 0666);
    if (fd < 0) {
        error = "打开状态文件锁失败";
        return std::nullopt;
    }
    while (true) {
        flock lock{};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        if (fcntl(fd, F_SETLK, &lock) == 0) {
            auto impl = std::make_unique<Impl>();
            impl->fd = fd;
            return ProtocolStateFileLock(std::move(impl));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            close(fd);
            error = "等待状态文件锁超时";
            return std::nullopt;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
#endif
}

ProtocolStateLoadResult loadProtocolStateRootForUpdate(const std::filesystem::path& statePath)
{
    ProtocolStateLoadResult result;
    result.root = emptyMapNode();
    if (!std::filesystem::exists(statePath)) {
        return result;
    }

    try {
        // 核心流程：先把状态文件读入内存并关闭句柄，再解析 YAML；解析失败后才能在 Windows 上稳定移走坏文件。
        result.root = YAML::Load(readFileText(statePath));
        if (!result.root || !result.root.IsMap()) {
            result.root = emptyMapNode();
        }
        return result;
    } catch (const std::exception& ex) {
        result.recovery.parseError = ex.what();
    }

    std::string directoryError;
    if (!ensureParentDirectory(statePath, directoryError)) {
        result.ok = false;
        result.error = directoryError;
        return result;
    }

    const auto backupPath = corruptBackupPath(statePath);
    std::error_code ec;
    std::filesystem::rename(statePath, backupPath, ec);
    if (ec) {
        result.ok = false;
        result.error = "备份损坏状态文件失败: " + ec.message();
        return result;
    }

    // 核心流程：损坏 YAML 先移走再重建，避免下一次启动继续读取同一个坏文件。
    result.recovery.recoveredCorruptFile = true;
    result.recovery.backupPath = backupPath;
    result.root = emptyMapNode();
    return result;
}

bool writeProtocolStateRootAtomically(const std::filesystem::path& statePath,
                                      const YAML::Node& root,
                                      std::string& error)
{
    error.clear();
    if (!ensureParentDirectory(statePath, error)) {
        return false;
    }

    const auto tempPath = temporaryStatePath(statePath);
    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out.good()) {
            error = "无法写入临时状态文件: " + tempPath.generic_string();
            return false;
        }
        out << root;
        out.flush();
        if (!out.good()) {
            error = "写入临时状态文件失败: " + tempPath.generic_string();
            std::error_code removeError;
            std::filesystem::remove(tempPath, removeError);
            return false;
        }
    }

    if (replaceFile(tempPath, statePath, error)) {
        return true;
    }

    std::error_code removeError;
    std::filesystem::remove(tempPath, removeError);
    return false;
}

} // namespace protoscope::ui
