#include "protoscope/ui/keyboard_shortcuts.hpp"

#include <array>

namespace protoscope::ui {

namespace {

constexpr std::array<ShortcutDescriptor, 22> kShortcutDescriptors{{
    {ShortcutAction::SaveConfig,
     ShortcutScope::Global,
     {.key = ShortcutKey::S, .ctrl = true},
     "Ctrl+S",
     "保存当前配置"},
    {ShortcutAction::ReloadConfig,
     ShortcutScope::Global,
     {.key = ShortcutKey::R, .ctrl = true},
     "Ctrl+R",
     "重新加载配置文件"},
    {ShortcutAction::ReloadProtocol,
     ShortcutScope::Global,
     {.key = ShortcutKey::F5},
     "F5",
     "重新加载当前协议"},
    {ShortcutAction::OpenElfDataFile,
     ShortcutScope::Global,
     {.key = ShortcutKey::O, .ctrl = true},
     "Ctrl+O",
     "打开 ELF/ElfStaticView 数据文件"},
    {ShortcutAction::ImportRawWave,
     ShortcutScope::Global,
     {.key = ShortcutKey::I, .ctrl = true},
     "Ctrl+I",
     "导入原始波形"},
    {ShortcutAction::ExportRawWave,
     ShortcutScope::Global,
     {.key = ShortcutKey::E, .ctrl = true},
     "Ctrl+E",
     "导出当前缓存快照"},
    {ShortcutAction::ToggleRawRecording,
     ShortcutScope::Global,
     {.key = ShortcutKey::R, .ctrl = true, .shift = true},
     "Ctrl+Shift+R",
     "开始或停止完整原始数据录制"},
    {ShortcutAction::ToggleCommDock,
     ShortcutScope::Global,
     {.key = ShortcutKey::Digit1, .ctrl = true},
     "Ctrl+1",
     "显示或隐藏通讯配置 Dock"},
    {ShortcutAction::ToggleProtocolDock,
     ShortcutScope::Global,
     {.key = ShortcutKey::Digit2, .ctrl = true},
     "Ctrl+2",
     "显示或隐藏协议 Dock"},
    {ShortcutAction::ToggleTransferDock,
     ShortcutScope::Global,
     {.key = ShortcutKey::Digit3, .ctrl = true},
     "Ctrl+3",
     "显示或隐藏收发数据 Dock"},
    {ShortcutAction::ToggleRequestTraceDock,
     ShortcutScope::Global,
     {.key = ShortcutKey::Digit7, .ctrl = true},
     "Ctrl+7",
     "显示或隐藏请求追踪 Dock"},
    {ShortcutAction::ToggleOfflineReplayDock,
     ShortcutScope::Global,
     {.key = ShortcutKey::Digit8, .ctrl = true},
     "Ctrl+8",
     "显示或隐藏离线复现 Dock"},
    {ShortcutAction::ToggleLogDock,
     ShortcutScope::Global,
     {.key = ShortcutKey::Digit4, .ctrl = true},
     "Ctrl+4",
     "显示或隐藏日志 Dock"},
    {ShortcutAction::ToggleScriptDock,
     ShortcutScope::Global,
     {.key = ShortcutKey::Digit5, .ctrl = true},
     "Ctrl+5",
     "显示或隐藏脚本 Dock"},
    {ShortcutAction::ToggleWaveDock,
     ShortcutScope::Global,
     {.key = ShortcutKey::Digit6, .ctrl = true},
     "Ctrl+6",
     "显示或隐藏波形 Dock"},
    {ShortcutAction::ShowShortcutHelp,
     ShortcutScope::Global,
     {.key = ShortcutKey::F1},
     "F1",
     "打开快捷键说明"},
    {ShortcutAction::WaveTogglePauseFollow,
     ShortcutScope::WaveDock,
     {.key = ShortcutKey::Space},
     "Space",
     "暂停或恢复波形自动跟随"},
    {ShortcutAction::WaveFitVisible,
     ShortcutScope::WaveDock,
     {.key = ShortcutKey::A},
     "A",
     "适配当前可见波形"},
    {ShortcutAction::WaveToggleZoomSelection,
     ShortcutScope::WaveDock,
     {.key = ShortcutKey::Z},
     "Z",
     "切换框选放大模式"},
    {ShortcutAction::WaveToggleFft,
     ShortcutScope::WaveDock,
     {.key = ShortcutKey::F},
     "F",
     "切换 FFT 频谱模式"},
    {ShortcutAction::WaveToggleFullscreen,
     ShortcutScope::WaveDock,
     {.key = ShortcutKey::F11},
     "F11",
     "切换波形主视图全屏"},
    {ShortcutAction::WaveClearHistory,
     ShortcutScope::WaveDock,
     {.key = ShortcutKey::C, .ctrl = true, .shift = true},
     "Ctrl+Shift+C",
     "清空当前波形历史"},
}};

} // namespace

std::span<const ShortcutDescriptor> shortcutDescriptors()
{
    return kShortcutDescriptors;
}

const ShortcutDescriptor* findShortcut(const ShortcutAction action)
{
    for (const auto& descriptor : kShortcutDescriptors) {
        if (descriptor.action == action) {
            return &descriptor;
        }
    }
    return nullptr;
}

std::string_view shortcutLabel(const ShortcutAction action)
{
    const auto* descriptor = findShortcut(action);
    return descriptor == nullptr ? std::string_view{} : std::string_view{descriptor->label};
}

bool sameShortcutChord(const ShortcutChord& left, const ShortcutChord& right)
{
    return left.key == right.key && left.ctrl == right.ctrl && left.shift == right.shift && left.alt == right.alt;
}

bool hasDuplicateShortcuts(const ShortcutScope scope)
{
    const auto descriptors = shortcutDescriptors();
    for (std::size_t leftIndex = 0; leftIndex < descriptors.size(); ++leftIndex) {
        if (descriptors[leftIndex].scope != scope) {
            continue;
        }
        for (std::size_t rightIndex = leftIndex + 1; rightIndex < descriptors.size(); ++rightIndex) {
            if (descriptors[rightIndex].scope == scope &&
                sameShortcutChord(descriptors[leftIndex].chord, descriptors[rightIndex].chord)) {
                return true;
            }
        }
    }
    return false;
}

} // namespace protoscope::ui
