#include "protoscope/ui/theme_manager.hpp"
#include "theme_test_contract.hpp"
#include <iomanip>
#include <sstream>

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
    ImGui::CreateContext();
    ImPlot::CreateContext();
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
        // 每个公开 UI 字段独立注入 8-bit RGBA；不复用生产字段表求预期。
        for (auto preset : {config::GuiTheme::ProfessionalDark, config::GuiTheme::DebugHighContrast,
                            config::GuiTheme::ProfessionalLight}) {
            const auto& base=ui::uiThemeDefinition(preset);
            const auto prefix="version: 1\nid: faithful\nname: Faithful\nbase: "+base.id+"\n";
            check(ui::ThemeManager::parse(prefix+"ui:\n  panel_border: '#12345600'\n  accent: '#65432120'\n",
                                          "legacy.yaml",theme,error),"旧 YAML 缺省新字段合法");
            for(std::size_t i=12;i<tests::theme::fields.size();++i) {
                const auto& field=tests::theme::fields[i];
                tests::theme::color(theme.ui.*field.member,base.ui.*field.member,field.yaml);
            }
            for(int sample=0;sample<3;++sample) {
                auto expected=base;
                std::ostringstream yaml;
                yaml<<prefix<<"ui:\n";
                unsigned index=0;
                for(const auto& field:tests::theme::fields) {
                    const unsigned r=sample==0 ? 30+index : 100;
                    const unsigned g=sample==0 ? 70+index : 100;
                    const unsigned b=sample==0 ? 110+index : 100;
                    const unsigned a=sample==2 ? 0 : sample==1 ? 64 : (index*11)%256;
                    expected.ui.*field.member=ImVec4(r/255.F,g/255.F,b/255.F,a/255.F);
                    yaml<<"  "<<field.yaml<<": '#"<<std::hex<<std::uppercase<<std::setfill('0')
                        <<std::setw(2)<<r<<std::setw(2)<<g<<std::setw(2)<<b<<std::setw(2)<<a<<"'\n";
                    ++index;
                }
                yaml<<"wave:\n  correct_contrast: true\n  legend_overlay_background: '#22334455'\n"
                      "  legend_overlay_row_hover: '#66778800'\n  legend_overlay_row_active: '#AABBCC22'\n"
                      "  channel_palette: ['#01234500', '#FEDCBA80']\n  cursor_palette: ['#56789040', '#A1B2C3FF']\n";
                check(ui::ThemeManager::parse(yaml.str(),"faithful.yaml",theme,error),"低对比和透明主题必须合法");
                for(const auto& field:tests::theme::fields)
                    tests::theme::color(theme.ui.*field.member,expected.ui.*field.member,field.yaml);
                const auto savedTheme=ui::ThemeManager::serialize(theme,"renamed");
                check(ui::ThemeManager::parse(savedTheme,"renamed.yaml",roundtrip,error),"全部字段往返");
                check(ui::ThemeManager::serialize(roundtrip,"faithful")==ui::ThemeManager::serialize(theme,"faithful"),
                      "完整序列化内容、色板顺序及 alpha 往返");
                for(const auto& field:tests::theme::fields)
                    tests::theme::color(roundtrip.ui.*field.member,expected.ui.*field.member,field.yaml);
                ui::applyUiTheme(theme); tests::theme::mapped(theme);
                const auto before=ImGui::GetStyle(); const auto plotBefore=ImPlot::GetStyle();
                ui::applyUiTheme(roundtrip); tests::theme::mapped(roundtrip);
                tests::theme::sameStyles(before,ImGui::GetStyle());
                for(int slot=0;slot<ImPlotCol_COUNT;++slot)
                    tests::theme::color(ImPlot::GetStyle().Colors[slot],plotBefore.Colors[slot],"导出改 ID 不改 ImPlot");
                roundtrip.wave.correctContrast=false; ui::applyUiTheme(roundtrip);
                tests::theme::sameStyles(before,ImGui::GetStyle());
            }
        }
        for (const auto* invalid : {"ui:\n  control_border: '#123456'\n  control_border: '#112233'\n",
                "ui:\n  tab_selected: [1, 2, 3]\n", "ui:\n  floating_background: '#12345Z'\n",
                "metrics:\n  frame_rounding: .inf\n", "wave:\n  selection_min_alpha: .nan\n",
                "wave:\n  cursor_palette: []\n", "wave:\n  correct_contrast: perhaps\n"}) {
            check(!ui::ThemeManager::parse(header+invalid,"invalid-new.yaml",roundtrip,error),"新字段信任边界仍拒绝非法输入");
            check(error.find("invalid-new.yaml:")!=std::string::npos,"新字段错误保留来源");
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

        ui::ThemeManager builtinThemes;
        for (const auto& [preset,name] : std::array{
                 std::pair{config::GuiTheme::ProfessionalDark,"石墨青绿"},
                 std::pair{config::GuiTheme::DebugHighContrast,"石墨琥珀"},
                 std::pair{config::GuiTheme::ProfessionalLight,"冷白青蓝"}}) {
            const auto* builtin=builtinThemes.find(config::guiThemeId(preset));
            check(builtin && builtin->name==name && ui::uiThemeDefinition(preset).name==name,
                  "管理器与预设名称只有一个来源且保留 ID");
        }
        for (const auto* id : {"graphite_cyan", "warm_industrial", "paper_lab"}) {
            check(builtinThemes.find(id) == nullptr, "退役主题不是内置主题");
            builtinThemes.startup(id, error);
            check(!error.empty() && builtinThemes.applyPending() &&
                  ui::activeThemeDefinition().id == "professional_dark", "旧 ID 缺失时回退深色");
            const auto original = store.loadText(std::string("gui:\n  theme: ") + id + "\n");
            check(original.config.gui.theme == id, "回退不改写配置原 ID");
        }

        const auto directory = std::filesystem::temp_directory_path() /
            ("protoscope-theme-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        manager.setConfigPath(directory / "config.yaml");
        check(manager.reload(error) && manager.find("professional_dark"), "缺失目录仍提供内置主题");
        std::filesystem::create_directories(directory / "themes");
        check(manager.reload(error) && manager.find("professional_light"), "空目录仍提供内置主题");
        // 同名用户文件不能被退役策略封禁，也不能清理运行目录旧副本。
        for (const auto* id : {"graphite_cyan", "warm_industrial", "paper_lab"}) {
            std::ofstream file(directory / "themes" / (std::string(id) + ".yaml"));
            file << "version: 1\nid: " << id << "\nname: User\nbase: professional_dark\n";
        }
        check(manager.reload(error), "发现用户自定义主题");
        for (const auto* id : {"graphite_cyan", "warm_industrial", "paper_lab"})
            check(manager.request(id, error) && manager.applyPending() &&
                  ui::activeThemeDefinition().id == id, "同名用户主题仍能应用");
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
        std::cout << "theme_manager: parsing, user discovery, inheritance, errors, roundtrip, deferred apply, retirement fallback passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        status = 1;
    }
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    return status;
}
