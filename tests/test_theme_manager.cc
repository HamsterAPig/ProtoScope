#include "protoscope/ui/theme_manager.hpp"

#include <iostream>
#include <cmath>
#include <chrono>
#include <fstream>
#include <stdexcept>

using namespace protoscope;
namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
}

int main()
{
    try {
        ui::UiThemeDefinition theme;
        std::string error;
        const std::string header = "version: 1\nid: custom\nname: Custom\nbase: professional_dark\n";
        check(ui::ThemeManager::parse(header + "ui:\n  text_strong: '#F1F2F3'\n", "sample.yaml", theme, error),
              "partial override");
        check(theme.ui.textStrong.x > .94F && theme.ui.panelBackground.x ==
              ui::uiThemeDefinition(config::GuiTheme::ProfessionalDark).ui.panelBackground.x, "inherit base");
        const auto serialized = ui::ThemeManager::serialize(theme, "exported");
        ui::UiThemeDefinition roundtrip;
        check(ui::ThemeManager::parse(serialized, "roundtrip.yaml", roundtrip, error), "export roundtrip");
        check(roundtrip.id == "exported" && std::abs(roundtrip.ui.textStrong.x - theme.ui.textStrong.x) < .004F,
              "roundtrip values");
        for (const auto& suffix : {"unknown: true\n", "ui:\n  typo: '#FFFFFF'\n",
                                   "ui:\n  accent: red\n", "metrics:\n  window_rounding: -1\n",
                                   "metrics:\n  grid_major_width: .nan\n", "id: duplicate\n"}) {
            check(!ui::ThemeManager::parse(header + suffix, "broken.yaml", roundtrip, error), "reject invalid");
            check(error.find("broken.yaml:") != std::string::npos, "error source location");
        }
        check(!ui::ThemeManager::parse("version: 1\nid: x\nname: X\nbase: custom\n", "x", theme, error),
              "reject user inheritance");
        // 显式颜色/透明度/色板优先于新预设，应用和序列化不能再次 finishPreset。
        for (const auto* base : {"professional_dark", "professional_light", "debug_high_contrast"}) {
            const std::string custom = std::string("version: 1\nid: explicit\nname: Explicit\nbase: ") + base +
                "\nui:\n  panel_background: '#12345678'\n  accent: '#FA102030'\nwave:\n  correct_contrast: false\n"
                "  plot_background: '#20406080'\n  channel_palette: ['#01A0FE20', '#12345600']\n"
                "  cursor_palette: ['#D0804040', '#90B0D000']\n";
            check(ui::ThemeManager::parse(custom, "explicit.yaml", theme, error), "显式定制解析");
            const auto encoded = ui::ThemeManager::serialize(theme, "explicit_copy");
            check(ui::ThemeManager::parse(encoded, "copy.yaml", roundtrip, error), "显式定制往返");
            check(!roundtrip.wave.correctContrast && roundtrip.wave.channelPalette.size() == 2 &&
                  roundtrip.wave.channelPalette[1].w == 0 && roundtrip.wave.cursorPalette[1].w == 0,
                  "透明色和关闭修正必须保留");
            check(std::abs(roundtrip.ui.panelBackground.w - 120.F/255) < 1e-6 &&
                  std::abs(roundtrip.wave.cursorPalette[0].w - 64.F/255) < 1e-6,
                  "显式 alpha 往返不变");
            ui::applyUiTheme(roundtrip);
            check(ui::activeUiStyleTokens().panelBackground.x == roundtrip.ui.panelBackground.x &&
                  ui::activeWaveStyleTokens().cursorPalette[0].x == roundtrip.wave.cursorPalette[0].x,
                  "应用不能覆盖用户字段");
        }
        ui::ThemeManager manager;
        ui::applyUiTheme(config::GuiTheme::DebugHighContrast);
        const auto revision = ui::activeThemeRevision();
        check(!manager.request("missing", error), "missing selection rejected");
        check(ui::activeThemeRevision() == revision, "failure preserves current");
        check(manager.request("professional_dark", error), "queue builtin");
        check(ui::activeThemeRevision() == revision, "deferred application");
        check(manager.applyPending() && ui::activeThemeRevision() == revision + 1, "apply at frame boundary");
        manager.startup("missing", error);
        check(!error.empty() && manager.applyPending(), "startup fallback");
        config::ConfigStore store;
        check(store.loadText("gui:\n  theme: custom\n").config.gui.theme == "custom", "preserve custom id");
        check(store.loadText("gui:\n  theme: debug_high_contrast\n").config.gui.theme == "debug_high_contrast",
              "legacy compatibility");
        const auto configured = store.loadText("gui:\n  wave:\n    overview_selection:\n      mode: fixed\n"
            "      fixed_color: '#8033AA'\n      min_alpha: 0.12\n      max_alpha: 0.23\n");
        check(configured.error.empty() && !configured.config.gui.wave.overviewSelection.automatic, "selection config");
        std::string saved;
        check(store.saveText(configured.config, saved, error), "save selection config");
        check(store.loadText(saved).config.gui.wave.overviewSelection == configured.config.gui.wave.overviewSelection,
              "selection config roundtrip");
        check(!store.loadText("gui:\n  wave:\n    overview_selection:\n      min_alpha: 0.8\n      max_alpha: 0.2\n").error.empty(),
              "selection alpha validation");
        const auto directory = std::filesystem::temp_directory_path() /
            ("protoscope-theme-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory / "themes");
        manager.setConfigPath(directory / "config.yaml");
        check(manager.exportCurrent(directory / "themes/custom.yaml", error), "export file");
        check(manager.reload(error) && manager.find("custom"), "scan user theme");
        check(manager.request("custom", error) && manager.applyPending(), "apply user theme");
        const auto beforeReload = ui::activeThemeRevision();
        {
            std::ofstream invalidFile(directory / "themes/broken.yaml");
            invalidFile << "version: 1\nid: custom\nname: Duplicate\nbase: professional_dark\n";
        }
        check(!manager.reload(error) && error.find("id:") != std::string::npos, "reject duplicate registry ID");
        check(ui::activeThemeRevision() == beforeReload && manager.find("custom"), "reload failure preserves registry");
        check(!manager.exportCurrent(directory / "themes/custom.yaml", error), "export refuses overwrite");
        std::cout << "theme_manager: parsing, inheritance, errors, roundtrip, deferred apply, fallback passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
