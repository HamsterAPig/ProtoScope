#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <random>
#include <span>
#include <vector>

namespace protoscope::plot {

// Paul Tol Bright：A/B 保留黄、青色，辅助游标只在剩余五色中分配。
inline constexpr std::array<std::uint32_t, 2> kMeasurementCursorRgb{0xCCBB44, 0x66CCEE};
inline constexpr std::array<std::uint32_t, 5> kAuxiliaryCursorRgb{
    0x4477AA, 0xEE6677, 0x228833, 0xAA3377, 0xBBBBBB};

struct WaveAuxiliaryCursor {
    std::uint64_t id{0};
    double time{0};
    std::size_t colorIndex{0};
};

struct WaveAuxiliaryInterval {
    WaveAuxiliaryCursor left;
    WaveAuxiliaryCursor right;
    double delta{0};
};

struct WaveAuxiliaryCursors {
    std::vector<WaveAuxiliaryCursor> items;
    std::vector<std::uint64_t> contextHits;
    std::uint64_t nextId{1};
    std::minstd_rand random{std::random_device{}()};

    double availableTime(double minTime, double maxTime, std::span<const double> occupied) const
    {
        if (maxTime < minTime) std::swap(minTime, maxTime);
        const auto halfSpan = maxTime * 0.5 - minTime * 0.5;
        if (!(halfSpan > 0)) return std::midpoint(minTime, maxTime);
        std::vector<double> positions;
        const auto collect = [&](double time) {
            if (std::isfinite(time) && time >= minTime && time <= maxTime)
                positions.push_back((time * 0.5 - minTime * 0.5) / halfSpan);
        };
        for (const auto& cursor : items) collect(cursor.time);
        for (const auto time : occupied) collect(time);
        const auto free = [&](double position) {
            return std::none_of(positions.begin(), positions.end(), [&](double other) {
                return std::abs(position - other) < 0.05 - 1e-12;
            });
        };
        // 在归一化窗口内有限步搜索，优先中心及右侧，再向左寻找，避免退化范围死循环。
        for (int step = 0; step <= 10; ++step) {
            const auto position = 0.5 + step * 0.05;
            if (free(position)) return std::lerp(minTime, maxTime, position);
        }
        for (int step = 1; step <= 10; ++step) {
            const auto position = 0.5 - step * 0.05;
            if (free(position)) return std::lerp(minTime, maxTime, position);
        }
        positions.push_back(0);
        positions.push_back(1);
        std::ranges::sort(positions);
        double largest = -1, position = 0.5;
        for (std::size_t i = 1; i < positions.size(); ++i) {
            const auto gap = positions[i] - positions[i - 1];
            if (gap > largest) {
                largest = gap;
                position = std::midpoint(positions[i - 1], positions[i]);
            }
        }
        return std::lerp(minTime, maxTime, position);
    }

    std::uint64_t add(double minTime, double maxTime, std::span<const double> occupied = {})
    {
        if (!std::isfinite(minTime) || !std::isfinite(maxTime)) return 0;
        std::array<std::size_t, kAuxiliaryCursorRgb.size()> usage{};
        for (const auto& cursor : items) ++usage[cursor.colorIndex];
        const auto least = *std::min_element(usage.begin(), usage.end());
        std::vector<std::size_t> candidates;
        for (std::size_t i = 0; i < usage.size(); ++i)
            if (usage[i] == least) candidates.push_back(i);
        // 优先未占用色；耗尽后在最少使用的颜色中随机选择，删除不会重排已有身份。
        const auto color = candidates[std::uniform_int_distribution<std::size_t>(0, candidates.size() - 1)(random)];
        const auto id = nextId++;
        items.push_back({id, availableTime(minTime, maxTime, occupied), color});
        return id;
    }

    bool remove(std::uint64_t id)
    {
        return std::erase_if(items, [id](const auto& cursor) { return cursor.id == id; }) != 0;
    }

    void clear()
    {
        items.clear();
        contextHits.clear();
        nextId = 1;
    }

    std::vector<WaveAuxiliaryInterval> intervals() const
    {
        auto ordered = items;
        std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) {
            return a.time == b.time ? a.id < b.id : a.time < b.time;
        });
        std::vector<WaveAuxiliaryInterval> result;
        for (std::size_t i = 1; i < ordered.size(); ++i)
            result.push_back({ordered[i - 1], ordered[i], ordered[i].time - ordered[i - 1].time});
        return result;
    }
};

} // namespace protoscope::plot
