#pragma once

#include <algorithm>
#include <cstdint>

namespace protoscope::ui {

inline std::uint64_t renderFrameIntervalMs(std::uint32_t fpsLimit) {
    const auto safeFpsLimit = (std::max)(std::uint32_t{1}, fpsLimit);
    return (std::max)(std::uint64_t{1}, 1000ULL / safeFpsLimit);
}

inline std::uint64_t nextRenderFrameAtMs(std::uint64_t lastRenderAtMs, std::uint32_t fpsLimit) {
    if (lastRenderAtMs == 0) {
        return 0;
    }
    return lastRenderAtMs + renderFrameIntervalMs(fpsLimit);
}

inline bool shouldRenderFrameNow(std::uint64_t nowMs, std::uint64_t lastRenderAtMs, std::uint32_t fpsLimit) {
    // 核心流程：是否渲染只由上一帧时间和 fps_limit 决定，避免 changed=true 让 Debug 高频接收无限重绘。
    const auto nextRenderAtMs = nextRenderFrameAtMs(lastRenderAtMs, fpsLimit);
    return nextRenderAtMs == 0 || nowMs >= nextRenderAtMs;
}

} // namespace protoscope::ui
