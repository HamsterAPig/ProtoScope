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

void testDigitalFidelity()
{
    OscilloscopeBuffer buffer;
    std::vector<WaveSample> samples(1000000);
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = {0.25 + static_cast<double>(i) * 0.01,
                      i >= 12345 && i < 765432 ? 1.0 : 0.0};
    buffer.append(0, {{}, samples});
    const auto analogBytes = buffer.snapshot(-1e20, 1e20, false).channels[0].summaryIndex->memoryBytes();
    buffer.setChannelSpec(0, {.bitDisplay = {.enabled = true, .bitCount = 8}});
    const auto snapshot = buffer.snapshot(-1e20, 1e20, false);
    require(snapshot.channels[0].summaryIndex->memoryBytes() > analogBytes, "digital-only count storage");
    for (const auto axis : {WaveTimeAxisSource::ScriptTime, WaveTimeAxisSource::SampleIndex,
                            WaveTimeAxisSource::SampleFrequency}) {
        const WaveQueryView query(snapshot.channels[0], axis, 200, snapshot.config.displayFormula);
        for (const auto budget : {8U, 32U, 128U}) {
            for (const auto width : {300U, 1200U}) {
                WaveQueryCounters counters;
                const auto segments = query.digitalSegments(query.time(0), query.time(samples.size() - 1),
                                                             0, budget, width, &counters);
                require(segments.size() <= budget, "digital primitive budget");
                std::vector<double> edges;
                for (const auto& segment : segments) {
                    require(!segment.activity, "sparse transitions must remain exact");
                    if (segment.firstState != segment.lastState) edges.push_back(segment.endTime);
                }
                require(edges == std::vector<double>{query.time(12345), query.time(765432)},
                        "zoom must not relocate digital edges");
                require(counters.rawSamples < 50000, "sparse million samples must use summaries");
            }
        }
        require(query.bitTransitions(query.time(12345), query.time(765432), 0) == 2, "inclusive edge count");
        require(query.bitTransitions(query.time(12345) + 0.001, query.time(765432) - 0.001, 0) == 0,
                "outside edge exclusion");
        require(query.bitTransitions(query.time(0), query.time(0), 0) == 0, "first sample is not an edge");
        for (const auto [left, right] : {std::pair{0U, 20000U}, std::pair{12000U, 790000U},
                                         std::pair{765000U, 780000U}, std::pair{12344U, 12346U}}) {
            const auto segments = query.digitalSegments(query.time(left), query.time(right), 0, 8, 300);
            std::vector<double> actual, expected;
            for (const auto i : {12345U, 765432U})
                if (i >= left && i <= right) expected.push_back(query.time(i));
            for (const auto& segment : segments) {
                require(!segment.activity, "sparse pan and zoom must remain exact");
                if (segment.firstState != segment.lastState) actual.push_back(segment.endTime);
            }
            require(actual == expected, "pan and zoom must preserve original edge times");
        }
    }
    buffer.setChannelSpec(0, {});
    require(buffer.snapshot(-1e20, 1e20, false).channels[0].summaryIndex->memoryBytes() == analogBytes,
            "disabling digital display releases count storage");
}

void testDigitalCounts()
{
    OscilloscopeBuffer buffer;
    buffer.setChannelSpec(0, {.bitDisplay = {.enabled = true, .bitCount = 8}});
    std::mt19937 rng(19);
    const auto check = [&] {
        const auto snapshot = buffer.snapshot(-1e20, 1e20, false);
        const auto& channel = snapshot.channels[0];
        for (const auto axis : {WaveTimeAxisSource::ScriptTime, WaveTimeAxisSource::SampleIndex,
                                WaveTimeAxisSource::SampleFrequency}) {
            const WaveQueryView query(channel, axis, 31, snapshot.config.displayFormula);
            for (int trial = 0; trial < 80; ++trial) {
                auto a = static_cast<std::size_t>(rng()) % channel.totalSamples;
                auto b = static_cast<std::size_t>(rng()) % channel.totalSamples;
                if (a > b) std::swap(a, b);
                for (std::size_t bit = 0; bit < 8; ++bit) {
                    std::size_t expected = 0;
                    // 独立逐样本扫描，以新状态的时间判定是否落窗。
                    for (std::size_t i = 1; i < channel.totalSamples; ++i)
                        if (query.time(i) >= query.time(a) && query.time(i) <= query.time(b) &&
                            ((static_cast<unsigned>(channel.samples[i - 1].value) ^
                              static_cast<unsigned>(channel.samples[i].value)) & (1U << bit)))
                            ++expected;
                    require(query.bitTransitions(query.time(a), query.time(b), bit) == expected,
                            "indexed count disagrees with independent scan");
                }
            }
        }
    };
    std::vector<WaveSample> samples;
    for (int i = 0; i < 4097; ++i) samples.push_back({i * 0.125, static_cast<double>(rng() % 256)});
    buffer.append(0, {{}, samples});
    check();
    for (int i = 4097; i < 4400; ++i) buffer.append(0, {{}, {{i * 0.125, static_cast<double>(rng() % 256)}}});
    check();
    buffer.setMaxTotalSamples(777);
    check();
    buffer.append(0, {{}, {{0, 1}, {1, 0}, {2, 3}}});
    check();
    buffer.clear();
    buffer.setChannelSpec(0, {.bitDisplay = {.enabled = true, .bitCount = 8}});
    buffer.appendImported(0, {{5, 1}, {5, 0}, {6, 3}, {9, 2}}, 1000);
    check();
    buffer.setChannelSpec(0, {});
    check();
    buffer.setChannelSpec(0, {.bitDisplay = {.enabled = true}});
    check();
}

void testDigitalActivity()
{
    OscilloscopeBuffer buffer;
    buffer.setChannelSpec(0, {.bitDisplay = {.enabled = true}});
    std::vector<WaveSample> samples;
    for (int i = 0; i < 8192; ++i) samples.push_back({i * 0.1, i >= 1024 && i < 2048 ? double(i % 2) : 0.0});
    buffer.append(0, {{}, samples});
    const auto snap = buffer.snapshot(0, 1000, false);
    const WaveQueryView query(snap.channels[0], WaveTimeAxisSource::ScriptTime, 0, snap.config.displayFormula);
    for (const auto budget : {1U, 2U, 3U, 8U, 32U, 1200U}) {
        for (const auto width : {20U, 300U, 1200U}) {
            const auto segments = query.digitalSegments(0, 819.1, 0, budget, width);
            require(segments.size() <= budget, "dense geometry budget");
            require(std::ranges::any_of(segments, [](const auto& s) { return s.activity; }),
                    "complex intervals must be activity bands");
            for (const auto& s : segments) {
                if (s.activity || s.firstState == s.lastState) continue;
                require(s.beginTime == s.endTime, "exact transition must be vertical");
                const auto found = std::ranges::find_if(samples, [&](const auto& sample) { return sample.time == s.endTime; });
                require(found != samples.end() && found != samples.begin() && found->value != (found - 1)->value,
                        "no bucket-generated digital edges");
            }
        }
    }
    const auto zoom = query.digitalSegments(102.4, 103.0, 0, 32, 1200);
    require(std::ranges::none_of(zoom, [](const auto& s) { return s.activity; }), "zoom restores narrow pulses");
    require(query.bitTransitions(102.4, 103.0, 0) == 6, "narrow pulse count");
}

void testTimeEnvelope()
{
    for (int signal = 0; signal < 4; ++signal) {
        OscilloscopeBuffer buffer;
        buffer.setChannelSpec(0, {.ratio = 2, .scale = -3, .offset = 5});
        std::vector<WaveSample> samples;
        double t = 0;
        for (int i = 0; i < 100000; ++i) {
            t += i % 3 == 0 ? 0.002 : 0.001;
            if (i == 50000) t += 100;
            const auto value = signal == 0 ? std::sin(i * 0.7) :
                signal == 1 ? (1 + 0.8 * std::sin(i * 0.0003)) * std::sin(i * 0.9) :
                signal == 2 ? (i == 255 ? 999.0 : i == 256 ? -777.0 : 0.0) : 4.0;
            samples.push_back({t, value});
        }
        buffer.appendImported(0, samples, 71);
        const auto snapshot = buffer.snapshot(-1e20, 1e20, false);
        for (const auto axis : {WaveTimeAxisSource::ScriptTime, WaveTimeAxisSource::SampleIndex,
                                WaveTimeAxisSource::SampleFrequency}) {
            const WaveQueryView query(snapshot.channels[0], axis, 123, WaveDisplayFormula::ScaleThenOffset);
            for (const auto buckets : {17U, 511U}) {
                const auto first = query.time(0), last = query.time(samples.size() - 1);
                const auto actual = query.timeEnvelope(first, last, buckets);
                std::size_t index = 0, output = 0;
                for (std::size_t b = 0; b < buckets; ++b) {
                    const auto t0 = first + (last - first) * double(b) / double(buckets);
                    const auto t1 = first + (last - first) * double(b + 1) / double(buckets);
                    const auto begin = index;
                    double low = 1e100, high = -1e100;
                    // 原始数据独立逐桶扫描，不使用摘要或查询返回的极值作为预期值。
                    while (index < samples.size() && (b + 1 == buckets || query.time(index) < t1)) {
                        const auto value = samples[index].value * -6 + 5;
                        low = (std::min)(low, value);
                        high = (std::max)(high, value);
                        ++index;
                    }
                    if (index == begin) continue;
                    require(output < actual.size(), "missing nonempty overview bucket");
                    const auto& a = actual[output++];
                    require(a.beginTime == t0 && a.endTime == t1, "overview must retain bucket time extent");
                    require(a.minValue == low && a.maxValue == high && a.sampleCount == index - begin,
                            "overview extrema disagree with raw samples");
                }
                require(output == actual.size(), "empty overview bucket must not be zero-filled");
            }
        }
    }
}

int main()
{
    try {
        testDigitalFidelity();
        testDigitalCounts();
        testDigitalActivity();
        testTimeEnvelope();
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
