#pragma once

#include "protoscope/ui/ui_theme.hpp"

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

namespace protoscope::ui {

class ThemeManager {
public:
    ThemeManager();
    const std::vector<UiThemeDefinition>& themes() const { return themes_; }
    const std::filesystem::path& directory() const { return directory_; }
    const UiThemeDefinition* find(std::string_view id) const;
    void setConfigPath(const std::filesystem::path& configPath);
    bool reload(std::string& error);
    bool request(std::string_view id, std::string& error);
    void startup(std::string_view id, std::string& error);
    bool applyPending();
    bool exportCurrent(const std::filesystem::path& path, std::string& error) const;
    static bool parse(std::string_view yaml, const std::filesystem::path& source,
                      UiThemeDefinition& result, std::string& error);
    static std::string serialize(const UiThemeDefinition& definition, std::string_view id);

private:
    std::filesystem::path directory_;
    std::vector<UiThemeDefinition> themes_;
    std::optional<UiThemeDefinition> pending_;
};

} // namespace protoscope::ui
