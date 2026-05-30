#pragma once

#include "protoscope/scripting/file_io_config.hpp"
#include "protoscope/scripting/script_host.hpp"
#include "protoscope/transport/transport.hpp"

#include <optional>

namespace protoscope::scripting {

struct ScriptHostQueues {
    // 核心流程说明：队列所有权仍在 ScriptHost，内部服务仅通过宿主门面调度。
};

struct ScriptHostContextInternal {
    ScriptHost& host;
    ScriptHostQueues& queues;
    FileIoConfig& fileIoConfig;
    std::optional<transport::ConnectionContext>& activeConnection;
};

} // namespace protoscope::scripting
