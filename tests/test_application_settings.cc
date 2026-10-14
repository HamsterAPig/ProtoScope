#include "protoscope/app/application.hpp"
#include "protoscope/config/settings.hpp"
#include "protoscope/session/session_package.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <cmath>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace protoscope::app {
struct ApplicationSettingsTestAccess {
    static void transferActive(Application& app, bool active) { app.dataImportActive_ = active; }
    static void clearBatchOverride(Application& app) { app.runtimeConfig_.performance.explicitOverrides.workerBatchBytes = false; }
    static void failFence(Application& app, bool stopped = false) {
        app.pendingSettingsConfig_ = app.captureConfig();
        app.pendingSettingsBaseline_ = app.captureConfig();
        app.settingsApplyResult_ = {};
        app.settingsApplyResult_.status = SettingsApplyStatus::Pending;
        std::promise<bool> failure;
        app.settingsBudgetFence_ = failure.get_future();
        if (stopped) failure.set_value(false);
        else failure.set_exception(std::make_exception_ptr(std::runtime_error("完成句柄故障")));
    }
};
}

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::string readFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void writeFile(const std::filesystem::path& path, const std::string& value)
{
    std::ofstream out(path, std::ios::binary); out << value; out.close();
    require(static_cast<bool>(out), "测试文件写入失败");
}
void testCaptureAndAtomicLoad()
{
    protoscope::app::Application application;
    require(application.initialize(), "初始化失败");
    auto value = application.captureConfig();
    value.gui.font.chineseGlyphRange = protoscope::config::GuiFontChineseGlyphRange::Full;
    value.gui.wave.fullscreenMode = protoscope::config::GuiWaveFullscreenMode::Focus;
    value.gui.wave.maxTotalSamples = 4321;
    value.gui.window.title = "保留标题";
    value.gui.wave.overviewShowBitChannels = true;
    value.gui.wave.measurementVisibleWindowOnly = true;
    value.gui.wave.measurementFollowRefreshIntervalMs = 0;
    value.app.language = "保留语言";
    require(application.applyConfig(value), "配置应用失败");
    const auto captured = application.captureConfig();
    require(captured.gui.font.chineseGlyphRange == value.gui.font.chineseGlyphRange, "capture 丢失字形配置");
    require(captured.gui.wave.fullscreenMode == value.gui.wave.fullscreenMode, "capture 丢失全屏策略");
    require(captured.gui.wave.maxTotalSamples == 4321, "capture 丢失历史上限");
    require(captured.gui.wave.measurementVisibleWindowOnly &&
            captured.gui.wave.measurementFollowRefreshIntervalMs == 0, "capture 丢失测量配置");
    require(captured.gui.window.title == value.gui.window.title && captured.app.language == value.app.language,
            "capture 丢失未展示配置");
    protoscope::config::ConfigStore store;
    std::string yaml, error;
    require(store.saveText(captured, yaml, error), "序列化失败");
    const auto loaded = store.loadText(yaml);
    require(loaded.error.empty() && loaded.config.gui.wave.maxTotalSamples == 4321 &&
            loaded.config.gui.font.chineseGlyphRange == value.gui.font.chineseGlyphRange &&
            loaded.config.gui.wave.fullscreenMode == value.gui.wave.fullscreenMode &&
            loaded.config.gui.wave.overviewShowBitChannels, "未展示字段往返丢失");
    require(loaded.config.gui.wave.measurementVisibleWindowOnly &&
            loaded.config.gui.wave.measurementFollowRefreshIntervalMs == 0, "测量配置往返丢失");
    const auto defaults = store.loadText("{}");
    require(!defaults.config.gui.wave.measurementVisibleWindowOnly &&
            defaults.config.gui.wave.measurementFollowRefreshIntervalMs == 200, "测量默认配置错误");
    const auto bad = store.loadText("app: {fps_limit: 144}\nlogging: {max_files: invalid}\n");
    require(!bad.error.empty() && !bad.loadedFromDisk && bad.config.app.fpsLimit == 60,
            "坏 YAML 暴露了部分加载结果");
    application.shutdown();
}
void testSafeSave(const std::filesystem::path& root)
{
    using namespace protoscope::config;
    ConfigStore store;
    const auto path = root / "config.yaml";
    std::string error;
    require(store.save(path, AppConfig{}, error), "首次安全保存失败");
    const auto before = readFile(path);
    auto snapshot = store.snapshot(path);
    const auto stamp = std::filesystem::last_write_time(path);
    writeFile(path, "app: {fps_limit: 321}\n");
    std::filesystem::last_write_time(path, stamp);
    require(store.hasChanged(snapshot), "同时间戳外部内容修改未检测");
    require(!store.saveChecked(path, AppConfig{}, snapshot, error), "冲突保存静默覆盖文件");
    require(readFile(path) == "app: {fps_limit: 321}\n", "冲突改变原文件");
    snapshot = store.snapshot(path);
    require(store.saveChecked(path, AppConfig{}, snapshot, error), "确认最新快照后保存失败");
    require(readFile(path) == before, "整份 YAML 格式不一致");
    // 旧临时路径可能是用户文件，唯一独占创建不能截断或删除它。
    const auto legacy = std::filesystem::path(path.string() + ".save.tmp");
    writeFile(legacy, "用户既有临时文件");
    require(store.save(path, AppConfig{}, error), "唯一临时保存失败");
    require(readFile(legacy) == "用户既有临时文件" && readFile(path) == before, "保存触碰了既有临时文件");
    for (const auto& entry : std::filesystem::directory_iterator(root))
        require(entry.path().filename().string().find("config.yaml.save.") != 0 || entry.path() == legacy,
                "成功替换遗留本次临时文件");
#if defined(_WIN32)
    const auto locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(locked != INVALID_HANDLE_VALUE, "测试原文件锁打开失败");
    const auto saved = store.save(path, AppConfig{}, error);
    CloseHandle(locked);
    require(!saved && !error.empty() && readFile(path) == before, "替换失败损坏原文件");
    bool retained = false;
    for (const auto& entry : std::filesystem::directory_iterator(root))
        if (entry.path() != legacy && entry.path().filename().string().find("config.yaml.save.") == 0)
            retained = error.find(entry.path().string()) != std::string::npos;
    require(retained, "替换失败未保留并报告临时文件路径");
#endif
    const auto badPath = root / "bad.yaml";
    writeFile(badPath, "gui: [broken\n");
    const auto bad = store.load(badPath);
    require(!bad.error.empty() && !bad.loadedFromDisk, "坏文件未拒绝");
}
void testLoggerCandidate(const std::filesystem::path& root)
{
    using namespace protoscope;
    logging::LoggingFacade logger;
    const auto oldConfig = logger.currentConfig();
    const auto file = root / "candidate.log";
    writeFile(file, "原有日志内容不得在预检旋转");
    const auto bytes = readFile(file);
    auto value = oldConfig;
    value.filePath = file.string(); value.maxFileSizeBytes = 1;
    std::string error;
    require(logger.prepareSettingsConfig(value, error), "候选日志预检失败");
    require(readFile(file) == bytes && logger.currentConfig().filePath == oldConfig.filePath,
            "候选日志预检修改旧内容或提前提交配置");
    logger.discardSettingsConfig();
    require(readFile(file) == bytes, "丢弃候选破坏日志");
    require(logger.prepareSettingsConfig(value, error), "日志门限测试预备失败");
    logger.commitSettingsConfig();
    logger.setLevel(config::LogLevel::Debug);
    require(readFile(file) == bytes && logger.currentConfig().level == config::LogLevel::Debug,
            "等级热改重开或旋转日志文件");
    app::Application levelApplication;
    levelApplication.docks().clearDirty("test");
    levelApplication.setLogLevel(config::LogLevel::Debug);
    require(levelApplication.docks().configState().dirty &&
            levelApplication.captureConfig().logging.level == config::LogLevel::Debug,
            "日志快捷修改未标记dirty");
    std::future<bool> completion;
    {
        scripting::ScriptRuntimeWorker worker;
        completion = worker.applyBudgetSettingsAsync(8192, 4096, 100, 1024);
        require(completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready && completion.get(),
                "worker 性能预算未提供真实完成确认");
    }
}

void testDraftAndMerge()
{
    using namespace protoscope::config;
    AppConfig baseline;
    SettingsDraft hidden(baseline);
    hidden.value.performance.explicitOverrides.workerRxQueueLimitBytes = true;
    hidden.value.performance.explicitOverrides.workerMemoryBudgetBytes = true;
    hidden.value.performance.explicitOverrides.workerOutputQueueLimit = true;
    hidden.value.performance.explicitOverrides.workerBatchBytes = true;
    hidden.restoreDefaults(SettingsPage::Performance);
    require(hidden.value.performance.explicitOverrides.workerRxQueueLimitBytes &&
            hidden.value.performance.explicitOverrides.workerMemoryBudgetBytes &&
            hidden.value.performance.explicitOverrides.workerOutputQueueLimit &&
            hidden.value.performance.explicitOverrides.workerBatchBytes, "性能页默认清掉隐藏覆盖");
    SettingsDraft following(baseline);
    following.value.gui.showAppHeader = true;
    auto followingLatest = baseline;
    followingLatest.logging.level = LogLevel::Debug;
    following.followUnedited(followingLatest);
    require(following.value.logging.level == LogLevel::Debug && following.baseline.logging.level == LogLevel::Debug &&
            following.value.gui.showAppHeader && !following.baseline.gui.showAppHeader, "未编辑跟随破坏编辑基线");
    SettingsDraft state(baseline);
    state.value.gui.showAppHeader = true;
    auto latest = baseline;
    latest.logging.level = LogLevel::Debug;
    latest.communication.serial.baudRate = 57600;
    auto merged = mergeSettingsChanges(state.baseline, state.value, latest);
    require(merged.error.empty() && merged.conflicts.empty() && merged.config.gui.showAppHeader &&
            merged.config.logging.level == LogLevel::Debug &&
            merged.config.communication.serial.baudRate == 57600, "差异合并覆盖了其它入口");
    state.value.app.fpsLimit = 144;
    latest.app.fpsLimit = 120;
    merged = mergeSettingsChanges(state.baseline, state.value, latest);
    require(!merged.conflicts.empty(), "同字段冲突未发现");
    state.restoreDefaults(SettingsPage::General);
    require(!state.value.gui.showAppHeader && state.value.app.fpsLimit == 144, "单页默认改变其它页");
    state.value.performance.adaptive.maxMultiplier = std::numeric_limits<double>::quiet_NaN();
    require(!mergeSettingsChanges(state.baseline, state.value, baseline).error.empty(), "NaN 校验缺失");
    state.refresh(baseline);
    state.value.gui.window.width = 0;
    require(!mergeSettingsChanges(state.baseline, state.value, baseline).error.empty(), "启动尺寸校验缺失");
    state.refresh(baseline);
    state.value.gui.wave.controlMode = static_cast<protoscope::plot::WaveControlMode>(999);
    require(!mergeSettingsChanges(state.baseline, state.value, baseline).error.empty(), "枚举校验缺失");
    state.refresh(baseline);
    state.value.logging.filePath = std::string("log\0bad", 7);
    require(!mergeSettingsChanges(state.baseline, state.value, baseline).error.empty(), "路径 NUL 校验缺失");
}
void testScale()
{
    using namespace protoscope::config;
    auto value = AppConfig{};
    std::string error;
    value.performance.scale = 2;
    require(recomputePerformanceDefaults(value, error), "倍率 2 失败");
    require(value.gui.realtimeBacklog.rxChunkBytesPerPump == 8192, "倍率缺省错误");
    value.performance.scale = 3;
    require(recomputePerformanceDefaults(value, error), "倍率 3 失败");
    require(value.gui.realtimeBacklog.rxChunkBytesPerPump == 12288, "倍率累乘当前值");
    value.performance.explicitOverrides.realtimeBacklogRxChunkBytesPerPump = true;
    value.gui.realtimeBacklog.rxChunkBytesPerPump = 7777;
    value.performance.scale = 2;
    require(recomputePerformanceDefaults(value, error) && value.gui.realtimeBacklog.rxChunkBytesPerPump == 7777,
            "显式覆盖被倍率改写");
    auto baseline = value;
    auto draft = value;
    draft.performance.explicitOverrides.realtimeBacklogRxChunkBytesPerPump = false;
    const auto merged = mergeSettingsChanges(baseline, draft, baseline);
    require(merged.error.empty() && merged.config.gui.realtimeBacklog.rxChunkBytesPerPump == 8192,
            "解除覆盖未从缺省基线重算");
    SettingsDraft defaultsState(value);
    defaultsState.restoreDefaults(SettingsPage::Performance);
    const auto defaultsMerged = mergeSettingsChanges(defaultsState.baseline, defaultsState.value, defaultsState.baseline);
    require(defaultsMerged.error.empty() && !defaultsMerged.config.performance.explicitOverrides.realtimeBacklogRxChunkBytesPerPump &&
            defaultsMerged.config.gui.realtimeBacklog.rxChunkBytesPerPump == 4096, "性能默认误标为显式覆盖");
    auto concurrent = baseline;
    concurrent.gui.realtimeBacklog.rxChunkBytesPerPump = 8888;
    require(!mergeSettingsChanges(baseline, draft, concurrent).conflicts.empty(), "预算数字与覆盖标记冲突漏报");
    value.performance.scale = std::numeric_limits<double>::max();
    require(!recomputePerformanceDefaults(value, error), "倍率溢出应拒绝");
    require(recomputePerformanceDefaults(value, error, true) &&
            value.receive.transportReadBufferBytes == (std::numeric_limits<std::size_t>::max)() &&
            std::isfinite(value.scripting.workerOutputFlushBudgetMs), "兼容倍率钳制不安全");
    value.performance.scale = std::numeric_limits<double>::infinity();
    require(!recomputePerformanceDefaults(value, error), "设置 Inf 倍率未拒绝");
    ConfigStore store;
    const auto huge = store.loadText("performance: {scale: 1.7976931348623157e308}\n");
    require(huge.error.empty() && huge.config.receive.transportReadBufferBytes ==
            (std::numeric_limits<std::size_t>::max)() && std::isfinite(huge.config.scripting.workerOutputFlushBudgetMs),
            "YAML 超大有限倍率转换溢出");
    const auto infinite = store.loadText("performance: {scale: .inf}\n");
    require(infinite.error.empty() && infinite.config.performance.scale == 1 &&
            infinite.config.receive.transportReadBufferBytes == AppConfig{}.receive.transportReadBufferBytes,
            "YAML Inf 倍率未安全回退");
    const auto explicitInfinite = store.loadText("scripting: {worker: {output_flush_budget_ms: .inf}}\n");
    require(explicitInfinite.error.empty() && std::isfinite(explicitInfinite.config.scripting.workerOutputFlushBudgetMs),
            "YAML 显式 Inf flush 未钳制");
    const auto boundary = std::ldexp(1.0, std::numeric_limits<std::size_t>::digits) /
        static_cast<double>(AppConfig{}.receive.transportReadBufferBytes);
    value = AppConfig{}; value.performance.scale = boundary;
    require(!recomputePerformanceDefaults(value, error), "2^digits 排他边界未拒绝");
    const auto regular = store.loadText("performance: {scale: 3}\n");
    value = AppConfig{}; value.performance.scale = 3;
    require(recomputePerformanceDefaults(value, error) && regular.error.empty() &&
            value.receive.transportReadBufferBytes == regular.config.receive.transportReadBufferBytes &&
            value.gui.realtimeBacklog.rxChunkBytesPerPump == regular.config.gui.realtimeBacklog.rxChunkBytesPerPump,
            "YAML 与设置数学基线不一致");
}
protoscope::app::SettingsApplyResult finish(protoscope::app::Application& application,
                                          protoscope::app::SettingsApplyResult result)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (result.status == protoscope::app::SettingsApplyStatus::Pending &&
           std::chrono::steady_clock::now() < deadline) {
        result = application.pollSettingsApply();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return result;
}
void testAsyncFailures(const std::filesystem::path& root)
{
    using namespace protoscope;
    app::Application application;
    require(application.initialize(), "异步测试初始化失败");
    config::SettingsDraft state(application.captureConfig());
    state.value.gui.theme = "settings-test-theme";
    app::SettingsApplyOptions options;
    options.prepareTheme = [](std::string_view, std::string&) -> bool { throw std::runtime_error("资源准备异常"); };
    options.applyTheme = [](std::string_view, std::string&) { return true; };
    state.value.performance.scale = 2;
    auto result = application.applySettingsChanges(state.baseline, state.value, options);
    require(result.status == app::SettingsApplyStatus::Failed && !result.partiallyApplied &&
            application.captureConfig().scripting.workerBatchBytes == state.baseline.scripting.workerBatchBytes,
            "准备异常提前应用预算");
    options.prepareTheme = [](std::string_view, std::string&) { return true; };
    options.applyTheme = [](std::string_view, std::string&) -> bool { throw 42; };
    result = application.applySettingsChanges(state.baseline, state.value, options);
    require(result.status == app::SettingsApplyStatus::Pending, "主题异常测试未进入异步");
    config::ConfigStore store;
    std::string error;
    config::SettingsPersistence persistence(store, root / "partial.yaml");
    require(!application.saveSettings(persistence, error), "Pending 允许保存");
    result = finish(application, result);
    const auto partial = application.captureConfig();
    require(result.status == app::SettingsApplyStatus::Failed && result.partiallyApplied &&
            application.docks().configState().dirty && partial.performance.scale == state.baseline.performance.scale &&
            partial.performance.explicitOverrides.workerBatchBytes &&
            partial.scripting.workerBatchBytes == config::AppConfig{}.scripting.workerBatchBytes * 2,
            "未知主题异常丢失真实预算或元数据");
    require(!application.saveSettings(persistence, error) && application.settingsSaveBlocked() &&
            application.docks().configState().dirty, "部分失败必须禁止保存并保留dirty");
    // 刷新和关闭后重建草稿都不能丢失Application持有的失败前标记。
    const auto originalWorkerFlags = state.baseline.performance.explicitOverrides;
    state.refresh(application.captureConfig());
    result = application.applySettingsChanges(state.baseline, state.value);
    require(result.status == app::SettingsApplyStatus::Failed && application.settingsSaveBlocked(),
            "部分失败no-op应用假成功或解除门禁");
    require(application.preparePartialWorkerRecovery(state) &&
            state.value.performance.explicitOverrides.workerRxQueueLimitBytes == originalWorkerFlags.workerRxQueueLimitBytes &&
            state.value.performance.explicitOverrides.workerMemoryBudgetBytes == originalWorkerFlags.workerMemoryBudgetBytes &&
            state.value.performance.explicitOverrides.workerOutputQueueLimit == originalWorkerFlags.workerOutputQueueLimit &&
            state.value.performance.explicitOverrides.workerBatchBytes == originalWorkerFlags.workerBatchBytes,
            "刷新后恢复丢失原始worker标记");
    config::SettingsDraft reopened(application.captureConfig());
    require(application.preparePartialWorkerRecovery(reopened) &&
            !reopened.value.performance.explicitOverrides.workerBatchBytes, "关闭重开后恢复丢失原始标记");
    const auto recovery = config::mergeSettingsChanges(reopened.baseline, reopened.value, application.captureConfig());
    require(recovery.error.empty() && recovery.config.scripting.workerBatchBytes == config::AppConfig{}.scripting.workerBatchBytes,
            "重建草稿后的恢复未派生缺省预算");
    require(application.acceptPartialSettings(error) && !application.settingsSaveBlocked() &&
            application.docks().configState().dirty &&
            application.captureConfig().scripting.workerBatchBytes == partial.scripting.workerBatchBytes &&
            application.captureConfig().performance.explicitOverrides.workerBatchBytes,
            "明确接受没有保留真实预算或没有解除门禁");
    require(!application.acceptPartialSettings(error), "接受操作可重复或缺失状态门禁");
    {
        app::Application recoveredApplication;
        require(recoveredApplication.initialize(), "恢复测试初始化失败");
        config::SettingsDraft failed(recoveredApplication.captureConfig());
        failed.value.performance.scale = 2;
        failed.value.gui.theme = "settings-test-theme";
        auto failedResult = finish(recoveredApplication,
            recoveredApplication.applySettingsChanges(failed.baseline, failed.value, options));
        require(failedResult.partiallyApplied, "恢复测试未进入部分失败");
        config::SettingsDraft rebuilt(recoveredApplication.captureConfig());
        require(recoveredApplication.preparePartialWorkerRecovery(rebuilt), "重建后不能准备恢复");
        const auto completed = finish(recoveredApplication,
            recoveredApplication.applySettingsChanges(rebuilt.baseline, rebuilt.value));
        require(completed.status == app::SettingsApplyStatus::Applied && !recoveredApplication.settingsSaveBlocked() &&
                recoveredApplication.captureConfig().scripting.workerBatchBytes == config::AppConfig{}.scripting.workerBatchBytes &&
                !recoveredApplication.captureConfig().performance.explicitOverrides.workerBatchBytes,
                "恢复实际完成未解除门禁或预算/标记不一致");
    }
    state.refresh(application.captureConfig());
    state.value.gui.theme = "sync-throw-theme";
    options.applyTheme = [](std::string_view, std::string&) -> bool { throw std::runtime_error("同步主题异常"); };
    result = application.applySettingsChanges(state.baseline, state.value, options);
    require(result.status == app::SettingsApplyStatus::Failed && !result.partiallyApplied,
            "同步主题异常未捕获");
    state.refresh(application.captureConfig());
    state.value.performance.scale = 3;
    // 部分完成预算是显式值，解除覆盖后再验证新的 worker fence。
    state.value.performance.explicitOverrides.workerBatchBytes = false;
    const auto resumed = config::mergeSettingsChanges(state.baseline, state.value, state.baseline);
    require(resumed.error.empty() && resumed.conflicts.empty() &&
            !resumed.config.performance.explicitOverrides.workerBatchBytes &&
            resumed.config.scripting.workerBatchBytes == config::AppConfig{}.scripting.workerBatchBytes * 3,
            "部分应用后解除worker覆盖未参与合并和基线重算");
    result = application.applySettingsChanges(state.baseline, state.value);
    require(result.status == app::SettingsApplyStatus::Pending, "任务门禁测试未进入异步");
    app::ApplicationSettingsTestAccess::transferActive(application, true);
    result = finish(application, result);
    require(result.status == app::SettingsApplyStatus::Failed && result.partiallyApplied &&
            application.captureConfig().performance.explicitOverrides.workerBatchBytes, "完成阶段遗漏传输门禁");
    require(!application.saveSettings(persistence, error), "传输活动允许保存");
    app::ApplicationSettingsTestAccess::transferActive(application, false);
    state.refresh(application.captureConfig());
    state.value.performance.scale = 4;
    state.value.performance.explicitOverrides.workerBatchBytes = false;
    result = application.applySettingsChanges(state.baseline, state.value);
    require(result.status == app::SettingsApplyStatus::Pending, "隐藏预算冲突测试未Pending");
    app::ApplicationSettingsTestAccess::clearBatchOverride(application);
    result = finish(application, result);
    require(result.status == app::SettingsApplyStatus::ConfirmationRequired && result.partiallyApplied &&
            result.confirmationId != 0, "隐藏标记冲突未保留预算及确认状态");
    app::ApplicationSettingsTestAccess::failFence(application);
    result = application.pollSettingsApply();
    require(result.status == app::SettingsApplyStatus::Failed && !result.error.empty(), "失效完成句柄异常未终止Pending");
    app::ApplicationSettingsTestAccess::failFence(application, true);
    result = application.pollSettingsApply();
    require(result.status == app::SettingsApplyStatus::Failed && !result.partiallyApplied,
            "worker拒绝伪造部分成功");
    state.refresh(application.captureConfig());
    state.value.gui.showAppHeader = !state.baseline.gui.showAppHeader;
    require(application.applySettingsChanges(state.baseline, state.value).status == app::SettingsApplyStatus::Applied,
            "失败遗留Pending阻止后续应用");
    application.shutdown();
}

void testApplication(const std::filesystem::path& root)
{
    using namespace protoscope;
    app::Application application;
    require(application.initialize(), "初始化失败");
    const auto baseline = application.captureConfig();
    config::SettingsDraft state(baseline);
    auto& wave = application.docks().waveState();
    wave.view.showCursors = true;
    const auto historyEpoch = wave.buffer.historyEpoch();
    const auto protocolDir = application.docks().luaState().protocolDir;
    application.docks().clearDirty("测试基线");
    state.value.gui.showAppHeader = !baseline.gui.showAppHeader;
    require(!application.docks().configState().dirty &&
            application.captureConfig().gui.showAppHeader == baseline.gui.showAppHeader, "草稿污染运行态");
    auto result = application.applySettingsChanges(state.baseline, state.value);
    require(result.status == app::SettingsApplyStatus::Applied, "普通局部应用失败");
    require(application.docks().configState().dirty && wave.buffer.historyEpoch() == historyEpoch &&
            wave.view.showCursors && application.docks().luaState().protocolDir == protocolDir, "应用扰动协议/数据/游标");
    state.refresh(application.captureConfig());
    state.value.performance.adaptive.enabled = true;
    state.value.performance.adaptive.maxMultiplier = .5;
    state.value.app.fpsLimit = 144;
    state.value.gui.wave.maxRenderPointsPerChannel = 9999;
    state.value.gui.realtimeBacklog.rxChunkBytesPerPump = 7777;
    result = finish(application, application.applySettingsChanges(state.baseline, state.value));
    require(result.status == app::SettingsApplyStatus::Applied, "自适应局部应用失败");
    require(application.effectiveSettingsPerformance().fpsLimit == 30 &&
            application.effectiveSettingsPerformance().maxRenderPointsPerChannel == 600, "自适应接管失效");
    require(application.captureConfig().app.fpsLimit == 144 &&
            application.captureConfig().gui.wave.maxRenderPointsPerChannel == 9999, "有效值污染保存静态值");
    state.refresh(application.captureConfig());
    state.value.performance.adaptive.enabled = false;
    result = finish(application, application.applySettingsChanges(state.baseline, state.value));
    require(result.status == app::SettingsApplyStatus::Applied && application.effectiveFpsLimit() == 144 &&
            application.captureConfig().gui.realtimeBacklog.rxChunkBytesPerPump == 7777,
            "关闭自适应未恢复静态显式值");
    state.refresh(application.captureConfig());
    const auto loggerBefore = application.logger().currentConfig();
    state.value.logging.filePath = (root / "log-is-directory").string();
    std::filesystem::create_directory(state.value.logging.filePath);
    result = application.applySettingsChanges(state.baseline, state.value);
    require(result.status == app::SettingsApplyStatus::Failed && !result.error.empty() &&
            application.logger().currentConfig().filePath == loggerBefore.filePath, "日志失败损坏旧后端");
    state.refresh(application.captureConfig());
    state.value.performance.scale = 2;
    result = application.applySettingsChanges(state.baseline, state.value);
    require(result.status == app::SettingsApplyStatus::Pending, "worker 入队冒充已完成");
    result = finish(application, result);
    require(result.status == app::SettingsApplyStatus::Applied && result.nextConnectionRequired,
            "worker 预算确认或下次连接标记缺失");
    state.refresh(application.captureConfig());
    application.docks().appendLogRow({});
    state.value.gui.logHistory.hostLimit = 0;
    result = application.applySettingsChanges(state.baseline, state.value);
    require(result.status == app::SettingsApplyStatus::ConfirmationRequired &&
            !application.docks().logState().rows.empty(), "历史缩减未经确认");
    app::SettingsApplyOptions options;
    options.confirmHistoryTrim = true;
    options.confirmationId = result.confirmationId;
    state.value.app.fpsLimit += 1;
    result = application.applySettingsChanges(state.baseline, state.value, options);
    require(result.status == app::SettingsApplyStatus::ConfirmationRequired, "过期确认覆盖了新草稿");
    options.confirmationId = result.confirmationId;
    application.docks().appendLogRow({});
    application.setGuiTheme("unrelated-theme");
    result = application.applySettingsChanges(state.baseline, state.value, options);
    require(result.status == app::SettingsApplyStatus::Applied && application.docks().logState().rows.empty(),
            "确认裁剪失败");
    // 手动/自动保存统一消费运行 capture，不消费草稿；失败保留 dirty。
    config::ConfigStore store;
    const auto path = root / "running.yaml";
    std::string error;
    require(store.save(path, application.captureConfig(), error), "运行态保存失败");
    config::SettingsPersistence persistence(store, path);
    state.refresh(application.captureConfig());
    state.value.gui.showAppHeader = !state.baseline.gui.showAppHeader;
    require(persistence.save(application.captureConfig(), error), "纯保存控制失败");
    require(store.load(path).config.gui.showAppHeader == state.baseline.gui.showAppHeader, "保存了未应用草稿");
    writeFile(path, "app: {fps_limit: 88}\n");
    require(!application.saveSettings(persistence, error) && !error.empty(), "未检测外部修改");
    require(application.docks().configState().dirty, "失败意外清 dirty");
    application.shutdown();
}

// 收发历史切换策略/范围回归：新枚举键读写、缺省、非法回退、旧 bool 兼容映射、新键优先。
static void testTransferHistorySwitchConfig()
{
    using namespace protoscope;
    using namespace protoscope::config;
    using protoscope::config::TransferLogHistoryOnParsedSwitch;
    using protoscope::config::TransferLogHistoryScope;
    config::ConfigStore store;
    std::string error;

    // 内建缺省：ask / retained_rows。
    const config::AppConfig defaults;
    require(defaults.gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::Ask &&
                defaults.gui.historyScope == TransferLogHistoryScope::RetainedRows,
            "内建缺省必须是 ask / retained_rows");

    // 新键读写：use_history / retained_capture 往返一致。
    config::AppConfig config;
    config.gui.historyOnParsedSwitch = TransferLogHistoryOnParsedSwitch::UseHistory;
    config.gui.historyScope = TransferLogHistoryScope::RetainedCapture;
    std::string yamlText;
    require(store.saveText(config, yamlText, error), error.c_str());
    const auto roundTrip = store.loadText(yamlText);
    require(roundTrip.error.empty(), roundTrip.error.c_str());
    require(roundTrip.config.gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::UseHistory &&
                roundTrip.config.gui.historyScope == TransferLogHistoryScope::RetainedCapture,
            "新键 use_history/retained_capture 读写往返失败");

    // 非法值回退到默认（ask/retained_rows），不得抛异常。
    const auto invalid = store.loadText("gui:\n  transfer_log:\n    history_on_parsed_switch: bogus\n    history_scope: nonsense\n");
    require(invalid.error.empty() && invalid.config.gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::Ask &&
                invalid.config.gui.historyScope == TransferLogHistoryScope::RetainedRows,
            "非法枚举值必须回退默认");

    // 旧 bool 兼容：仅旧键存在时映射（true=UseHistory / false=Ask）。
    const auto legacyTrue = store.loadText("gui:\n  transfer_log:\n    replay_raw_history_on_schema_switch: true\n");
    require(legacyTrue.config.gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::UseHistory,
            "旧键 true 必须映射 use_history");
    const auto legacyFalse = store.loadText("gui:\n  transfer_log:\n    replay_raw_history_on_schema_switch: false\n");
    require(legacyFalse.config.gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::Ask,
            "旧键 false 必须映射 ask");
    const auto explicitUse = store.loadText(
        "gui:\n  transfer_log:\n    history_on_parsed_switch: use_history\n    replay_raw_history_on_schema_switch: false\n");
    require(explicitUse.config.gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::UseHistory,
            "新键 use_history 必须优先于旧 false");

    // 新键优先：新旧键同时存在时以新键为准（旧键不再覆盖）。
    const auto newerWins = store.loadText(
        "gui:\n  transfer_log:\n    history_on_parsed_switch: skip_history\n    replay_raw_history_on_schema_switch: true\n");
    require(newerWins.config.gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::SkipHistory,
            "新键存在时旧 bool 不得覆盖");
    require(newerWins.config.gui.replayRawHistoryOnSchemaSwitch, "旧字段须作为兼容存档保留，而非运行时策略");
    require(store.saveText(newerWins.config, yamlText, error), error.c_str());
    const auto compatibilityRoundTrip = store.loadText(yamlText);
    require(compatibilityRoundTrip.config.gui.replayRawHistoryOnSchemaSwitch &&
                compatibilityRoundTrip.config.gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::SkipHistory,
            "新旧键共存 roundtrip 必须保留旧字段且继续以新枚举为唯一策略");

    // 现场包恢复保护：applySessionPackage 不得覆盖本机两枚举偏好。
    protoscope::app::Application application;
    require(application.initialize(), "现场包恢复保护初始化失败");
    // 通过设置入口把本机偏好改为 use_history/retained_capture 并生效。
    const auto liveBefore = application.captureConfig();
    config::SettingsDraft state(liveBefore);
    state.value.gui.historyOnParsedSwitch = TransferLogHistoryOnParsedSwitch::UseHistory;
    state.value.gui.historyScope = TransferLogHistoryScope::RetainedCapture;
    const auto applyResult = application.applySettingsChanges(state.baseline, state.value);
    require(applyResult.status == protoscope::app::SettingsApplyStatus::Applied ||
                applyResult.status == protoscope::app::SettingsApplyStatus::Pending,
            "本机偏好设置应用失败");
    // 仅改本机展示偏好，不应触发需要 pump 的 worker 预算异步路径；若进入 Pending 由后续统一验证轮处理。
    require(application.captureConfig().gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::UseHistory &&
                application.captureConfig().gui.historyScope == TransferLogHistoryScope::RetainedCapture,
            "本机偏好未生效");

    // 构造最小现场包：config.yaml 使用默认配置 + 空 capture 事件。
    plot::RawCaptureFileData packageCapture;
    packageCapture.source = "fixture";
    protoscope::session::SessionPackageData package;
    require(store.saveText(config::AppConfig{}, yamlText, error), error.c_str());
    package.entries.push_back({"config.yaml", std::vector<std::uint8_t>(yamlText.begin(), yamlText.end())});
    const tests::ScopedTempPath packagePath(tests::makeUniqueTempFile("protoscope-session-history-pref", ".psess"));
    require(protoscope::session::writeSessionPackage(packagePath.path(), package, error), error.c_str());
    require(application.importSessionPackage(packagePath.path(), error), error.c_str());
    require(application.captureConfig().gui.historyOnParsedSwitch == TransferLogHistoryOnParsedSwitch::UseHistory &&
                application.captureConfig().gui.historyScope == TransferLogHistoryScope::RetainedCapture,
            "现场包恢复不得覆盖本机历史切换偏好");
    application.shutdown();
}
static void testImportProtocolPreference()
{
    using namespace protoscope;
    config::ConfigStore store;
    require(config::AppConfig{}.gui.importProtocolSource == config::ImportProtocolSource::PreferPackage,
            "导入默认必须优先包内协议");
    const auto invalid = store.loadText("gui:\n  data_import:\n    protocol_source: invalid\n");
    require(invalid.error.empty() && invalid.config.gui.importProtocolSource == config::ImportProtocolSource::PreferPackage,
            "非法导入来源必须回退默认");
    app::Application application;
    require(application.initialize(), "导入偏好测试初始化失败");
    const auto before = application.captureConfig();
    auto draft = before;
    draft.gui.importProtocolSource = config::ImportProtocolSource::CurrentProtocol;
    const auto applied = application.applySettingsChanges(before, draft);
    require(applied.status == app::SettingsApplyStatus::Applied &&
        application.captureConfig().gui.importProtocolSource == config::ImportProtocolSource::CurrentProtocol,
        "设置字段必须合并、应用并捕获导入偏好");
    std::string yaml, error;
    require(store.saveText(application.captureConfig(), yaml, error), error.c_str());
    require(store.loadText(yaml).config.gui.importProtocolSource == config::ImportProtocolSource::CurrentProtocol,
            "导入偏好持久往返失败");
    auto bad = application.captureConfig();
    bad.gui.importProtocolSource = static_cast<config::ImportProtocolSource>(99);
    require(application.applySettingsChanges(application.captureConfig(), bad).status == app::SettingsApplyStatus::Failed,
            "设置必须拒绝非法导入枚举");
    application.shutdown();
}
} // namespace

int main()
{
    try {
        const auto root = std::filesystem::temp_directory_path() /
            ("protoscope-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        testCaptureAndAtomicLoad(); testSafeSave(root); testLoggerCandidate(root); testDraftAndMerge(); testScale(); testAsyncFailures(root); testApplication(root); testTransferHistorySwitchConfig(); testImportProtocolPreference();
        std::cout << "[PASS] 应用设置核心回归\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[FAIL] " << ex.what() << '\n'; return 1;
    }
}
