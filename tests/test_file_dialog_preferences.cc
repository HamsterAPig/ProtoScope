#include "protoscope/config/config.hpp"
#include "protoscope/ui/file_dialog_paths.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
}

int main()
{
    using namespace protoscope;
    try {
        config::ConfigStore store;
        const auto legacy = store.loadText("gui:\n  last_data_export:\n    directory: old-export\n");
        require(legacy.config.gui.fileDialogs.lastExportDirectory == "old-export", "legacy migration without wave");
        const auto explicitEmpty = store.loadText(
            "gui:\n  last_data_export:\n    directory: old\n  file_dialogs:\n    last_export_directory: ''\n");
        require(explicitEmpty.config.gui.fileDialogs.lastExportDirectory.empty(), "explicit empty overrides legacy");
        const auto root = std::filesystem::temp_directory_path() /
            ("protoscope-dialog-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto unicodeDir = root / std::filesystem::u8path("中文 空格");
        std::filesystem::create_directories(unicodeDir);
        const auto path = root / "config.yaml";
        { std::ofstream out(path); out << "app:\n  fps_limit: 37\ncustom: keep\n"; }
        config::GuiFileDialogConfig preferences{ui::fileDialogPathText(unicodeDir), "export space"};
        preferences.directories = {{"elf.open", ui::fileDialogPathText(unicodeDir)},
                                   {"recording", "record space"}, {"log.host", "host space"},
                                   {"lua:protocol:open:title", "lua space"}};
        std::string error;
        require(store.saveFileDialogPreferences(path, preferences, error), error.c_str());
        const auto loaded = store.load(path);
        require(loaded.loadedFromDisk, "reload");
        require(loaded.config.gui.fileDialogs.lastImportDirectory == preferences.lastImportDirectory, "unicode reload");
        require(loaded.config.gui.fileDialogs.lastExportDirectory == "export space", "separate export");
        require(loaded.config.gui.fileDialogs.directories == preferences.directories, "用途目录重启恢复");
        std::string roundtrip;
        require(store.saveText(loaded.config, roundtrip, error) &&
                store.loadText(roundtrip).config.gui.fileDialogs.directories == preferences.directories,
                "用途目录整份配置往返");
        require(loaded.config.app.fpsLimit == 37, "preserve saved settings");
        std::ifstream input(path);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        require(text.find("custom: keep") != std::string::npos, "preserve unknown fields");
        config::DataExportConfig lastExport;
        lastExport.valid = true;
        lastExport.directory = "successful-export";
#if defined(_WIN32)
        require(!store.saveFileDialogPreferences(path, preferences, error, &lastExport),
                "locked config reports save failure");
#endif
        input.close();
        // 固定 Unix 毫秒值同时覆盖纪元转换和毫秒部分，不能只验证编译或往返相等。
        constexpr std::uint64_t expectedTimestampMs = 1483228800123ULL;
        const std::chrono::sys_time<std::chrono::milliseconds> fixedTime{
            std::chrono::milliseconds{expectedTimestampMs}};
        const auto fixedFileTime = std::chrono::clock_cast<std::chrono::file_clock>(fixedTime);
        std::filesystem::last_write_time(path, fixedFileTime);
        // MinGW 的文件元数据接口可能截断到秒；用同一文件时钟的差值计算实际期望，
        // 不复用待测的系统时钟转换，因此仍可发现纪元或毫秒单位错误。
        const auto storedFileTime = std::filesystem::last_write_time(path);
        const auto storedTimestampMs = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(expectedTimestampMs) +
            std::chrono::duration_cast<std::chrono::milliseconds>(storedFileTime - fixedFileTime).count());
        const auto stableSnapshot = store.snapshot(path);
        require(stableSnapshot.error.empty() && stableSnapshot.exists, "固定时间快照读取失败");
        require(stableSnapshot.timestampMs == storedTimestampMs, "文件时间未转换为 Unix 毫秒");
        // 反复跨时钟采样检查：同一文件时间不得因转换估算漂移产生假外部冲突。
        for (int i = 0; i < 100; ++i)
            require(!store.hasChanged(stableSnapshot), "unchanged snapshot must remain stable");
        // 内容不变时也必须检测到文件修改时间变化。
        std::filesystem::last_write_time(path, storedFileTime + std::chrono::seconds{2});
        const auto changedSnapshot = store.snapshot(path);
        require(changedSnapshot.error.empty() && changedSnapshot.contents == stableSnapshot.contents,
                "仅修改时间不应改变文件内容");
        require(changedSnapshot.timestampMs == storedTimestampMs + 2000ULL && store.hasChanged(stableSnapshot),
                "仅修改文件时间未检测");
        require(store.saveFileDialogPreferences(path, preferences, error, &lastExport), "save last export only");
        require(store.load(path).config.gui.lastDataExport.valid, "last export reload without wave");
        preferences.lastExportDirectory = "failed-export";
        require(store.saveFileDialogPreferences(path, preferences, error), "remember later confirmation");
        require(store.load(path).config.gui.lastDataExport.directory == "successful-export",
                "confirmation must not overwrite successful export parameters");
        require(store.load(path).config.gui.fileDialogs.lastExportDirectory == "failed-export",
                "directory independent from successful parameters");
        require(ui::resolveFileDialogDirectory(ui::fileDialogPathText(unicodeDir / "missing" / "nested"), {}, root)
                    == unicodeDir, "nearest existing parent");
        require(ui::resolveFileDialogDirectory("?:/missing", unicodeDir, root) == unicodeDir, "invalid drive fallback");
        require(ui::resolveFileDialogDirectory("", root / "missing" / "nested", {}) == root, "default parents");
        require(ui::resolveFileDialogDirectory("", {}, root) == root, "executable fallback");
        require(ui::fileDialogHistory(preferences.directories, "elf.open", "legacy") ==
                    ui::fileDialogPathText(unicodeDir), "用途历史优先旧目录");
        require(ui::fileDialogHistory(preferences.directories, "unknown", "legacy") == "legacy",
                "未记录用途兼容回退");
        const auto key = ui::luaFileDialogHistoryKey("protocol-a", 0, "Title", "");
        require(key != ui::luaFileDialogHistoryKey("protocol-b", 0, "Title", "") &&
                key != ui::luaFileDialogHistoryKey("protocol-a", 1, "Title", "") &&
                key != ui::luaFileDialogHistoryKey("protocol-a", 0, "Other", ""), "Lua 默认分组隔离");
        require(ui::luaFileDialogHistoryKey("protocol-a", 0, "Title", "explicit") ==
                ui::luaFileDialogHistoryKey("protocol-a", 0, "Other", "explicit"), "Lua 显式历史键");
        require(ui::luaFileDialogInitialPath(ui::fileDialogPathText(unicodeDir), "script.csv", false, root) ==
                unicodeDir / "script.csv", "Lua 历史目录优先且保留文件名");
        require(ui::luaFileDialogInitialPath(ui::fileDialogPathText(unicodeDir), "missing", true, root) ==
                unicodeDir, "Lua 目录选择历史优先");
        { std::ofstream out(path); out << "gui: [broken"; }
        require(!store.saveFileDialogPreferences(path, preferences, error), "reject corrupt config");
        std::ifstream broken(path);
        const std::string unchanged((std::istreambuf_iterator<char>(broken)), {});
        require(unchanged == "gui: [broken", "do not overwrite corrupt config");
        require(!store.saveFileDialogPreferences(root, preferences, error), "unwritable config");
        std::cout << "file dialog preferences passed\n";
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        return 1;
    }
}
