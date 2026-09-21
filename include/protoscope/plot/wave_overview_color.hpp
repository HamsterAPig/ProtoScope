#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace protoscope::plot {

struct OverviewColor {
    double r{0}, g{0}, b{0}, a{1};
    bool operator==(const OverviewColor&) const = default;
};

inline constexpr std::array<std::uint32_t, 7> kOverviewPalette{
    0x4477AA, 0xEE6677, 0x228833, 0xCCBB44, 0x66CCEE, 0xAA3377, 0xBBBBBB};

inline OverviewColor overviewRgb(std::uint32_t rgb)
{
    return {double((rgb >> 16) & 255) / 255, double((rgb >> 8) & 255) / 255, double(rgb & 255) / 255, 1};
}

inline OverviewColor compositeOverviewColor(OverviewColor foreground, OverviewColor background)
{
    const auto alpha = std::clamp(foreground.a, 0.0, 1.0);
    return {std::lerp(background.r, foreground.r, alpha),
            std::lerp(background.g, foreground.g, alpha),
            std::lerp(background.b, foreground.b, alpha), 1};
}

inline double overviewLinear(double value)
{
    value = std::clamp(value, 0.0, 1.0);
    return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
}

inline double overviewLuminance(OverviewColor color)
{
    return 0.2126 * overviewLinear(color.r) + 0.7152 * overviewLinear(color.g) + 0.0722 * overviewLinear(color.b);
}

inline double overviewContrast(OverviewColor a, OverviewColor b)
{
    const auto x = overviewLuminance(a), y = overviewLuminance(b);
    return ((std::max)(x, y) + 0.05) / ((std::min)(x, y) + 0.05);
}

inline std::array<double, 3> overviewLab(OverviewColor color)
{
    const auto r = overviewLinear(color.r), g = overviewLinear(color.g), b = overviewLinear(color.b);
    const auto f = [](double value) {
        constexpr double delta = 6.0 / 29.0;
        return value > delta * delta * delta ? std::cbrt(value) : value / (3 * delta * delta) + 4.0 / 29.0;
    };
    // sRGB -> D65 XYZ -> CIELAB；按实际透明合成后的颜色比较感知色差。
    const auto x = f((0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047);
    const auto y = f(0.2126729 * r + 0.7151522 * g + 0.0721750 * b);
    const auto z = f((0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883);
    return {116 * y - 16, 500 * (x - y), 200 * (y - z)};
}

inline std::uint32_t selectOverviewColor(std::span<const OverviewColor> channels, OverviewColor background)
{
    std::vector<std::array<double, 3>> labs;
    for (const auto color : channels) labs.push_back(overviewLab(compositeOverviewColor(color, background)));
    std::uint32_t selected = kOverviewPalette.front();
    double bestDistance = -1, bestContrast = -1;
    bool hasContrast = false;
    for (const auto rgb : kOverviewPalette) {
        const auto color = overviewRgb(rgb);
        const auto contrast = overviewContrast(color, background);
        const bool admissible = contrast >= 3;
        if (hasContrast && !admissible) continue;
        const auto lab = overviewLab(color);
        double nearest = std::numeric_limits<double>::infinity();
        for (const auto& other : labs) {
            double distance = 0;
            for (std::size_t i = 0; i < lab.size(); ++i) distance += (lab[i] - other[i]) * (lab[i] - other[i]);
            nearest = (std::min)(nearest, distance);
        }
        // 正常主题优先满足 3:1；若自定义背景无任何合格候选，退化为最大背景对比度。
        if ((admissible && !hasContrast) || (admissible && nearest > bestDistance) ||
            (!hasContrast && !admissible && contrast > bestContrast)) {
            selected = rgb;
            bestDistance = nearest;
            bestContrast = contrast;
            hasContrast = admissible;
        }
    }
    return selected;
}

struct OverviewColorCache {
    std::vector<OverviewColor> channels;
    OverviewColor background;
    std::uint32_t selected{0};
    std::size_t updates{0};

    std::uint32_t resolve(std::span<const OverviewColor> colors, OverviewColor backdrop)
    {
        if (updates == 0 || background != backdrop ||
            !std::equal(channels.begin(), channels.end(), colors.begin(), colors.end())) {
            channels.assign(colors.begin(), colors.end());
            background = backdrop;
            selected = selectOverviewColor(channels, background);
            ++updates;
        }
        return selected;
    }
};

} // namespace protoscope::plot
