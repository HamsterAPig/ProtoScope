#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/keyboard_shortcuts.hpp"

#include <imgui.h>

namespace protoscope::ui {

namespace {

ImGuiKey toImGuiKey(const ShortcutKey key)
{
    switch (key) {
        case ShortcutKey::S:
            return ImGuiKey_S;
        case ShortcutKey::R:
            return ImGuiKey_R;
        case ShortcutKey::O:
            return ImGuiKey_O;
        case ShortcutKey::I:
            return ImGuiKey_I;
        case ShortcutKey::E:
            return ImGuiKey_E;
        case ShortcutKey::F:
            return ImGuiKey_F;
        case ShortcutKey::A:
            return ImGuiKey_A;
        case ShortcutKey::C:
            return ImGuiKey_C;
        case ShortcutKey::Z:
            return ImGuiKey_Z;
        case ShortcutKey::Digit1:
            return ImGuiKey_1;
        case ShortcutKey::Digit2:
            return ImGuiKey_2;
        case ShortcutKey::Digit3:
            return ImGuiKey_3;
        case ShortcutKey::Digit4:
            return ImGuiKey_4;
        case ShortcutKey::Digit5:
            return ImGuiKey_5;
        case ShortcutKey::Digit6:
            return ImGuiKey_6;
        case ShortcutKey::Space:
            return ImGuiKey_Space;
        case ShortcutKey::Escape:
            return ImGuiKey_Escape;
        case ShortcutKey::F1:
            return ImGuiKey_F1;
        case ShortcutKey::F5:
            return ImGuiKey_F5;
    }
    return ImGuiKey_None;
}

bool chordPressed(const ShortcutChord& chord)
{
    const auto& io = ImGui::GetIO();
    if (io.KeyCtrl != chord.ctrl || io.KeyShift != chord.shift || io.KeyAlt != chord.alt) {
        return false;
    }
    return ImGui::IsKeyPressed(toImGuiKey(chord.key), false);
}

bool globalShortcutInputBlocked()
{
    const auto& io = ImGui::GetIO();
    // 核心流程：全局快捷键只在普通浏览态生效，输入框和弹窗优先获得键盘控制权。
    return io.WantTextInput ||
           ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}

bool shortcutPressed(const ShortcutAction action)
{
    const auto* descriptor = findShortcut(action);
    return descriptor != nullptr && chordPressed(descriptor->chord);
}

void toggleDock(bool& visible, bool& pendingSave)
{
    visible = !visible;
    pendingSave = true;
}

} // namespace

void GuiRuntime::handleGlobalShortcuts()
{
    if (globalShortcutInputBlocked()) {
        return;
    }

    auto& lua = application_.docks().luaState();
    if (shortcutPressed(ShortcutAction::SaveConfig)) {
        saveCurrentConfigToDisk();
        return;
    }
    if (shortcutPressed(ShortcutAction::ReloadConfig)) {
        if (!reloadConfigFromDisk()) {
            application_.setStatusMessage("从磁盘重载配置失败", true);
        }
        return;
    }
    if (shortcutPressed(ShortcutAction::ReloadProtocol)) {
        requestProtocolWorkspaceSwitch(lua.protocolDir, true);
        return;
    }
    if (shortcutPressed(ShortcutAction::OpenElfDataFile)) {
        openElfStaticAddressDialog();
        return;
    }
    if (shortcutPressed(ShortcutAction::ImportRawWave)) {
        openRawCaptureImportDialog();
        return;
    }
    if (shortcutPressed(ShortcutAction::ExportRawWave)) {
        openRawCaptureExportDialog();
        return;
    }
    if (shortcutPressed(ShortcutAction::ToggleRawRecording)) {
        if (application_.isRawCaptureRecording()) {
            stopRawCaptureRecordingWithStatus();
        } else {
            openRawCaptureRecordingDialog();
        }
        return;
    }
    if (shortcutPressed(ShortcutAction::ToggleCommDock)) {
        toggleDock(showCommDock_, pendingProtocolWorkspaceSave_);
        return;
    }
    if (shortcutPressed(ShortcutAction::ToggleProtocolDock)) {
        toggleDock(showProtocolDock_, pendingProtocolWorkspaceSave_);
        return;
    }
    if (shortcutPressed(ShortcutAction::ToggleTransferDock)) {
        toggleDock(showTransferDock_, pendingProtocolWorkspaceSave_);
        return;
    }
    if (shortcutPressed(ShortcutAction::ToggleLogDock)) {
        toggleDock(showLogDock_, pendingProtocolWorkspaceSave_);
        return;
    }
    if (shortcutPressed(ShortcutAction::ToggleScriptDock)) {
        toggleDock(showScriptDock_, pendingProtocolWorkspaceSave_);
        return;
    }
    if (shortcutPressed(ShortcutAction::ToggleWaveDock)) {
        toggleDock(showWaveDock_, pendingProtocolWorkspaceSave_);
        return;
    }
    if (shortcutPressed(ShortcutAction::ShowShortcutHelp)) {
        requestShortcutHelpDialog();
    }
}

} // namespace protoscope::ui
