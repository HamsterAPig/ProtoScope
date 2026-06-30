#pragma once

#include "protoscope/config/config.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace protoscope::app {

struct StartupCommandLine {
    bool diagnose{false};
    std::optional<config::GuiRendererBackend> rendererBackend;
    std::optional<std::string> rendererArgument;
    std::string error;
    std::vector<std::string> argv;
};

struct DiagnosticsLogPathAttempt {
    std::filesystem::path directory;
    bool writable{false};
    std::string error;
};

struct DiagnosticsLogPathResult {
    std::filesystem::path path;
    std::vector<DiagnosticsLogPathAttempt> attempts;
};

struct DiagnosticsPathCandidates {
    std::filesystem::path exeLogDir;
    std::filesystem::path localAppDataLogDir;
    std::filesystem::path tempLogDir;
};

struct StartupDiagnosticsOptions {
    bool diagnose{false};
    std::string version{"unknown"};
    std::vector<std::string> commandLine;
    std::filesystem::path exePath;
    std::filesystem::path currentDir;
    std::filesystem::path configPath;
    std::filesystem::path protocolRootDir;
    std::filesystem::path protocolSelectedDir;
    DiagnosticsPathCandidates pathCandidates;
};

struct StartupFailureInfo {
    std::string stage;
    std::string reason;
    std::filesystem::path logPath;
    std::string diagnosticsState;
};

class StartupDiagnosticsSink {
public:
    virtual ~StartupDiagnosticsSink() = default;

    virtual void setStage(std::string_view stage) = 0;
    virtual void logEvent(std::string_view stage, std::string_view message) = 0;
    virtual void logFailure(std::string_view stage, std::string_view reason) = 0;
};

StartupCommandLine parseStartupCommandLine(int argc, const char* const* argv);
StartupCommandLine parseStartupCommandLine(const std::vector<std::string>& argv);
DiagnosticsLogPathResult selectDiagnosticsLogPath(const DiagnosticsPathCandidates& candidates,
                                                  std::string_view fileName);
std::string formatStartupFatalMessage(const StartupFailureInfo& failure);
StartupDiagnosticsOptions makeDefaultStartupDiagnosticsOptions(bool diagnose);

class StartupDiagnostics final : public StartupDiagnosticsSink {
public:
    explicit StartupDiagnostics(StartupDiagnosticsOptions options);
    ~StartupDiagnostics() override;

    void setStage(std::string_view stage) override;
    void logEvent(std::string_view stage, std::string_view message) override;
    void logFailure(std::string_view stage, std::string_view reason) override;

    void logCommandLineParsed(const StartupCommandLine& commandLine);
    void completeStage(std::string_view stage);
    void writeCrash(std::string_view reason, std::optional<unsigned long> exceptionCode = std::nullopt);
    bool writeCrashDump(void* exceptionPointers);

    [[nodiscard]] bool diagnoseEnabled() const;
    [[nodiscard]] const std::filesystem::path& logPath() const;
    [[nodiscard]] const std::string& currentStage() const;
    [[nodiscard]] std::string logWriteState() const;

private:
    void ensureLogOpen();
    void writeHeader();
    void writePathAttempts(std::ostream& out);
    void writeProcessStart(std::ostream& out);
    void writeLine(std::string_view line);
    [[nodiscard]] double elapsedMs() const;

    StartupDiagnosticsOptions options_;
    DiagnosticsLogPathResult logPath_;
    std::chrono::steady_clock::time_point startedAt_;
    std::string currentStage_{"process_start"};
    bool headerWritten_{false};
    std::size_t appendOpenFailureCount_{0};
    std::string lastAppendOpenError_;
    bool appendFailurePending_{false};
};

} // namespace protoscope::app
