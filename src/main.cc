#include "protoscope/app/application.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/ui/gui_runtime.hpp"

namespace {

int runProtoScope()
{
    protoscope::app::Application app;
    if (!app.initialize()) {
        app.logger().error("main", "ProtoScope 初始化失败");
        return 1;
    }

    protoscope::config::ConfigStore configStore;
    protoscope::ui::GuiRuntime runtime(app, configStore);
    if (!runtime.initialize()) {
        app.logger().error("main", "ProtoScope GUI 初始化失败");
        app.shutdown();
        return 1;
    }

    const int exitCode = runtime.run();
    runtime.shutdown();
    app.shutdown();
    return exitCode;
}

} // namespace

int main()
{
    return runProtoScope();
}

#if defined(_WIN32)
#include <windows.h>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    return runProtoScope();
}
#endif
