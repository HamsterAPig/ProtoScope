#include "test_registry.hpp"

#include "protoscope/ui/dock_layout.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
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

std::filesystem::path uniqueLayoutRoot(const char* name) {
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(ticks));
}

} // namespace

void test_lua_dock_layout_key_uses_protocol_and_script() {
    const auto key = protoscope::ui::luaDockLayoutKey("protocols\\default_protocol\\", "scripts/main.lua");
    require(key == "protocols_default_protocol", "布局 key 应按协议目录生成");
    require(
        protoscope::ui::luaDockLayoutKey("protocols/default_protocol", "scripts\\main.lua") == key,
        "路径分隔符差异不应改变布局 key");
    require(
        protoscope::ui::luaDockLayoutKey("protocols/default_protocol", "scripts/advanced.lua") == key,
        "同协议目录下不同入口脚本应共享布局 key");
    require(
        protoscope::ui::legacyLuaDockLayoutKey("protocols\\default_protocol\\", "scripts/main.lua")
            == "protocols_default_protocol__scripts_main.lua",
        "旧布局 key 应支持从脚本级布局迁移");
    require(protoscope::ui::luaDockLayoutKey("", "") == "default", "空路径应回退到 default");
}

void test_lua_dock_layout_key_falls_back_to_script_directory() {
    const auto mainKey = protoscope::ui::luaDockLayoutKey("", "protocols/demo/main.lua");
    const auto advancedKey = protoscope::ui::luaDockLayoutKey("", "protocols/demo/advanced.lua");
    require(mainKey == "protocols_demo", "未提供协议目录时应使用脚本所在目录作为协议 key");
    require(mainKey == advancedKey, "同一脚本目录下多个 Lua 文件应共享布局 key");
}

void test_lua_dock_layout_paths_prefer_user_layout() {
    const auto root = uniqueLayoutRoot("protoscope-layout-paths");
    const auto protocolDir = std::filesystem::path("protocols") / "demo";
    const auto scriptPath = protocolDir / "main.lua";
    const auto layoutPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo");
    std::filesystem::create_directories(layoutPath.parent_path());
    std::ofstream(layoutPath) << "[Window][demo]\n";

    const auto paths = protoscope::ui::resolveLuaDockLayoutPaths(root, protocolDir.generic_string(), scriptPath.generic_string());
    require(paths.protocolKey == "protocols_demo", "协议布局 key 应按目录生成");
    require(paths.layoutPath == layoutPath, "新布局路径应位于 exe/config/ui 下");
    require(paths.hasUserLayout, "已有用户布局文件时应优先识别为用户持久化布局");
}

void test_lua_dock_layout_paths_detect_legacy_source() {
    const auto root = uniqueLayoutRoot("protoscope-layout-legacy");
    const auto protocolDir = std::filesystem::path("protocols") / "demo";
    const auto scriptPath = protocolDir / "main.lua";
    const auto legacyPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo__protocols_demo_main.lua");
    std::filesystem::create_directories(legacyPath.parent_path());
    std::ofstream(legacyPath) << "[Window][legacy]\n";

    const auto paths = protoscope::ui::resolveLuaDockLayoutPaths(root, protocolDir.generic_string(), scriptPath.generic_string());
    require(!paths.hasUserLayout, "新布局不存在时不应误判为用户布局");
    require(paths.hasLegacyLayout, "旧脚本级布局文件应作为迁移来源");
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

void test_lua_dock_settings_filter_keeps_only_current_windows() {
    std::vector<protoscope::scripting::DockSnapshot> docks{
        makeDock("active", "活动面板", "left_bottom", ""),
    };
    const auto activeIds = protoscope::ui::buildLuaDockStableIds(docks, "protocols_demo");

    require(
        protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:protocols_demo:active", "protocols_demo", activeIds),
        "当前协议真实存在的 Lua Dock 状态应保留");
    require(
        !protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:protocols_demo:removed", "protocols_demo", activeIds),
        "当前协议已删除的 Lua Dock 状态应清理");
    require(
        !protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:other:active", "protocols_demo", activeIds),
        "其它协议的 Lua Dock 状态应清理");
    require(
        protoscope::ui::shouldKeepLuaWindowSettings("通讯配置", "protocols_demo", activeIds),
        "静态窗口状态不应被 Lua Dock 过滤影响");
}
