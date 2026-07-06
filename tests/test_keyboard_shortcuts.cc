#include "protoscope/ui/keyboard_shortcuts.hpp"

#include "test_registry.hpp"

#include <stdexcept>
#include <string_view>

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void requireShortcut(protoscope::ui::ShortcutAction action,
                     const char* label,
                     protoscope::ui::ShortcutScope scope,
                     protoscope::ui::ShortcutKey key,
                     bool ctrl = false,
                     bool shift = false,
                     bool alt = false)
{
    const auto* shortcut = protoscope::ui::findShortcut(action);
    require(shortcut != nullptr, "快捷键应注册到快捷键表");
    require(std::string_view{shortcut->label} == std::string_view{label}, "快捷键显示文本不符合预期");
    require(shortcut->scope == scope, "快捷键作用域不符合预期");
    require(shortcut->chord.key == key, "快捷键按键不符合预期");
    require(shortcut->chord.ctrl == ctrl, "快捷键 Ctrl 修饰键不符合预期");
    require(shortcut->chord.shift == shift, "快捷键 Shift 修饰键不符合预期");
    require(shortcut->chord.alt == alt, "快捷键 Alt 修饰键不符合预期");
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
    using protoscope::ui::ShortcutKey;
    using protoscope::ui::ShortcutScope;

    require(protoscope::ui::shortcutLabel(ShortcutAction::SaveConfig) == "Ctrl+S", "保存配置快捷键应为 Ctrl+S");
    require(protoscope::ui::shortcutLabel(ShortcutAction::ReloadProtocol) == "F5", "重载协议快捷键应为 F5");

    requireShortcut(ShortcutAction::ToggleCommDock, "Ctrl+1", ShortcutScope::Global, ShortcutKey::Digit1, true);
    requireShortcut(ShortcutAction::ToggleProtocolDock, "Ctrl+2", ShortcutScope::Global, ShortcutKey::Digit2, true);
    requireShortcut(ShortcutAction::ToggleTransferDock, "Ctrl+3", ShortcutScope::Global, ShortcutKey::Digit3, true);
    requireShortcut(ShortcutAction::ToggleRequestTraceDock, "Ctrl+4", ShortcutScope::Global, ShortcutKey::Digit4, true);
    requireShortcut(ShortcutAction::ToggleOfflineReplayDock, "Ctrl+5", ShortcutScope::Global, ShortcutKey::Digit5, true);
    requireShortcut(ShortcutAction::ToggleLogDock, "Ctrl+6", ShortcutScope::Global, ShortcutKey::Digit6, true);
    requireShortcut(ShortcutAction::ToggleScriptDock, "Ctrl+7", ShortcutScope::Global, ShortcutKey::Digit7, true);
    requireShortcut(ShortcutAction::ToggleWaveDock, "Ctrl+8", ShortcutScope::Global, ShortcutKey::Digit8, true);

    requireShortcut(
        ShortcutAction::PlaybackTogglePlayPause, "F6", ShortcutScope::Global, ShortcutKey::F6);
    requireShortcut(ShortcutAction::PlaybackStepForward, "F7", ShortcutScope::Global, ShortcutKey::F7);
    requireShortcut(
        ShortcutAction::PlaybackUnloadTimeline, "Shift+F7", ShortcutScope::Global, ShortcutKey::F7, false, true);

    requireShortcut(ShortcutAction::WaveToggleFullscreen, "F11", ShortcutScope::WaveDock, ShortcutKey::F11);
    requireShortcut(
        ShortcutAction::WaveClearHistory, "Ctrl+Shift+C", ShortcutScope::WaveDock, ShortcutKey::C, true, true);
}
