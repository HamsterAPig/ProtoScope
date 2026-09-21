#include "protoscope/plot/wave_query.hpp"

#include "protoscope/plot/oscilloscope.hpp"
#include "protoscope/plot/wave_math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>

namespace protoscope::plot {
namespace {
    WaveSummary merge(WaveSummary a, const WaveSummary& b)
    {
        if (a.count == 0)
            return b;
        if (b.count == 0)
            return a;
        // 摘要保留最强相邻跳变的前点，合并时补上块间边界；同强度取较早位置。
        const auto keep = [](double strength, std::size_t before, double& best, std::size_t& bestBefore) {
            if (strength > best || (strength > 0 && strength == best && before < bestBefore)) {
                best = strength;
                bestBefore = before;
            }
        };
        keep(b.rise, b.riseBefore, a.rise, a.riseBefore);
        keep(b.fall, b.fallBefore, a.fall, a.fallBefore);
        if (std::isfinite(a.lastValue) && std::isfinite(b.firstValue) && a.last + 1 == b.first) {
            const auto delta = b.firstValue - a.lastValue;
            keep(delta, a.last, a.rise, a.riseBefore);
            keep(-delta, a.last, a.fall, a.fallBefore);
        }
        a.lastValue = b.lastValue;
        const auto step = b.firstTime - a.lastTime;
        a.timeIncreasing = a.timeIncreasing && b.timeIncreasing && step > 0;
        if (step > 0 && (a.minStep == 0 || step < a.minStep))
            a.minStep = step;
        if (b.minStep > 0 && (a.minStep == 0 || b.minStep < a.minStep))
            a.minStep = b.minStep;
        a.lastTime = b.lastTime;
        a.count += b.count;
        a.finiteCount += b.finiteCount;
        a.last = b.last;
        a.lastBits = b.lastBits;
        if ((b.minValue < a.minValue || !std::isfinite(a.minValue)) && std::isfinite(b.minValue)) {
            a.minValue = b.minValue;
            a.minimum = b.minimum;
        }
        if ((b.maxValue > a.maxValue || !std::isfinite(a.maxValue)) && std::isfinite(b.maxValue)) {
            a.maxValue = b.maxValue;
            a.maximum = b.maximum;
        }
        a.bitsAnd &= b.bitsAnd;
        a.bitsOr |= b.bitsOr;
        return a;
    }

    WaveSummary scan(std::span<const WaveSample> samples,
                     std::size_t offset,
                     std::size_t begin,
                     std::size_t end,
                     WaveQueryCounters* counters)
    {
        WaveSummary result;
        for (auto i = begin; i < end; ++i) {
            const auto value = samples[i - offset].value;
            const auto bits = waveRawBits(value);
            const auto time = samples[i - offset].time;
            result = merge(result,
                           {1,
                            i,
                            i,
                            i,
                            i,
                            value,
                            value,
                            bits,
                            bits,
                            bits,
                            bits,
                            time,
                            time,
                            0,
                            std::isfinite(time),
                            std::isfinite(value) ? 1U : 0U,
                            value,
                            value});
        }
        if (counters)
            counters->rawSamples += end - begin;
        return result;
    }
} // namespace

std::uint64_t waveRawBits(double value)
{
    if (!std::isfinite(value) || value <= 0)
        return 0;
    if (value >= static_cast<double>((std::numeric_limits<std::uint64_t>::max)()))
        return (std::numeric_limits<std::uint64_t>::max)();
    return static_cast<std::uint64_t>(value);
}

void WaveSummaryIndex::clear()
{
    levels_.clear();
    begin_ = end_ = 0;
}

void WaveSummaryIndex::synchronize(std::span<const WaveSample> samples, std::size_t offset, bool digital)
{
    if (digital_ != digital) clear();
    digital_ = digital;
    if (samples.empty()) {
        clear();
        return;
    }
    const auto newEnd = offset + samples.size();
    if (offset < begin_ || newEnd < end_ || offset >= end_)
        clear();
    const auto oldEnd = end_;
    begin_ = offset;
    end_ = newEnd;
    std::size_t width = blockSize;
    std::size_t levelIndex = 0;
    // 首块因裁剪而变化，尾部因追加而变化；中间完整块始终复用。
    while (true) {
        if (levels_.size() <= levelIndex)
            levels_.emplace_back();
        auto& level = levels_[levelIndex];
        const auto first = offset / width;
        const auto last = (newEnd - 1) / width;
        while (!level.blocks.empty() && level.firstBlock < first) {
            level.blocks.pop_front();
            if (digital_) level.transitions->pop_front();
            ++level.firstBlock;
        }
        if (level.blocks.empty())
            level.firstBlock = first;
        level.blocks.resize(last - first + 1);
        if (digital_) {
            if (!level.transitions) level.transitions.emplace();
            level.transitions->resize(last - first + 1);
        }
        const auto rebuild = [&](std::size_t block) {
            if (digital_) {
                auto& counts = (*level.transitions)[block - first];
                counts.fill(0);
                const auto addBoundary = [&](std::uint64_t changed) {
                    while (changed) {
                        ++counts[std::countr_zero(changed)];
                        changed &= changed - 1;
                    }
                };
                if (levelIndex == 0) {
                    const auto begin = (std::max)(offset, block * width);
                    const auto end = (std::min)(newEnd, (block + 1) * width);
                    for (auto i = begin + 1; i < end; ++i)
                        addBoundary(waveRawBits(samples[i - offset - 1].value) ^
                                    waveRawBits(samples[i - offset].value));
                } else {
                    const auto& child = levels_[levelIndex - 1];
                    const WaveSummary* previous = nullptr;
                    // 子块只记内部翻转，合并时补计两个子块交界处的真实变化。
                    for (auto id = block * 2; id < block * 2 + 2; ++id) {
                        if (id < child.firstBlock || id - child.firstBlock >= child.blocks.size()) continue;
                        const auto index = id - child.firstBlock;
                        for (std::size_t bit = 0; bit < 64; ++bit)
                            counts[bit] += (*child.transitions)[index][bit];
                        if (previous) addBoundary(previous->lastBits ^ child.blocks[index].firstBits);
                        previous = &child.blocks[index];
                    }
                }
            }
            if (levelIndex == 0) {
                return scan(samples,
                            offset,
                            (std::max)(offset, block * width),
                            (std::min)(newEnd, (block + 1) * width),
                            nullptr);
            }
            WaveSummary result;
            const auto& child = levels_[levelIndex - 1];
            for (auto id = block * 2; id < block * 2 + 2; ++id) {
                if (id >= child.firstBlock && id - child.firstBlock < child.blocks.size())
                    result = merge(result, child.blocks[id - child.firstBlock]);
            }
            return result;
        };
        level.blocks.front() = rebuild(first);
        const auto tail = (std::max)(first + 1, oldEnd / width);
        for (auto block = tail; block <= last; ++block)
            level.blocks[block - first] = rebuild(block);
        ++levelIndex;
        if (first == last)
            break;
        width *= 2;
    }
    levels_.resize(levelIndex);
}

WaveSummary WaveSummaryIndex::query(std::span<const WaveSample> samples,
                                    std::size_t offset,
                                    std::size_t begin,
                                    std::size_t end,
                                    WaveQueryCounters* counters) const
{
    begin = (std::min)(begin, samples.size()) + offset;
    end = (std::min)(end, samples.size()) + offset;
    WaveSummary result;
    // 两端不足基础块的部分精确读取，中间贪心选取最大的对齐摘要。
    while (begin < end) {
        std::size_t selected = levels_.size();
        std::size_t width = blockSize;
        for (std::size_t l = 0; l < levels_.size(); ++l, width *= 2) {
            if (begin % width != 0 || width > end - begin)
                break;
            const auto id = begin / width;
            const auto& level = levels_[l];
            if (id >= level.firstBlock && id - level.firstBlock < level.blocks.size() &&
                level.blocks[id - level.firstBlock].count == width)
                selected = l;
        }
        if (selected < levels_.size()) {
            width = blockSize << selected;
            const auto& level = levels_[selected];
            result = merge(result, level.blocks[begin / width - level.firstBlock]);
            begin += width;
            if (counters)
                ++counters->summaryHits;
        } else {
            const auto next = (std::min)(end, (begin / blockSize + 1) * blockSize);
            result = merge(result, scan(samples, offset, begin, next, counters));
            begin = next;
        }
    }
    return result;
}

std::size_t WaveSummaryIndex::memoryBytes() const
{
    std::size_t bytes = levels_.capacity() * sizeof(Level);
    for (const auto& level : levels_)
        bytes += level.blocks.size() * sizeof(WaveSummary) +
                 (level.transitions ? level.transitions->size() * sizeof(std::array<std::uint64_t, 64>) : 0);
    return bytes;
}

std::uint64_t WaveSummaryIndex::bitTransitions(std::span<const WaveSample> samples, std::size_t offset,
                                               std::size_t begin, std::size_t end, std::size_t bit,
                                               WaveQueryCounters* counters) const
{
    if (bit >= 64) return 0;
    begin = (std::min)(begin, samples.size()) + offset;
    end = (std::min)(end, samples.size()) + offset;
    const auto mask = std::uint64_t{1} << bit;
    std::uint64_t result = 0, previous = 0;
    bool hasPrevious = false;
    while (begin < end) {
        std::size_t selected = levels_.size(), width = blockSize;
        if (digital_) {
            for (std::size_t l = 0; l < levels_.size(); ++l, width *= 2) {
                if (begin % width || width > end - begin) break;
                const auto& level = levels_[l];
                const auto id = begin / width;
                if (id >= level.firstBlock && id - level.firstBlock < level.blocks.size() &&
                    level.blocks[id - level.firstBlock].count == width) selected = l;
            }
        }
        if (selected < levels_.size()) {
            width = blockSize << selected;
            const auto& level = levels_[selected];
            const auto id = begin / width - level.firstBlock;
            const auto& summary = level.blocks[id];
            result += (*level.transitions)[id][bit];
            if (hasPrevious && ((previous ^ summary.firstBits) & mask)) ++result;
            previous = summary.lastBits;
            hasPrevious = true;
            begin += width;
            if (counters) ++counters->summaryHits;
        } else {
            const auto bits = waveRawBits(samples[begin - offset].value);
            if (hasPrevious && ((previous ^ bits) & mask)) ++result;
            previous = bits;
            hasPrevious = true;
            ++begin;
            if (counters) ++counters->rawSamples;
        }
    }
    return result;
}

WaveQueryView::WaveQueryView(const ChannelView& channel,
                             WaveTimeAxisSource axis,
                             double frequency,
                             WaveDisplayFormula formula)
    : channel_(channel), axis_(axis), frequency_(frequency), formula_(formula)
{
}

std::size_t WaveQueryView::size() const
{
    return channel_.samples ? channel_.totalSamples : 0;
}

double WaveQueryView::time(std::size_t index) const
{
    if (axis_ == WaveTimeAxisSource::ScriptTime)
        return channel_.samples[index].time;
    const auto global = static_cast<double>(channel_.sampleIndexOffset + index);
    return axis_ == WaveTimeAxisSource::SampleFrequency && frequency_ > 0 ? global / frequency_ : global;
}

double WaveQueryView::actual(std::size_t index) const
{
    return channel_.samples[index].value * channel_.ratio;
}

WaveSample WaveQueryView::sample(std::size_t index) const
{
    const auto value = actual(index);
    return {time(index),
            formula_ == WaveDisplayFormula::OffsetThenScale ? (value + channel_.offset) * channel_.scale
                                                            : value * channel_.scale + channel_.offset};
}

std::pair<std::size_t, std::size_t> WaveQueryView::range(double minTime, double maxTime, bool guards) const
{
    if (maxTime < minTime)
        std::swap(minTime, maxTime);
    const auto bound = [&](double t, bool upper) {
        std::size_t lo = 0, hi = size();
        while (lo < hi) {
            const auto mid = lo + (hi - lo) / 2;
            if (time(mid) < t || (upper && time(mid) == t))
                lo = mid + 1;
            else
                hi = mid;
        }
        return lo;
    };
    auto begin = bound(minTime, false), end = bound(maxTime, true);
    if (guards && begin > 0)
        --begin;
    if (guards && end < size())
        ++end;
    return {begin, end};
}

WaveSummary WaveQueryView::summary(std::size_t begin, std::size_t end, WaveQueryCounters* counters) const
{
    const std::span<const WaveSample> samples{channel_.samples, size()};
    if (channel_.summaryIndex)
        return channel_.summaryIndex->query(samples, channel_.sampleIndexOffset, begin, end, counters);
    begin = (std::min)(begin, size());
    end = (std::max)(begin, (std::min)(end, size()));
    return scan(samples,
                channel_.sampleIndexOffset,
                begin + channel_.sampleIndexOffset,
                end + channel_.sampleIndexOffset,
                counters);
}

std::vector<std::size_t> WaveQueryView::traceIndices(
    double minTime, double maxTime, std::size_t budget, WaveQueryCounters* counters, bool guards) const
{
    std::vector<std::size_t> result;
    const auto [begin, end] = range(minTime, maxTime, guards);
    if (begin == end || budget == 0)
        return result;
    result.reserve((std::min)(budget, end - begin));
    if (end - begin <= budget) {
        for (auto i = begin; i < end; ++i)
            result.push_back(i);
        if (counters)
            counters->rawSamples += end - begin;
        return result;
    }
    const auto add = [&](std::size_t global) {
        const auto index = global - channel_.sampleIndexOffset;
        if (std::ranges::find(result, index) == result.end()) result.push_back(index);
    };
    const auto addSummary = [&](const WaveSummary& s) {
        if (!s.count) return;
        std::array<std::size_t, 8> indices{s.first, s.last, s.minimum, s.maximum,
            s.rise > 0 ? s.riseBefore : s.first, s.rise > 0 ? s.riseBefore + 1 : s.first,
            s.fall > 0 ? s.fallBefore : s.first, s.fall > 0 ? s.fallBefore + 1 : s.first};
        std::ranges::sort(indices);
        for (const auto global : indices) {
            const auto index = global - channel_.sampleIndexOffset;
            if (result.empty() || result.back() < index) result.push_back(index);
        }
    };
    // 低预算优先端点，再按强度保留完整跳变点对，最后才填入极值。
    if (budget < 18) {
        result.push_back(begin);
        if (budget > 1) result.push_back(end - 1);
        const auto s = summary(begin, end, counters);
        std::array<std::pair<double, std::size_t>, 2> jumps{{{s.rise, s.riseBefore}, {s.fall, s.fallBefore}}};
        std::sort(jumps.begin(), jumps.end(), [](auto a, auto b) {
            return a.first == b.first ? a.second < b.second : a.first > b.first;
        });
        for (const auto [strength, before] : jumps) {
            if (!(strength > 0)) continue;
            const auto first = before - channel_.sampleIndexOffset;
            const auto extra = std::size_t(std::ranges::find(result, first) == result.end()) +
                std::size_t(std::ranges::find(result, first + 1) == result.end());
            if (result.size() + extra <= budget) { add(before); add(before + 1); }
        }
        for (const auto index : {s.minimum, s.maximum})
            if (result.size() < budget) add(index);
        std::ranges::sort(result);
        return result;
    }
    if (maxTime < minTime) std::swap(minTime, maxTime);
    const auto buckets = (budget - 2) / 8;
    // 完整桶以横轴零点为锚，桶宽只在二倍层级上改变，滚动不会重分完整桶。
    const auto span = maxTime - minTime;
    const auto requestedWidth = span / static_cast<double>(buckets - 1);
    double width = std::exp2(std::ceil(std::log2(requestedWidth)));
    if (width < requestedWidth) width *= 2;
    if (!(width > 0) || !std::isfinite(width)) {
        addSummary(summary(begin, end, counters));
        std::ranges::sort(result);
        return result;
    }
    result.push_back(begin);
    const auto firstBucket = std::floor(minTime / width);
    auto left = begin;
    for (std::size_t bucket = 0; bucket < buckets; ++bucket) {
        auto right = end;
        if (bucket + 1 < buckets) {
            const auto t = (firstBucket + static_cast<double>(bucket + 1)) * width;
            right = range(t, t, false).first;
            right = (std::clamp)(right, left, end);
        }
        // 左邻点纳入当前摘要，桶边界上的跳变也必须保留原始点对。
        if (right > left) addSummary(summary(left > begin ? left - 1 : left, right, counters));
        left = right;
        if (left == end) break;
    }
    add(end - 1 + channel_.sampleIndexOffset);
    std::ranges::sort(result);
    return result;
}

std::vector<WaveSample> WaveQueryView::extract(double minTime, double maxTime) const
{
    const auto [begin, end] = range(minTime, maxTime, false);
    std::vector<WaveSample> result;
    result.reserve(end - begin);
    for (auto i = begin; i < end; ++i)
        result.push_back({time(i), actual(i)});
    return result;
}

std::uint64_t WaveQueryView::bitTransitions(double minTime, double maxTime, std::size_t bit,
                                           WaveQueryCounters* counters) const
{
    if (bit >= 64 || size() < 2) return 0;
    auto [begin, end] = range(minTime, maxTime, false);
    if (begin >= end) return 0;
    // 左邻样本只用于判断边界变化；首个留存样本没有已知前态，不计为跳变。
    if (begin > 0) --begin;
    if (channel_.summaryIndex)
        return channel_.summaryIndex->bitTransitions({channel_.samples, size()}, channel_.sampleIndexOffset,
                                                     begin, end, bit, counters);
    std::uint64_t result = 0;
    const auto mask = std::uint64_t{1} << bit;
    for (auto i = begin + 1; i < end; ++i)
        if ((waveRawBits(channel_.samples[i - 1].value) ^ waveRawBits(channel_.samples[i].value)) & mask) ++result;
    if (counters) counters->rawSamples += end - begin;
    return result;
}

std::vector<WaveDigitalSegment> WaveQueryView::digitalSegments(
    double minTime, double maxTime, std::size_t bit, std::size_t budget, std::size_t pixelWidth,
    WaveQueryCounters* counters) const
{
    std::vector<WaveDigitalSegment> result;
    if (bit >= 64 || size() == 0 || budget == 0) return result;
    if (maxTime < minTime) std::swap(minTime, maxTime);
    auto [begin, end] = range(minTime, maxTime, false);
    if (begin > 0) --begin;
    if (begin >= end) return result;
    const auto mask = std::uint64_t{1} << bit;
    const auto state = [&](std::size_t i) { return (waveRawBits(channel_.samples[i].value) & mask) != 0; };
    const auto start = (std::max)(minTime, time(begin));
    const double pixelTime = (maxTime - minTime) / static_cast<double>((std::max)(pixelWidth, std::size_t{1}));
    std::vector<std::size_t> edges;
    // 只枚举预算内的真实边沿，摘要排除恒定区间；绝不为绘制调用整窗次数统计。
    const auto findEdges = [&](auto&& self, std::size_t left, std::size_t right, std::size_t limit) -> bool {
        if (left >= right) return true;
        const auto s = summary(left - 1, right, counters);
        if (((s.bitsAnd ^ s.bitsOr) & mask) == 0) return true;
        if (right - left <= WaveSummaryIndex::blockSize) {
            for (auto i = left; i < right; ++i) {
                if (counters) ++counters->rawSamples;
                if (state(i) != state(i - 1)) {
                    if (edges.size() == limit) return false;
                    edges.push_back(i);
                }
            }
            return true;
        }
        const auto mid = left + (right - left) / 2;
        return self(self, left, mid, limit) && self(self, mid, right, limit);
    };
    const auto append = [&](WaveDigitalSegment segment) {
        if (!result.empty() && !segment.activity && !result.back().activity &&
            segment.firstState == segment.lastState && result.back().firstState == segment.firstState &&
            result.back().lastState == segment.firstState && result.back().endTime == segment.beginTime)
            result.back().endTime = segment.endTime;
        else result.push_back(segment);
    };
    const auto emitExact = [&](double left, double right, bool initial) {
        auto t = left;
        auto current = initial;
        for (std::size_t n = 0; n < edges.size();) {
            auto last = n;
            // 同像素内多个边沿使用活动带，窄脉冲不会变成虚假的单边沿。
            while (last + 1 < edges.size() &&
                   std::floor((time(edges[last + 1]) - minTime) / (std::max)(pixelTime, 1e-300)) ==
                   std::floor((time(edges[n]) - minTime) / (std::max)(pixelTime, 1e-300))) ++last;
            const auto edgeTime = time(edges[n]);
            if (edgeTime > t) append({t, edgeTime, current, current, false});
            const auto next = state(edges[last]);
            append({edgeTime, time(edges[last]), current, next, last != n});
            t = time(edges[last]);
            current = next;
            n = last + 1;
        }
        if (right > t || result.empty()) append({t, right, current, current, false});
    };
    if (findEdges(findEdges, begin + 1, end, (budget - 1) / 2)) {
        emitExact(start, maxTime, state(begin));
        return result;
    }
    // 预算不足时按时间细分。每桶最多三个图元，复杂桶只画活动带。
    const auto buckets = (std::max)(std::size_t{1}, budget / 3);
    auto left = begin;
    for (std::size_t b = 0; b < buckets; ++b) {
        const auto t0 = start + (maxTime - start) * static_cast<double>(b) / static_cast<double>(buckets);
        const auto t1 = start + (maxTime - start) * static_cast<double>(b + 1) / static_cast<double>(buckets);
        const auto right = b + 1 == buckets ? end : (std::clamp)(range(t1, t1, false).first, left + 1, end);
        edges.clear();
        const auto initial = state(left);
        if (budget >= 3 && findEdges(findEdges, left + 1, right, 1))
            emitExact(t0, t1, initial);
        else {
            const auto s = summary(left, right, counters);
            const bool activity = ((s.bitsAnd ^ s.bitsOr) & mask) != 0;
            append({t0, t1, initial, state(right - 1), activity});
        }
        left = right - 1;
    }
    return result;
}

std::vector<WaveTimeEnvelope> WaveQueryView::timeEnvelope(
    double minTime, double maxTime, std::size_t buckets, WaveQueryCounters* counters) const
{
    std::vector<WaveTimeEnvelope> result;
    if (maxTime < minTime) std::swap(minTime, maxTime);
    const auto [begin, end] = range(minTime, maxTime, false);
    if (begin == end || buckets == 0) return result;
    auto left = begin;
    for (std::size_t b = 0; b < buckets; ++b) {
        const auto t0 = minTime + (maxTime - minTime) * static_cast<double>(b) / static_cast<double>(buckets);
        const auto t1 = minTime + (maxTime - minTime) * static_cast<double>(b + 1) / static_cast<double>(buckets);
        const auto right = b + 1 == buckets ? end : (std::clamp)(range(t1, t1, false).first, left, end);
        const auto s = summary(left, right, counters);
        if (s.finiteCount) {
            const auto a = sample(s.minimum - channel_.sampleIndexOffset).value;
            const auto z = sample(s.maximum - channel_.sampleIndexOffset).value;
            if (std::isfinite(a) && std::isfinite(z))
                result.push_back({t0, t1, (std::min)(a, z), (std::max)(a, z), s.count});
        }
        left = right;
    }
    return result;
}

std::vector<WaveDigitalBucket> WaveQueryView::digitalBuckets(double minTime,
                                                             double maxTime,
                                                             std::size_t budget,
                                                             WaveQueryCounters* counters) const
{
    std::vector<WaveDigitalBucket> result;
    const auto [begin, end] = range(minTime, maxTime);
    if (begin == end || budget == 0)
        return result;
    const auto count = (std::min)(budget, end - begin);
    result.reserve(count);
    auto left = begin;
    for (std::size_t b = 0; b < count; ++b) {
        const auto t = minTime + (maxTime - minTime) * static_cast<double>(b + 1) / static_cast<double>(count);
        const auto right = b + 1 == count ? end : (std::clamp)(range(t, t, false).first, left, end);
        if (left == right)
            continue;
        const auto s = summary(left, right, counters);
        result.push_back({time(left), time(right - 1), s.firstBits, s.lastBits, s.bitsAnd ^ s.bitsOr});
        left = right;
    }
    return result;
}

std::optional<std::size_t> WaveQueryView::bitEdge(
    double minTime, double maxTime, std::size_t bit, bool state, bool reverse) const
{
    if (bit >= 64 || size() < 2)
        return std::nullopt;
    auto [begin, end] = range(minTime, maxTime, false);
    begin = (std::max)(std::size_t{1}, begin);
    const auto mask = std::uint64_t{1} << bit;
    // 恒定块由 AND/OR 一次排除，只在含目标跳变的叶块读取原始样本。
    const auto search = [&](auto&& self, std::size_t left, std::size_t right) -> std::optional<std::size_t> {
        if (left >= right)
            return std::nullopt;
        const auto s = summary(left - 1, right);
        if (((s.bitsAnd ^ s.bitsOr) & mask) == 0)
            return std::nullopt;
        if (right - left > WaveSummaryIndex::blockSize) {
            const auto mid = left + (right - left) / 2;
            if (auto hit = reverse ? self(self, mid, right) : self(self, left, mid))
                return hit;
            return reverse ? self(self, left, mid) : self(self, mid, right);
        }
        for (auto n = left; n < right; ++n) {
            const auto i = reverse ? right - 1 - (n - left) : n;
            const bool previous = (waveRawBits(channel_.samples[i - 1].value) & mask) != 0;
            const bool current = (waveRawBits(channel_.samples[i].value) & mask) != 0;
            if (current != previous && current == state)
                return i;
        }
        return std::nullopt;
    };
    return search(search, begin, end);
}

std::optional<double> WaveQueryView::firstCrossing(double minTime, double maxTime, double threshold, bool rising) const
{
    if (!std::isfinite(threshold) || size() < 2)
        return std::nullopt;
    auto [begin, end] = range(minTime, maxTime);
    begin = (std::max)(begin, std::size_t{1});
    const auto search = [&](auto&& self, std::size_t left, std::size_t right) -> std::optional<double> {
        if (left >= right)
            return std::nullopt;
        const auto s = summary(left - 1, right);
        const auto a = sample(s.minimum - channel_.sampleIndexOffset).value;
        const auto b = sample(s.maximum - channel_.sampleIndexOffset).value;
        if ((std::min)(a, b) > threshold || (std::max)(a, b) < threshold || a == b)
            return std::nullopt;
        if (right - left > WaveSummaryIndex::blockSize) {
            const auto mid = left + (right - left) / 2;
            if (auto hit = self(self, left, mid))
                return hit;
            return self(self, mid, right);
        }
        for (auto i = left; i < right; ++i) {
            const auto previous = sample(i - 1), current = sample(i);
            const bool crosses = rising ? previous.value < threshold && current.value >= threshold
                                        : previous.value > threshold && current.value <= threshold;
            if (!crosses || current.value == previous.value)
                continue;
            const auto t = previous.time + (threshold - previous.value) * (current.time - previous.time) /
                                               (current.value - previous.value);
            if (t >= minTime && t <= maxTime)
                return t;
        }
        return std::nullopt;
    };
    return search(search, begin, end);
}
} // namespace protoscope::plot
