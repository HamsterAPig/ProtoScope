#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace protoscope::scripting {

struct FileDialogConfig {
    bool enabled{true};
    bool rememberLastDir{true};
};

struct SendFileConfig {
    std::size_t defaultChunkBytes{65536};
    std::size_t maxInflightChunks{2};
};

struct FileIoConfig {
    bool enabled{true};
    bool allowProtocolDir{true};
    bool allowDialogPaths{true};
    std::vector<std::string> extraAllowedRoots;
    std::size_t maxOpenFiles{8};
    std::size_t defaultChunkBytes{65536};
    std::size_t maxChunkBytes{1048576};
    std::uint64_t maxFileSizeBytes{1073741824};
    std::uint64_t maxWriteFileSizeBytes{1073741824};
    FileDialogConfig dialog{};
    SendFileConfig sendFile{};
};

} // namespace protoscope::scripting
