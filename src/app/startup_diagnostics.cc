#include "protoscope/app/startup_diagnostics.hpp"

#include "protoscope/build/version.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <dbghelp.h>
#endif

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <ostream>
#include <sstream>
#include <string>
#include <system_error>
#include <thread>

namespace protoscope::app {

namespace {

std::mutex g_logMutex;

std::string pathText(const std::filesystem::path& path)
{
    return path.empty() ? std::string("<empty>") : path.generic_string();
}

std::string nowText()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &timeValue);
#else
    localtime_r(&timeValue, &localTm);
#endif

    std::ostringstream out;
    out << std::put_time(&localTm, "%Y-%m-%d %H:%M:%S");
    return out.str();
}

std::string timestampFileName()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(now);
    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &timeValue);
#else
    localtime_r(&timeValue, &localTm);
#endif

    std::ostringstream out;
    out << "startup-diagnostics-" << std::put_time(&localTm, "%Y%m%d-%H%M%S") << ".log";
    return out.str();
}

bool probeWritableDirectory(const std::filesystem::path& directory, std::string& error)
{
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        error = ec.message();
        return false;
    }

    const auto probePath = directory / ".protoscope-startup-write-test";
    {
        std::ofstream probe(probePath, std::ios::binary | std::ios::trunc);
        if (!probe.good()) {
            error = "open probe file failed";
            return false;
        }
        probe << "probe\n";
        if (!probe.good()) {
            error = "write probe file failed";
            return false;
        }
    }

    std::filesystem::remove(probePath, ec);
    error.clear();
    return true;
}

std::filesystem::path executablePath()
{
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    while (size == buffer.size()) {
        buffer.resize(buffer.size() * 2);
        size = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    }
    if (size > 0) {
        buffer.resize(size);
        return std::filesystem::path(buffer);
    }
#endif
    return std::filesystem::current_path() / "ProtoScope";
}

std::filesystem::path currentDirectory()
{
    std::error_code ec;
    auto path = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path{} : path;
}

std::filesystem::path envPath(const char* name)
{
#if defined(_WIN32)
    std::size_t requiredSize = 0;
    getenv_s(&requiredSize, nullptr, 0, name);
    if (requiredSize == 0) {
        return {};
    }
    std::string value(requiredSize, '\0');
    getenv_s(&requiredSize, value.data(), value.size(), name);
    if (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value.empty() ? std::filesystem::path{} : std::filesystem::path(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return {};
    }
    return std::filesystem::path(value);
#endif
}

std::string processArchitecture()
{
#if defined(_M_X64) || defined(__x86_64__)
    return "x64";
#elif defined(_M_IX86) || defined(__i386__)
    return "x86";
#elif defined(_M_ARM64) || defined(__aarch64__)
    return "arm64";
#else
    return "unknown";
#endif
}

std::string systemArchitecture()
{
#if defined(_WIN32)
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    switch (info.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            return "x64";
        case PROCESSOR_ARCHITECTURE_INTEL:
            return "x86";
        case PROCESSOR_ARCHITECTURE_ARM64:
            return "arm64";
        default:
            return "unknown";
    }
#else
    return processArchitecture();
#endif
}

std::string exceptionCodeText(unsigned long code)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << code;
    return out.str();
}

std::string currentThreadIdText()
{
#if defined(_WIN32)
    return std::to_string(GetCurrentThreadId());
#else
    std::ostringstream out;
    out << std::this_thread::get_id();
    return out.str();
#endif
}

std::string quoteCommandLineArg(std::string_view arg)
{
    if (arg.empty()) {
        return "\"\"";
    }

    if (arg.find_first_of(" \t\r\n\"") == std::string_view::npos) {
        return std::string(arg);
    }

    std::string quoted;
    quoted.reserve(arg.size() + 2);
    quoted.push_back('"');
    for (const char ch : arg) {
        if (ch == '"' || ch == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

std::string commandLineText(const std::vector<std::string>& argv)
{
    if (argv.empty()) {
        return "<empty>";
    }

    std::ostringstream out;
    for (std::size_t index = 0; index < argv.size(); ++index) {
        if (index > 0) {
            out << ' ';
        }
        out << quoteCommandLineArg(argv[index]);
    }
    return out.str();
}

std::string commandLineParsedText(const StartupCommandLine& commandLine)
{
    const std::string rendererSource = commandLine.rendererArgument.has_value() ? "cli" : "unset";
    const std::string rendererBackend = commandLine.rendererBackend.has_value()
                                            ? std::string(config::guiRendererBackendId(*commandLine.rendererBackend))
                                            : "<unset>";
    const std::string rendererValue = commandLine.rendererArgument.value_or("<unset>");

    std::ostringstream out;
    out << "diagnose=" << (commandLine.diagnose ? "true" : "false")
        << ", diagnose_renderer_probe=" << (commandLine.diagnoseRendererProbe ? "true" : "false")
        << ", renderer_backend=" << rendererBackend << ", renderer_source=" << rendererSource
        << ", renderer_value=" << rendererValue;
    if (!commandLine.error.empty()) {
        out << ", parse_error=" << commandLine.error;
    }
    out << ", command_line=" << commandLineText(commandLine.argv);
    return out.str();
}

} // namespace

StartupCommandLine parseStartupCommandLine(const int argc, const char* const* argv)
{
    StartupCommandLine options;
    options.argv.reserve(static_cast<std::size_t>((std::max)(argc, 0)));
    for (int index = 0; index < argc; ++index) {
        options.argv.push_back(argv[index] == nullptr ? std::string{} : std::string(argv[index]));
    }

    for (int index = 1; index < argc; ++index) {
        if (argv[index] == nullptr) {
            continue;
        }

        const std::string_view arg(argv[index]);
        if (arg == "--diagnose") {
            options.diagnose = true;
            continue;
        }
        if (arg == "--diagnose-renderer-probe") {
            options.diagnose = true;
            options.diagnoseRendererProbe = true;
            continue;
        }

        std::optional<std::string_view> rendererValue;
        if (arg == "--renderer") {
            if (index + 1 >= argc || argv[index + 1] == nullptr || std::string_view(argv[index + 1]).starts_with("--")) {
                options.error = "缺少 --renderer 参数值";
                continue;
            }
            rendererValue = std::string_view(argv[++index]);
        } else if (arg.starts_with("--renderer=")) {
            rendererValue = arg.substr(std::string_view("--renderer=").size());
            if (rendererValue->empty()) {
                options.error = "缺少 --renderer 参数值";
                continue;
            }
        }

        if (rendererValue.has_value()) {
            options.rendererArgument = std::string(*rendererValue);
            const auto backend = config::parseGuiRendererBackend(*rendererValue);
            if (!backend.has_value()) {
                options.error = "非法 --renderer 参数值: " + std::string(*rendererValue);
                continue;
            }
            options.rendererBackend = *backend;
        }
    }
    return options;
}

StartupCommandLine parseStartupCommandLine(const std::vector<std::string>& argv)
{
    std::vector<const char*> rawArgs;
    rawArgs.reserve(argv.size());
    for (const auto& arg : argv) {
        rawArgs.push_back(arg.c_str());
    }
    return parseStartupCommandLine(static_cast<int>(rawArgs.size()), rawArgs.data());
}

DiagnosticsLogPathResult selectDiagnosticsLogPath(const DiagnosticsPathCandidates& candidates, std::string_view fileName)
{
    DiagnosticsLogPathResult result;
    const std::filesystem::path fileNamePath{std::string(fileName)};
    for (const auto& directory : {candidates.exeLogDir, candidates.localAppDataLogDir, candidates.tempLogDir}) {
        if (directory.empty()) {
            result.attempts.push_back({.directory = directory, .writable = false, .error = "empty path"});
            continue;
        }

        std::string error;
        const bool writable = probeWritableDirectory(directory, error);
        result.attempts.push_back({.directory = directory, .writable = writable, .error = error});
        if (writable && result.path.empty()) {
            result.path = directory / fileNamePath;
        }
    }
    return result;
}

std::string formatStartupFatalMessage(const StartupFailureInfo& failure)
{
    std::ostringstream out;
    out << "ProtoScope 启动失败\n\n";
    out << "阶段: " << failure.stage << '\n';
    out << "原因: " << failure.reason << '\n';
    if (!failure.logPath.empty()) {
        out << "诊断日志: " << pathText(failure.logPath) << '\n';
    }
    if (!failure.diagnosticsState.empty()) {
        out << "诊断状态: " << failure.diagnosticsState << '\n';
    }
    return out.str();
}

StartupDiagnosticsOptions makeDefaultStartupDiagnosticsOptions(bool diagnose)
{
    StartupDiagnosticsOptions options;
    options.diagnose = diagnose;
    options.version = build::kVersion;
    options.exePath = executablePath();
    options.currentDir = currentDirectory();

    const auto exeDir = options.exePath.empty() ? currentDirectory() : options.exePath.parent_path();
    options.configPath = exeDir / "config" / "protoscope.yaml";
    options.protocolRootDir = exeDir / "protocols";
    options.protocolSelectedDir = exeDir / "protocols" / "templates" / "default_protocol";

    const auto localAppData = envPath("LOCALAPPDATA");
    const auto temp = std::filesystem::temp_directory_path();
    options.pathCandidates = DiagnosticsPathCandidates{
        .exeLogDir = exeDir / "logs",
        .localAppDataLogDir = localAppData.empty() ? std::filesystem::path{} : localAppData / "ProtoScope" / "logs",
        .tempLogDir = temp / "ProtoScope" / "logs",
    };
    return options;
}

StartupDiagnostics::StartupDiagnostics(StartupDiagnosticsOptions options)
    : options_(std::move(options)), startedAt_(std::chrono::steady_clock::now())
{
    if (options_.diagnose) {
        logPath_ = selectDiagnosticsLogPath(options_.pathCandidates, timestampFileName());
        ensureLogOpen();
        writeStateFile();
    }
}

StartupDiagnostics::~StartupDiagnostics()
{
    if (options_.diagnose && headerWritten_) {
        logEvent("process_exit", "诊断结束");
    }
}

void StartupDiagnostics::setStage(std::string_view stage)
{
    currentStage_ = std::string(stage);
    if (options_.diagnose) {
        logEvent(stage, "进入启动阶段");
        writeStateFile();
    }
}

void StartupDiagnostics::logEvent(std::string_view stage, std::string_view message)
{
    if (!options_.diagnose) {
        return;
    }
    ensureLogOpen();

    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << elapsedMs() << "ms"
        << " [" << stage << "] " << message;
    writeLine(stage, out.str());
}

void StartupDiagnostics::logCommandLineParsed(const StartupCommandLine& commandLine)
{
    if (!options_.diagnose) {
        return;
    }

    currentStage_ = "command_line_parsed";
    if (!commandLineParsedWritten_) {
        logEvent("command_line_parsed", commandLineParsedText(commandLine));
        commandLineParsedWritten_ = true;
    }
    writeStateFile();
}

void StartupDiagnostics::logFailure(std::string_view stage, std::string_view reason)
{
    if (!options_.diagnose) {
        return;
    }

    ensureLogOpen();

    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << elapsedMs() << "ms"
        << " [" << stage << "] FAILED: " << reason;
    writeLine(stage, out.str());
}

void StartupDiagnostics::completeStage(std::string_view stage)
{
    if (!options_.diagnose) {
        return;
    }

    std::ostringstream out;
    out << "阶段完成, elapsed_ms=" << std::fixed << std::setprecision(3) << elapsedMs();
    logEvent(stage, out.str());
}

void StartupDiagnostics::writeCrash(std::string_view reason, std::optional<unsigned long> exceptionCode)
{
    if (!options_.diagnose) {
        return;
    }

    ensureLogOpen();

    std::ostringstream out;
    out << "crash_reason=" << reason << ", stage=" << currentStage_ << ", thread_id=" << currentThreadIdText();
    if (exceptionCode.has_value()) {
        out << ", exception_code=" << exceptionCodeText(*exceptionCode);
    }
    const auto state = logWriteState();
    if (!state.empty()) {
        out << ", diagnostics_state=" << state;
    }
    writeLine("crash", out.str());
    writeStateFile();
}

bool StartupDiagnostics::writeCrashDump(void* exceptionPointers)
{
    if (!options_.diagnose || logPath_.path.empty()) {
        return false;
    }

#if defined(_WIN32)
    const auto dumpPath = logPath_.path.parent_path() / (logPath_.path.stem().generic_string() + ".dmp");
    HMODULE dbgHelp = LoadLibraryW(L"DbgHelp.dll");
    if (dbgHelp == nullptr) {
        logEvent("crash_dump", "DbgHelp.dll 加载失败");
        return false;
    }

    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE,
                                              DWORD,
                                              HANDLE,
                                              MINIDUMP_TYPE,
                                              PMINIDUMP_EXCEPTION_INFORMATION,
                                              PMINIDUMP_USER_STREAM_INFORMATION,
                                              PMINIDUMP_CALLBACK_INFORMATION);
    auto* miniDumpWriteDump =
        reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbgHelp, "MiniDumpWriteDump"));
    if (miniDumpWriteDump == nullptr) {
        FreeLibrary(dbgHelp);
        logEvent("crash_dump", "MiniDumpWriteDump 不可用");
        return false;
    }

    HANDLE dumpFile = CreateFileW(dumpPath.wstring().c_str(),
                                  GENERIC_WRITE,
                                  0,
                                  nullptr,
                                  CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (dumpFile == INVALID_HANDLE_VALUE) {
        FreeLibrary(dbgHelp);
        logEvent("crash_dump", "创建 dmp 文件失败");
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
    exceptionInfo.ThreadId = GetCurrentThreadId();
    exceptionInfo.ExceptionPointers = static_cast<EXCEPTION_POINTERS*>(exceptionPointers);
    exceptionInfo.ClientPointers = FALSE;

    const BOOL ok = miniDumpWriteDump(GetCurrentProcess(),
                                      GetCurrentProcessId(),
                                      dumpFile,
                                      MiniDumpNormal,
                                      exceptionPointers == nullptr ? nullptr : &exceptionInfo,
                                      nullptr,
                                      nullptr);
    CloseHandle(dumpFile);
    FreeLibrary(dbgHelp);
    logEvent("crash_dump", ok ? "已写入 dmp: " + pathText(dumpPath) : "写入 dmp 失败");
    return ok == TRUE;
#else
    (void) exceptionPointers;
    return false;
#endif
}

bool StartupDiagnostics::diagnoseEnabled() const
{
    return options_.diagnose;
}

const std::filesystem::path& StartupDiagnostics::logPath() const
{
    return logPath_.path;
}

std::filesystem::path StartupDiagnostics::statePath() const
{
    return logPath_.path.empty() ? std::filesystem::path{} : logPath_.path.parent_path() / "diagnostics-state.txt";
}

const std::string& StartupDiagnostics::currentStage() const
{
    return currentStage_;
}

std::string StartupDiagnostics::logWriteState() const
{
    if (appendOpenFailureCount_ == 0 && lastAppendOpenError_.empty()) {
        return {};
    }

    std::ostringstream out;
    out << "append_open_failed_count=" << appendOpenFailureCount_;
    if (!lastAppendOpenError_.empty()) {
        out << ", last_append_error=" << lastAppendOpenError_;
    }
    if (!lastAppendStage_.empty()) {
        out << ", last_append_stage=" << lastAppendStage_;
    }
    return out.str();
}

void StartupDiagnostics::ensureLogOpen()
{
    if (headerWritten_) {
        return;
    }
    writeHeader();
}

void StartupDiagnostics::writeHeader()
{
    if (logPath_.path.empty() && logPath_.attempts.empty()) {
        logPath_ = selectDiagnosticsLogPath(options_.pathCandidates, timestampFileName());
    }
    if (logPath_.path.empty()) {
        headerWritten_ = true;
        return;
    }

    std::error_code ec;
    std::filesystem::create_directories(logPath_.path.parent_path(), ec);

    std::ofstream out(logPath_.path, std::ios::out | std::ios::trunc);
    if (!out.good()) {
        headerWritten_ = true;
        ++appendOpenFailureCount_;
        lastAppendOpenError_ = "header_open_failed";
        lastAppendStage_ = currentStage_;
        appendFailurePending_ = true;
        writeStateFile();
        return;
    }

    out << "ProtoScope startup diagnostics\n";
    out << "created_at: " << nowText() << '\n';
    out << "version: " << options_.version << '\n';
    out << "diagnose: " << (options_.diagnose ? "true" : "false") << '\n';
    out << "process_arch: " << processArchitecture() << '\n';
    out << "system_arch: " << systemArchitecture() << '\n';
    out << "selected_log_path: " << pathText(logPath_.path) << '\n';
    out << "command_line: " << commandLineText(options_.commandLine) << '\n';
    out << "exe_path: " << pathText(options_.exePath) << '\n';
    out << "current_dir: " << pathText(options_.currentDir) << '\n';
    out << "expected_config_path: " << pathText(options_.configPath) << '\n';
    out << "expected_protocol_root: " << pathText(options_.protocolRootDir) << '\n';
    out << "expected_protocol_selected: " << pathText(options_.protocolSelectedDir) << '\n';
    // 启动早期最容易崩溃，header、路径探测和 process_start 必须同一次打开写完。
    writePathAttempts(out);
    writeProcessStart(out);
    if (!options_.commandLine.empty()) {
        writeCommandLineParsed(out, parseStartupCommandLine(options_.commandLine));
        commandLineParsedWritten_ = true;
        currentStage_ = "command_line_parsed";
    }
    out.flush();
    if (!out.good()) {
        ++appendOpenFailureCount_;
        lastAppendOpenError_ = "header_flush_failed";
        lastAppendStage_ = currentStage_;
        appendFailurePending_ = true;
    }

    headerWritten_ = true;
    writeStateFile();
}

void StartupDiagnostics::writePathAttempts(std::ostream& out)
{
    for (const auto& attempt : logPath_.attempts) {
        out << "log_path_attempt: dir=" << pathText(attempt.directory)
            << ", writable=" << (attempt.writable ? "true" : "false");
        if (!attempt.error.empty()) {
            out << ", error=" << attempt.error;
        }
        out << '\n';
    }
}

void StartupDiagnostics::writeProcessStart(std::ostream& out)
{
    out << std::fixed << std::setprecision(3) << elapsedMs() << "ms"
        << " [process_start] 诊断模式已启用\n";
}

void StartupDiagnostics::writeCommandLineParsed(std::ostream& out, const StartupCommandLine& commandLine)
{
    out << std::fixed << std::setprecision(3) << elapsedMs() << "ms"
        << " [command_line_parsed] " << commandLineParsedText(commandLine) << '\n';
}

void StartupDiagnostics::writeStateFile()
{
    if (!options_.diagnose) {
        return;
    }
    const auto path = statePath();
    if (path.empty()) {
        return;
    }

    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.good()) {
        return;
    }
    out << "last_stage: " << currentStage_ << '\n';
    out << "main_log_path: " << pathText(logPath_.path) << '\n';
    out << "append_open_failed_count: " << appendOpenFailureCount_ << '\n';
    if (!lastAppendOpenError_.empty()) {
        out << "last_append_error: " << lastAppendOpenError_ << '\n';
    }
    if (!lastAppendStage_.empty()) {
        out << "last_append_stage: " << lastAppendStage_ << '\n';
    }
    out.flush();
}

void StartupDiagnostics::writeLine(std::string_view stage, std::string_view line)
{
    if (logPath_.path.empty()) {
        return;
    }

    std::lock_guard lock(g_logMutex);
    std::ofstream out(logPath_.path, std::ios::out | std::ios::app);
    if (!out.good()) {
        ++appendOpenFailureCount_;
        lastAppendOpenError_ = "append_open_failed";
        lastAppendStage_ = std::string(stage);
        appendFailurePending_ = true;
        writeStateFile();
        return;
    }

    if (appendFailurePending_) {
        out << "append_open_failed: count=" << appendOpenFailureCount_;
        if (!lastAppendOpenError_.empty()) {
            out << ", last_error=" << lastAppendOpenError_;
        }
        if (!lastAppendStage_.empty()) {
            out << ", last_stage=" << lastAppendStage_;
        }
        out << '\n';
        appendFailurePending_ = false;
    }

    out << line << '\n';
    out.flush();
    if (!out.good()) {
        ++appendOpenFailureCount_;
        lastAppendOpenError_ = "append_write_failed";
        lastAppendStage_ = std::string(stage);
        appendFailurePending_ = true;
        writeStateFile();
    }
}

double StartupDiagnostics::elapsedMs() const
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - startedAt_).count();
}

} // namespace protoscope::app
