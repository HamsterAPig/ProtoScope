#include "protoscope/scripting/pipeline_threading.hpp"

#include <algorithm>

namespace protoscope::scripting {

std::size_t resolvePipelineWorkerThreads(const std::optional<std::size_t> configuredThreads,
                                         const unsigned int hardwareConcurrency)
{
    const auto machineLimit = hardwareConcurrency > 1U ? static_cast<std::size_t>(hardwareConcurrency - 1U) : 1U;
    if (!configuredThreads.has_value()) {
        return machineLimit;
    }
    // 核心流程：显式配置只收窄到当前机器可安全使用的后处理线程数，不额外占用 UI/parser/Lua worker。
    return std::max<std::size_t>(1U, std::min<std::size_t>(*configuredThreads, machineLimit));
}

} // namespace protoscope::scripting
