#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace protoscope::ui {

class GuiRuntime;

class WorkspaceController {
public:
    explicit WorkspaceController(GuiRuntime& runtime);

    void requestProtocolWorkspaceSwitch(std::string protocolDir, bool forceReload);
    void processPendingProtocolWorkspaceSwitch();
    bool switchProtocolWorkspace(const std::string& protocolDir, bool forceReload);
    void loadCurrentProtocolWorkspace();
    void saveCurrentProtocolWorkspace();
    void resetCurrentProtocolWorkspaceLayout();
    void loadCurrentProtocolControlState();
    void saveCurrentProtocolControlState();
    void pruneCurrentLuaDockSettings();
    bool isLuaDockVisible(std::string_view stableId) const;
    bool setLuaDockVisible(std::string_view stableId, bool visible);
    void syncLuaDockVisibilityDefaults();

    std::filesystem::path currentProtocolLayoutPath() const;
    std::filesystem::path legacyProtocolLayoutPath() const;
    std::filesystem::path protocolControlStatePath() const;

private:
    GuiRuntime& runtime_;
};

} // namespace protoscope::ui
