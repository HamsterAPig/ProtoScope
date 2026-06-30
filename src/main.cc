#include "protoscope/app/application.hpp"
#include "protoscope/app/startup_diagnostics.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/ui/gui_runtime.hpp"

#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace {

protoscope::app::StartupDiagnostics* g_startupDiagnostics = nullptr;

void showStartupFailureMessage(const protoscope::app::StartupFailureInfo& failure)
{
#if defined(_WIN32)
    const auto message = protoscope::app::formatStartupFatalMessage(failure);
    MessageBoxA(nullptr, message.c_str(), "ProtoScope 启动失败", MB_OK | MB_ICONERROR);
#else
    (void) failure;
#endif
}

void installCrashHandlers(protoscope::app::StartupDiagnostics& diagnostics)
{
    g_startupDiagnostics = &diagnostics;
    std::set_terminate([] {
        if (g_startupDiagnostics != nullptr) {
            g_startupDiagnostics->writeCrash("std::terminate");
        }
        std::abort();
    });

#if defined(_WIN32)
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exceptionPointers) -> LONG {
        if (g_startupDiagnostics != nullptr) {
            const auto code = exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
                                  ? exceptionPointers->ExceptionRecord->ExceptionCode
                                  : 0UL;
            g_startupDiagnostics->writeCrash("windows unhandled exception", code);
            g_startupDiagnostics->writeCrashDump(exceptionPointers);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    });
#endif
}

int failStartup(protoscope::app::StartupDiagnostics& diagnostics,
                std::string stage,
                std::string reason,
                protoscope::app::Application* app = nullptr,
                protoscope::ui::GuiRuntime* runtime = nullptr)
{
    diagnostics.logFailure(stage, reason);
    if (runtime != nullptr) {
        runtime->shutdown();
    }
    if (app != nullptr) {
        app->shutdown();
    }
    showStartupFailureMessage({.stage = std::move(stage), .reason = std::move(reason), .logPath = diagnostics.logPath()});
    return 1;
}

int runProtoScope(const protoscope::app::StartupCommandLine& commandLine)
{
    protoscope::app::StartupDiagnostics diagnostics(
        protoscope::app::makeDefaultStartupDiagnosticsOptions(commandLine.diagnose));
    installCrashHandlers(diagnostics);

    try {
        protoscope::app::Application app;
        diagnostics.setStage("Application::initialize");
        if (!app.initialize()) {
            app.logger().error("main", "ProtoScope 初始化失败");
            return failStartup(diagnostics, "Application::initialize", "Application::initialize 返回失败", &app);
        }
        diagnostics.completeStage("Application::initialize");

        protoscope::config::ConfigStore configStore;
        protoscope::ui::GuiRuntime runtime(app, configStore, &diagnostics);
        diagnostics.setStage("GuiRuntime::initialize");
        if (!runtime.initialize()) {
            app.logger().error("main", "ProtoScope GUI 初始化失败");
            return failStartup(
                diagnostics, "GuiRuntime::initialize", "GuiRuntime::initialize 返回失败", &app, &runtime);
        }
        diagnostics.completeStage("GuiRuntime::initialize");

        diagnostics.setStage("GuiRuntime::run");
        const int exitCode = runtime.run();
        diagnostics.completeStage("GuiRuntime::run");
        runtime.shutdown();
        app.shutdown();
        return exitCode;
    } catch (const std::exception& ex) {
        return failStartup(diagnostics, diagnostics.currentStage(), ex.what());
    } catch (...) {
        return failStartup(diagnostics, diagnostics.currentStage(), "unknown exception");
    }
}

} // namespace

int main(int argc, char** argv)
{
    return runProtoScope(protoscope::app::parseStartupCommandLine(argc, argv));
}

#if defined(_WIN32)
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    PWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>((std::max)(argc, 0)));
    for (int index = 0; wideArgs != nullptr && index < argc; ++index) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, wideArgs[index], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(static_cast<std::size_t>((std::max)(size - 1, 0)), '\0');
        if (size > 0) {
            WideCharToMultiByte(CP_UTF8, 0, wideArgs[index], -1, arg.data(), size, nullptr, nullptr);
        }
        args.push_back(std::move(arg));
    }
    if (wideArgs != nullptr) {
        LocalFree(wideArgs);
    }
    return runProtoScope(protoscope::app::parseStartupCommandLine(args));
}
#endif
