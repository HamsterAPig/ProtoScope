#include "protoscope/ui/keyboard_shortcuts.hpp"

#include "test_registry.hpp"

#include <stdexcept>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

} // namespace

void test_keyboard_shortcut_table_has_no_scope_duplicates()
{
    require(!protoscope::ui::hasDuplicateShortcuts(protoscope::ui::ShortcutScope::Global),
            "全局快捷键不应存在同作用域重复绑定");
    require(!protoscope::ui::hasDuplicateShortcuts(protoscope::ui::ShortcutScope::WaveDock),
            "波形 Dock 快捷键不应存在同作用域重复绑定");
}

void test_keyboard_shortcut_labels_match_plan()
{
    using protoscope::ui::ShortcutAction;

    require(protoscope::ui::shortcutLabel(ShortcutAction::SaveConfig) == "Ctrl+S", "保存配置快捷键应为 Ctrl+S");
    require(protoscope::ui::shortcutLabel(ShortcutAction::ReloadProtocol) == "F5", "重载协议快捷键应为 F5");
    require(protoscope::ui::shortcutLabel(ShortcutAction::ToggleWaveDock) == "Ctrl+6",
            "波形 Dock 快捷键应为 Ctrl+6");
    require(protoscope::ui::shortcutLabel(ShortcutAction::WaveClearHistory) == "Ctrl+Shift+C",
            "清空波形历史应使用防误触组合键");
}
