#pragma once

#include "protoscope/dock/docks.hpp"

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>

namespace protoscope::config {

struct AppAutoSaveConfig {
    bool enabled{false};
    std::uint64_t intervalMs{5000};
};

struct AppConfigHotReloadConfig {
    bool enabled{false};
};

enum class LogLevel {
    Debug,
    Info,
    Warn,
    Error,
};

struct AppLoggingConfig {
    LogLevel level{LogLevel::Info};
    std::string filePath;
};

struct AppRuntimeConfig {
    std::string language{"zh-CN"};
    std::uint32_t fpsLimit{60};
    std::string idleRender{"dirty_only"};
    AppAutoSaveConfig autoSave{};
    AppConfigHotReloadConfig configHotReload{};
};

struct GuiWindowConfig {
    std::string title{"ProtoScope"};
    int width{1600};
    int height{900};
    bool maximized{false};
};

struct GuiWaveConfig {
    std::size_t maxRenderPointsPerChannel{1200};
    std::size_t maxRenderVertices{60000};
    std::size_t overviewMaxSamples{20000};
    double minVisibleTimeSpan{0.001};
};

struct GuiConfig {
    GuiWindowConfig window{};
    GuiWaveConfig wave{};
    bool luaDockLayoutDebug{false};
};

struct ProtocolConfig {
    std::string rootDir{"protocols"};
    std::string selectedDir{"protocols/default_protocol"};
};

struct AppConfig {
    AppRuntimeConfig app{};
    GuiConfig gui{};
    ProtocolConfig protocol{};
    AppLoggingConfig logging{};
    dock::CommDockState communication{};
    std::string configPath{"config/protoscope.yaml"};
};

struct ConfigLoadResult {
    AppConfig config{};
    std::filesystem::path resolvedPath;
    bool loadedFromDisk{false};
    std::string error;
};

struct FileSnapshot {
    std::filesystem::path path;
    std::uint64_t timestampMs{0};
    bool exists{false};
};

class ConfigStore {
public:
    ConfigStore();

    ConfigLoadResult load(const std::filesystem::path& path) const;
    bool save(const std::filesystem::path& path, const AppConfig& config, std::string& error) const;

    std::filesystem::path normalizeProtocolDir(const std::filesystem::path& dir) const;
    std::filesystem::path normalizeProtocolDir(const std::filesystem::path& rootDir, const std::filesystem::path& dir) const;
    std::filesystem::path mainLuaPath(const std::filesystem::path& protocolDir) const;
    std::string protocolName(const std::filesystem::path& protocolDir) const;
    bool protocolEntryExists(const std::filesystem::path& protocolDir) const;
    std::vector<std::string> scanProtocolDirectories(const std::filesystem::path& rootDir) const;
    std::filesystem::path defaultScriptWorkspaceDir() const;
    std::filesystem::path defaultScriptHelpPath() const;
    bool ensureDefaultProtocolScript(const std::filesystem::path& protocolDir, std::string& error) const;
    bool ensureDefaultProtocolWorkspace(std::string& error) const;
    bool ensureDefaultScriptWorkspace(std::string& error) const;

    FileSnapshot snapshot(const std::filesystem::path& path) const;
    bool hasChanged(const FileSnapshot& previous) const;

    void applyToDock(const AppConfig& config, dock::DockStore& dockStore) const;
    AppConfig captureFromDock(const dock::DockStore& dockStore) const;

    const std::filesystem::path& defaultConfigPath() const;
    const std::filesystem::path& defaultProtocolDir() const;

private:
    AppConfig withDefaults() const;
    static std::uint64_t toTimestampMs(const std::filesystem::file_time_type& fileTime);

private:
    std::filesystem::path defaultConfigPath_;
    std::filesystem::path defaultProtocolDir_;
};

} // namespace protoscope::config
