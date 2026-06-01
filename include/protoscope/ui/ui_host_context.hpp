#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

struct GLFWwindow;

namespace protoscope::app { class Application; }
namespace protoscope::config { class ConfigStore; }

namespace protoscope::ui {

class WorkspaceController;

struct GuiRuntimeState {
    GLFWwindow* window{nullptr};
    bool running{false};
    bool showWaveDock{true};
    std::string activeWorkspaceProtocolKey{};
    std::optional<std::string> pendingProtocolDir{};
    bool pendingProtocolForceReload{false};
    std::unordered_map<std::string, bool> dockVisibility{};
    std::uint64_t lastRenderAtMs{0};
};

struct RuntimeUiContext {
    app::Application& application;
    const config::ConfigStore& configStore;
    GLFWwindow* window;
    GuiRuntimeState& runtimeState;
    WorkspaceController& workspace;
};

} // namespace protoscope::ui
