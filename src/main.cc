#include "protoscope/app/application.hpp"
#include "protoscope/app/startup_diagnostics.hpp"
#include "protoscope/config/config.hpp"
#include "protoscope/ui/gui_runtime.hpp"

#if defined(_WIN32)
#include "protoscope/app/windows_args.hpp"

#include <shellapi.h>
#include <windows.h>
#endif

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cwchar>
#include <exception>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

protoscope::app::StartupDiagnostics* g_startupDiagnostics = nullptr;
protoscope::app::EarlyStartupDiagnostics* g_earlyStartupDiagnostics = nullptr;

#if defined(_WIN32)
const char* g_windowsEntryStage = "process_start";

void setWindowsEntryStage(const char* stage)
{
    g_windowsEntryStage = stage == nullptr ? "unknown" : stage;
}

void appendAscii(char* buffer, DWORD capacity, DWORD& offset, const char* text)
{
    if (buffer == nullptr || text == nullptr || capacity == 0) {
        return;
    }
    while (*text != '\0' && offset + 1 < capacity) {
        buffer[offset++] = *text++;
    }
    buffer[offset] = '\0';
}

void appendHex(char* buffer, DWORD capacity, DWORD& offset, unsigned long value)
{
    constexpr char kHex[] = "0123456789ABCDEF";
    appendAscii(buffer, capacity, offset, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        if (offset + 1 >= capacity) {
            return;
        }
        buffer[offset++] = kHex[(value >> shift) & 0x0FUL];
    }
    buffer[offset] = '\0';
}

bool appendWide(wchar_t* buffer, DWORD capacity, const wchar_t* suffix)
{
    if (buffer == nullptr || suffix == nullptr || capacity == 0) {
        return false;
    }
    const DWORD baseLength = lstrlenW(buffer);
    const DWORD suffixLength = lstrlenW(suffix);
    if (baseLength + suffixLength >= capacity) {
        return false;
    }
    for (DWORD index = 0; index <= suffixLength; ++index) {
        buffer[baseLength + index] = suffix[index];
    }
    return true;
}

bool makeDirectoryIfNeeded(const wchar_t* directory)
{
    if (CreateDirectoryW(directory, nullptr) != FALSE) {
        return true;
    }
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

HANDLE openWindowsEntryFallbackLog()
{
    wchar_t logPath[MAX_PATH]{};
    DWORD size = GetModuleFileNameW(nullptr, logPath, MAX_PATH);
    if (size > 0 && size < MAX_PATH) {
        for (DWORD index = size; index > 0; --index) {
            const wchar_t ch = logPath[index - 1];
            if (ch == L'\\' || ch == L'/') {
                logPath[index - 1] = L'\0';
                break;
            }
        }
        if (appendWide(logPath, MAX_PATH, L"\\logs") && makeDirectoryIfNeeded(logPath) &&
            appendWide(logPath, MAX_PATH, L"\\startup-early.log")) {
            HANDLE file = CreateFileW(
                logPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                return file;
            }
        }
    }

    wchar_t tempPath[MAX_PATH]{};
    size = GetTempPathW(MAX_PATH, tempPath);
    if (size == 0 || size >= MAX_PATH) {
        return INVALID_HANDLE_VALUE;
    }
    if (!appendWide(tempPath, MAX_PATH, L"ProtoScope")) {
        return INVALID_HANDLE_VALUE;
    }
    makeDirectoryIfNeeded(tempPath);
    if (!appendWide(tempPath, MAX_PATH, L"\\logs")) {
        return INVALID_HANDLE_VALUE;
    }
    makeDirectoryIfNeeded(tempPath);
    if (!appendWide(tempPath, MAX_PATH, L"\\startup-early.log")) {
        return INVALID_HANDLE_VALUE;
    }
    return CreateFileW(tempPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

int writeWindowsEntrySehFallback(unsigned long exceptionCode)
{
    HANDLE file = openWindowsEntryFallbackLog();
    if (file != INVALID_HANDLE_VALUE) {
        char buffer[512]{};
        DWORD offset = 0;
        appendAscii(buffer, static_cast<DWORD>(sizeof(buffer)), offset, "ProtoScope early startup diagnostics\n");
        appendAscii(buffer, static_cast<DWORD>(sizeof(buffer)), offset, "created_by: windows_entry_seh_fallback\n");
        appendAscii(buffer, static_cast<DWORD>(sizeof(buffer)), offset, "stage: ");
        appendAscii(buffer, static_cast<DWORD>(sizeof(buffer)), offset, g_windowsEntryStage);
        appendAscii(buffer, static_cast<DWORD>(sizeof(buffer)), offset, "\nseh_code: ");
        appendHex(buffer, static_cast<DWORD>(sizeof(buffer)), offset, exceptionCode);
        appendAscii(buffer, static_cast<DWORD>(sizeof(buffer)), offset, "\n");

        DWORD written = 0;
        WriteFile(file, buffer, offset, &written, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

class StartupDiagnosticsRegistration {
public:
    explicit StartupDiagnosticsRegistration(protoscope::app::StartupDiagnostics& diagnostics)
        : previous_(g_startupDiagnostics)
    {
        g_startupDiagnostics = &diagnostics;
    }

    ~StartupDiagnosticsRegistration()
    {
        g_startupDiagnostics = previous_;
    }

private:
    protoscope::app::StartupDiagnostics* previous_{nullptr};
};

std::string windowsExceptionCodeText(unsigned long code)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << code;
    return out.str();
}

void writeCrashRecord(std::string reason, std::optional<unsigned long> exceptionCode = std::nullopt)
{
    if (g_earlyStartupDiagnostics != nullptr) {
        g_earlyStartupDiagnostics->writeCrash(reason, exceptionCode);
    }
    if (g_startupDiagnostics != nullptr) {
        g_startupDiagnostics->writeCrash(reason, exceptionCode);
    }
}

void writeSignalCrash(int signalValue)
{
    writeCrashRecord("signal " + std::to_string(signalValue));
    std::_Exit(128 + signalValue);
}

void showStartupFailureMessage(const protoscope::app::StartupFailureInfo& failure)
{
#if defined(_WIN32)
    const auto message = protoscope::app::formatStartupFatalMessage(failure);
    MessageBoxA(nullptr, message.c_str(), "ProtoScope 启动失败", MB_OK | MB_ICONERROR);
#else
    (void) failure;
#endif
}

int failEarlyStartup(protoscope::app::EarlyStartupDiagnostics& diagnostics,
                     std::string reason,
                     std::optional<unsigned long> exceptionCode = std::nullopt)
{
    diagnostics.writeCrash(reason, exceptionCode);
    showStartupFailureMessage(diagnostics.failureInfo(reason));
    return 1;
}

void installCrashHandlers(protoscope::app::EarlyStartupDiagnostics& diagnostics)
{
    g_earlyStartupDiagnostics = &diagnostics;
    std::signal(SIGABRT, writeSignalCrash);
    std::signal(SIGFPE, writeSignalCrash);
    std::signal(SIGILL, writeSignalCrash);
    std::signal(SIGSEGV, writeSignalCrash);
    std::signal(SIGTERM, writeSignalCrash);

    std::set_terminate([] {
        std::string reason = "std::terminate";
        if (const auto exception = std::current_exception()) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& ex) {
                reason += ": ";
                reason += ex.what();
            } catch (...) {
                reason += ": unknown exception";
            }
        }
        writeCrashRecord(reason);
        if (g_earlyStartupDiagnostics != nullptr) {
            showStartupFailureMessage(g_earlyStartupDiagnostics->failureInfo(reason));
        }
        std::abort();
    });

#if defined(_WIN32)
    AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* exceptionPointers) -> LONG {
        if (g_earlyStartupDiagnostics != nullptr || g_startupDiagnostics != nullptr) {
            const auto code = exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
                                  ? exceptionPointers->ExceptionRecord->ExceptionCode
                                  : 0UL;
            // Windows 会用一阶异常承载 C++ EH 和调试线程名事件，这些不是崩溃证据。
            if (code == 0xE06D7363UL || code == 0x406D1388UL) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            writeCrashRecord("windows vectored exception", code);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    });
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* exceptionPointers) -> LONG {
        const auto code = exceptionPointers != nullptr && exceptionPointers->ExceptionRecord != nullptr
                              ? exceptionPointers->ExceptionRecord->ExceptionCode
                              : 0UL;
        writeCrashRecord("windows unhandled exception", code);
        if (g_startupDiagnostics != nullptr) {
            g_startupDiagnostics->writeCrashDump(exceptionPointers);
        }
        if (g_earlyStartupDiagnostics != nullptr) {
            const auto reason = "windows unhandled exception, exception_code=" + windowsExceptionCodeText(code);
            showStartupFailureMessage(
                g_earlyStartupDiagnostics->failureInfo(reason));
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

bool hasDiagnosticsArg(const std::vector<std::string>& args)
{
    return std::any_of(args.begin(), args.end(), [](const std::string& arg) {
        return arg == "--diagnose" || arg == "--diagnose-renderer-probe";
    });
}

std::vector<std::string> collectArgs(int argc, char** argv)
{
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>((std::max)(argc, 0)));
    for (int index = 0; index < argc; ++index) {
        args.push_back(argv[index] == nullptr ? std::string{} : std::string(argv[index]));
    }
    return args;
}

#if defined(_WIN32)
bool hasDiagnosticsArg(PCWSTR commandLine)
{
    return commandLine != nullptr &&
           (std::wcsstr(commandLine, L"--diagnose") != nullptr ||
            std::wcsstr(commandLine, L"--diagnose-renderer-probe") != nullptr);
}
#endif

int runProtoScope(const protoscope::app::StartupCommandLine& commandLine,
                  protoscope::app::EarlyStartupDiagnostics& earlyDiagnostics)
{
    earlyDiagnostics.setStage("before_startup_diagnostics_construct");
    auto diagnosticsOptions = protoscope::app::makeDefaultStartupDiagnosticsOptions(commandLine.diagnose);
    diagnosticsOptions.commandLine = commandLine.argv;
    protoscope::app::StartupDiagnostics diagnostics(std::move(diagnosticsOptions));
    StartupDiagnosticsRegistration diagnosticsRegistration(diagnostics);
    earlyDiagnostics.setStage("after_startup_diagnostics_construct");
    diagnostics.logCommandLineParsed(commandLine);
    diagnostics.setStage("after_crash_handlers");

    try {
        if (!commandLine.error.empty()) {
            return failStartup(diagnostics, "parseStartupCommandLine", commandLine.error);
        }
        if (commandLine.diagnoseRendererProbe) {
            diagnostics.setStage("diagnose_renderer_probe");
            const bool ok = protoscope::ui::runRendererProbe(&diagnostics);
            return ok ? 0 : failStartup(diagnostics, "diagnose_renderer_probe", "renderer probe 失败");
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
        const std::string rendererSource =
            commandLine.rendererBackend.has_value() ? "cli" : (app.loadedConfigFromDisk() ? "yaml" : "default");
        diagnostics.logEvent(
            "GuiRuntime::rendererBackend",
            "renderer_backend=" + std::string(protoscope::config::guiRendererBackendId(rendererBackend)) +
                ", source=" + rendererSource);
        app.logger().info(
            "main",
            "GUI 渲染后端: " + std::string(protoscope::config::guiRendererBackendId(rendererBackend)) +
                " (" + rendererSource + ")");

        protoscope::config::ConfigStore configStore;
        diagnostics.setStage("before_gui_runtime_construct");
        protoscope::ui::GuiRuntime runtime(
            app, configStore, protoscope::ui::GuiRuntimeOptions{.rendererBackend = rendererBackend}, &diagnostics);
        diagnostics.setStage("before_gui_runtime_initialize");
        if (!runtime.initialize()) {
            app.logger().error("main", "ProtoScope GUI 初始化失败");
            return failStartup(
                diagnostics, "GuiRuntime::initialize", "GuiRuntime::initialize 返回失败", &app, &runtime);
        }
        diagnostics.setStage("after_gui_runtime_initialize");

        diagnostics.setStage("GuiRuntime::run");
        const int exitCode = runtime.run();
        diagnostics.completeStage("GuiRuntime::run");
        runtime.shutdown();
        app.shutdown();
        return exitCode;
    } catch (const std::exception& ex) {
        earlyDiagnostics.writeCrash("std::exception: " + std::string(ex.what()));
        diagnostics.logFailure("exception",
                               "stage=" + diagnostics.currentStage() + ", what=" + std::string(ex.what()));
        return failStartup(diagnostics, diagnostics.currentStage(), ex.what());
    } catch (...) {
        earlyDiagnostics.writeCrash("unknown exception");
        diagnostics.logFailure("exception", "stage=" + diagnostics.currentStage() + ", what=unknown exception");
        return failStartup(diagnostics, diagnostics.currentStage(), "unknown exception");
    }
}

int runWithEarlyDiagnostics(const std::vector<std::string>& args,
                            protoscope::app::EarlyStartupDiagnostics& earlyDiagnostics)
{
    try {
        earlyDiagnostics.setCommandLine(args);
        earlyDiagnostics.setStage("before_parse_command_line");
        const auto commandLine = protoscope::app::parseStartupCommandLine(args);
        earlyDiagnostics.setStage("after_parse_command_line");
        return runProtoScope(commandLine, earlyDiagnostics);
    } catch (const std::exception& ex) {
        return failEarlyStartup(earlyDiagnostics, "std::exception: " + std::string(ex.what()));
    } catch (...) {
        return failEarlyStartup(earlyDiagnostics, "unknown exception");
    }
}

} // namespace

int main(int argc, char** argv)
{
    const auto args = collectArgs(argc, argv);
    auto earlyOptions = protoscope::app::makeDefaultEarlyStartupDiagnosticsOptions(hasDiagnosticsArg(args));
    earlyOptions.commandLine = args;
    protoscope::app::EarlyStartupDiagnostics earlyDiagnostics(std::move(earlyOptions));
    earlyDiagnostics.setStage("before_crash_handlers");
    installCrashHandlers(earlyDiagnostics);
    earlyDiagnostics.setStage("after_crash_handlers");
    return runWithEarlyDiagnostics(args, earlyDiagnostics);
}

#if defined(_WIN32)
int runWindowsGuiEntry()
{
    setWindowsEntryStage("before_early_diagnostics_construct");
    auto earlyOptions =
        protoscope::app::makeDefaultEarlyStartupDiagnosticsOptions(hasDiagnosticsArg(GetCommandLineW()));
    protoscope::app::EarlyStartupDiagnostics earlyDiagnostics(std::move(earlyOptions));
    setWindowsEntryStage("before_crash_handlers");
    earlyDiagnostics.setStage("before_crash_handlers");
    installCrashHandlers(earlyDiagnostics);
    setWindowsEntryStage("after_crash_handlers");
    earlyDiagnostics.setStage("after_crash_handlers");

    setWindowsEntryStage("before_command_line_to_argv");
    int argc = 0;
    PWSTR* wideArgs = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (wideArgs == nullptr) {
        return failEarlyStartup(
            earlyDiagnostics, "CommandLineToArgvW failed, last_error=" + windowsExceptionCodeText(GetLastError()));
    }

    std::vector<std::string> args;
    try {
        setWindowsEntryStage("before_windows_args_utf8");
        earlyDiagnostics.setStage("before_windows_args_utf8");
        args.reserve(static_cast<std::size_t>((std::max)(argc, 0)));
        for (int index = 0; wideArgs != nullptr && index < argc; ++index) {
            args.push_back(protoscope::app::wideArgToUtf8(wideArgs[index]));
        }
        setWindowsEntryStage("after_windows_args_utf8");
        earlyDiagnostics.setStage("after_windows_args_utf8");
    } catch (const std::exception& ex) {
        if (wideArgs != nullptr) {
            LocalFree(wideArgs);
        }
        return failEarlyStartup(earlyDiagnostics, "std::exception: " + std::string(ex.what()));
    } catch (...) {
        if (wideArgs != nullptr) {
            LocalFree(wideArgs);
        }
        return failEarlyStartup(earlyDiagnostics, "unknown exception");
    }
    if (wideArgs != nullptr) {
        LocalFree(wideArgs);
    }
    setWindowsEntryStage("before_run_with_early_diagnostics");
    return runWithEarlyDiagnostics(args, earlyDiagnostics);
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
#if defined(_MSC_VER)
    __try {
        return runWindowsGuiEntry();
    } __except (writeWindowsEntrySehFallback(GetExceptionCode())) {
        return 1;
    }
#else
    return runWindowsGuiEntry();
#endif
}
#endif
