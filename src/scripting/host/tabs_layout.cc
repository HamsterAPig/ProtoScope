#include "tabs_layout.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace protoscope::scripting {
namespace {
std::string requiredText(const sol::table& table, const char* name)
{
    const sol::object value = table[name];
    if (value.get_type() != sol::type::string) throw std::invalid_argument(std::string(name) + " must be string");
    auto text = value.as<std::string>();
    if (text.empty() || text.size() > 256 || text.find('\0') != text.npos)
        throw std::invalid_argument(std::string(name) + " requires 1..256 bytes without NUL");
    return text;
}

bool registerNode(LayoutNodeDescriptor& node, DockDescriptor& dock, std::set<std::string>& ids, std::string& error)
{
    if (node.kind == LayoutNodeKind::Tabs) {
        if (!ids.insert(node.controlId).second) {
            error = "duplicate tabs/control ID: " + node.controlId;
            return false;
        }
        // 页选择复用控件事件、代次和记忆链路，但布局结构保持固定，不支持动态 options。
        ControlDescriptor selector;
        selector.type = ControlType::TabSelection;
        selector.id = node.controlId;
        selector.label = node.controlId;
        selector.comboOptions = node.tabIds;
        selector.textDefault = node.defaultTab;
        selector.maxLength = 256;
        node.controlIndex = dock.controls.size();
        dock.controls.push_back(std::move(selector));
    }
    for (auto& child : node.children)
        if (!registerNode(child, dock, ids, error)) return false;
    for (auto& row : node.rows)
        for (auto& child : row)
            if (!registerNode(child, dock, ids, error)) return false;
    return true;
}
}

std::optional<LayoutNodeDescriptor> parseTabsLayout(
    const sol::table& table, const std::string& path, const ParseTabChildren& parseChildren, std::string& error)
{
    try {
        LayoutNodeDescriptor node;
        node.kind = LayoutNodeKind::Tabs;
        node.controlId = requiredText(table, "id");
        const sol::object pagesObject = table["pages"];
        if (pagesObject.get_type() != sol::type::table) throw std::invalid_argument("pages must be array");
        const auto pages = pagesObject.as<sol::table>();
        std::size_t count = 0;
        for (const auto& [key, value] : pages) {
            if (key.get_type() != sol::type::number || !key.is<int>() || key.as<int>() < 1 ||
                key.as<int>() > 64 || value.get_type() != sol::type::table)
                throw std::invalid_argument("pages requires a dense array of 1..64 tables");
            ++count;
        }
        if (count == 0 || count > 64) throw std::invalid_argument("tabs requires 1..64 pages");
        std::set<std::string> ids;
        for (std::size_t i = 1; i <= count; ++i) {
            const sol::object pageObject = pages[i];
            if (pageObject.get_type() != sol::type::table) throw std::invalid_argument("sparse pages");
            const auto page = pageObject.as<sol::table>();
            auto id = requiredText(page, "id");
            if (!ids.insert(id).second) throw std::invalid_argument("duplicate page ID");
            LayoutNodeDescriptor child;
            child.title = requiredText(page, "title");
            const sol::object contents = page["children"];
            if (contents.get_type() != sol::type::table) throw std::invalid_argument("page children must be array");
            const auto entries = contents.as<sol::table>();
            std::size_t childCount = 0;
            for (const auto& [key, value] : entries) {
                if (key.get_type() != sol::type::number || !key.is<int>() ||
                    key.as<int>() < 1 || key.as<int>() > static_cast<int>(entries.size()))
                    throw std::invalid_argument("page children must be dense array");
                ++childCount;
            }
            if (childCount != entries.size()) throw std::invalid_argument("sparse page children");
            // 空页可作为固定占位页；非空页沿用已有布局解析和控件唯一性校验。
            if (childCount != 0) {
                auto children = parseChildren(page, path + ".pages[" + std::to_string(i) + "]", error);
                if (!children) return {};
                child.children = std::move(*children);
            }
            node.children.push_back(std::move(child));
            node.tabIds.push_back(std::move(id));
        }
        const sol::object selected = table["default"];
        node.defaultTab = selected.get_type() == sol::type::lua_nil ? node.tabIds.front() : requiredText(table, "default");
        if (!ids.contains(node.defaultTab)) throw std::invalid_argument("unknown default page ID");
        return node;
    } catch (const std::exception& exception) {
        error = path + ": " + exception.what();
        return {};
    }
}

bool registerTabSelections(DockDescriptor& dock, std::string& error)
{
    if (!dock.layout) return true;
    std::set<std::string> ids;
    for (const auto& control : dock.controls) ids.insert(control.id);
    return registerNode(dock.layout->root, dock, ids, error);
}
} // namespace protoscope::scripting
