#pragma once

#include <cstddef>
#include <optional>

namespace protoscope::scripting {

[[nodiscard]] std::size_t resolvePipelineWorkerThreads(std::optional<std::size_t> configuredThreads,
                                                       unsigned int hardwareConcurrency);

} // namespace protoscope::scripting
