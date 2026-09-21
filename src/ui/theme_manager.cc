#include "protoscope/ui/theme_manager.hpp"

#include <yaml-cpp/yaml.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace protoscope::ui {
namespace {

// 字段表同时服务解析与导出，避免新增令牌只支持单向序列化。
const std::map<std::string, ImVec4 UiStyleTokens::*> uiColors{
    {"app_background", &UiStyleTokens::appBackground},
    {"panel_background", &UiStyleTokens::panelBackground},
    {"panel_background_alt", &UiStyleTokens::panelBackgroundAlt},
    {"panel_border", &UiStyleTokens::panelBorder},
    {"accent", &UiStyleTokens::accent}, {"accent_muted", &UiStyleTokens::accentMuted},
    {"success", &UiStyleTokens::success}, {"warning", &UiStyleTokens::warning},
    {"danger", &UiStyleTokens::danger}, {"text_strong", &UiStyleTokens::textStrong},
    {"text_muted", &UiStyleTokens::textMuted}, {"generic_plot_background", &UiStyleTokens::genericPlotBackground}};
const std::map<std::string, ImVec4 WaveStyleTokens::*> waveColors{
    {"plot_background", &WaveStyleTokens::plotBackground},
    {"grid_major", &WaveStyleTokens::gridMajor}, {"grid_minor_tick", &WaveStyleTokens::gridMinorTick},
    {"grid_center", &WaveStyleTokens::gridCenter},
    {"status_overlay_background", &WaveStyleTokens::statusOverlayBackground},
    {"status_overlay_border", &WaveStyleTokens::statusOverlayBorder},
    {"status_overlay_text", &WaveStyleTokens::statusOverlayText},
    {"channel_separator", &WaveStyleTokens::channelSeparator}, {"channel_label", &WaveStyleTokens::channelLabel},
    {"split_channel_label", &WaveStyleTokens::splitChannelLabel}, {"bit_label", &WaveStyleTokens::bitLabel},
    {"legend_overlay_background", &WaveStyleTokens::legendOverlayBackground},
    {"legend_overlay_border", &WaveStyleTokens::legendOverlayBorder},
    {"legend_overlay_text_primary", &WaveStyleTokens::legendOverlayTextPrimary},
    {"legend_overlay_text_secondary", &WaveStyleTokens::legendOverlayTextSecondary},
    {"legend_overlay_row_hover", &WaveStyleTokens::legendOverlayRowHover},
    {"legend_overlay_row_active", &WaveStyleTokens::legendOverlayRowActive},
    {"legend_overlay_row_active_border", &WaveStyleTokens::legendOverlayRowActiveBorder},
    {"measurement_overlay_background", &WaveStyleTokens::measurementOverlayBackground},
    {"measurement_overlay_border", &WaveStyleTokens::measurementOverlayBorder},
    {"measurement_overlay_accent", &WaveStyleTokens::measurementOverlayAccent},
    {"measurement_overlay_title", &WaveStyleTokens::measurementOverlayTitle},
    {"measurement_chip_background", &WaveStyleTokens::measurementChipBackground},
    {"measurement_chip_border", &WaveStyleTokens::measurementChipBorder},
    {"measurement_chip_label", &WaveStyleTokens::measurementChipLabel},
    {"measurement_chip_value", &WaveStyleTokens::measurementChipValue},
    {"selection_color", &WaveStyleTokens::selectionColor}};
const std::map<std::string, float UiStyleTokens::*> uiMetrics{
    {"window_rounding", &UiStyleTokens::windowRounding}, {"frame_rounding", &UiStyleTokens::frameRounding},
    {"grab_rounding", &UiStyleTokens::grabRounding}, {"tab_rounding", &UiStyleTokens::tabRounding},
    {"item_spacing_x", &UiStyleTokens::itemSpacingX}, {"item_spacing_y", &UiStyleTokens::itemSpacingY},
    {"window_padding_x", &UiStyleTokens::windowPaddingX}, {"window_padding_y", &UiStyleTokens::windowPaddingY},
    {"frame_padding_x", &UiStyleTokens::framePaddingX}, {"frame_padding_y", &UiStyleTokens::framePaddingY}};
const std::map<std::string, float WaveStyleTokens::*> waveMetrics{
    {"grid_major_width", &WaveStyleTokens::gridMajorWidth},
    {"grid_minor_tick_width", &WaveStyleTokens::gridMinorTickWidth},
    {"grid_center_width", &WaveStyleTokens::gridCenterWidth},
    {"grid_minor_tick_half_length", &WaveStyleTokens::gridMinorTickHalfLength}};

[[noreturn]] void invalid(const YAML::Node& node, const std::string& field, const std::string& message)
{
    throw YAML::Exception(node.Mark(), field + ": " + message);
}
template<class T> T scalar(const YAML::Node& node, const std::string& field)
{
    try { return node.as<T>(); }
    catch (const YAML::Exception&) { invalid(node, field, "类型或数值无效"); }
}
void keys(const YAML::Node& node, const std::string& field)
{
    if (!node.IsMap()) invalid(node, field, "应为映射");
    std::set<std::string> seen;
    for (const auto& entry : node) {
        if (!entry.first.IsScalar()) invalid(entry.first, field, "字段名应为字符串");
        const auto name = entry.first.as<std::string>();
        if (!seen.insert(name).second) invalid(entry.first, field + "." + name, "重复字段");
    }
}
ImVec4 color(const YAML::Node& node, const std::string& field)
{
    if (!node.IsScalar()) invalid(node, field, "颜色应为带引号的十六进制字符串");
    const auto text = node.as<std::string>();
    if ((text.size() != 7 && text.size() != 9) || text.front() != '#' ||
        text.find_first_not_of("0123456789aAbBcCdDeEfF", 1) != std::string::npos)
        invalid(node, field, "颜色应为 #RRGGBB 或 #RRGGBBAA");
    const auto component = [&](std::size_t offset) {
        return static_cast<float>(std::stoul(text.substr(offset, 2), nullptr, 16)) / 255.F;
    };
    return {component(1), component(3), component(5), text.size() == 9 ? component(7) : 1.F};
}
std::string colorText(ImVec4 value)
{
    std::ostringstream out;
    out << '#' << std::hex << std::uppercase << std::setfill('0');
    for (auto part : {value.x, value.y, value.z, value.w})
        out << std::setw(2) << static_cast<int>(std::lround(std::clamp(part, 0.F, 1.F) * 255.F));
    return out.str();
}
std::vector<UiThemeDefinition> builtins()
{
    std::vector<UiThemeDefinition> result;
    for (const auto theme : {config::GuiTheme::ProfessionalDark, config::GuiTheme::DebugHighContrast,
                             config::GuiTheme::ProfessionalLight}) {
        auto value = uiThemeDefinition(theme);
        value.id = config::guiThemeId(theme);
        value.base = value.id;
        value.name = theme == config::GuiTheme::ProfessionalDark ? "专业深色" :
                     theme == config::GuiTheme::DebugHighContrast ? "仪器深黑（高对比）" : "专业浅色";
        result.push_back(std::move(value));
    }
    return result;
}
}

ThemeManager::ThemeManager() : themes_(builtins()) {}
void ThemeManager::setConfigPath(const std::filesystem::path& path) { directory_ = path.parent_path() / "themes"; }
const UiThemeDefinition* ThemeManager::find(std::string_view id) const
{
    const auto found = std::find_if(themes_.begin(), themes_.end(), [&](const auto& theme) { return theme.id == id; });
    return found == themes_.end() ? nullptr : &*found;
}

bool ThemeManager::parse(std::string_view yaml, const std::filesystem::path& source,
                         UiThemeDefinition& result, std::string& error)
{
    error.clear();
    try {
        const auto root = YAML::Load(std::string(yaml));
        keys(root, "theme");
        for (const auto& item : root) {
            const auto key = item.first.as<std::string>();
            if (key != "version" && key != "id" && key != "name" && key != "base" &&
                key != "ui" && key != "wave" && key != "metrics") invalid(item.first, key, "未知字段");
        }
        if (!root["version"] || scalar<int>(root["version"], "version") != 1) invalid(root, "version", "只支持 version: 1");
        for (const auto* field : {"id", "name", "base"})
            if (!root[field] || !root[field].IsScalar() || root[field].as<std::string>().empty())
                invalid(root, field, "缺少非空字符串");
        const auto base = root["base"].as<std::string>();
        const auto presets = builtins();
        const auto found = std::find_if(presets.begin(), presets.end(), [&](const auto& p) { return p.id == base; });
        if (found == presets.end()) invalid(root["base"], "base", "只允许继承内置主题");
        auto parsed = *found;
        parsed.id = root["id"].as<std::string>();
        if (parsed.id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)
            invalid(root["id"], "id", "只允许字母、数字、下划线及连字符");
        parsed.name = root["name"].as<std::string>();
        parsed.base = base;
        const auto loadColors = [&](const char* group, auto& tokens, const auto& fields) {
            const auto node = root[group];
            if (!node) return;
            keys(node, group);
            for (const auto& item : node) {
                const auto key = item.first.template as<std::string>();
                const auto member = fields.find(key);
                if (std::string_view(group) == "wave" &&
                    (key == "channel_palette" || key == "cursor_palette" || key == "correct_contrast" ||
                     key == "light_persistence" || key == "selection_min_alpha" || key == "selection_max_alpha")) continue;
                if (member == fields.end()) invalid(item.first, std::string(group) + "." + key, "未知字段");
                tokens.*(member->second) = color(item.second, std::string(group) + "." + key);
            }
        };
        loadColors("ui", parsed.ui, uiColors);
        loadColors("wave", parsed.wave, waveColors);
        if (const auto node = root["wave"]) {
            for (const auto* field : {"channel_palette", "cursor_palette"}) {
                if (!node[field]) continue;
                if (!node[field].IsSequence() || node[field].size() == 0 || node[field].size() > 64)
                    invalid(node[field], std::string("wave.") + field, "色板应包含 1 至 64 个颜色");
                auto& palette = std::string_view(field) == "channel_palette" ? parsed.wave.channelPalette : parsed.wave.cursorPalette;
                palette.clear();
                for (std::size_t i = 0; i < node[field].size(); ++i)
                    palette.push_back(color(node[field][i], std::string("wave.") + field + "[" + std::to_string(i) + "]"));
            }
            if (node["correct_contrast"]) parsed.wave.correctContrast = scalar<bool>(node["correct_contrast"], "wave.correct_contrast");
            if (node["light_persistence"]) parsed.wave.lightPersistence = scalar<bool>(node["light_persistence"], "wave.light_persistence");
            if (node["selection_min_alpha"]) parsed.wave.selectionMinAlpha = scalar<float>(node["selection_min_alpha"], "wave.selection_min_alpha");
            if (node["selection_max_alpha"]) parsed.wave.selectionMaxAlpha = scalar<float>(node["selection_max_alpha"], "wave.selection_max_alpha");
            if (!std::isfinite(parsed.wave.selectionMinAlpha) || !std::isfinite(parsed.wave.selectionMaxAlpha) ||
                parsed.wave.selectionMinAlpha < 0 || parsed.wave.selectionMaxAlpha > 1 ||
                parsed.wave.selectionMinAlpha > parsed.wave.selectionMaxAlpha)
                invalid(node, "wave.selection_min_alpha/selection_max_alpha", "必须满足 0 <= min <= max <= 1");
        }
        if (const auto node = root["metrics"]) {
            keys(node, "metrics");
            for (const auto& item : node) {
                const auto key = item.first.as<std::string>();
                const auto ui = uiMetrics.find(key);
                const auto wave = waveMetrics.find(key);
                if (ui == uiMetrics.end() && wave == waveMetrics.end()) invalid(item.first, "metrics." + key, "未知字段");
                const auto value = scalar<float>(item.second, "metrics." + key);
                if (!std::isfinite(value) || value < 0 || value > 64 || (wave != waveMetrics.end() && value <= 0))
                    invalid(item.second, "metrics." + key, "尺寸超出合法范围");
                if (ui != uiMetrics.end()) parsed.ui.*(ui->second) = value;
                else parsed.wave.*(wave->second) = value;
            }
        }
        result = std::move(parsed);
        return true;
    } catch (const YAML::Exception& exception) {
        error = source.string() + ":" + std::to_string(exception.mark.line + 1) + ":" +
                std::to_string(exception.mark.column + 1) + ": " + exception.msg;
    } catch (const std::exception& exception) {
        error = source.string() + ":1:1: " + exception.what();
    }
    return false;
}

bool ThemeManager::reload(std::string& error)
{
    error.clear();
    auto loaded = builtins();
    try {
        if (!directory_.empty() && std::filesystem::exists(directory_)) {
            std::vector<std::filesystem::path> paths;
            for (const auto& entry : std::filesystem::directory_iterator(directory_))
                if (entry.is_regular_file() && (entry.path().extension() == ".yaml" || entry.path().extension() == ".yml"))
                    paths.push_back(entry.path());
            std::sort(paths.begin(), paths.end());
            for (const auto& path : paths) {
                std::ifstream stream(path);
                if (!stream) throw std::runtime_error(path.string() + ": 无法读取主题文件");
                std::ostringstream text;
                text << stream.rdbuf();
                UiThemeDefinition parsed;
                if (!parse(text.str(), path, parsed, error)) return false;
                if (std::any_of(loaded.begin(), loaded.end(), [&](const auto& theme) { return theme.id == parsed.id; })) {
                    const auto idNode = YAML::Load(text.str())["id"];
                    error = path.string() + ":" + std::to_string(idNode.Mark().line + 1) + ":" +
                            std::to_string(idNode.Mark().column + 1) + ": id: 重复 ID " + parsed.id;
                    return false;
                }
                loaded.push_back(std::move(parsed));
            }
        }
        // 扫描全部成功才替换注册表，任何单文件错误都不破坏当前活动主题。
        themes_ = std::move(loaded);
        return true;
    } catch (const std::exception& exception) { error = directory_.string() + ": " + exception.what(); }
    return false;
}
bool ThemeManager::request(std::string_view id, std::string& error)
{
    error.clear();
    if (const auto* theme = find(id)) { pending_ = *theme; return true; }
    error = directory_.string() + ": theme.id: 未找到主题 " + std::string(id);
    return false;
}
void ThemeManager::startup(std::string_view id, std::string& error)
{
    if (!reload(error) || !request(id, error)) pending_ = builtins().front();
}
bool ThemeManager::applyPending()
{
    if (!pending_) return false;
    applyUiTheme(*pending_);
    pending_.reset();
    return true;
}
std::string ThemeManager::serialize(const UiThemeDefinition& theme, std::string_view id)
{
    YAML::Node root;
    root["version"] = 1;
    root["id"] = std::string(id);
    root["name"] = theme.name;
    root["base"] = theme.base;
    for (const auto& [name, member] : uiColors) root["ui"][name] = colorText(theme.ui.*member);
    for (const auto& [name, member] : waveColors) root["wave"][name] = colorText(theme.wave.*member);
    for (const auto& value : theme.wave.channelPalette) root["wave"]["channel_palette"].push_back(colorText(value));
    for (const auto& value : theme.wave.cursorPalette) root["wave"]["cursor_palette"].push_back(colorText(value));
    root["wave"]["correct_contrast"] = theme.wave.correctContrast;
    root["wave"]["light_persistence"] = theme.wave.lightPersistence;
    root["wave"]["selection_min_alpha"] = theme.wave.selectionMinAlpha;
    root["wave"]["selection_max_alpha"] = theme.wave.selectionMaxAlpha;
    for (const auto& [name, member] : uiMetrics) root["metrics"][name] = theme.ui.*member;
    for (const auto& [name, member] : waveMetrics) root["metrics"][name] = theme.wave.*member;
    YAML::Emitter out;
    out << root;
    return out.c_str();
}
bool ThemeManager::exportCurrent(const std::filesystem::path& path, std::string& error) const
{
    error.clear();
    try {
        if (std::filesystem::exists(path)) throw std::runtime_error("文件已存在，导出不会覆盖已有主题");
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        const auto id = path.stem().string();
        if (find(id)) throw std::runtime_error("导出 ID 已存在");
        const auto text = serialize(activeThemeDefinition(), id);
        UiThemeDefinition checked;
        if (!parse(text, path, checked, error)) return false;
        std::ofstream output(path);
        output << text << '\n';
        output.close();
        if (!output) throw std::runtime_error("写入失败");
        return true;
    } catch (const std::exception& exception) { error = path.string() + ": " + exception.what(); }
    return false;
}
} // namespace protoscope::ui
