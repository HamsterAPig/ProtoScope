#pragma once

#include <span>
#include <string_view>

namespace protoscope::ui {

enum class ShortcutScope {
    Global,
    WaveDock,
};

enum class ShortcutKey {
    S,
    R,
    O,
    I,
    E,
    F,
    A,
    C,
    Z,
    Digit1,
    Digit2,
    Digit3,
    Digit4,
    Digit5,
    Digit6,
    Space,
    Escape,
    F1,
    F5,
    F11,
};

enum class ShortcutAction {
    SaveConfig,
    ReloadConfig,
    ReloadProtocol,
    OpenElfDataFile,
    ImportRawWave,
    ExportRawWave,
    ToggleRawRecording,
    ToggleCommDock,
    ToggleProtocolDock,
    ToggleTransferDock,
    ToggleLogDock,
    ToggleScriptDock,
    ToggleWaveDock,
    ShowShortcutHelp,
    WaveTogglePauseFollow,
    WaveFitVisible,
    WaveToggleZoomSelection,
    WaveToggleFft,
    WaveToggleFullscreen,
    WaveClearHistory,
};

struct ShortcutChord {
    ShortcutKey key;
    bool ctrl{false};
    bool shift{false};
    bool alt{false};
};

struct ShortcutDescriptor {
    ShortcutAction action;
    ShortcutScope scope;
    ShortcutChord chord;
    const char* label;
    const char* description;
};

std::span<const ShortcutDescriptor> shortcutDescriptors();
const ShortcutDescriptor* findShortcut(ShortcutAction action);
std::string_view shortcutLabel(ShortcutAction action);
bool hasDuplicateShortcuts(ShortcutScope scope);
bool sameShortcutChord(const ShortcutChord& left, const ShortcutChord& right);

} // namespace protoscope::ui
