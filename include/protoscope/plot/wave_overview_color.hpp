#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace protoscope::plot {

struct OverviewColor {
    double r{0}, g{0}, b{0}, a{1};
    bool operator==(const OverviewColor&) const = default;
};

// Paul Tol Bright 与 Okabe-Ito；保留重复色相身份，候选顺序保证平分时稳定。
inline constexpr std::array<std::uint32_t, 15> kOverviewPalette{
    0x4477AA, 0xEE6677, 0x228833, 0xCCBB44, 0x66CCEE, 0xAA3377, 0xBBBBBB,
    0xE69F00, 0x56B4E9, 0x009E73, 0xF0E442, 0x0072B2, 0xD55E00, 0xCC79A7, 0x000000};

struct OverviewSelectionConfig {
    bool automatic{true};
    std::optional<std::array<float, 3>> fixedColor;
    std::optional<float> minAlpha;
    std::optional<float> maxAlpha;
    bool operator==(const OverviewSelectionConfig&) const = default;
};

struct OverviewSelectionStyle {
    bool automatic{true};
    OverviewColor fixedColor{.267, .467, .667, 1};
    double minAlpha{.10}, maxAlpha{.28};
    bool operator==(const OverviewSelectionStyle&) const = default;
};

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

inline std::pair<double, double> overviewColorScore(OverviewColor color,
                                                    std::span<const OverviewColor> samples, OverviewColor background)
{
    if (samples.empty()) return {overviewContrast(color, background), 0};
    std::vector<double> contrasts;
    contrasts.reserve(samples.size());
    const auto lab = overviewLab(color);
    double nearest = std::numeric_limits<double>::infinity();
    for (const auto sample : samples) {
        const auto actual = compositeOverviewColor(sample, background);
        contrasts.push_back(overviewContrast(color, actual));
        const auto other = overviewLab(actual);
        nearest = (std::min)(nearest, std::hypot(lab[0] - other[0], lab[1] - other[1], lab[2] - other[2]));
    }
    const auto index = contrasts.size() / 5;
    std::nth_element(contrasts.begin(), contrasts.begin() + index, contrasts.end());
    return {contrasts[index], nearest};
}

inline std::uint32_t selectOverviewColor(std::span<const OverviewColor> samples, OverviewColor background)
{
    std::uint32_t selected = kOverviewPalette.front();
    std::pair<double, double> bestScore{-1, -1};
    double bestContrast = -1;
    bool hasContrast = false;
    for (const auto rgb : kOverviewPalette) {
        const auto color = overviewRgb(rgb);
        const auto contrast = overviewContrast(color, background);
        const bool admissible = contrast >= 3;
        if (hasContrast && !admissible) continue;
        const auto score = overviewColorScore(color, samples, background);
        // 正常主题优先满足 3:1；若自定义背景无任何合格候选，退化为最大背景对比度。
        if ((admissible && !hasContrast) || (admissible && score > bestScore) ||
            (!hasContrast && !admissible && contrast > bestContrast)) {
            selected = rgb;
            bestScore = score;
            bestContrast = contrast;
            hasContrast = admissible;
        }
    }
    return selected;
}

inline double overviewFillAlpha(OverviewColor color, std::span<const OverviewColor> samples,
                                 OverviewColor background, double minimum, double maximum)
{
    for (int step = 0; step <= 24; ++step) {
        color.a = std::lerp(minimum, maximum, step / 24.0);
        std::size_t changed = 0;
        for (const auto& sample : samples)
            changed += overviewContrast(compositeOverviewColor(color, sample), sample) >= 1.15;
        if ((samples.empty() && overviewContrast(compositeOverviewColor(color, background), background) >= 1.15) ||
            (!samples.empty() && changed * 5 >= samples.size() * 3)) return color.a;
    }
    return maximum;
}

// 仅光栅化已有绘制几何，固定 2304 个颜色采样，不访问采集数据或 GPU。
struct OverviewColorRaster {
    static constexpr int width = 96, height = 24;
    std::array<OverviewColor, width * height> pixels;
    explicit OverviewColorRaster(OverviewColor background) { pixels.fill(background); }
    void blend(int x, int y, OverviewColor color)
    {
        if (x >= 0 && x < width && y >= 0 && y < height)
            pixels[y * width + x] = compositeOverviewColor(color, pixels[y * width + x]);
    }
    void line(double x0, double y0, double x1, double y1, OverviewColor color)
    {
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return;
        x0 = std::clamp(x0, 0., 1.) * (width - 1); x1 = std::clamp(x1, 0., 1.) * (width - 1);
        y0 = std::clamp(y0, 0., 1.) * (height - 1); y1 = std::clamp(y1, 0., 1.) * (height - 1);
        const auto steps = (std::max)(1, static_cast<int>(std::ceil((std::max)(std::abs(x1-x0), std::abs(y1-y0)))));
        for (int i = 0; i <= steps; ++i)
            blend(static_cast<int>(std::lround(std::lerp(x0, x1, double(i)/steps))),
                  static_cast<int>(std::lround(std::lerp(y0, y1, double(i)/steps))), color);
    }
    void rectangle(double x0, double y0, double x1, double y1, OverviewColor color)
    {
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) return;
        const int left = static_cast<int>(std::clamp((std::min)(x0,x1), 0.,1.) * (width-1));
        const int right = static_cast<int>(std::clamp((std::max)(x0,x1), 0.,1.) * (width-1));
        const int top = static_cast<int>(std::clamp((std::min)(y0,y1), 0.,1.) * (height-1));
        const int bottom = static_cast<int>(std::clamp((std::max)(y0,y1), 0.,1.) * (height-1));
        for (int y = top; y <= bottom; ++y) for (int x = left; x <= right; ++x) blend(x,y,color);
    }
};

struct OverviewColorCache {
    std::vector<OverviewColor> channels;
    OverviewColor background;
    std::uint32_t selected{0};
    std::size_t updates{0};
    OverviewSelectionStyle style;
    std::uint64_t revision{0}, visibility{0};
    double evaluatedAt{-1}, candidateSince{-1}, alpha{.1}, targetAlpha{.1};
    std::uint32_t candidate{0};
    bool dragging{false};

    bool needsEvaluation(double now, bool invalidated) const
    {
        return invalidated || updates == 0 || (!dragging && now - evaluatedAt >= .2);
    }

    OverviewColor resolveSamples(std::span<const OverviewColor> colors, OverviewColor backdrop,
        std::span<const OverviewColor> samples, const OverviewSelectionStyle& requested,
        std::uint64_t themeRevision, std::uint64_t channelVisibility, double now, double delta)
    {
        const bool invalidated = updates == 0 || revision != themeRevision || visibility != channelVisibility ||
            background != backdrop || style != requested ||
            !std::equal(channels.begin(), channels.end(), colors.begin(), colors.end());
        if (needsEvaluation(now, invalidated)) {
            const auto proposed = selectOverviewColor(samples, backdrop);
            if (invalidated && (!dragging || updates == 0)) {
                selected = proposed;
                candidateSince = -1;
            } else if (!dragging && proposed != selected &&
                       overviewColorScore(overviewRgb(proposed), samples, backdrop).first >=
                       overviewColorScore(overviewRgb(selected), samples, backdrop).first * 1.15) {
                if (candidate != proposed || candidateSince < 0) { candidate = proposed; candidateSince = now; }
                else if (now - candidateSince >= .4) { selected = proposed; candidateSince = -1; }
            } else candidateSince = -1;
            const auto fill = requested.automatic ? overviewRgb(selected) : requested.fixedColor;
            targetAlpha = overviewFillAlpha(fill, samples, backdrop, requested.minAlpha, requested.maxAlpha);
            channels.assign(colors.begin(), colors.end());
            background = backdrop;
            style = requested;
            revision = themeRevision;
            visibility = channelVisibility;
            evaluatedAt = now;
            ++updates;
        }
        alpha = std::clamp(std::lerp(alpha, targetAlpha, 1. - std::exp(-12. * (std::max)(0., delta))),
                           requested.minAlpha, requested.maxAlpha);
        auto result = requested.automatic ? overviewRgb(selected) : requested.fixedColor;
        result.a = alpha;
        return result;
    }

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
