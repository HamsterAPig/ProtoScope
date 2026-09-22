#include "protoscope/plot/wave_analysis.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace protoscope::plot;

int main()
{
    try {
        WaveAnalysisWorker worker;
        WaveMeasurementInput input{1, 0, {0, 1, 2, 3}, {1, 4, 2, 8}, {0, 0, 0, 0}};
        const auto expected = makeMeasurementReadout(0, input.times, input.values, &input.reference);
        worker.submit(input);
        std::optional<WaveMeasurementOutput> result;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!result && std::chrono::steady_clock::now() < deadline) {
            result = worker.takeMeasurement();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!result || result->generation != 1 || result->result.meanValue != expected.meanValue ||
            result->result.medianValue != expected.medianValue || result->result.rmsValue != expected.rmsValue)
            throw std::runtime_error("measurement mismatch");
        for (std::uint64_t generation = 2; generation <= 100; ++generation) {
            input.generation = generation;
            worker.submit(input);
        }
        bool latest = false;
        while (!latest && std::chrono::steady_clock::now() < deadline) {
            if (auto output = worker.takeMeasurement())
                latest = output->generation == 100;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!latest)
            throw std::runtime_error("latest request starved");
        OscilloscopeBuffer buffer;
        WaveAppendRequest append;
        for (int i = 0; i < 1024; ++i)
            append.samples.push_back({i / 1024.0, std::sin(i * 0.1)});
        buffer.append(0, std::move(append));
        auto snapshot = buffer.snapshot(0, 1, false);
        auto display = buildDisplayData(snapshot, 1024);
        WaveFftCacheKey key{.viewMinTime = 0,
                            .viewMaxTime = 1,
                            .sampleFrequencyHz = 1024,
                            .config = {.enabled = true},
                            .channelEnabled = {1}};
        const auto expectedFft = buildWaveFftFrame(snapshot, display, key.config, {1}, 0, 1, 1024);
        worker.submit(WaveFftInput{9, key, std::move(snapshot), std::move(display)});
        buffer.clear();
        std::optional<WaveFftOutput> fft;
        while (!fft && std::chrono::steady_clock::now() < deadline) {
            fft = worker.takeFft();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!fft || !fft->result.valid || fft->result.channels[0].bins.size() != expectedFft.channels[0].bins.size())
            throw std::runtime_error("FFT snapshot lifetime");
        for (std::size_t i = 0; i < expectedFft.channels[0].bins.size(); ++i)
            if (fft->result.channels[0].bins[i].magnitude != expectedFft.channels[0].bins[i].magnitude)
                throw std::runtime_error("FFT mismatch");
        std::cout << "wave_analysis: all checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
