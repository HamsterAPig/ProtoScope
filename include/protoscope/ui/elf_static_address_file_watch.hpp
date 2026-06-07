#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace protoscope::ui {

struct ElfStaticAddressFileWatchState {
    std::filesystem::path path;
    bool watching{false};
    bool lastExists{false};
    std::uint64_t lastWriteTimeNs{0};
    std::uintmax_t fileSize{0};
    bool pendingReload{false};
    std::uint64_t pendingReloadSinceMs{0};
    std::uint64_t lastPollAtMs{0};
    std::string pendingStatusMessage;
};

struct ElfStaticAddressFilePollResult {
    bool changed{false};
    bool shouldReload{false};
    bool clearComboCache{false};
    std::string statusMessage;
};

inline ElfStaticAddressFilePollResult pollElfStaticAddressFileWatchState(ElfStaticAddressFileWatchState& state,
                                                                         std::uint64_t currentMs,
                                                                         std::error_code& error)
{
    constexpr std::uint64_t kElfFilePollIntervalMs = 500;
    constexpr std::uint64_t kElfFileReloadStableMs = 1000;

    auto toFileTimeNs = [](const std::filesystem::file_time_type& value) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(value.time_since_epoch()).count());
    };

    ElfStaticAddressFilePollResult result;
    error.clear();
    if (!state.watching || state.path.empty()) {
        return result;
    }
    if (state.lastPollAtMs != 0 && currentMs - state.lastPollAtMs < kElfFilePollIntervalMs) {
        return result;
    }
    state.lastPollAtMs = currentMs;

    const bool exists = std::filesystem::exists(state.path, error);
    if (error) {
        result.changed = true;
        result.statusMessage = "检测 ELF 数据文件状态失败: " + error.message();
        return result;
    }

    if (!exists) {
        if (state.lastExists) {
            state.lastExists = false;
            state.pendingReload = true;
            state.pendingReloadSinceMs = 0;
            state.pendingStatusMessage = "ELF 数据文件已删除，继续使用旧模型";
            result.changed = true;
            result.statusMessage = state.pendingStatusMessage;
        }
        return result;
    }

    const auto lastWriteTime = std::filesystem::last_write_time(state.path, error);
    if (error) {
        result.changed = true;
        result.statusMessage = "读取 ELF 数据文件时间戳失败: " + error.message();
        return result;
    }

    const auto fileSize = std::filesystem::file_size(state.path, error);
    if (error) {
        result.changed = true;
        result.statusMessage = "读取 ELF 数据文件大小失败: " + error.message();
        return result;
    }

    const auto lastWriteTimeNs = toFileTimeNs(lastWriteTime);
    const bool metadataChanged =
        !state.lastExists || state.lastWriteTimeNs != lastWriteTimeNs || state.fileSize != fileSize;

    if (!state.lastExists) {
        state.lastExists = true;
        state.lastWriteTimeNs = lastWriteTimeNs;
        state.fileSize = fileSize;
        if (state.pendingReload) {
            state.pendingReloadSinceMs = currentMs;
            state.pendingStatusMessage = "检测到 ELF 数据文件重建，等待写入稳定后自动重载";
            result.changed = true;
            result.statusMessage = state.pendingStatusMessage;
        }
        return result;
    }

    if (metadataChanged) {
        state.lastWriteTimeNs = lastWriteTimeNs;
        state.fileSize = fileSize;
        result.changed = true;
        if (state.pendingReload) {
            state.pendingReloadSinceMs = currentMs;
            state.pendingStatusMessage = "检测到 ELF 数据文件重建，等待写入稳定后自动重载";
            result.statusMessage = state.pendingStatusMessage;
        } else {
            result.statusMessage = "检测到 ELF 数据文件变更，请按需手动重载";
        }
        return result;
    }

    if (state.pendingReload && state.pendingReloadSinceMs != 0 &&
        currentMs - state.pendingReloadSinceMs >= kElfFileReloadStableMs) {
        state.pendingReload = false;
        state.pendingReloadSinceMs = 0;
        state.pendingStatusMessage.clear();
        result.changed = true;
        result.shouldReload = true;
        result.clearComboCache = true;
        result.statusMessage = "ELF 数据文件重建稳定，正在自动重载";
    }
    return result;
}

} // namespace protoscope::ui
