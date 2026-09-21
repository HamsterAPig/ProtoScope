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
        std::string error;
        require(store.saveFileDialogPreferences(path, preferences, error), error.c_str());
        const auto loaded = store.load(path);
        require(loaded.loadedFromDisk, "reload");
        require(loaded.config.gui.fileDialogs.lastImportDirectory == preferences.lastImportDirectory, "unicode reload");
        require(loaded.config.gui.fileDialogs.lastExportDirectory == "export space", "separate export");
        require(loaded.config.app.fpsLimit == 37, "preserve saved settings");
        std::ifstream input(path);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        require(text.find("custom: keep") != std::string::npos, "preserve unknown fields");
        require(ui::resolveFileDialogDirectory(ui::fileDialogPathText(unicodeDir / "missing" / "nested"), {}, root)
                    == unicodeDir, "nearest existing parent");
        require(ui::resolveFileDialogDirectory("?:/missing", unicodeDir, root) == unicodeDir, "invalid drive fallback");
        require(ui::resolveFileDialogDirectory("", root / "missing" / "nested", {}) == root, "default parents");
        require(ui::resolveFileDialogDirectory("", {}, root) == root, "executable fallback");
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
