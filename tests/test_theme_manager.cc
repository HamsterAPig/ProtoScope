#include "protoscope/ui/theme_manager.hpp"

#include <iostream>
#include <cmath>
#include <chrono>
#include <fstream>
#include <stdexcept>

using namespace protoscope;
namespace {
void check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
bool nearComponent(float value, int component) { return std::abs(value - component / 255.F) <= 1.F / 255.F; }
}

int main()
{
    ImGui::CreateContext();
    int status = 0;
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

        ui::ThemeManager bundledThemes;
        bundledThemes.setConfigPath(std::filesystem::path{"config"} / "protoscope.yaml");
        check(bundledThemes.reload(error), "加载随附主题");
        const auto* graphite = bundledThemes.find("graphite_cyan");
        const auto* warm = bundledThemes.find("warm_industrial");
        const auto* paper = bundledThemes.find("paper_lab");
        check(graphite && graphite->name == "Graphite + Cyan" && graphite->base == "professional_dark",
              "发现 Graphite + Cyan");
        check(warm && warm->name == "Warm Industrial" && warm->base == "professional_dark",
              "发现 Warm Industrial");
        check(paper && paper->name == "Paper Lab" && paper->base == "professional_light",
              "发现 Paper Lab");
        check(nearComponent(graphite->ui.appBackground.x, 11) && nearComponent(graphite->ui.accent.y, 211) &&
                  nearComponent(graphite->wave.plotBackground.z, 18) && nearComponent(graphite->wave.selectionColor.z, 246) &&
                  graphite->wave.channelPalette.size() == 8 && nearComponent(graphite->wave.channelPalette[0].y, 211),
              "Graphite + Cyan 关键颜色");
        check(nearComponent(warm->ui.panelBackground.x, 36) && nearComponent(warm->ui.accent.x, 217) &&
                  nearComponent(warm->wave.plotBackground.x, 21) && nearComponent(warm->wave.selectionColor.y, 122) &&
                  warm->wave.channelPalette.size() == 8 && nearComponent(warm->wave.channelPalette[1].y, 174),
              "Warm Industrial 关键颜色");
        check(nearComponent(paper->ui.appBackground.x, 243) && nearComponent(paper->ui.accent.z, 179) &&
                  nearComponent(paper->wave.plotBackground.x, 255) && nearComponent(paper->wave.selectionColor.z, 230) &&
                  paper->wave.channelPalette.size() == 8 && nearComponent(paper->wave.channelPalette[3].y, 130),
              "Paper Lab 关键颜色");
        check(graphite->ui.windowRounding ==
                  ui::uiThemeDefinition(config::GuiTheme::ProfessionalDark).ui.windowRounding &&
                  paper->ui.framePaddingY ==
                  ui::uiThemeDefinition(config::GuiTheme::ProfessionalLight).ui.framePaddingY,
              "随附主题继承基底尺寸");
        check(bundledThemes.request("paper_lab", error) && bundledThemes.applyPending() &&
                  ui::activeThemeDefinition().id == "paper_lab" &&
                  nearComponent(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).x, 243) &&
                  nearComponent(ImGui::GetStyleColorVec4(ImGuiCol_ChildBg).y, 252) &&
                  nearComponent(ImGui::GetStyleColorVec4(ImGuiCol_CheckMark).z, 179),
              "外部主题应用到 ImGui 样式");
        const auto contrast = [](ImVec4 foreground, ImVec4 background) {
            const auto linear = [](float component) {
                return component <= 0.04045F ? component / 12.92F :
                    std::pow((component + 0.055F) / 1.055F, 2.4F);
            };
            const auto luminance = [&](ImVec4 color) {
                return 0.2126F * linear(color.x) + 0.7152F * linear(color.y) + 0.0722F * linear(color.z);
            };
            const float foregroundLuminance = luminance(foreground);
            const float backgroundLuminance = luminance(background);
            return (std::max(foregroundLuminance, backgroundLuminance) + 0.05F) /
                (std::min(foregroundLuminance, backgroundLuminance) + 0.05F);
        };
        for (const auto* bundled : {graphite, warm, paper}) {
            ui::applyUiTheme(*bundled);
            for (const auto semantic : {bundled->ui.success, bundled->ui.warning, bundled->ui.danger}) {
                const auto readable = ui::displayColor(semantic, bundled->ui.panelBackgroundAlt, 1.0F, 4.5F);
                check(contrast(readable, bundled->ui.panelBackgroundAlt) >= 4.49F,
                      "语义正文颜色需达到 4.5:1");
            }
        }

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
        std::cout << "theme_manager: parsing, bundled discovery, key colors, ImGui apply, inheritance, errors, roundtrip, deferred apply, fallback passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    ImGui::DestroyContext();
    return status;
}
