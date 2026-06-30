#include "test_helpers.hpp"
#include "test_registry.hpp"

#include "protoscope/app/startup_diagnostics.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string readTextFile(const std::filesystem::path& path)
{
    std::ifstream in(path);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

protoscope::app::StartupDiagnosticsOptions makeDiagnosticsOptions(const std::filesystem::path& root, bool diagnose)
{
    return {
        .diagnose = diagnose,
        .version = "test-version",
        .exePath = root / "ProtoScope.exe",
        .currentDir = root,
        .configPath = root / "config" / "protoscope.yaml",
        .protocolRootDir = root / "protocols",
        .protocolSelectedDir = root / "protocols" / "templates" / "default_protocol",
        .pathCandidates =
            {
                .exeLogDir = root / "logs",
                .localAppDataLogDir = root / "local" / "ProtoScope" / "logs",
                .tempLogDir = root / "temp" / "ProtoScope" / "logs",
            },
    };
}

} // namespace

void test_startup_diagnostics_parse_diagnose_arg()
{
    const auto parsed = protoscope::app::parseStartupCommandLine(std::vector<std::string>{"ProtoScope.exe", "--diagnose"});
    protoscope::tests::require(parsed.diagnose, "--diagnose should enable startup diagnostics");
}

void test_startup_diagnostics_parse_default_off()
{
    const auto parsed = protoscope::app::parseStartupCommandLine(std::vector<std::string>{"ProtoScope.exe"});
    protoscope::tests::require(!parsed.diagnose, "diagnostics should be disabled by default");
    protoscope::tests::require(!parsed.rendererBackend.has_value(), "renderer should be unset by default");
}

void test_startup_diagnostics_parse_renderer_equals()
{
    const auto parsed =
        protoscope::app::parseStartupCommandLine(std::vector<std::string>{"ProtoScope.exe", "--renderer=d3d11_warp"});
    protoscope::tests::require(parsed.rendererBackend == protoscope::config::GuiRendererBackend::D3D11Warp,
                               "--renderer=value should parse d3d11_warp");
    protoscope::tests::require(parsed.error.empty(), "valid renderer should not set parse error");
}

void test_startup_diagnostics_parse_renderer_space()
{
    const auto parsed =
        protoscope::app::parseStartupCommandLine(std::vector<std::string>{"ProtoScope.exe", "--renderer", "D3D11"});
    protoscope::tests::require(parsed.rendererBackend == protoscope::config::GuiRendererBackend::D3D11,
                               "--renderer value should parse d3d11 case-insensitively");
    protoscope::tests::require(parsed.error.empty(), "valid renderer should not set parse error");
}

void test_startup_diagnostics_parse_renderer_missing_value()
{
    const auto parsed =
        protoscope::app::parseStartupCommandLine(std::vector<std::string>{"ProtoScope.exe", "--renderer"});
    protoscope::tests::require(!parsed.error.empty(), "missing --renderer value should set parse error");
}

void test_startup_diagnostics_parse_renderer_invalid_value()
{
    const auto parsed =
        protoscope::app::parseStartupCommandLine(std::vector<std::string>{"ProtoScope.exe", "--renderer=metal"});
    protoscope::tests::require(!parsed.error.empty(), "invalid --renderer value should set parse error");
}

void test_startup_diagnostics_parse_renderer_with_diagnose()
{
    const auto parsed = protoscope::app::parseStartupCommandLine(
        std::vector<std::string>{"ProtoScope.exe", "--diagnose", "--renderer", "d3d11-warp"});
    protoscope::tests::require(parsed.diagnose, "--diagnose should coexist with --renderer");
    protoscope::tests::require(parsed.rendererBackend == protoscope::config::GuiRendererBackend::D3D11Warp,
                               "d3d11-warp should normalize to d3d11_warp");
    protoscope::tests::require(parsed.error.empty(), "valid diagnose + renderer args should not set parse error");
}

void test_startup_renderer_backend_priority()
{
    auto yamlConfig = protoscope::config::AppConfig{};
    yamlConfig.gui.rendererBackend = protoscope::config::GuiRendererBackend::D3D11;

    const auto noCli = protoscope::app::parseStartupCommandLine(std::vector<std::string>{"ProtoScope.exe"});
    const auto fromYaml = noCli.rendererBackend.value_or(yamlConfig.gui.rendererBackend);
    protoscope::tests::require(fromYaml == protoscope::config::GuiRendererBackend::D3D11,
                               "无 CLI 时应使用 YAML renderer_backend");

    const auto cli = protoscope::app::parseStartupCommandLine(
        std::vector<std::string>{"ProtoScope.exe", "--renderer=opengl"});
    const auto fromCli = cli.rendererBackend.value_or(yamlConfig.gui.rendererBackend);
    protoscope::tests::require(fromCli == protoscope::config::GuiRendererBackend::OpenGL,
                               "CLI renderer 应覆盖 YAML renderer_backend");

    const auto defaults = protoscope::config::AppConfig{};
    const auto fromDefault = noCli.rendererBackend.value_or(defaults.gui.rendererBackend);
    protoscope::tests::require(fromDefault == protoscope::config::GuiRendererBackend::OpenGL,
                               "无 CLI 且无 YAML 覆盖时应使用默认 opengl");
}

void test_startup_diagnostics_log_path_fallback()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-paths");
    const protoscope::tests::ScopedTempPath cleanup(root);

    const auto blockedExeLogs = root / "blocked-exe-logs";
    {
        std::ofstream out(blockedExeLogs);
        out << "not a directory";
    }

    const auto localLogs = root / "local" / "ProtoScope" / "logs";
    const auto tempLogs = root / "temp" / "ProtoScope" / "logs";
    const auto selected = protoscope::app::selectDiagnosticsLogPath(
        {.exeLogDir = blockedExeLogs, .localAppDataLogDir = localLogs, .tempLogDir = tempLogs},
        "startup.log");

    protoscope::tests::require(selected.path == localLogs / "startup.log", "log path should fall back to LocalAppData");
    protoscope::tests::require(selected.attempts.size() == 3, "all path candidates should be recorded");
    protoscope::tests::require(!selected.attempts[0].writable, "blocked exe log directory should fail probe");
    protoscope::tests::require(selected.attempts[1].writable, "LocalAppData log directory should be writable");
}

void test_startup_diagnostics_construct_with_diagnose_writes_process_start()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-process-start");
    const protoscope::tests::ScopedTempPath cleanup(root);

    protoscope::app::StartupDiagnostics diagnostics(makeDiagnosticsOptions(root, true));

    const auto report = readTextFile(diagnostics.logPath());
    protoscope::tests::require(report.find("[process_start] 诊断模式已启用") != std::string::npos,
                               "启用 diagnose 后构造函数应立即写入 process_start");
}

void test_startup_diagnostics_header_writes_selected_path_and_attempts()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-header-attempts");
    const protoscope::tests::ScopedTempPath cleanup(root);

    protoscope::app::StartupDiagnostics diagnostics(makeDiagnosticsOptions(root, true));

    const auto report = readTextFile(diagnostics.logPath());
    protoscope::tests::require(report.find("selected_log_path:") != std::string::npos,
                               "header 应写入最终 selected_log_path");
    protoscope::tests::require(report.find("log_path_attempt: dir=") != std::string::npos,
                               "header 同一次打开周期应写入 log_path_attempt");
    protoscope::tests::require(report.find("[process_start]") != std::string::npos,
                               "header 同一次打开周期应写入 process_start");
}

void test_startup_diagnostics_log_failure_no_diagnose_does_not_create_log()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-no-log-failure");
    const protoscope::tests::ScopedTempPath cleanup(root);

    protoscope::app::StartupDiagnostics diagnostics(makeDiagnosticsOptions(root, false));
    diagnostics.logFailure("test_stage", "test reason");

    std::error_code error;
    protoscope::tests::require(diagnostics.logPath().empty(), "未启用 diagnose 时 logFailure 不应选择日志路径");
    protoscope::tests::require(!std::filesystem::exists(root / "logs", error),
                               "未启用 diagnose 时 logFailure 不应创建 exe logs 目录");
}

void test_startup_diagnostics_write_crash_no_diagnose_does_not_create_log()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-no-crash");
    const protoscope::tests::ScopedTempPath cleanup(root);

    protoscope::app::StartupDiagnostics diagnostics(makeDiagnosticsOptions(root, false));
    diagnostics.writeCrash("test crash", 0xC0000005UL);

    std::error_code error;
    protoscope::tests::require(diagnostics.logPath().empty(), "未启用 diagnose 时 writeCrash 不应选择日志路径");
    protoscope::tests::require(!std::filesystem::exists(root / "logs", error),
                               "未启用 diagnose 时 writeCrash 不应创建 exe logs 目录");
}

void test_startup_diagnostics_set_stage_with_diagnose_appends_stage()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-set-stage");
    const protoscope::tests::ScopedTempPath cleanup(root);

    protoscope::app::StartupDiagnostics diagnostics(makeDiagnosticsOptions(root, true));
    diagnostics.setStage("Application::initialize");

    const auto report = readTextFile(diagnostics.logPath());
    protoscope::tests::require(report.find("[Application::initialize] 进入启动阶段") != std::string::npos,
                               "启用 diagnose 时 setStage 应正常追加阶段日志");
}

void test_startup_diagnostics_command_line_parsed_logs_renderer_cli()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-command-line");
    const protoscope::tests::ScopedTempPath cleanup(root);

    const auto parsed = protoscope::app::parseStartupCommandLine(
        std::vector<std::string>{"ProtoScope.exe", "--diagnose", "--renderer", "d3d11"});
    auto options = makeDiagnosticsOptions(root, true);
    options.commandLine = parsed.argv;
    protoscope::app::StartupDiagnostics diagnostics(std::move(options));
    diagnostics.logCommandLineParsed(parsed);

    const auto report = readTextFile(diagnostics.logPath());
    protoscope::tests::require(report.find("[command_line_parsed]") != std::string::npos,
                               "command_line_parsed 应写入诊断日志");
    protoscope::tests::require(report.find("diagnose=true") != std::string::npos,
                               "command_line_parsed 应记录 diagnose=true");
    protoscope::tests::require(report.find("renderer_backend=d3d11") != std::string::npos,
                               "command_line_parsed 应记录 CLI renderer 值");
    protoscope::tests::require(report.find("renderer_source=cli") != std::string::npos,
                               "command_line_parsed 应记录 renderer 来源为 cli");
}

void test_startup_diagnostics_log_failure_with_diagnose_writes_stage_reason()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-log-failure");
    const protoscope::tests::ScopedTempPath cleanup(root);

    protoscope::app::StartupDiagnostics diagnostics(makeDiagnosticsOptions(root, true));
    diagnostics.logFailure("test_stage", "test reason");

    const auto report = readTextFile(diagnostics.logPath());
    protoscope::tests::require(report.find("[test_stage] FAILED: test reason") != std::string::npos,
                               "启用 diagnose 时 logFailure 应写入 stage 和 reason");
}

void test_startup_diagnostics_report_omits_config_body()
{
    const auto root = protoscope::tests::makeUniqueTempDir("protoscope-diagnostics-report");
    const protoscope::tests::ScopedTempPath cleanup(root);

    const auto configPath = root / "config" / "protoscope.yaml";
    std::filesystem::create_directories(configPath.parent_path());
    {
        std::ofstream out(configPath);
        out << "communication:\n  serial:\n    port: COM_SECRET\n";
    }

    protoscope::app::StartupDiagnostics diagnostics({
        .diagnose = true,
        .version = "test-version",
        .exePath = root / "ProtoScope.exe",
        .currentDir = root,
        .configPath = configPath,
        .protocolRootDir = root / "protocols",
        .protocolSelectedDir = root / "protocols" / "templates" / "default_protocol",
        .pathCandidates =
            {
                .exeLogDir = root / "logs",
                .localAppDataLogDir = root / "local" / "ProtoScope" / "logs",
                .tempLogDir = root / "temp" / "ProtoScope" / "logs",
            },
    });
    diagnostics.logEvent("test", "只写环境和路径");

    const auto report = readTextFile(diagnostics.logPath());
    protoscope::tests::require(report.find("expected_config_path:") != std::string::npos,
                               "report should include expected config path");
    protoscope::tests::require(report.find("communication:") == std::string::npos,
                               "report should not include YAML config body");
    protoscope::tests::require(report.find("COM_SECRET") == std::string::npos,
                               "report should not include config values");
}

void test_startup_diagnostics_fatal_message_includes_stage_reason_log()
{
    const auto message = protoscope::app::formatStartupFatalMessage({
        .stage = "GuiRuntime::initialize",
        .reason = "GLFW 初始化失败",
        .logPath = "C:/Temp/ProtoScope/logs/startup.log",
    });

    protoscope::tests::require(message.find("GuiRuntime::initialize") != std::string::npos,
                               "fatal message should include stage");
    protoscope::tests::require(message.find("GLFW 初始化失败") != std::string::npos,
                               "fatal message should include reason");
    protoscope::tests::require(message.find("startup.log") != std::string::npos,
                               "fatal message should include log path");
}

void test_startup_diagnostics_fatal_message_omits_empty_log_path()
{
    const auto message = protoscope::app::formatStartupFatalMessage({
        .stage = "GuiRuntime::initialize",
        .reason = "GLFW 初始化失败",
        .logPath = {},
    });

    protoscope::tests::require(message.find("GuiRuntime::initialize") != std::string::npos,
                               "fatal message should include stage");
    protoscope::tests::require(message.find("GLFW 初始化失败") != std::string::npos,
                               "fatal message should include reason");
    protoscope::tests::require(message.find("诊断日志:") == std::string::npos,
                               "empty log path should omit diagnostics log field");
}
