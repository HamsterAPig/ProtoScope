#pragma once

#include "protoscope/app/application.hpp"
#include "protoscope/config/config.hpp"

#include <cstdint>
#include <string>

struct GLFWwindow;

namespace protoscope::ui {

class GuiRuntime {
public:
    GuiRuntime(app::Application& application, const config::ConfigStore& configStore);
    ~GuiRuntime();

    bool initialize();
    int run();
    void shutdown();

private:
    bool initializeWindow();
    bool initializeImGui();
    bool initializePlotContext();
    void shutdownImGui();
    void shutdownPlotContext();
    void shutdownWindow();

    void ensureChineseFont();
    void renderFrame();
    void drawStatusBar();
    void drawCommDock();
    void drawProtocolDock();
    void drawSendDock();
    void drawReceiveDock();
    void drawWaveDock();
    void drawDynamicControl(const scripting::ControlSnapshot& control);

    bool reloadConfigFromDisk();
    bool pollConfigFileChanges();
    bool maybeAutoSave();
    void sleepUntilNextFrame(std::uint64_t frameStartMs) const;

    static std::uint64_t nowMs();

private:
    app::Application& application_;
    const config::ConfigStore& configStore_;
    GLFWwindow* window_{nullptr};
    std::uint64_t lastRenderAtMs_{0};
    std::uint64_t lastAutoSaveAtMs_{0};
    config::FileSnapshot configSnapshot_{};
    bool layoutInitialized_{false};
    bool running_{false};
};

} // namespace protoscope::ui
