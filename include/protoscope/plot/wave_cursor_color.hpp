#pragma once

#include "protoscope/plot/wave_overview_color.hpp"

#include <tuple>
#include <utility>

namespace protoscope::plot {

struct CursorColorChoice {
    OverviewColor color;
    double graphicContrast{0}, textContrast{0};
    bool graphicsPass{false}, textPass{false};
};

// 输入均为实际合成色；先满足背景，再最大化最差波形对比及最小 Lab 色差。
// 候选顺序固定，完全平局保持第一项；无解明确返回未达标状态。
inline CursorColorChoice selectCursorColor(std::span<const OverviewColor> waves,
    std::span<const OverviewColor> backgrounds, std::span<const OverviewColor> textBackgrounds,
    std::span<const OverviewColor> used = {})
{
    CursorColorChoice result;
    std::tuple<bool, bool, bool, double, double, double> best{false, false, false, -1, -1, -1};
    bool first = true;
    for (const auto rgb : kOverviewPalette) {
        const auto color = overviewRgb(rgb);
        double graphic = 21, text = 21, waveContrast = 21, distance = 1e9;
        for (const auto bg : backgrounds) graphic = (std::min)(graphic, overviewContrast(color, bg));
        for (const auto bg : textBackgrounds) text = (std::min)(text, overviewContrast(color, bg));
        const auto lab = overviewLab(color);
        for (const auto sample : waves) {
            waveContrast = (std::min)(waveContrast, overviewContrast(color, sample));
            const auto other = overviewLab(sample);
            distance = (std::min)(distance, std::hypot(lab[0] - other[0], lab[1] - other[1], lab[2] - other[2]));
        }
        bool unused = true;
        for (const auto other : used) {
            unused = unused && color != other;
            const auto otherLab = overviewLab(other);
            distance = (std::min)(distance, std::hypot(lab[0] - otherLab[0], lab[1] - otherLab[1], lab[2] - otherLab[2]));
        }
        const bool graphicsPass = graphic >= 3, textPass = text >= 4.5;
        const auto score = std::tuple{graphicsPass, textPass, unused,
            graphicsPass ? waveContrast : graphic, distance, text};
        if (first || score > best) {
            first = false;
            best = score;
            result = {color, graphic, text, graphicsPass, textPass};
        }
    }
    return result;
}

struct CursorColorKey {
    std::uint64_t themeRevision{0};
    bool automatic{true};
    int mode{0};
    std::vector<std::size_t> channels;
    std::vector<OverviewColor> colors, backgrounds, textBackgrounds, manualPalette;
    bool operator==(const CursorColorKey&) const = default;
};

struct CursorColorCache {
    struct Entry { std::uint64_t identity; CursorColorChoice choice; };
    CursorColorKey key;
    std::vector<Entry> entries;
    std::size_t updates{0};
    int frame{-1};
    bool initialized{false};
    OverviewColor labelBackground{}, labelText{1, 1, 1, 1};

    // 同帧再次准备不会发布另一色板。背景/主题/开关变化不被普通拖动锁屏蔽。
    void prepare(CursorColorKey requested, int frameNumber, bool dragging)
    {
        if (initialized && frame == frameNumber) return;
        frame = frameNumber;
        const bool urgent = !initialized || key.themeRevision != requested.themeRevision ||
            key.automatic != requested.automatic || key.backgrounds != requested.backgrounds ||
            key.textBackgrounds != requested.textBackgrounds || key.manualPalette != requested.manualPalette;
        if (urgent || (!dragging && key != requested)) {
            key = std::move(requested);
            entries.clear();
            initialized = true;
            ++updates;
        }
    }

    const CursorColorChoice& resolve(std::uint64_t identity, std::size_t manualIndex)
    {
        const auto found = std::find_if(entries.begin(), entries.end(),
            [identity](const auto& entry) { return entry.identity == identity; });
        if (found != entries.end()) return found->choice;
        CursorColorChoice choice;
        if (!key.automatic && !key.manualPalette.empty()) {
            choice.color = key.manualPalette[manualIndex % key.manualPalette.size()];
            choice.graphicsPass = choice.textPass = true;
        } else {
            std::vector<OverviewColor> used;
            for (const auto& entry : entries) used.push_back(entry.choice.color);
            choice = selectCursorColor(key.colors, key.backgrounds, key.textBackgrounds, used);
        }
        entries.push_back({identity, choice});
        return entries.back().choice;
    }
};

} // namespace protoscope::plot
