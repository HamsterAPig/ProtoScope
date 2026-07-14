#pragma once

#include "protoscope/config/config.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace protoscope::app {

enum class AdaptivePressureLevel {
    Normal,
    Elevated,
    High,
    Critical,
};

struct SystemPressureSample {
    std::optional<double> cpuBusyRatio{};
    std::optional<double> availableMemoryRatio{};
};

struct AdaptivePerformanceInput {
    std::uint64_t nowMs{0};
    SystemPressureSample system{};
    std::size_t pendingRxBytes{0};
    std::size_t rawBacklogWarnBytes{0};
    std::size_t pendingTransferFrameRows{0};
    std::size_t pendingPlotAppends{0};
    std::size_t pendingWorkerRxBytes{0};
    std::size_t workerInputQueueSize{0};
    std::size_t workerOutputQueueSize{0};
    double scriptPumpMs{0.0};
};

struct AdaptivePerformanceBudget {
    std::uint32_t fpsLimit{60};
    std::size_t maxRenderPointsPerChannel{1200};
    std::size_t maxRenderVertices{60000};
    std::size_t overviewMaxSamples{20000};
    std::size_t rxChunkBytesPerPump{4096};
    std::size_t transferFrameRowsPerPump{2000};
    std::size_t plotAppendsPerPump{128};
    double workerOutputFlushBudgetMs{2.0};
};

struct AdaptivePerformanceStatus {
    bool enabled{false};
    double maxMultiplier{1.0};
    double effectiveMultiplier{1.0};
    AdaptivePressureLevel pressureLevel{AdaptivePressureLevel::Normal};
    std::string reason{"disabled"};
    bool systemMetricsAvailable{false};
};

class AdaptivePerformanceController {
public:
    void configure(config::AdaptivePerformanceConfig config);
    void update(const AdaptivePerformanceInput& input);

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool shouldSample(std::uint64_t nowMs) const;
    [[nodiscard]] const AdaptivePerformanceBudget& budget() const;
    [[nodiscard]] const AdaptivePerformanceStatus& status() const;

private:
    [[nodiscard]] AdaptivePressureLevel classifyPressure(const AdaptivePerformanceInput& input,
                                                          std::string& reason) const;
    void refreshBudget();

private:
    config::AdaptivePerformanceConfig config_{};
    AdaptivePerformanceBudget budget_{};
    AdaptivePerformanceStatus status_{};
    std::uint64_t lastSampleAtMs_{0};
    std::size_t healthySamples_{0};
};

[[nodiscard]] SystemPressureSample sampleSystemPressure();
[[nodiscard]] const char* adaptivePressureLevelId(AdaptivePressureLevel level);

} // namespace protoscope::app
