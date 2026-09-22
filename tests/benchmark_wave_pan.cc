#include "protoscope/plot/wave_math.hpp"
#include "protoscope/plot/wave_query.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>

using namespace protoscope::plot;
using Clock = std::chrono::steady_clock;

double elapsed(Clock::time_point start)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

int main(int argc, char** argv)
{
    const bool baseline = argc > 1 && std::string_view(argv[1]) == "--baseline";
    for (const std::size_t count : {100000U, 1000000U}) {
        OscilloscopeBuffer buffer;
        double indexMs = 0;
        for (std::size_t c = 0; c < 5; ++c) {
            std::vector<WaveSample> samples(count + 30000);
            for (std::size_t i = 0; i < samples.size(); ++i)
                samples[i] = {static_cast<double>(i),
                              c == 4 ? static_cast<double>(i * 2654435761U)
                                     : std::sin(static_cast<double>(i) * 0.03 + static_cast<double>(c))};
            const auto start = Clock::now();
            buffer.append(c, {{}, std::move(samples)});
            indexMs += elapsed(start);
        }
        std::vector<double> times;
        std::size_t points = 0;
        WaveQueryCounters counters;
        const auto snapshot = buffer.snapshot(-1e100, 1e100, false);
        for (int frame = -10; frame < 300; ++frame) {
            const double begin = static_cast<double>((std::max)(frame, 0) * 100);
            const auto start = Clock::now();
            if (baseline) {
                auto visible = buffer.snapshot(begin, begin + static_cast<double>(count), false);
                const auto display = buildDisplayData(visible, 0);
                points += display.channels[0].samples.size();
            } else {
                for (const auto& channel : snapshot.channels) {
                    WaveQueryView query(channel, WaveTimeAxisSource::ScriptTime, 0, snapshot.config.displayFormula);
                    points += query.traceIndices(begin, begin + static_cast<double>(count), 1200, &counters).size();
                }
            }
            const auto ms = elapsed(start);
            if (frame >= 0)
                times.push_back(ms);
        }
        std::ranges::sort(times);
        std::size_t bytes = 0;
        for (const auto& channel : snapshot.channels)
            bytes += channel.summaryIndex->memoryBytes();
        std::cout << (baseline ? "copy_baseline" : "summary_query") << " samples=" << count << " p50_ms=" << times[150]
                  << " p95_ms=" << times[285] << " append_index_ms=" << indexMs << " summary_bytes=" << bytes
                  << " raw_accesses=" << counters.rawSamples << " summary_hits=" << counters.summaryHits
                  << " output_points=" << points << '\n';
    }
}
