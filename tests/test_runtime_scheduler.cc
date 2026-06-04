#include "protoscope/ui/render_frame_scheduler.hpp"

#include "test_registry.hpp"

#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

void test_runtime_scheduler_limits_busy_render_frames()
{
    constexpr std::uint32_t fpsLimit = 60;
    constexpr std::uint64_t lastRenderAtMs = 1000;

    require(protoscope::ui::renderFrameIntervalMs(fpsLimit) == 16, "60fps 最小帧间隔应为 16ms");
    require(!protoscope::ui::shouldRenderFrameNow(1015, lastRenderAtMs, fpsLimit),
            "changed=true 的忙碌场景也不应绕过 fps_limit");
    require(protoscope::ui::shouldRenderFrameNow(1016, lastRenderAtMs, fpsLimit), "达到 fps_limit 间隔后应允许渲染");
    require(protoscope::ui::shouldRenderFrameNow(1000, 0, fpsLimit), "首帧应立即渲染");
}
