#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>

namespace protoscope::storage {
struct RecordSessionState {
    std::uint64_t session{0},run{0},abnormalRuns{0};
    bool recording{false},cleanExit{true},faulted{false};
    std::string activeIdentity,pendingIdentity,error;
    std::int64_t pendingFirstId{0},pendingOpenedUs{0},lastCommittedId{0};
    std::optional<std::int64_t> lastCommittedTimeUs,interruptedFromUs,interruptedToUs;
};

// 调用者持有目录单写者资格。切换意图持久化先于文件准备，提交指针先于新卷发布。
class RecordSession {
public:
    RecordSession(std::filesystem::path recordsRoot,std::string protocol);
    RecordSessionState state() const;
    void beginRun();
    void cleanExit();
    void start();
    void stop();
    void prepareRotation(std::string identity,std::int64_t firstId,std::int64_t openedAtUs);
    void commitRotation(const std::string& identity);
    void cancelRotation(const std::string& identity);
    void committed(std::int64_t id,std::int64_t receivedAtUs);
    void fault(std::string error,std::optional<std::int64_t> fromUs={},std::optional<std::int64_t> toUs={});
private:
    void save(RecordSessionState state);
    std::filesystem::path index_;
    std::string protocol_;
    mutable std::mutex mutex_;
    RecordSessionState state_;
};
} // namespace protoscope::storage
