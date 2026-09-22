#include "protoscope/plot/wave_analysis.hpp"

#include <utility>

namespace protoscope::plot {
WaveAnalysisWorker::WaveAnalysisWorker() : thread_([this] { run(); }) {}

WaveAnalysisWorker::~WaveAnalysisWorker()
{
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        measurement_.reset();
        fft_.reset();
    }
    ready_.notify_one();
    thread_.join();
}

void WaveAnalysisWorker::submit(WaveMeasurementInput input)
{
    {
        std::lock_guard lock(mutex_);
        measurement_ = std::move(input);
    }
    ready_.notify_one();
}

void WaveAnalysisWorker::submit(WaveFftInput input)
{
    // 明确移除所有活动指针，算法只读取 display 内的独立向量和通道元数据。
    for (auto& c : input.snapshot.channels) {
        c.samples = nullptr;
        c.summaryIndex = nullptr;
    }
    for (auto& c : input.display.channels)
        c.source.reset();
    {
        std::lock_guard lock(mutex_);
        fft_ = std::move(input);
    }
    ready_.notify_one();
}

std::optional<WaveMeasurementOutput> WaveAnalysisWorker::takeMeasurement()
{
    std::lock_guard lock(mutex_);
    return std::exchange(measurementResult_, std::nullopt);
}

std::optional<WaveFftOutput> WaveAnalysisWorker::takeFft()
{
    std::lock_guard lock(mutex_);
    return std::exchange(fftResult_, std::nullopt);
}

void WaveAnalysisWorker::run()
{
    while (true) {
        std::optional<WaveMeasurementInput> measurement;
        std::optional<WaveFftInput> fft;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || measurement_ || fft_; });
            if (stopping_)
                return;
            measurement = std::exchange(measurement_, std::nullopt);
            fft = std::exchange(fft_, std::nullopt);
        }
        if (measurement) {
            WaveMeasurementOutput output{measurement->generation, {}};
            try {
                output.result = makeMeasurementReadout(
                    measurement->channel,
                    measurement->times,
                    measurement->values,
                    measurement->reference.size() == measurement->values.size() ? &measurement->reference : nullptr);
            } catch (...) {
                output.result = {};
            }
            std::lock_guard lock(mutex_);
            measurementResult_ = std::move(output);
        }
        if (fft) {
            WaveFftOutput output{fft->generation, fft->key, {}};
            try {
                output.result = buildWaveFftFrame(fft->snapshot,
                                                  fft->display,
                                                  fft->key.config,
                                                  fft->key.channelEnabled,
                                                  fft->key.viewMinTime,
                                                  fft->key.viewMaxTime,
                                                  fft->key.sampleFrequencyHz);
                for (auto& c : output.result.channels) {
                    c.magnitudeTrace.reserve(c.bins.size());
                    c.phaseTrace.reserve(c.bins.size());
                    for (const auto& bin : c.bins) {
                        c.magnitudeTrace.push_back({bin.frequencyHz, bin.displayMagnitude});
                        c.phaseTrace.push_back({bin.frequencyHz, bin.phaseDegrees});
                    }
                    c.magnitudeSummary.synchronize(c.magnitudeTrace, 0);
                    c.phaseSummary.synchronize(c.phaseTrace, 0);
                }
            } catch (...) {
                output.result.message = "FFT 计算失败";
            }
            std::lock_guard lock(mutex_);
            fftResult_ = std::move(output);
        }
    }
}
} // namespace protoscope::plot
