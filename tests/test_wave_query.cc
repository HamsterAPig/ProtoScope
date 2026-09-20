#include "protoscope/plot/wave_math.hpp"
#include "protoscope/plot/wave_query.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace protoscope::plot;

void require(bool value, const char* message)
{
    if (!value)
        throw std::runtime_error(message);
}

int main()
{
    try {
        OscilloscopeBuffer buffer;
        std::vector<WaveSample> samples(10000);
        for (std::size_t i = 0; i < samples.size(); ++i)
            samples[i] = {static_cast<double>(i), static_cast<double>((i * 17) % 31)};
        samples[255].value = 65536;
        samples[256].value = -500;
        buffer.append(0, {{}, samples});
        auto snap = buffer.snapshot(0, 10000, false);
        std::mt19937 rng(42);
        // 用独立线性扫描对比随机区间，覆盖摘要边界和多 bit 活动。
        const auto check = [&]() {
            snap = buffer.snapshot(-1e100, 1e100, false);
            const auto& c = snap.channels[0];
            WaveQueryView query(c, WaveTimeAxisSource::ScriptTime, 0, snap.config.displayFormula);
            for (int trial = 0; trial < 1000; ++trial) {
                auto begin = static_cast<std::size_t>(rng()) % c.totalSamples;
                auto end = static_cast<std::size_t>(rng()) % c.totalSamples;
                if (begin > end)
                    std::swap(begin, end);
                ++end;
                const auto summary = query.summary(begin, end);
                double minimum = c.samples[begin].value, maximum = minimum;
                auto bitsAnd = waveRawBits(minimum), bitsOr = bitsAnd;
                for (auto i = begin; i < end; ++i) {
                    minimum = (std::min)(minimum, c.samples[i].value);
                    maximum = (std::max)(maximum, c.samples[i].value);
                    bitsAnd &= waveRawBits(c.samples[i].value);
                    bitsOr |= waveRawBits(c.samples[i].value);
                }
                require(summary.count == end - begin, "summary count");
                require(summary.minValue == minimum && summary.maxValue == maximum, "summary extrema");
                require(summary.bitsAnd == bitsAnd && summary.bitsOr == bitsOr, "digital summary");
                require(summary.first == c.sampleIndexOffset + begin, "stable source index");
            }
        };
        check();
        buffer.setChannelSpec(0, {.ratio = 2, .scale = -3, .offset = 5});
        snap = buffer.snapshot(0, 10000, false);
        WaveQueryView query(
            snap.channels[0], WaveTimeAxisSource::SampleFrequency, 2, WaveDisplayFormula::ScaleThenOffset);
        require(query.sample(255).value == samples[255].value * -6 + 5, "negative transform");
        require(query.time(256) == 128, "sample frequency time");
        const auto trace = query.traceIndices(0, 5000, 1200);
        require(trace.size() <= 1200, "point budget");
        require(std::ranges::find(trace, 255) != trace.end(), "boundary pulse");
        require(std::ranges::find(trace, 256) != trace.end(), "boundary negative pulse");
        WaveQueryView rawQuery(snap.channels[0], WaveTimeAxisSource::ScriptTime, 0,
                                WaveDisplayFormula::ScaleThenOffset);
        require(rawQuery.bitEdge(254, 257, 16, true, false) == 255, "summary bit edge");
        require(rawQuery.bitEdge(254, 257, 16, false, false) == 256, "summary bit falling edge");
        const auto crossing = rawQuery.firstCrossing(254, 256, -1000, false);
        require(crossing && *crossing > 254 && *crossing < 255, "raw interpolated crossing");
        WaveDisplayData bounded;
        buildQueryDisplayDataInto(snap, 2, 100, bounded);
        const auto exactReadout = findNearestDisplayByTime(bounded, 0, 1234.5, 0);
        require(exactReadout && exactReadout->value == samples[2469].value * 2, "exact readout behind summary");
        require(extractDisplayWindow(bounded.channels[0], 0, 100).samples.size() == 201,
                "analysis must retain original samples");
        buffer.setMaxTotalSamples(4321);
        check();
        for (std::size_t i = 10000; i < 10700; ++i)
            buffer.append(0, {{}, {{static_cast<double>(i), static_cast<double>(i % 13)}}});
        check();
        buffer.append(0, {{}, {{0, 7}, {1, 7}, {2, 7}}});
        check();
        buffer.clear();
        require(buffer.channelCount() == 0, "clear");
        std::cout << "wave_query: all checks passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
