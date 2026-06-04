#include "protoscope/ui/dock_layout.hpp"

#include "test_registry.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

protoscope::scripting::DockSnapshot makeDock(const char* id,
                                             const char* title,
                                             const char* anchor,
                                             const char* tabGroup)
{
    protoscope::scripting::DockDescriptor descriptor;
    descriptor.id = id;
    descriptor.title = title;
    descriptor.anchor = anchor;
    descriptor.tabGroup = tabGroup;
    return protoscope::scripting::DockSnapshot{.descriptor = descriptor, .controls = {}};
}

std::filesystem::path uniqueLayoutRoot(const char* name)
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() / (std::string(name) + "-" + std::to_string(ticks));
}

} // namespace

void test_lua_dock_layout_key_uses_protocol_and_script()
{
    const auto key = protoscope::ui::luaDockLayoutKey("protocols\\templates\\default_protocol\\",
                                                      "protocols/templates/default_protocol/main.lua");
    require(key == "protocols_templates_default_protocol", "布局 key 应按协议目录生成");
    require(protoscope::ui::luaDockLayoutKey("protocols/templates/default_protocol",
                                             "protocols\\templates\\default_protocol\\main.lua") == key,
            "路径分隔符差异不应改变布局 key");
    require(protoscope::ui::luaDockLayoutKey("protocols/templates/default_protocol",
                                             "protocols/templates/default_protocol/advanced.lua") == key,
            "同协议目录下不同入口脚本应共享布局 key");
    require(protoscope::ui::legacyLuaDockLayoutKey("protocols\\templates\\default_protocol\\",
                                                   "protocols/templates/default_protocol/main.lua") ==
                "protocols_templates_default_protocol__protocols_templates_default_protocol_main.lua",
            "旧布局 key 应支持从脚本级布局迁移");
    require(protoscope::ui::luaDockLayoutKey("", "") == "default", "空路径应回退到 default");
}

void test_lua_dock_layout_key_falls_back_to_script_directory()
{
    const auto mainKey = protoscope::ui::luaDockLayoutKey("", "protocols/demo/main.lua");
    const auto advancedKey = protoscope::ui::luaDockLayoutKey("", "protocols/demo/advanced.lua");
    require(mainKey == "protocols_demo", "未提供协议目录时应使用脚本所在目录作为协议 key");
    require(mainKey == advancedKey, "同一脚本目录下多个 Lua 文件应共享布局 key");
}

void test_lua_dock_layout_paths_prefer_user_layout()
{
    const auto root = uniqueLayoutRoot("protoscope-layout-paths");
    const auto protocolDir = std::filesystem::path("protocols") / "demo";
    const auto scriptPath = protocolDir / "main.lua";
    const auto layoutPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo");
    std::filesystem::create_directories(layoutPath.parent_path());
    std::ofstream(layoutPath) << "[Window][demo]\n";

    const auto paths =
        protoscope::ui::resolveLuaDockLayoutPaths(root, protocolDir.generic_string(), scriptPath.generic_string());
    require(paths.protocolKey == "protocols_demo", "协议布局 key 应按目录生成");
    require(paths.layoutPath == layoutPath, "新布局路径应位于 exe/config/ui 下");
    require(paths.hasUserLayout, "已有用户布局文件时应优先识别为用户持久化布局");
    require(paths.isLegacyLayout, "已有 ini 但缺少 meta 时应按 legacy 布局迁移");
}

void test_lua_dock_layout_paths_detect_legacy_source()
{
    const auto root = uniqueLayoutRoot("protoscope-layout-legacy");
    const auto protocolDir = std::filesystem::path("protocols") / "demo";
    const auto scriptPath = protocolDir / "main.lua";
    const auto legacyPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo__protocols_demo_main.lua");
    std::filesystem::create_directories(legacyPath.parent_path());
    std::ofstream(legacyPath) << "[Window][legacy]\n";

    const auto paths =
        protoscope::ui::resolveLuaDockLayoutPaths(root, protocolDir.generic_string(), scriptPath.generic_string());
    require(!paths.hasUserLayout, "新布局不存在时不应误判为用户布局");
    require(paths.hasLegacyLayout, "旧脚本级布局文件应作为迁移来源");
    require(paths.isLegacyLayout, "旧脚本级布局文件应触发一次性迁移");
}

void test_lua_dock_layout_meta_path_is_sibling_yaml()
{
    const auto root = uniqueLayoutRoot("protoscope-layout-meta-path");

    const auto layoutPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo");
    const auto metaPath = protoscope::ui::luaDockLayoutMetaPath(root, "protocols_demo");

    require(metaPath.parent_path() == layoutPath.parent_path(), "layout meta 应与 ImGui ini 并列保存");
    require(metaPath.filename() == "protocols_demo.layout.yaml", "layout meta 文件名应使用 layout key");
}

void test_lua_dock_layout_meta_schema_v2_marks_modern_layout()
{
    const auto root = uniqueLayoutRoot("protoscope-layout-modern");
    const auto protocolDir = std::filesystem::path("protocols") / "demo";
    const auto scriptPath = protocolDir / "main.lua";
    const auto layoutPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo");
    const auto metaPath = protoscope::ui::luaDockLayoutMetaPath(root, "protocols_demo");
    std::filesystem::create_directories(layoutPath.parent_path());
    std::ofstream(layoutPath) << "[Window][demo]\nDockId=0x1\n";
    protoscope::ui::writeLuaDockLayoutMeta(metaPath, 2);

    const auto paths =
        protoscope::ui::resolveLuaDockLayoutPaths(root, protocolDir.generic_string(), scriptPath.generic_string());
    require(paths.hasUserLayout, "现代布局仍应识别已有 ImGui ini");
    require(paths.hasMeta, "schema v2 meta 应被读取");
    require(paths.schemaVersion == 2, "schema version 应从 meta 读取");
    require(paths.isLegacyLayout, "schema v2 meta 在当前最小版本(v3)下应标记为旧布局");
    require(
        protoscope::ui::workspaceLayoutModeAfterLoad(paths) == protoscope::ui::WorkspaceLayoutMode::NeedsDefaultBuild,
        "v2 布局在当前最小版本(v3)下应触发默认重建");
}

void test_lua_dock_layout_meta_read_failure_falls_back_to_legacy()
{
    const auto root = uniqueLayoutRoot("protoscope-layout-broken-meta");
    const auto protocolDir = std::filesystem::path("protocols") / "demo";
    const auto scriptPath = protocolDir / "main.lua";
    const auto layoutPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo");
    const auto metaPath = protoscope::ui::luaDockLayoutMetaPath(root, "protocols_demo");
    std::filesystem::create_directories(layoutPath.parent_path());
    std::ofstream(layoutPath) << "[Window][demo]\n";
    std::ofstream(metaPath) << "schema_version: [\n";

    const auto paths =
        protoscope::ui::resolveLuaDockLayoutPaths(root, protocolDir.generic_string(), scriptPath.generic_string());
    require(paths.hasUserLayout, "损坏 meta 不应影响 ini 识别");
    require(!paths.hasMeta, "损坏 meta 应视为缺失");
    require(paths.isLegacyLayout, "损坏 meta 应回退为 legacy 布局");
}

void test_lua_dock_layout_meta_schema_v3_marks_modern_layout()
{
    const auto root = uniqueLayoutRoot("protoscope-layout-v3-modern");
    const auto protocolDir = std::filesystem::path("protocols") / "demo";
    const auto scriptPath = protocolDir / "main.lua";
    const auto layoutPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo");
    const auto metaPath = protoscope::ui::luaDockLayoutMetaPath(root, "protocols_demo");
    std::filesystem::create_directories(layoutPath.parent_path());
    std::ofstream(layoutPath) << "[Window][demo]\nDockId=0x1\n";
    protoscope::ui::writeLuaDockLayoutMeta(metaPath, 3);

    const auto paths =
        protoscope::ui::resolveLuaDockLayoutPaths(root, protocolDir.generic_string(), scriptPath.generic_string());
    require(paths.hasUserLayout, "v3 布局应识别已有 ImGui ini");
    require(paths.hasMeta, "schema v3 meta 应被读取");
    require(paths.schemaVersion == 3, "schema version 应从 meta 读取");
    require(!paths.isLegacyLayout, "schema v3 meta 应标记为现代布局");
    require(protoscope::ui::workspaceLayoutModeAfterLoad(paths) == protoscope::ui::WorkspaceLayoutMode::Ready,
            "v3 现代布局应直接尊重用户布局");
}

void test_lua_dock_layout_dock_id_sharing_does_not_mark_modern_legacy()
{
    const auto root = uniqueLayoutRoot("protoscope-layout-shared-dockid");
    const auto protocolDir = std::filesystem::path("protocols") / "demo";
    const auto scriptPath = protocolDir / "main.lua";
    const auto layoutPath = protoscope::ui::luaDockLayoutPath(root, "protocols_demo");
    const auto metaPath = protoscope::ui::luaDockLayoutMetaPath(root, "protocols_demo");
    std::filesystem::create_directories(layoutPath.parent_path());
    std::ofstream(layoutPath) << "[Window][协议脚本 / 动态控件]\nDockId=0x00000001\n"
                              << "[Window][协议动作###LuaDock:protocols_demo:protocol]\nDockId=0x00000001\n";
    protoscope::ui::writeLuaDockLayoutMeta(metaPath, 3);

    const auto paths =
        protoscope::ui::resolveLuaDockLayoutPaths(root, protocolDir.generic_string(), scriptPath.generic_string());
    require(!paths.isLegacyLayout, "现代布局不应因 Lua Dock 与静态 Dock 共用 DockId 被迁移");
}

void test_lua_dock_window_name_keeps_stable_id()
{
    protoscope::scripting::DockDescriptor dock;
    dock.id = "protocol";
    dock.title = "协议动作";

    const auto name = protoscope::ui::luaDockWindowName(dock, "protocols_default_protocol");
    require(name == "协议动作###LuaDock:protocols_default_protocol:protocol", "Lua Dock 窗口名应包含稳定 ID");

    dock.title = "新标题";
    const auto renamed = protoscope::ui::luaDockWindowName(dock, "protocols_default_protocol");
    require(renamed == "新标题###LuaDock:protocols_default_protocol:protocol", "显示标题变化不应改变稳定 ID 后缀");
}

void test_lua_dock_layout_requests_group_tabs()
{
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

void test_lua_dock_layout_requests_default_anchor_falls_back_left_bottom()
{
    std::vector<protoscope::scripting::DockSnapshot> docks{
        makeDock("default", "默认面板", "", ""),
        makeDock("explicit", "显式右侧", "right_top", ""),
    };

    const auto requests = protoscope::ui::buildLuaDockLayoutRequests(docks, "demo");
    require(requests.size() == 2, "缺省与显式 anchor 都应生成默认布局请求");
    require(requests[0].anchor == "left_bottom", "未显式声明 anchor 时应回退到 left_bottom");
    require(requests[1].anchor == "right_top", "显式 anchor 仍应覆盖默认停靠点");
}

void test_dock_layout_ini_requires_exactly_one_central_node()
{
    const char* noCentralNodeIni = R"ini(
[Docking][Data]
DockSpace     ID=0x08BD597D Window=0x1BBC0F80 Pos=0,24 Size=2560,1345 Split=X
  DockNode    ID=0x00000001 Parent=0x08BD597D SizeRef=370,1369 Split=Y
    DockNode  ID=0x00000003 Parent=0x00000001 SizeRef=639,710 Selected=0x39C767C0
    DockNode  ID=0x00000004 Parent=0x00000001 SizeRef=639,657 Selected=0x7ED89664
  DockNode    ID=0x00000002 Parent=0x08BD597D SizeRef=2188,1369 Split=Y
    DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=1919,341
    DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=1919,1026 Selected=0x4F544A96
)ini";
    const auto noCentralNodeHealth = protoscope::ui::inspectDockLayoutIni(noCentralNodeIni);
    require(noCentralNodeHealth.centralNodeCount == 0, "无 CentralNode 布局应统计为 0");
    require(protoscope::ui::shouldRebuildDockLayout(noCentralNodeHealth), "没有 CentralNode 的布局必须回退默认布局");

    const char* singleCentralNodeIni = R"ini(
[Docking][Data]
DockSpace     ID=0x08BD597D Window=0x1BBC0F80 Pos=0,24 Size=2560,1345 Split=X
  DockNode    ID=0x00000001 Parent=0x08BD597D SizeRef=370,1369 Split=Y
    DockNode  ID=0x00000003 Parent=0x00000001 SizeRef=639,710 Selected=0x39C767C0
    DockNode  ID=0x00000004 Parent=0x00000001 SizeRef=639,657 Selected=0x7ED89664
  DockNode    ID=0x00000002 Parent=0x08BD597D SizeRef=2188,1369 Split=Y
    DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=1919,341
    DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=1919,1026 CentralNode=1 Selected=0x4F544A96
)ini";
    const auto singleCentralNodeHealth = protoscope::ui::inspectDockLayoutIni(singleCentralNodeIni);
    require(singleCentralNodeHealth.centralNodeCount == 1, "单一 CentralNode 布局应统计为 1");
    require(!singleCentralNodeHealth.centralNodeInLegacyLeftPane, "健康布局的 CentralNode 不应落在旧左栏分支");
    require(!protoscope::ui::shouldRebuildDockLayout(singleCentralNodeHealth),
            "单一且位置正确的 CentralNode 布局不应重建");

    const char* duplicateCentralNodeIni = R"ini(
[Docking][Data]
DockSpace     ID=0x08BD597D Window=0x1BBC0F80 Pos=0,24 Size=2560,1345 Split=X
  DockNode    ID=0x00000001 Parent=0x08BD597D SizeRef=370,1369 Split=Y
    DockNode  ID=0x00000003 Parent=0x00000001 SizeRef=639,710 CentralNode=1 Selected=0x39C767C0
    DockNode  ID=0x00000004 Parent=0x00000001 SizeRef=639,657 Selected=0x7ED89664
  DockNode    ID=0x00000002 Parent=0x08BD597D SizeRef=2188,1369 Split=Y
    DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=1919,341
    DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=1919,1026 CentralNode=1 Selected=0x4F544A96
)ini";
    const auto duplicateCentralNodeHealth = protoscope::ui::inspectDockLayoutIni(duplicateCentralNodeIni);
    require(duplicateCentralNodeHealth.centralNodeCount == 2, "双 CentralNode 布局应统计为 2");
    require(protoscope::ui::shouldRebuildDockLayout(duplicateCentralNodeHealth),
            "多个 CentralNode 的布局必须回退默认布局");
}

void test_dock_layout_ini_rebuilds_legacy_left_central_node()
{
    const char* legacyRootCentralNodeIni = R"ini(
[Docking][Data]
DockSpace     ID=0x08BD597D Window=0x1BBC0F80 Pos=0,24 Size=2560,1345 Split=X
  DockNode    ID=0x00000001 Parent=0x08BD597D SizeRef=32,900 CentralNode=1
  DockNode    ID=0x00000002 Parent=0x08BD597D SizeRef=2526,900 Split=Y
    DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=1199,224
    DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=1199,674 Selected=0x4F544A96
)ini";
    const auto legacyRootHealth = protoscope::ui::inspectDockLayoutIni(legacyRootCentralNodeIni);
    require(legacyRootHealth.centralNodeCount == 1, "左栏根节点坏样本也应只有一个 CentralNode");
    require(legacyRootHealth.centralNodeInLegacyLeftPane, "左栏根节点 CentralNode 必须识别为坏布局");
    require(protoscope::ui::shouldRebuildDockLayout(legacyRootHealth), "左栏根节点 CentralNode 布局必须重建");

    const char* legacyNestedCentralNodeIni = R"ini(
[Docking][Data]
DockSpace     ID=0x08BD597D Window=0x1BBC0F80 Pos=0,24 Size=2560,1345 Split=X
  DockNode    ID=0x00000001 Parent=0x08BD597D SizeRef=370,1369 Split=Y
    DockNode  ID=0x00000003 Parent=0x00000001 SizeRef=639,710 CentralNode=1 Selected=0x39C767C0
    DockNode  ID=0x00000004 Parent=0x00000001 SizeRef=639,657 Selected=0x7ED89664
  DockNode    ID=0x00000002 Parent=0x08BD597D SizeRef=2188,1369 Split=Y
    DockNode  ID=0x00000005 Parent=0x00000002 SizeRef=1919,341
    DockNode  ID=0x00000006 Parent=0x00000002 SizeRef=1919,1026 Selected=0x4F544A96
)ini";
    const auto legacyNestedHealth = protoscope::ui::inspectDockLayoutIni(legacyNestedCentralNodeIni);
    require(legacyNestedHealth.centralNodeCount == 1, "左栏子树坏样本也应只有一个 CentralNode");
    require(legacyNestedHealth.centralNodeInLegacyLeftPane, "左栏子树 CentralNode 必须识别为坏布局");
    require(protoscope::ui::shouldRebuildDockLayout(legacyNestedHealth), "左栏子树 CentralNode 布局必须重建");
}

void test_lua_dock_layout_requests_preserve_supported_anchors()
{
    std::vector<protoscope::scripting::DockSnapshot> docks{
        makeDock("top", "顶部", "right_top", ""),
        makeDock("bottom", "底部", "right_bottom", ""),
        makeDock("main", "主区", "main_bottom", ""),
    };

    const auto requests = protoscope::ui::buildLuaDockLayoutRequests(docks, "demo");
    require(requests.size() == 3, "应为每个 anchor 生成默认停靠请求");
    require(requests[0].anchor == "right_top", "right_top 应保留为有效默认停靠点");
    require(requests[1].anchor == "right_bottom", "right_bottom 应保留为有效默认停靠点");
    require(requests[2].anchor == "main_bottom", "main_bottom 应保留为有效默认停靠点");
}

void test_lua_dock_settings_filter_keeps_current_protocol_windows()
{
    require(protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:protocols_demo:active", "protocols_demo"),
            "当前协议真实存在的 Lua Dock 状态应保留");
    require(protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:protocols_demo:removed", "protocols_demo"),
            "当前协议暂未声明的 Lua Dock 状态也应保留");
    require(!protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:other:active", "protocols_demo"),
            "其它协议的 Lua Dock 状态应清理");
    require(protoscope::ui::shouldKeepLuaWindowSettings("通讯配置", "protocols_demo"),
            "静态窗口状态不应被 Lua Dock 过滤影响");
}

void test_lua_dock_settings_filter_keeps_current_windows_without_active_docks()
{
    require(protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:protocols_demo:removed", "protocols_demo"),
            "空 Dock 集合不应清理当前协议 Lua Dock 状态");
    require(!protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:other:active", "protocols_demo"),
            "空 Dock 集合也应清理其它协议 Lua Dock 状态");
    require(protoscope::ui::shouldKeepLuaWindowSettings("通讯配置", "protocols_demo"),
            "空 Dock 集合不应影响静态窗口状态");
}

void test_lua_dock_settings_filter_keeps_same_dock_id_tab_stack()
{
    const auto root = uniqueLayoutRoot("protoscope-layout-no-meta");
    const auto metaPath = protoscope::ui::luaDockLayoutMetaPath(root, "protocols_demo");
    const auto iniSnippet = "[Window][协议脚本]\n"
                            "DockId=0x00000001\n"
                            "[Window][协议动作###LuaDock:protocols_demo:protocol]\n"
                            "DockId=0x00000001\n";

    require(std::string_view(iniSnippet).find("DockId=0x00000001") != std::string_view::npos,
            "回归样本应包含同 Tab 栈 DockId");
    require(!std::filesystem::exists(metaPath), "回归行为不应依赖 .layout.yaml 是否存在");
    require(protoscope::ui::shouldKeepLuaWindowSettings("LuaDock:protocols_demo:protocol", "protocols_demo"),
            "Lua Dock 与静态 Dock 同 Tab 时不应按 stable ID 判定删除");
    require(protoscope::ui::shouldKeepLuaWindowSettings("协议脚本", "protocols_demo"),
            "静态 Dock 与 Lua Dock 共用 DockId 时也必须保留");
}

void test_workspace_layout_mode_after_load_prefers_default_build_only_when_missing()
{
    protoscope::ui::LuaDockLayoutPaths layoutPaths;

    require(protoscope::ui::workspaceLayoutModeAfterLoad(layoutPaths) ==
                protoscope::ui::WorkspaceLayoutMode::NeedsDefaultBuild,
            "没有布局文件时应构建默认 Dock 布局");

    layoutPaths.hasUserLayout = true;
    layoutPaths.hasMeta = true;
    layoutPaths.schemaVersion = 2;
    require(protoscope::ui::workspaceLayoutModeAfterLoad(layoutPaths) == protoscope::ui::WorkspaceLayoutMode::Ready,
            "加载用户布局后应直接进入 Ready");

    layoutPaths.hasUserLayout = false;
    layoutPaths.hasMeta = false;
    layoutPaths.schemaVersion = 0;
    layoutPaths.hasLegacyLayout = true;
    layoutPaths.isLegacyLayout = true;
    require(protoscope::ui::workspaceLayoutModeAfterLoad(layoutPaths) ==
                protoscope::ui::WorkspaceLayoutMode::NeedsDefaultBuild,
            "加载 legacy 布局后应触发默认布局迁移");
}

void test_protocol_workspace_switch_decision_uses_draft_only_until_reload()
{
    const auto decision = protoscope::ui::decideProtocolWorkspaceSwitch(
        "protocols/templates/default_protocol", "protocols/lua_waveform_demo", false);
    require(decision.draftChanged, "草稿协议与已加载协议不一致时应标记 draftChanged");
    require(!decision.reloadProtocolDir.has_value(), "未点击重载前不应发起真实协议切换");
}

void test_protocol_workspace_switch_decision_reloads_draft_when_clicked()
{
    const auto decision = protoscope::ui::decideProtocolWorkspaceSwitch(
        "protocols/templates/default_protocol", "protocols/lua_waveform_demo", true);
    require(decision.draftChanged, "点击重载时仍应保留草稿差异语义");
    require(decision.reloadProtocolDir.has_value(), "点击重载后应返回真实切换目标");
    require(*decision.reloadProtocolDir == "protocols/lua_waveform_demo", "点击重载时必须使用草稿协议目录");
}

void test_protocol_switch_resets_lua_default_dock_state_only_when_changed()
{
    require(protoscope::ui::shouldResetLuaDefaultDockStateOnProtocolSwitch(false), "协议切换时应清空 Lua 默认停靠缓存");
    require(!protoscope::ui::shouldResetLuaDefaultDockStateOnProtocolSwitch(true),
            "同协议重载不应清空 Lua 默认停靠缓存");
}

void test_lua_default_dock_layout_runs_only_during_default_build()
{
    require(protoscope::ui::shouldRunLuaDefaultDockLayout(protoscope::ui::WorkspaceLayoutMode::NeedsDefaultBuild, true),
            "默认布局初始化事务应允许首次停靠 Lua Dock");
    require(!protoscope::ui::shouldRunLuaDefaultDockLayout(protoscope::ui::WorkspaceLayoutMode::Ready, true),
            "已有用户布局 Ready 后不应再次执行 Lua Dock 默认停靠");
    require(
        !protoscope::ui::shouldRunLuaDefaultDockLayout(protoscope::ui::WorkspaceLayoutMode::NeedsDefaultBuild, false),
        "没有待处理事务时不应执行 Lua Dock 默认停靠");
}

void test_protocol_workspace_layout_reset_requires_loaded_protocol()
{
    require(!protoscope::ui::canResetProtocolWorkspaceLayout(false, "protocols_demo"),
            "未加载协议工作区时不应允许重置布局");
    require(!protoscope::ui::canResetProtocolWorkspaceLayout(true, ""), "缺少当前协议布局 key 时不应允许重置布局");
    require(protoscope::ui::canResetProtocolWorkspaceLayout(true, "protocols_demo"),
            "已加载协议且有布局 key 时应允许手动重置布局");
}
