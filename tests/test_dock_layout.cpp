#include "test_registry.hpp"

#include "protoscope/ui/dock_layout.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

protoscope::scripting::DockSnapshot makeDock(const char* id, const char* title, const char* anchor, const char* tabGroup) {
    protoscope::scripting::DockDescriptor descriptor;
    descriptor.id = id;
    descriptor.title = title;
    descriptor.anchor = anchor;
    descriptor.tabGroup = tabGroup;
    return protoscope::scripting::DockSnapshot{.descriptor = descriptor, .controls = {}};
}

} // namespace

void test_lua_dock_layout_key_prefers_protocol_dir() {
    const auto key = protoscope::ui::luaDockLayoutKey("protocols\\default_protocol\\", "scripts/main.lua");
    require(key == "protocols_default_protocol", "布局 key 应优先来自规范化协议目录");
    require(protoscope::ui::luaDockLayoutKey("", "") == "default", "空路径应回退到 default");
}

void test_lua_dock_window_name_keeps_stable_id() {
    protoscope::scripting::DockDescriptor dock;
    dock.id = "protocol";
    dock.title = "协议动作";

    const auto name = protoscope::ui::luaDockWindowName(dock, "protocols_default_protocol");
    require(name == "协议动作###LuaDock:protocols_default_protocol:protocol", "Lua Dock 窗口名应包含稳定 ID");

    dock.title = "新标题";
    const auto renamed = protoscope::ui::luaDockWindowName(dock, "protocols_default_protocol");
    require(renamed == "新标题###LuaDock:protocols_default_protocol:protocol", "显示标题变化不应改变稳定 ID 后缀");
}

void test_lua_dock_layout_requests_group_tabs() {
    std::vector<protoscope::scripting::DockSnapshot> docks{
        makeDock("protocol", "协议动作", "left_bottom", "protocol_tools"),
        makeDock("advanced", "高级参数", "right_mid", "protocol_tools"),
        makeDock("single", "独立面板", "right_bottom", ""),
    };

    const auto requests = protoscope::ui::buildLuaDockLayoutRequests(docks, "demo");
    require(requests.size() == 3, "应为每个 Lua Dock 生成默认布局请求");
    require(requests[0].anchor == "left_bottom", "同组第一个 Dock 应保留声明停靠点");
    require(requests[1].anchor == "left_bottom", "同 tab_group 后续 Dock 应复用首个停靠点形成 Tab");
    require(requests[0].tabGroup == "protocol_tools", "显式 tab_group 应作为聚合键");
    require(requests[2].tabGroup == "single", "缺省 tab_group 应按自身 id 独立停靠");
}
