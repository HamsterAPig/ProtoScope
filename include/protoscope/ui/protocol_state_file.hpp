#pragma once

#include <yaml-cpp/yaml.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace protoscope::ui {

struct ProtocolStateFileOptions {
    std::chrono::milliseconds lockWaitTimeout{std::chrono::milliseconds(2000)};
};

struct ProtocolStateFileRecovery {
    bool recoveredCorruptFile{false};
    std::filesystem::path backupPath;
    std::string parseError;
};

struct ProtocolStateLoadResult {
    YAML::Node root;
    ProtocolStateFileRecovery recovery;
    bool ok{true};
    std::string error;
};

class ProtocolStateFileLock {
public:
    ProtocolStateFileLock();
    ~ProtocolStateFileLock();

    ProtocolStateFileLock(ProtocolStateFileLock&& other) noexcept;
    ProtocolStateFileLock& operator=(ProtocolStateFileLock&& other) noexcept;

    ProtocolStateFileLock(const ProtocolStateFileLock&) = delete;
    ProtocolStateFileLock& operator=(const ProtocolStateFileLock&) = delete;

    explicit operator bool() const noexcept;

    static std::optional<ProtocolStateFileLock> acquire(const std::filesystem::path& statePath,
                                                        std::chrono::milliseconds timeout,
                                                        std::string& error);

private:
    struct Impl;

    explicit ProtocolStateFileLock(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

ProtocolStateLoadResult loadProtocolStateRootForUpdate(const std::filesystem::path& statePath);
bool writeProtocolStateRootAtomically(const std::filesystem::path& statePath,
                                      const YAML::Node& root,
                                      std::string& error);

} // namespace protoscope::ui
