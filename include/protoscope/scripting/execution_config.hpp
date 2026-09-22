#pragma once

#include <cstdint>

namespace protoscope::scripting {

struct ExecutionConfig {
    std::uint64_t loadTimeoutMs{5000};
    std::uint64_t callbackTimeoutMs{500};
};

} // namespace protoscope::scripting
