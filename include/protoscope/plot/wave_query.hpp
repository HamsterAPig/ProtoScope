#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace protoscope::plot {

struct WaveSample;
struct ChannelView;
enum class WaveDisplayFormula;
enum class WaveTimeAxisSource;
enum class WaveBitDenseRenderMode { CompressedSteps, ActivityBand };

struct WaveDigitalBucket {
    double beginTime{0};
    double endTime{0};
    std::uint64_t firstBits{0};
    std::uint64_t lastBits{0};
    std::uint64_t activity{0};
};

struct WaveDigitalSegment {
    double beginTime{0};
    double endTime{0};
    bool firstState{false};
    bool lastState{false};
    bool activity{false};
};

struct WaveTimeEnvelope {
    double beginTime{0};
    double endTime{0};
    double minValue{0};
    double maxValue{0};
    std::size_t sampleCount{0};
};

struct WaveSummary {
    std::size_t count{0};
    std::size_t first{0};
    std::size_t last{0};
    std::size_t minimum{0};
    std::size_t maximum{0};
    double minValue{0};
    double maxValue{0};
    std::uint64_t firstBits{0};
    std::uint64_t lastBits{0};
    std::uint64_t bitsAnd{0};
    std::uint64_t bitsOr{0};
    double firstTime{0};
    double lastTime{0};
    double minStep{0};
    bool timeIncreasing{false};
    std::size_t finiteCount{0};
    double firstValue{0};
    double lastValue{0};
    double rise{0};
    double fall{0};
    std::size_t riseBefore{0};
    std::size_t fallBefore{0};
};

struct WaveQueryCounters {
    std::size_t rawSamples{0};
    std::size_t summaryHits{0};
};

std::uint64_t waveRawBits(double value);

class WaveSummaryIndex {
public:
    static constexpr std::size_t blockSize = 256;
    void clear();
    void synchronize(std::span<const WaveSample> samples, std::size_t sampleOffset, bool digital = false);
    WaveSummary query(std::span<const WaveSample> samples,
                      std::size_t sampleOffset,
                      std::size_t begin,
                      std::size_t end,
                      WaveQueryCounters* counters = nullptr) const;
    std::size_t memoryBytes() const;
    std::uint64_t bitTransitions(std::span<const WaveSample> samples, std::size_t sampleOffset,
                                 std::size_t begin, std::size_t end, std::size_t bit,
                                 WaveQueryCounters* counters = nullptr) const;

private:
    struct Level {
        std::size_t firstBlock{0};
        std::deque<WaveSummary> blocks;
        std::optional<std::deque<std::array<std::uint64_t, 64>>> transitions;
    };

    std::vector<Level> levels_;
    std::size_t begin_{0};
    std::size_t end_{0};
    bool digital_{false};
};

// 查询视图只在当前 UI 帧借用原始样本；跨线程分析必须使用 extract() 的独立副本。
class WaveQueryView {
public:
    WaveQueryView(const ChannelView& channel, WaveTimeAxisSource axis, double frequency, WaveDisplayFormula formula);
    std::size_t size() const;
    double time(std::size_t index) const;
    double actual(std::size_t index) const;
    WaveSample sample(std::size_t index) const;
    std::pair<std::size_t, std::size_t> range(double minTime, double maxTime, bool guards = true) const;
    WaveSummary summary(std::size_t begin, std::size_t end, WaveQueryCounters* counters = nullptr) const;
    std::vector<std::size_t> traceIndices(double minTime,
                                          double maxTime,
                                          std::size_t budget,
                                          WaveQueryCounters* counters = nullptr,
                                          bool guards = true) const;
    std::vector<WaveSample> extract(double minTime, double maxTime) const;
    std::vector<WaveDigitalBucket> digitalBuckets(double minTime,
                                                  double maxTime,
                                                  std::size_t budget,
                                                  WaveQueryCounters* counters = nullptr) const;
    std::vector<WaveDigitalSegment> digitalSegments(double minTime, double maxTime, std::size_t bit,
                                                     std::size_t primitiveBudget, std::size_t pixelWidth,
                                                     WaveQueryCounters* counters = nullptr) const;
    std::uint64_t bitTransitions(double minTime, double maxTime, std::size_t bit,
                                 WaveQueryCounters* counters = nullptr) const;
    std::vector<WaveTimeEnvelope> timeEnvelope(double minTime, double maxTime, std::size_t buckets,
                                               WaveQueryCounters* counters = nullptr) const;
    std::optional<double> firstCrossing(double minTime, double maxTime, double threshold, bool rising) const;
    std::optional<std::size_t> bitEdge(double minTime, double maxTime, std::size_t bit, bool state, bool reverse) const;

private:
    const ChannelView& channel_;
    WaveTimeAxisSource axis_;
    double frequency_;
    WaveDisplayFormula formula_;
};

} // namespace protoscope::plot
