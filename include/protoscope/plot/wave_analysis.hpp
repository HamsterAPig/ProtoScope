#pragma once
#include "protoscope/plot/wave_fft.hpp"

#include <condition_variable>
#include <mutex>
#include <thread>

namespace protoscope::plot {

struct WaveMeasurementInput {
    std::uint64_t generation{0};
    std::size_t channel{0};
    std::vector<double> times;
    std::vector<double> values;
    std::vector<double> reference;
};

struct WaveFftInput {
    std::uint64_t generation{0};
    WaveFftCacheKey key;
    WaveSnapshot snapshot;
    WaveDisplayData display;
};

struct WaveMeasurementOutput {
    std::uint64_t generation{0};
    MeasurementReadout result;
};

struct WaveFftOutput {
    std::uint64_t generation{0};
    WaveFftCacheKey key;
    WaveFftFrame result;
};

// 波形专用工作线程：每类最多一个待执行快照，正在计算的输入拥有独立生命周期。
class WaveAnalysisWorker {
public:
    WaveAnalysisWorker();
    ~WaveAnalysisWorker();
    WaveAnalysisWorker(const WaveAnalysisWorker&) = delete;
    WaveAnalysisWorker& operator=(const WaveAnalysisWorker&) = delete;
    void submit(WaveMeasurementInput input);
    void submit(WaveFftInput input);
    std::optional<WaveMeasurementOutput> takeMeasurement();
    std::optional<WaveFftOutput> takeFft();

private:
    void run();
    std::mutex mutex_;
    std::condition_variable ready_;
    bool stopping_{false};
    std::optional<WaveMeasurementInput> measurement_;
    std::optional<WaveFftInput> fft_;
    std::optional<WaveMeasurementOutput> measurementResult_;
    std::optional<WaveFftOutput> fftResult_;
    std::thread thread_;
};
} // namespace protoscope::plot
