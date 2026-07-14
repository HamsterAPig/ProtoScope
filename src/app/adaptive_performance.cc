#include "protoscope/app/adaptive_performance.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif

namespace protoscope::app {

namespace {

constexpr double kMinMultiplier = 0.25;
constexpr double kMaxMultiplier = 4.0;
constexpr std::uint64_t kSampleIntervalMs = 1000;
constexpr std::size_t kRecoverySamples = 5;
constexpr std::size_t kBaseTransferFrameRows = 2000;
constexpr std::size_t kBasePlotAppends = 128;
constexpr std::size_t kBaseWorkerQueueItems = 128;
constexpr double kBaseScriptPumpMs = 16.0;

double clampRatio(const double value)
{
    return (std::clamp)(value, 0.0, 1.0);
}

double normalizeMultiplier(const double value)
{
    if (!std::isfinite(value) || value <= 0.0) {
        return 1.0;
    }
    return (std::clamp)(value, kMinMultiplier, kMaxMultiplier);
}

std::size_t scaledBudget(const std::size_t base, const double multiplier)
{
    const auto scaled = std::round(static_cast<double>(base) * multiplier);
    if (scaled <= 1.0) {
        return 1U;
    }
    const auto limit = static_cast<double>((std::numeric_limits<std::size_t>::max)());
    return scaled >= limit ? (std::numeric_limits<std::size_t>::max)() : static_cast<std::size_t>(scaled);
}

std::uint32_t scaledFpsLimit(const std::uint32_t base, const double multiplier)
{
    const auto scaled = scaledBudget(base, multiplier);
    const auto limit = static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)());
    return static_cast<std::uint32_t>((std::min)(scaled, limit));
}

int pressureSeverity(const AdaptivePressureLevel level)
{
    return static_cast<int>(level);
}

AdaptivePressureLevel lowerPressureLevel(const AdaptivePressureLevel level)
{
    switch (level) {
        case AdaptivePressureLevel::Critical:
            return AdaptivePressureLevel::High;
        case AdaptivePressureLevel::High:
            return AdaptivePressureLevel::Elevated;
        case AdaptivePressureLevel::Elevated:
            return AdaptivePressureLevel::Normal;
        case AdaptivePressureLevel::Normal:
            return AdaptivePressureLevel::Normal;
    }
    return AdaptivePressureLevel::Normal;
}

AdaptivePressureLevel pressureForSystem(const SystemPressureSample& system, std::string& reason)
{
    const auto cpu = system.cpuBusyRatio.value_or(-1.0);
    const auto memory = system.availableMemoryRatio.value_or(2.0);
    if (cpu >= 0.95 || memory <= 0.08) {
        reason = cpu >= 0.95 ? "system_cpu_critical" : "system_memory_critical";
        return AdaptivePressureLevel::Critical;
    }
    if (cpu >= 0.85 || memory <= 0.15) {
        reason = cpu >= 0.85 ? "system_cpu_high" : "system_memory_low";
        return AdaptivePressureLevel::High;
    }
    if (cpu >= 0.75 || memory <= 0.25) {
        reason = cpu >= 0.75 ? "system_cpu_elevated" : "system_memory_elevated";
        return AdaptivePressureLevel::Elevated;
    }
    return AdaptivePressureLevel::Normal;
}

AdaptivePressureLevel pressureForSoftware(const AdaptivePerformanceInput& input, std::string& reason)
{
    const auto rawWarnBytes = input.rawBacklogWarnBytes > 0U ? input.rawBacklogWarnBytes : 32U * 1024U * 1024U;
    const auto rawBacklogRatio = static_cast<double>(input.pendingRxBytes) / static_cast<double>(rawWarnBytes);
    if (rawBacklogRatio >= 2.0) {
        reason = "raw_backlog_critical";
        return AdaptivePressureLevel::Critical;
    }
    if (rawBacklogRatio >= 1.0) {
        reason = "raw_backlog_high";
        return AdaptivePressureLevel::High;
    }
    if (rawBacklogRatio >= 0.5) {
        reason = "raw_backlog_elevated";
        return AdaptivePressureLevel::Elevated;
    }

    const auto derivedBacklog =
        input.pendingTransferFrameRows >= kBaseTransferFrameRows ||
        input.pendingPlotAppends >= kBasePlotAppends ||
        input.pendingWorkerRxBytes >= rawWarnBytes / 2U ||
        input.workerInputQueueSize >= kBaseWorkerQueueItems ||
        input.workerOutputQueueSize >= kBaseWorkerQueueItems;
    if (derivedBacklog && input.scriptPumpMs >= kBaseScriptPumpMs * 2.0) {
        reason = "software_backlog_high";
        return AdaptivePressureLevel::High;
    }
    if (derivedBacklog || input.scriptPumpMs >= kBaseScriptPumpMs) {
        reason = "software_backlog_elevated";
        return AdaptivePressureLevel::Elevated;
    }
    return AdaptivePressureLevel::Normal;
}

#ifdef _WIN32
std::uint64_t fileTimeValue(const FILETIME& value)
{
    ULARGE_INTEGER result{};
    result.LowPart = value.dwLowDateTime;
    result.HighPart = value.dwHighDateTime;
    return result.QuadPart;
}

std::optional<double> sampleWindowsCpuBusyRatio()
{
    static std::optional<std::uint64_t> lastIdle;
    static std::optional<std::uint64_t> lastKernel;
    static std::optional<std::uint64_t> lastUser;

    FILETIME idle{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetSystemTimes(&idle, &kernel, &user) == 0) {
        return std::nullopt;
    }
    const auto currentIdle = fileTimeValue(idle);
    const auto currentKernel = fileTimeValue(kernel);
    const auto currentUser = fileTimeValue(user);
    std::optional<double> result;
    if (lastIdle.has_value() && lastKernel.has_value() && lastUser.has_value()) {
        const auto totalDelta = (currentKernel - *lastKernel) + (currentUser - *lastUser);
        const auto idleDelta = currentIdle - *lastIdle;
        if (totalDelta > 0U) {
            result = clampRatio(1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
        }
    }
    lastIdle = currentIdle;
    lastKernel = currentKernel;
    lastUser = currentUser;
    return result;
}
#elif defined(__linux__)
std::optional<double> sampleLinuxCpuBusyRatio()
{
    static std::optional<std::uint64_t> lastTotal;
    static std::optional<std::uint64_t> lastIdle;

    std::ifstream stream("/proc/stat");
    std::string label;
    if (!(stream >> label) || label != "cpu") {
        return std::nullopt;
    }
    std::uint64_t user = 0;
    std::uint64_t nice = 0;
    std::uint64_t system = 0;
    std::uint64_t idle = 0;
    std::uint64_t iowait = 0;
    std::uint64_t irq = 0;
    std::uint64_t softirq = 0;
    std::uint64_t steal = 0;
    stream >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
    const auto total = user + nice + system + idle + iowait + irq + softirq + steal;
    const auto idleTotal = idle + iowait;
    std::optional<double> result;
    if (lastTotal.has_value() && lastIdle.has_value() && total > *lastTotal) {
        const auto totalDelta = total - *lastTotal;
        const auto idleDelta = idleTotal - *lastIdle;
        result = clampRatio(1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
    }
    lastTotal = total;
    lastIdle = idleTotal;
    return result;
}

std::optional<double> sampleLinuxAvailableMemoryRatio()
{
    std::ifstream stream("/proc/meminfo");
    std::string key;
    std::uint64_t valueKb = 0;
    std::string unit;
    std::uint64_t totalKb = 0;
    std::uint64_t availableKb = 0;
    while (stream >> key >> valueKb >> unit) {
        if (key == "MemTotal:") {
            totalKb = valueKb;
        } else if (key == "MemAvailable:") {
            availableKb = valueKb;
        }
    }
    if (totalKb == 0U || availableKb == 0U) {
        return std::nullopt;
    }
    return clampRatio(static_cast<double>(availableKb) / static_cast<double>(totalKb));
}
#elif defined(__APPLE__)
std::optional<double> sampleAppleCpuBusyRatio()
{
    static std::optional<std::uint64_t> lastTotal;
    static std::optional<std::uint64_t> lastIdle;

    host_cpu_load_info_data_t cpuLoad{};
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    if (host_statistics(mach_host_self(), HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpuLoad), &count) !=
        KERN_SUCCESS) {
        return std::nullopt;
    }
    const auto idle = static_cast<std::uint64_t>(cpuLoad.cpu_ticks[CPU_STATE_IDLE]);
    const auto total = static_cast<std::uint64_t>(cpuLoad.cpu_ticks[CPU_STATE_USER]) +
                       static_cast<std::uint64_t>(cpuLoad.cpu_ticks[CPU_STATE_NICE]) +
                       static_cast<std::uint64_t>(cpuLoad.cpu_ticks[CPU_STATE_SYSTEM]) + idle;
    std::optional<double> result;
    if (lastTotal.has_value() && lastIdle.has_value() && total > *lastTotal) {
        result = clampRatio(
            1.0 - static_cast<double>(idle - *lastIdle) / static_cast<double>(total - *lastTotal));
    }
    lastTotal = total;
    lastIdle = idle;
    return result;
}

std::optional<double> sampleAppleAvailableMemoryRatio()
{
    std::uint64_t totalBytes = 0;
    std::size_t totalBytesSize = sizeof(totalBytes);
    if (sysctlbyname("hw.memsize", &totalBytes, &totalBytesSize, nullptr, 0) != 0 || totalBytes == 0U) {
        return std::nullopt;
    }
    vm_statistics64_data_t memory{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&memory), &count) !=
        KERN_SUCCESS) {
        return std::nullopt;
    }
    const auto availablePages = static_cast<std::uint64_t>(memory.free_count) +
                                static_cast<std::uint64_t>(memory.inactive_count);
    const auto availableBytes = availablePages * static_cast<std::uint64_t>(vm_page_size);
    return clampRatio(static_cast<double>(availableBytes) / static_cast<double>(totalBytes));
}
#endif

} // namespace

void AdaptivePerformanceController::configure(config::AdaptivePerformanceConfig config)
{
    config.maxMultiplier = normalizeMultiplier(config.maxMultiplier);
    config_ = config;
    lastSampleAtMs_ = 0;
    hasSample_ = false;
    healthySamples_ = 0;
    status_ = AdaptivePerformanceStatus{
        .enabled = config_.enabled,
        .maxMultiplier = config_.maxMultiplier,
        .effectiveMultiplier = config_.maxMultiplier,
        .pressureLevel = AdaptivePressureLevel::Normal,
        .reason = config_.enabled ? "normal" : "disabled",
        .systemMetricsAvailable = false,
    };
    refreshBudget();
}

void AdaptivePerformanceController::update(const AdaptivePerformanceInput& input)
{
    if (!shouldSample(input.nowMs)) {
        return;
    }
    lastSampleAtMs_ = input.nowMs;
    hasSample_ = true;
    status_.systemMetricsAvailable =
        input.system.cpuBusyRatio.has_value() || input.system.availableMemoryRatio.has_value();

    std::string reason;
    const auto desiredLevel = classifyPressure(input, reason);
    if (pressureSeverity(desiredLevel) > pressureSeverity(status_.pressureLevel)) {
        status_.pressureLevel = desiredLevel;
        healthySamples_ = 0;
    } else if (desiredLevel == AdaptivePressureLevel::Normal &&
               pressureSeverity(desiredLevel) < pressureSeverity(status_.pressureLevel)) {
        ++healthySamples_;
        if (healthySamples_ >= kRecoverySamples) {
            status_.pressureLevel = lowerPressureLevel(status_.pressureLevel);
            healthySamples_ = 0;
        }
    } else {
        healthySamples_ = 0;
    }
    status_.reason = reason.empty() ? "normal" : std::move(reason);
    refreshBudget();
}

bool AdaptivePerformanceController::enabled() const
{
    return config_.enabled;
}

bool AdaptivePerformanceController::shouldSample(const std::uint64_t nowMs) const
{
    return config_.enabled && (!hasSample_ || nowMs - lastSampleAtMs_ >= kSampleIntervalMs);
}

const AdaptivePerformanceBudget& AdaptivePerformanceController::budget() const
{
    return budget_;
}

const AdaptivePerformanceStatus& AdaptivePerformanceController::status() const
{
    return status_;
}

AdaptivePressureLevel AdaptivePerformanceController::classifyPressure(const AdaptivePerformanceInput& input,
                                                                       std::string& reason) const
{
    std::string systemReason;
    const auto systemLevel = pressureForSystem(input.system, systemReason);
    std::string softwareReason;
    const auto softwareLevel = pressureForSoftware(input, softwareReason);
    if (pressureSeverity(systemLevel) >= pressureSeverity(softwareLevel)) {
        reason = std::move(systemReason);
        return systemLevel;
    }
    reason = std::move(softwareReason);
    return softwareLevel;
}

void AdaptivePerformanceController::refreshBudget()
{
    double factor = 1.0;
    switch (status_.pressureLevel) {
        case AdaptivePressureLevel::Normal:
            factor = 1.0;
            break;
        case AdaptivePressureLevel::Elevated:
            factor = 0.75;
            break;
        case AdaptivePressureLevel::High:
            factor = 0.5;
            break;
        case AdaptivePressureLevel::Critical:
            factor = 0.25;
            break;
    }
    status_.effectiveMultiplier = (std::max)(kMinMultiplier, config_.maxMultiplier * factor);
    const auto multiplier = status_.effectiveMultiplier;
    budget_ = AdaptivePerformanceBudget{
        .fpsLimit = scaledFpsLimit(60U, multiplier),
        .maxRenderPointsPerChannel = scaledBudget(1200U, multiplier),
        .maxRenderVertices = scaledBudget(60000U, multiplier),
        .overviewMaxSamples = scaledBudget(20000U, multiplier),
        .rxChunkBytesPerPump = scaledBudget(4096U, multiplier),
        .transferFrameRowsPerPump = scaledBudget(2000U, multiplier),
        .plotAppendsPerPump = scaledBudget(128U, multiplier),
        .workerOutputFlushBudgetMs = 2.0 * multiplier,
    };
}

SystemPressureSample sampleSystemPressure()
{
    SystemPressureSample sample;
#ifdef _WIN32
    sample.cpuBusyRatio = sampleWindowsCpuBusyRatio();
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status) != 0 && status.ullTotalPhys > 0U) {
        sample.availableMemoryRatio =
            clampRatio(static_cast<double>(status.ullAvailPhys) / static_cast<double>(status.ullTotalPhys));
    }
#elif defined(__linux__)
    sample.cpuBusyRatio = sampleLinuxCpuBusyRatio();
    sample.availableMemoryRatio = sampleLinuxAvailableMemoryRatio();
#elif defined(__APPLE__)
    sample.cpuBusyRatio = sampleAppleCpuBusyRatio();
    sample.availableMemoryRatio = sampleAppleAvailableMemoryRatio();
#endif
    return sample;
}

const char* adaptivePressureLevelId(const AdaptivePressureLevel level)
{
    switch (level) {
        case AdaptivePressureLevel::Normal:
            return "normal";
        case AdaptivePressureLevel::Elevated:
            return "elevated";
        case AdaptivePressureLevel::High:
            return "high";
        case AdaptivePressureLevel::Critical:
            return "critical";
    }
    return "normal";
}

} // namespace protoscope::app
