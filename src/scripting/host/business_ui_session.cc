#include "business_ui_session.hpp"
#include "script_host_internal.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace protoscope::scripting {
namespace {
void readText(const sol::table& table, const char* key, std::string& value, bool required = false)
{
    const sol::object object = table[key];
    if ((!object.valid() || object.get_type() == sol::type::lua_nil) && !required) return;
    if (object.get_type() != sol::type::string) throw std::invalid_argument(std::string(key) + " must be string");
    value = object.as<std::string>();
    if ((required && value.empty()) || value.size() > 4096 || value.find('\0') != value.npos)
        throw std::invalid_argument(std::string(key) + " exceeds limit or contains NUL");
}
void readBool(const sol::table& table, const char* key, bool& value)
{
    const sol::object object = table[key];
    if (!object.valid() || object.get_type() == sol::type::lua_nil) return;
    if (object.get_type() != sol::type::boolean) throw std::invalid_argument(std::string(key) + " must be boolean");
    value = object.as<bool>();
}
void checkKeys(const sol::table& table, bool patch)
{
    static const std::set<std::string> mutableKeys{"label","tooltip","visible","disabled","checked"};
    static const std::set<std::string> declarationKeys{"id","separator","checkable","children"};
    for (const auto& [key, value] : table) {
        if (key.get_type() != sol::type::string) throw std::invalid_argument("menu property must be string");
        const auto text = key.as<std::string>();
        if (!mutableKeys.contains(text) && (patch || !declarationKeys.contains(text)))
            throw std::invalid_argument("unknown menu property: " + text);
    }
}
void applyProperties(BusinessMenuItem& item, const sol::table& table)
{
    readText(table,"label",item.label);
    readText(table,"tooltip",item.tooltip);
    readBool(table,"visible",item.visible);
    readBool(table,"disabled",item.disabled);
    readBool(table,"checked",item.checked);
    if (!item.separator && item.label.empty()) throw std::invalid_argument("menu label must not be empty");
    if (item.checked && !item.checkable) throw std::invalid_argument("checked requires checkable menu item");
    if (item.checkable && (!item.children.empty() || item.separator))
        throw std::invalid_argument("only command items may be checkable");
}
std::vector<BusinessMenuItem> parseItems(const sol::table& table, std::set<std::string>& ids,
                                       std::size_t& count, unsigned depth)
{
    if (depth > 8) throw std::invalid_argument("menu depth exceeds 8");
    std::size_t length = 0;
    for (const auto& [key, value] : table) {
        if (key.get_type() != sol::type::number || !key.is<int>() || key.as<int>() < 1 || key.as<int>() > 256 ||
            value.get_type() != sol::type::table) throw std::invalid_argument("menu must be dense table array");
        ++length;
    }
    std::vector<BusinessMenuItem> items;
    for (std::size_t i = 1; i <= length; ++i) {
        if (++count > 256) throw std::invalid_argument("menu exceeds 256 items");
        const sol::object object = table[i];
        if (object.get_type() != sol::type::table) throw std::invalid_argument("sparse menu array");
        const auto entry = object.as<sol::table>();
        checkKeys(entry,false);
        BusinessMenuItem item;
        readBool(entry,"separator",item.separator);
        if (!item.separator) {
            readText(entry,"id",item.id,true);
            if (!ids.insert(item.id).second) throw std::invalid_argument("duplicate menu ID");
            readText(entry,"label",item.label,true);
        }
        readBool(entry,"checkable",item.checkable);
        const sol::object children = entry["children"];
        if (children.valid() && children.get_type() != sol::type::lua_nil) {
            if (item.separator || children.get_type() != sol::type::table)
                throw std::invalid_argument("invalid submenu children");
            item.children = parseItems(children.as<sol::table>(),ids,count,depth+1);
            if (item.children.empty()) throw std::invalid_argument("submenu must not be empty");
        }
        applyProperties(item,entry);
        items.push_back(std::move(item));
    }
    return items;
}
}

BusinessMenuItem* findBusinessMenu(std::vector<BusinessMenuItem>& items, const std::string& id, bool requireEnabled)
{
    for (auto& item : items) {
        if (item.separator || (requireEnabled && (!item.visible || item.disabled))) continue;
        if (item.id == id) return &item;
        if (auto* found = findBusinessMenu(item.children,id,requireEnabled)) return found;
    }
    return nullptr;
}

bool setBusinessMenu(BusinessUiSnapshot& state, const sol::table& items, std::string& error)
{
    try {
        std::set<std::string> ids;
        std::size_t count = 0;
        auto next = parseItems(items,ids,count,1);
        state.menu.swap(next);
        ++state.menuRevision;
        return true;
    } catch (const std::exception& exception) { error=exception.what(); return false; }
}

bool updateBusinessMenu(BusinessUiSnapshot& state, const std::string& id, const sol::table& patch, std::string& error)
{
    try {
        checkKeys(patch,true);
        auto* item = findBusinessMenu(state.menu,id,false);
        if (!item) throw std::invalid_argument("unknown menu ID");
        // 先验证副本，避免 checked/label 等属性校验失败时留下部分修改。
        auto next = *item;
        applyProperties(next,patch);
        *item = std::move(next);
        return true;
    } catch (const std::exception& exception) { error=exception.what(); return false; }
}

BusinessUiSnapshot ScriptHost::businessUiSnapshot() const
{
    auto state = runtime_ ? runtime_->businessUi : BusinessUiSnapshot{};
    state.runtimeGeneration = runtimeGeneration_;
    return state;
}

bool ScriptHost::showBusinessDock(const std::string& id, bool visible, std::string& error)
{
    if (!scriptLoaded_ || !runtime_) { error="show_dock is unavailable during declaration"; return false; }
    if (std::none_of(docks_.begin(),docks_.end(),[&](const auto& dock) {return dock.id==id;})) {
        error="unknown Lua dock ID"; return false;
    }
    auto& state = runtime_->businessUi;
    const auto found = std::find_if(state.dockRequests.begin(),state.dockRequests.end(),
                                   [&](const auto& request) {return request.id==id;});
    DockVisibilityRequest request{id,visible,++state.dockRevision};
    if (found == state.dockRequests.end()) state.dockRequests.push_back(std::move(request));
    else *found = std::move(request);
    return true;
}

void ScriptHost::onMenu(const transport::ConnectionContext& context, const std::string& id, bool checked,
                        std::uint64_t generation, std::uint64_t revision)
{
    if (!scriptLoaded_ || !runtime_ || executionFaulted() || generation != runtimeGeneration_ ||
        revision != runtime_->businessUi.menuRevision) return;
    auto* item = findBusinessMenu(runtime_->businessUi.menu,id,true);
    if (!item || !item->children.empty() || (!item->checkable && checked)) return;
    item->checked = checked;
    const auto callback = resolveGlobalCallback("on_menu");
    if (!callback) return;
    CallbackScope execution(*this);
    try {
        sol::state_view lua(runtime_->lua.lua_state());
        const auto result = (*callback)(makeContextTable(lua,context),id,checked);
        if (!result.valid()) { sol::error error=result; protoLog("error","on_menu: "+std::string(error.what())); }
    } catch (const std::exception& exception) { protoLog("error","on_menu: "+std::string(exception.what())); }
}
} // namespace protoscope::scripting
