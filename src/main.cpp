#include "protoscope/app/application.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/ui/gui_runtime.hpp"

#include <spdlog/spdlog.h>

int main() {
    protoscope::app::Application app;
    if (!app.initialize()) {
        spdlog::error("ProtoScope 初始化失败");
        return 1;
    }

    protoscope::config::ConfigStore configStore;
    protoscope::ui::GuiRuntime runtime(app, configStore);
    if (!runtime.initialize()) {
        spdlog::error("ProtoScope GUI 初始化失败");
        app.shutdown();
        return 1;
    }

    const int exitCode = runtime.run();
    runtime.shutdown();
    app.shutdown();
    return exitCode;
}
