#pragma once

#include "protoscope/config/config.hpp"
#include <vector>

namespace protoscope::config {

enum class SettingsPage { General, Wave, Logging, Performance };

struct SettingsMergeResult {
    AppConfig config{};
    std::vector<std::string> changedFields;
    std::vector<std::string> conflicts;
    std::string error;
};

// 仅本任务的四页白名单；不是通用属性系统，不比较通讯/协议/视口/游标。
SettingsMergeResult mergeSettingsChanges(const AppConfig& baseline, const AppConfig& draft,
                                        const AppConfig& latest, bool overwriteConflicts = false);
// 设置严格拒绝溢出；YAML 兼容路径可钳制派生预算，数学基线与显式覆盖规则相同。
bool recomputePerformanceDefaults(AppConfig& config, std::string& error, bool clampOverflow = false);

struct SettingsDraft {
    explicit SettingsDraft(const AppConfig& current) : baseline(current), value(current) {}
    AppConfig baseline;
    AppConfig value;
    void refresh(const AppConfig& current) { baseline = current; value = current; }
    void restoreDefaults(SettingsPage page);
    void followUnedited(const AppConfig& current);
    [[nodiscard]] bool hasChanges() const;
};

// 宿主负责成功后 clearDirty；失败永不修改运行态或草稿。
class SettingsPersistence {
public:
    SettingsPersistence(const ConfigStore& store, std::filesystem::path path);
    bool save(const AppConfig& current, std::string& error);
    bool acceptExternalSnapshot(std::string& error);
    bool hasExternalChange() const;
    const FileSnapshot& savedSnapshot() const { return saved_; }
private:
    const ConfigStore& store_;
    std::filesystem::path path_;
    FileSnapshot saved_;
};

} // namespace protoscope::config
