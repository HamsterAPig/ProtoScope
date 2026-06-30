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
    showStartupFailureMessage({.stage = std::move(stage),
                               .reason = std::move(reason),
                               .logPath = diagnostics.logPath(),
                               .diagnosticsState = diagnostics.logWriteState()});
    return 1;
}

int runProtoScope(const protoscope::app::StartupCommandLine& commandLine)
{
    auto diagnosticsOptions = protoscope::app::makeDefaultStartupDiagnosticsOptions(commandLine.diagnose);
    diagnosticsOptions.commandLine = commandLine.argv;
    protoscope::app::StartupDiagnostics diagnostics(std::move(diagnosticsOptions));
    // 崩溃处理器安装前后都落盘，方便判断是否死在异常处理器安装阶段。
    diagnostics.logCommandLineParsed(commandLine);
    diagnostics.setStage("before_crash_handlers");
    installCrashHandlers(diagnostics);
    diagnostics.setStage("after_crash_handlers");

    try {
        if (!commandLine.error.empty()) {
            return failStartup(diagnostics, "parseStartupCommandLine", commandLine.error);
        }

        diagnostics.setStage("before_application_construct");
        protoscope::app::Application app;
        diagnostics.setStage("after_application_construct");

        diagnostics.setStage("before_application_initialize");
        if (!app.initialize()) {
            app.logger().error("main", "ProtoScope 初始化失败");
            return failStartup(diagnostics, "Application::initialize", "Application::initialize 返回失败", &app);
        }
        diagnostics.setStage("after_application_initialize");

        const auto rendererBackend =
            commandLine.rendererBackend.value_or(app.runtimeConfig().gui.rendererBackend);
        const std::string rendererSource = commandLine.rendererBackend.has_value() ? "cli" : "yaml_or_default";
        diagnostics.logEvent(
            "GuiRuntime::rendererBackend",
            "renderer_backend=" + std::string(protoscope::config::guiRendererBackendId(rendererBackend)) +
                ", source=" + rendererSource);
        app.logger().info(
            "main",
            "GUI 渲染后端: " + std::string(protoscope::config::guiRendererBackendId(rendererBackend)) +
                " (" + rendererSource + ")");

        protoscope::config::ConfigStore configStore;
        protoscope::ui::GuiRuntime runtime(
            app, configStore, protoscope::ui::GuiRuntimeOptions{.rendererBackend = rendererBackend}, &diagnostics);
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
        diagnostics.logFailure("exception",
                               "stage=" + diagnostics.currentStage() + ", what=" + std::string(ex.what()));
        return failStartup(diagnostics, diagnostics.currentStage(), ex.what());
    } catch (...) {
        diagnostics.logFailure("exception", "stage=" + diagnostics.currentStage() + ", what=unknown exception");
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
