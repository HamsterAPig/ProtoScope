#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace protoscope::scripting {

struct BusinessMenuItem {
    std::string id;
    std::string label;
    std::string tooltip;
    bool separator{false};
    bool visible{true};
    bool disabled{false};
    bool checkable{false};
    bool checked{false};
    std::vector<BusinessMenuItem> children;
};

struct DockVisibilityRequest {
    std::string id;
    bool visible{true};
    std::uint64_t revision{0};
};

struct BusinessUiSnapshot {
    std::uint64_t runtimeGeneration{0};
    std::uint64_t menuRevision{0};
    std::uint64_t dockRevision{0};
    std::vector<BusinessMenuItem> menu;
    std::vector<DockVisibilityRequest> dockRequests;
};

} // namespace protoscope::scripting
