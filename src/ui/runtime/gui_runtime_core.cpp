#include "protoscope/ui/gui_runtime.hpp"

#include "protoscope/build/version.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/transport/transport.hpp"
#include "protoscope/ui/dock_layout.hpp"
#include "protoscope/ui/editable_combo.hpp"
#include "protoscope/ui/icons.hpp"
#include "protoscope/ui/protocol_ui_state.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <implot.h>
#include <yaml-cpp/yaml.h>
#include <cmrc/cmrc.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

CMRC_DECLARE(ui_resources);

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <vector>

namespace protoscope::ui {

namespace {

const char* kGlslVersion = "#version 150";
constexpr std::uint32_t kCommonBaudRates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};

std::string currentProtocolTitle(const dock::LuaDockState& lua) {
    if (!lua.protocolName.empty()) {
        return lua.protocolName;
    }
    const auto filename = std::filesystem::path(lua.protocolDir).filename().string();
    return filename.empty() ? std::string("unknown_protocol") : filename;
}

std::vector<std::filesystem::path> candidateChineseFonts() {
    return {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "3rdparty/imgui/misc/fonts/DroidSans.ttf",
    };
}

const char* transportStateLabel(transport::TransportState state) {
    switch (state) {
    case transport::TransportState::Closed:
        return "已关闭";
    case transport::TransportState::Opening:
        return "连接中";
    case transport::TransportState::Open:
        return "已连接";
    case transport::TransportState::Error:
        return "错误";
    }
    return "未知";
}

int hexInputFilter(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackCharFilter) {
        return 0;
    }

    const ImWchar ch = data->EventChar;
    if (ch < 128U && std::isspace(static_cast<unsigned char>(ch))) {
        return 0;
    }
    if (std::isxdigit(static_cast<unsigned char>(ch))) {
        return 0;
    }
    return 1;
}

#if defined(_WIN32)
HWND nativeWindowHandle(GLFWwindow* window) {
    return window == nullptr ? nullptr : glfwGetWin32Window(window);
}

int CALLBACK browseInitialDirCallback(HWND hwnd, UINT message, LPARAM, LPARAM data);

std::optional<std::filesystem::path> nativeFileDialog(GLFWwindow* window,
                                                      const wchar_t* title,
                                                      const wchar_t* filter,
                                                      const std::filesystem::path& defaultPath,
                                                      bool saveDialog,
                                                      const wchar_t* defaultExtension,
                                                      std::string& error) {
    // 核心流程：Windows 下文件选择统一走系统原生对话框，避免在 ImGui 弹窗里手输路径。
    std::array<wchar_t, 32768> buffer{};
    std::wstring initialDir;
    try {
        if (!defaultPath.empty()) {
            const bool isDirectory = std::filesystem::exists(defaultPath) && std::filesystem::is_directory(defaultPath);
            const auto fileName = isDirectory ? std::filesystem::path{} : defaultPath.filename();
            if (!fileName.empty()) {
                const auto text = fileName.wstring();
                std::wcsncpy(buffer.data(), text.c_str(), buffer.size() - 1);
            }
            const auto dir = isDirectory ? defaultPath : defaultPath.parent_path();
            if (!dir.empty()) {
                initialDir = dir.wstring();
            }
        }
    } catch (const std::exception&) {
        initialDir.clear();
    }

    OPENFILENAMEW options{};
    options.lStructSize = sizeof(options);
    options.hwndOwner = nativeWindowHandle(window);
    options.lpstrTitle = title;
    options.lpstrFilter = filter;
    options.lpstrFile = buffer.data();
    options.nMaxFile = static_cast<DWORD>(buffer.size());
    options.lpstrInitialDir = initialDir.empty() ? nullptr : initialDir.c_str();
    options.lpstrDefExt = defaultExtension;
    options.Flags = OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
    if (saveDialog) {
        options.Flags |= OFN_OVERWRITEPROMPT;
    } else {
        options.Flags |= OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    }

    const BOOL ok = saveDialog ? GetSaveFileNameW(&options) : GetOpenFileNameW(&options);
    if (ok == TRUE) {
        error.clear();
        return std::filesystem::path(buffer.data());
    }

    const DWORD code = CommDlgExtendedError();
    if (code != 0) {
        error = "Windows 文件对话框失败: " + std::to_string(code);
    } else {
        error.clear();
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> nativeDirectoryDialog(GLFWwindow* window,
                                                           const wchar_t* title,
                                                           const std::filesystem::path& defaultPath,
                                                           std::string& error) {
    // 核心流程：目录选择和文件导入导出一样走系统原生对话框，并显式指定初始目录。
    BROWSEINFOW browseInfo{};
    browseInfo.hwndOwner = nativeWindowHandle(window);
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    browseInfo.lpszTitle = title;

    std::wstring initialDir;
    try {
        initialDir = defaultPath.wstring();
    } catch (const std::exception&) {
        initialDir.clear();
    }
    browseInfo.lParam = reinterpret_cast<LPARAM>(initialDir.empty() ? nullptr : initialDir.c_str());
    browseInfo.lpfn = browseInitialDirCallback;

    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&browseInfo);
    if (selected == nullptr) {
        error.clear();
        return std::nullopt;
    }

    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(selected, path) == TRUE;
    CoTaskMemFree(selected);
    if (!ok) {
        error = "Windows 目录对话框返回路径失败";
        return std::nullopt;
    }

    error.clear();
    return std::filesystem::path(path);
}
#endif

int hexEditorCallback(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        return hexInputFilter(data);
    }
    if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit) {
        return 0;
    }

    const std::string current(data->Buf, static_cast<std::size_t>(data->BufTextLen));
    const auto normalized = protocol_utils::normalizeHexEditorInput(current, static_cast<std::size_t>(data->CursorPos));
    if (normalized.text != current) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, normalized.text.c_str());
    }

    const int cursorPos = static_cast<int>((std::min)(normalized.cursorPos, static_cast<std::size_t>(data->BufTextLen)));
    data->CursorPos = cursorPos;
    data->SelectionStart = cursorPos;
    data->SelectionEnd = cursorPos;
    return 0;
}

std::string visibleWindowTitle(std::string_view windowName) {
    const auto stableIdPos = windowName.find("###");
    if (stableIdPos == std::string_view::npos) {
        return std::string(windowName);
    }
    return std::string(windowName.substr(0, stableIdPos));
}

std::string stableWindowId(std::string_view windowName) {
    const auto stableIdPos = windowName.find("###");
    if (stableIdPos == std::string_view::npos) {
        return std::string(windowName);
    }
    return std::string(windowName.substr(stableIdPos + 3));
}

bool dockWindowIfMissing(std::string_view windowName, ImGuiID targetNode) {
    const auto name = std::string(windowName);
    if (targetNode == 0 || ImGui::FindWindowByName(name.c_str()) != nullptr) {
        return false;
    }
    ImGui::DockBuilderDockWindow(name.c_str(), targetNode);
    return true;
}

std::optional<DockLayoutIniHealth> readDockLayoutIniHealth(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        return std::nullopt;
    }
    return inspectDockLayoutIni(buffer.str());
}

void keepOnlyCurrentLuaDockSettings(std::string_view layoutKey) {
    auto& settings = ImGui::GetCurrentContext()->SettingsWindows;

    // 核心逻辑：只清理其它协议的 Lua Dock 状态，当前协议状态留给 ini 归档以避免运行时 Dock 树抖动。
    for (ImGuiWindowSettings* setting = settings.begin(); setting != nullptr; setting = settings.next_chunk(setting)) {
        const std::string_view name = setting->GetName();
        const auto stableIdPos = name.find("###");
        const auto stableId = stableIdPos == std::string_view::npos ? name : name.substr(stableIdPos + 3);
        if (!shouldKeepLuaWindowSettings(stableId, layoutKey)) {
            setting->WantDelete = true;
            setting->ID = 0;

            // 同步清除引用该 chunk 的 window->SettingsOffset,
            // 防止 WriteAll 时 FindWindowSettingsByWindow 通过原始 offset
            // 返回 ID=0 的 chunk 导致 settings->ID == window->ID 断言失败。
            auto& g = *ImGui::GetCurrentContext();
            for (int j = 0; j < g.Windows.Size; j++) {
                auto* w = g.Windows[j];
                if (w && w->SettingsOffset != -1) {
                    auto* ws = g.SettingsWindows.ptr_from_offset(w->SettingsOffset);
                    if (ws == setting) {
                        w->SettingsOffset = -1;
                    }
                }
            }
        }
    }
}

const char* controlTypeName(scripting::ControlType type) {
    switch (type) {
    case scripting::ControlType::Button:
        return "button";
    case scripting::ControlType::InputText:
        return "input_text";
    case scripting::ControlType::InputInt:
        return "input_int";
    case scripting::ControlType::InputFloat:
        return "input_float";
    case scripting::ControlType::Checkbox:
        return "checkbox";
    case scripting::ControlType::Combo:
        return "combo";
    case scripting::ControlType::ElfSymbolCombo:
        return "elf_symbol_combo";
    }
    return "unknown";
}

bool isPersistedControlType(scripting::ControlType type) {
    return type == scripting::ControlType::Checkbox || type == scripting::ControlType::InputText ||
           type == scripting::ControlType::Combo || type == scripting::ControlType::InputInt ||
           type == scripting::ControlType::InputFloat;
}

std::optional<scripting::ControlValue> readControlValue(const YAML::Node& node, scripting::ControlType type) {
    try {
        switch (type) {
        case scripting::ControlType::Checkbox:
            if (node.IsScalar()) {
                return node.as<bool>();
            }
            break;
        case scripting::ControlType::InputText:
            if (node.IsScalar()) {
                return node.as<std::string>();
            }
            break;
        case scripting::ControlType::Combo:
        case scripting::ControlType::InputInt:
            if (node.IsScalar()) {
                return node.as<int>();
            }
            break;
        case scripting::ControlType::InputFloat:
            if (node.IsScalar()) {
                return node.as<float>();
            }
            break;
        case scripting::ControlType::ElfSymbolCombo:
            break;
        case scripting::ControlType::Button:
            break;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

void writeControlValue(YAML::Node node, const scripting::ControlSnapshot& control) {
    switch (control.descriptor.type) {
    case scripting::ControlType::Checkbox:
        node = std::get<bool>(control.value);
        break;
    case scripting::ControlType::InputText:
        node = std::get<std::string>(control.value);
        break;
    case scripting::ControlType::Combo:
    case scripting::ControlType::InputInt:
        node = std::get<int>(control.value);
        break;
    case scripting::ControlType::InputFloat:
        node = std::get<float>(control.value);
        break;
    case scripting::ControlType::ElfSymbolCombo:
        break;
    case scripting::ControlType::Button:
        break;
    }
}

std::string multilineChildWindowName(const char* label) {
    ImGuiWindow* parentWindow = ImGui::GetCurrentWindow();
    const ImGuiID id = parentWindow->GetID(label);

    char windowName[512]{};
    std::snprintf(windowName, sizeof(windowName), "%s/%s_%08X", parentWindow->Name, label, id);
    return windowName;
}

ImGuiWindow* findMultilineChildWindow(const char* label) {
    const auto childWindowName = multilineChildWindowName(label);
    return ImGui::FindWindowByName(childWindowName.c_str());
}

bool shouldStickMultilineChildToBottom(const ImGuiWindow* window) {
    if (window == nullptr || window->ScrollMax.y <= 4.0F) {
        return true;
    }

    const bool alreadyAtBottom =
        window->Scroll.y >= window->ScrollMax.y - 4.0F;
    const bool pendingScrollToBottom =
        window->ScrollTarget.y != FLT_MAX &&
        window->ScrollTarget.y >= window->ScrollMax.y - 4.0F;
    return alreadyAtBottom || pendingScrollToBottom;
}

std::string formatShortLogTimestamp(std::uint64_t timestampMs) {
    const auto timePoint = std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs));
    const auto secondsPoint = std::chrono::time_point_cast<std::chrono::seconds>(timePoint);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(timePoint - secondsPoint).count();
    const std::time_t timeValue = std::chrono::system_clock::to_time_t(timePoint);

    std::tm localTm{};
#if defined(_WIN32)
    localtime_s(&localTm, &timeValue);
#else
    localtime_r(&timeValue, &localTm);
#endif

    char buffer[32]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%02d:%02d:%02d.%03d",
                  localTm.tm_hour,
                  localTm.tm_min,
                  localTm.tm_sec,
                  static_cast<int>(millis));
    return buffer;
}

struct LogRowPalette {
    ImVec4 accent;
    ImVec4 badgeBackground;
    ImVec4 badgeText;
    ImVec4 rowBackground;
};

LogRowPalette paletteForRow(const dock::ReceiveRow& row) {
    switch (dock::classifyReceiveRow(row)) {
    case dock::ReceiveRowVisualKind::Rx:
        return {ImVec4(0.22F, 0.78F, 0.62F, 1.0F),
                ImVec4(0.08F, 0.34F, 0.28F, 1.0F),
                ImVec4(0.70F, 1.0F, 0.88F, 1.0F),
                ImVec4(0.05F, 0.22F, 0.17F, 0.28F)};
    case dock::ReceiveRowVisualKind::Tx:
        return {ImVec4(1.0F, 0.63F, 0.20F, 1.0F),
                ImVec4(0.40F, 0.23F, 0.06F, 1.0F),
                ImVec4(1.0F, 0.88F, 0.58F, 1.0F),
                ImVec4(0.26F, 0.15F, 0.04F, 0.32F)};
    case dock::ReceiveRowVisualKind::Error:
        return {ImVec4(1.0F, 0.30F, 0.34F, 1.0F),
                ImVec4(0.42F, 0.10F, 0.13F, 1.0F),
                ImVec4(1.0F, 0.78F, 0.80F, 1.0F),
                ImVec4(0.30F, 0.05F, 0.08F, 0.32F)};
    case dock::ReceiveRowVisualKind::Warn:
        return {ImVec4(1.0F, 0.78F, 0.24F, 1.0F),
                ImVec4(0.43F, 0.32F, 0.08F, 1.0F),
                ImVec4(1.0F, 0.92F, 0.62F, 1.0F),
                ImVec4(0.28F, 0.21F, 0.05F, 0.28F)};
    case dock::ReceiveRowVisualKind::Event:
        return {ImVec4(0.66F, 0.48F, 1.0F, 1.0F),
                ImVec4(0.26F, 0.18F, 0.48F, 1.0F),
                ImVec4(0.86F, 0.78F, 1.0F, 1.0F),
                ImVec4(0.16F, 0.10F, 0.30F, 0.30F)};
    case dock::ReceiveRowVisualKind::ScriptLog:
        return {ImVec4(0.36F, 0.66F, 1.0F, 1.0F),
                ImVec4(0.10F, 0.25F, 0.50F, 1.0F),
                ImVec4(0.75F, 0.88F, 1.0F, 1.0F),
                ImVec4(0.06F, 0.15F, 0.30F, 0.28F)};
    case dock::ReceiveRowVisualKind::Debug:
        return {ImVec4(0.56F, 0.58F, 0.70F, 1.0F),
                ImVec4(0.20F, 0.21F, 0.28F, 1.0F),
                ImVec4(0.82F, 0.84F, 0.92F, 1.0F),
                ImVec4(0.12F, 0.13F, 0.18F, 0.26F)};
    case dock::ReceiveRowVisualKind::Info:
        return {ImVec4(0.30F, 0.70F, 1.0F, 1.0F),
                ImVec4(0.08F, 0.28F, 0.46F, 1.0F),
                ImVec4(0.72F, 0.88F, 1.0F, 1.0F),
                ImVec4(0.05F, 0.16F, 0.27F, 0.26F)};
    case dock::ReceiveRowVisualKind::Other:
    default:
        return {ImVec4(0.54F, 0.60F, 0.68F, 1.0F),
                ImVec4(0.20F, 0.24F, 0.30F, 1.0F),
                ImVec4(0.86F, 0.90F, 0.96F, 1.0F),
                ImVec4(0.13F, 0.15F, 0.18F, 0.24F)};
    }
}

void drawFilledBadge(ImDrawList* drawList,
                     const ImVec2& pos,
                     const std::string& text,
                     const LogRowPalette& palette,
                     float minWidth) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 textSize = ImGui::CalcTextSize(text.c_str());
    const float badgeHeight = ImGui::GetTextLineHeight() + style.FramePadding.y;
    const float badgeWidth = (std::max)(minWidth, textSize.x + style.FramePadding.x * 2.4F);
    drawList->AddRectFilled(pos,
                            ImVec2(pos.x + badgeWidth, pos.y + badgeHeight),
                            ImGui::ColorConvertFloat4ToU32(palette.badgeBackground),
                            badgeHeight * 0.45F);
    drawList->AddText(ImVec2(pos.x + (badgeWidth - textSize.x) * 0.5F,
                             pos.y + (badgeHeight - textSize.y) * 0.5F),
                      ImGui::ColorConvertFloat4ToU32(palette.badgeText),
                      text.c_str());
}

void drawModernLogRow(const dock::ReceiveRow& row,
                      bool showTimestamps,
                      bool showHex,
                      std::size_t index,
                      float endpointWidth) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const auto palette = paletteForRow(row);
    const std::string badge = row.direction.empty() ? "-" : row.direction;
    std::string content = dock::formatReceiveRowContent(row, showHex);
    if (content.empty()) {
        content = "（空）";
    }

    const std::string timestamp = showTimestamps ? formatShortLogTimestamp(row.timestampMs) : std::string{};
    const std::string copyLine = dock::formatReceiveRowSingleLine(row, showTimestamps, showHex);
    const ImVec2 contentSize = ImGui::CalcTextSize(content.c_str());
    const float rowHeight = ImGui::GetTextLineHeightWithSpacing() + style.FramePadding.y * 1.8F;
    const float leftPadding = style.FramePadding.x + 6.0F;
    const float badgeWidth = 66.0F;
    const float gap = style.ItemSpacing.x + 8.0F;
    const float timeWidth = showTimestamps ? ImGui::CalcTextSize("00:00:00.000").x + gap : 0.0F;
    const float endpointColumnWidth = endpointWidth + gap;
    const float contentX = leftPadding + badgeWidth + gap + timeWidth + endpointColumnWidth;
    const float rowWidth = (std::max)(ImGui::GetContentRegionAvail().x,
                                      contentX + contentSize.x + leftPadding * 2.0F);

    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
    const ImVec2 rowMax(rowMin.x + rowWidth, rowMin.y + rowHeight);
    ImGui::PushID(static_cast<int>(index));
    ImGui::InvisibleButton("##modern_log_row", ImVec2(rowWidth, rowHeight));
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::BeginPopupContextItem("##modern_log_row_menu")) {
        if (ImGui::MenuItem("复制本行")) {
            ImGui::SetClipboardText(copyLine.c_str());
        }
        if (ImGui::MenuItem("复制内容")) {
            ImGui::SetClipboardText(content.c_str());
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();

    auto* drawList = ImGui::GetWindowDrawList();
    const float rounding = style.FrameRounding + 3.0F;
    drawList->AddRectFilled(rowMin,
                            rowMax,
                            ImGui::ColorConvertFloat4ToU32(palette.rowBackground),
                            rounding);
    if (hovered) {
        drawList->AddRectFilled(rowMin,
                                rowMax,
                                ImGui::ColorConvertFloat4ToU32(ImVec4(1.0F, 1.0F, 1.0F, 0.06F)),
                                rounding);
    }
    drawList->AddRectFilled(rowMin,
                            ImVec2(rowMin.x + 3.0F, rowMax.y),
                            ImGui::ColorConvertFloat4ToU32(palette.accent),
                            rounding,
                            ImDrawFlags_RoundCornersLeft);

    const float textY = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5F;
    drawFilledBadge(drawList,
                    ImVec2(rowMin.x + leftPadding,
                           rowMin.y + (rowHeight - ImGui::GetTextLineHeight() - style.FramePadding.y) * 0.5F),
                    badge,
                    palette,
                    badgeWidth);

    float cursorX = rowMin.x + leftPadding + badgeWidth + gap;
    const ImU32 mutedText = ImGui::ColorConvertFloat4ToU32(ImVec4(0.68F, 0.72F, 0.78F, 1.0F));
    const ImU32 endpointText = ImGui::ColorConvertFloat4ToU32(ImVec4(0.82F, 0.86F, 0.92F, 1.0F));
    const ImU32 contentText = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyleColorVec4(ImGuiCol_Text));
    if (showTimestamps) {
        drawList->AddText(ImVec2(cursorX, textY), mutedText, timestamp.c_str());
        cursorX += timeWidth;
    }
    drawList->AddText(ImVec2(cursorX, textY), endpointText, row.endpoint.empty() ? "-" : row.endpoint.c_str());
    cursorX += endpointColumnWidth;

    const ImVec2 contentMin(cursorX - style.FramePadding.x, rowMin.y + style.FramePadding.y * 0.55F);
    const ImVec2 contentMax(rowMax.x - leftPadding, rowMax.y - style.FramePadding.y * 0.55F);
    drawList->AddRectFilled(contentMin,
                            contentMax,
                            ImGui::ColorConvertFloat4ToU32(ImVec4(0.0F, 0.0F, 0.0F, 0.12F)),
                            style.FrameRounding);
    drawList->AddText(ImVec2(cursorX, textY), contentText, content.c_str());
}

void drawModernLogRows(const char* childId,
                       const std::vector<const dock::ReceiveRow*>& rows,
                       bool showTimestamps,
                       bool showHex,
                       bool& pauseScroll,
                       const std::string& emptyText,
                       float endpointWidth) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 childSize(available.x, (std::max)(available.y, ImGui::GetTextLineHeightWithSpacing() * 4.0F));
    const ImGuiWindow* existingWindow = findMultilineChildWindow(childId);
    const bool stickToBottom = shouldStickMultilineChildToBottom(existingWindow);
    const float previousScrollY = existingWindow != nullptr ? existingWindow->Scroll.y : 0.0F;

    if (ImGui::BeginChild(childId, childSize, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiWindow* childWindow = ImGui::GetCurrentWindow();
        if (rows.empty()) {
            ImGui::TextDisabled("%s", emptyText.c_str());
        } else {
            const float rowHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().FramePadding.y * 1.8F;
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(rows.size()), rowHeight);
            while (clipper.Step()) {
                for (int rowIndex = clipper.DisplayStart; rowIndex < clipper.DisplayEnd; ++rowIndex) {
                    const auto* row = rows[static_cast<std::size_t>(rowIndex)];
                    if (row == nullptr) {
                        continue;
                    }
                    // 核心流程：日志历史可能很长，只绘制当前视口内的行，避免停流后每帧重画全部历史。
                    drawModernLogRow(*row, showTimestamps, showHex, static_cast<std::size_t>(rowIndex), endpointWidth);
                }
            }
        }

        // 核心流程：保留“停在底部才跟随”的行为，避免新数据打断用户查看历史记录。
        if (!pauseScroll && stickToBottom) {
            const bool userScrolledUpThisFrame =
                existingWindow != nullptr &&
                childWindow->Scroll.y < previousScrollY - 1.0F;
            if (!userScrolledUpThisFrame) {
                ImGui::SetScrollY(childWindow, childWindow->ScrollMax.y);
            }
        }
    }
    ImGui::EndChild();
}

std::size_t configuredSendHistoryLimit(const config::AppConfig& config) {
    constexpr std::size_t kMaxSendHistoryLimit = 200;
    return (std::min)(config.gui.sendHistoryLimit, kMaxSendHistoryLimit);
}

void restoreSendHistoryFromNode(const YAML::Node& sendNode, dock::SendDockState& sendState, std::size_t limit) {
    sendState.history.clear();
    if (limit == 0U || !sendNode || !sendNode["history"] || !sendNode["history"].IsSequence()) {
        return;
    }

    for (const auto& item : sendNode["history"]) {
        const auto payload = item.as<std::string>("");
        if (payload.empty()) {
            continue;
        }
        if (std::find(sendState.history.begin(), sendState.history.end(), payload) != sendState.history.end()) {
            continue;
        }
        sendState.history.push_back(payload);
        if (sendState.history.size() >= limit) {
            break;
        }
    }
}

void writeSendHistoryNode(YAML::Node& protocolNode, const dock::SendDockState& sendState, std::size_t limit) {
    YAML::Node historyNode(YAML::NodeType::Sequence);
    std::size_t written = 0;
    for (const auto& payload : sendState.history) {
        if (written >= limit) {
            break;
        }
        if (!payload.empty()) {
            historyNode.push_back(payload);
            ++written;
        }
    }
    protocolNode["send"]["history"] = historyNode;
}

void drawIconTooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) {
        ImGui::SetTooltip("%s", text);
    }
}

bool drawIconButton(const char* icon, const char* tooltip, const ImVec2& size = ImVec2(0.0F, 0.0F)) {
    const bool clicked = ImGui::Button(icon, size);
    drawIconTooltip(tooltip);
    return clicked;
}

bool drawIconCheckbox(const char* icon, bool* value, const char* tooltip) {
    ImGui::PushID(icon);
    const bool changed = ImGui::Checkbox("##icon_checkbox", value);
    ImGui::SameLine(0.0F, 0.0F);
    ImGui::TextUnformatted(icon);
    drawIconTooltip(tooltip);
    ImGui::PopID();
    return changed;
}

bool drawLogStatusFilterButton(const char* label, dock::LogStatusFilter value, dock::LogFilterState& filter) {
    const bool active = filter.status == value;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool clicked = ImGui::SmallButton(label);
    if (active) {
        ImGui::PopStyleColor();
    }
    if (clicked) {
        filter.status = value;
    }
    return clicked;
}

void drawLogKeywordFilterInput(const char* id, dock::LogFilterState& filter, float width) {
    char buffer[128]{};
    std::snprintf(buffer, sizeof(buffer), "%s", filter.keyword.c_str());
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(id, buffer, sizeof(buffer))) {
        filter.keyword = buffer;
    }
    drawIconTooltip("按关键字筛选 STATUS、端点、消息和收发内容");
}

void drawLogStatusFilterCombo(const char* id, dock::LogFilterState& filter) {
    struct StatusOption {
        dock::LogStatusFilter value;
        const char* label;
    };
    constexpr StatusOption options[] = {
        {dock::LogStatusFilter::All, "全部"},
        {dock::LogStatusFilter::Rx, "RX"},
        {dock::LogStatusFilter::Tx, "TX"},
        {dock::LogStatusFilter::Debug, "DEBUG"},
        {dock::LogStatusFilter::Info, "INFO"},
        {dock::LogStatusFilter::Warn, "WARN"},
        {dock::LogStatusFilter::Error, "ERROR"},
        {dock::LogStatusFilter::Event, "EVENT"},
        {dock::LogStatusFilter::ScriptLog, "LOG"},
        {dock::LogStatusFilter::Other, "Other"},
    };

    const char* currentLabel = "全部";
    for (const auto& option : options) {
        if (option.value == filter.status) {
            currentLabel = option.label;
            break;
        }
    }

    ImGui::SetNextItemWidth(96.0F);
    if (ImGui::BeginCombo(id, currentLabel)) {
        for (const auto& option : options) {
            const bool selected = filter.status == option.value;
            if (ImGui::Selectable(option.label, selected)) {
                filter.status = option.value;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    drawIconTooltip("按日志 STATUS 筛选");
}

bool drawHorizontalSplitter(const char* id, float& topHeight, float minTopHeight, float minBottomHeight, float totalHeight, float thickness) {
    const float safeThickness = (std::max)(thickness, 4.0F);
    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_Separator));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorHovered));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyleColorVec4(ImGuiCol_SeparatorActive));
    ImGui::Button(id, ImVec2(-1.0F, safeThickness));
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemActive()) {
        topHeight += ImGui::GetIO().MouseDelta.y;
        topHeight = (std::clamp)(topHeight, minTopHeight, (std::max)(minTopHeight, totalHeight - minBottomHeight - safeThickness));
        return true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    return false;
}

void drawTransferLogRows(const char* childId,
                         const std::vector<const dock::ReceiveRow*>& rows,
                         bool showTimestamps,
                         bool showHex,
                         bool& pauseScroll,
                         const std::string& emptyText,
                         float endpointWidth) {
    drawModernLogRows(childId, rows, showTimestamps, showHex, pauseScroll, emptyText, endpointWidth);
}

void drawRowList(const char* childId,
                 const std::vector<const dock::ReceiveRow*>& rows,
                 bool showTimestamps,
                 bool showHex,
                 bool& pauseScroll,
                 const std::string& emptyText,
                 float endpointWidth) {
    drawModernLogRows(childId, rows, showTimestamps, showHex, pauseScroll, emptyText, endpointWidth);
}

bool digitsOnly(const std::string& text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char ch) {
        return std::isdigit(ch) != 0;
    });
}

void refreshProtocolRoot(config::ConfigStore const& configStore,
                         dock::LuaDockState& lua,
                         std::string& protocolDirDraft,
                         std::string& protocolDirDraftModel) {
    // 核心流程：根目录变化后立即重扫并校正当前协议目录，避免下拉框保留失效路径。
    lua.protocolDirOptions = configStore.scanProtocolDirectories(lua.protocolRootDir);
    const auto correctedDir = configStore.normalizeProtocolDir(lua.protocolRootDir, lua.protocolDir);
    lua.protocolDir = correctedDir.generic_string();
    protocolDirDraft = lua.protocolDir;
    protocolDirDraftModel = lua.protocolDir;
}

std::string normalizeProtocolDraft(const config::ConfigStore& configStore,
                                   const std::string& protocolRootDir,
                                   const std::string& protocolDir) {
    return configStore.normalizeProtocolDir(protocolRootDir, protocolDir).generic_string();
}

#if defined(_WIN32)
int CALLBACK browseInitialDirCallback(HWND hwnd, UINT message, LPARAM, LPARAM data) {
    if (message == BFFM_INITIALIZED && data != 0) {
        SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    }
    return 0;
}

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}

std::wstring fileDialogFilterText(const std::vector<scripting::FileDialogFilter>& filters) {
    if (filters.empty()) {
        return std::wstring(L"All Files\0*.*\0\0", 15);
    }

    std::wstring text;
    for (const auto& filter : filters) {
        text += utf8ToWide(filter.name.empty() ? "Files" : filter.name);
        text.push_back(L'\0');
        if (filter.patterns.empty()) {
            text += L"*.*";
        } else {
            for (std::size_t index = 0; index < filter.patterns.size(); ++index) {
                if (index > 0) {
                    text.push_back(L';');
                }
                text += utf8ToWide(filter.patterns[index]);
            }
        }
        text.push_back(L'\0');
    }
    text.push_back(L'\0');
    return text;
}
#endif

scripting::FileDialogEvent runLuaFileDialog(GLFWwindow* window, const scripting::FileDialogRequest& request) {
    scripting::FileDialogEvent event{};
    event.id = request.id;
    event.kind = request.kind;
    event.timestampMs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());

#if defined(_WIN32)
    if (request.kind == scripting::FileDialogKind::OpenDir) {
        std::string error;
        const auto title = utf8ToWide(request.title.empty() ? "选择目录" : request.title);
        const auto selected = nativeDirectoryDialog(window,
                                                    title.c_str(),
                                                    request.defaultPath.empty() ? std::filesystem::path{} : std::filesystem::path(request.defaultPath),
                                                    error);
        if (selected.has_value()) {
            event.state = "selected";
            event.path = selected->generic_string();
        } else if (!error.empty()) {
            event.state = "error";
            event.error = error;
        } else {
            event.state = "canceled";
        }
        return event;
    }

    std::string error;
    const auto title = utf8ToWide(request.title.empty() ? "选择文件" : request.title);
    const auto filters = fileDialogFilterText(request.filters);
    const auto selected = nativeFileDialog(window,
                                           title.c_str(),
                                           filters.c_str(),
                                           request.defaultPath.empty() ? std::filesystem::path{} : std::filesystem::path(request.defaultPath),
                                           request.kind == scripting::FileDialogKind::SaveFile,
                                           nullptr,
                                           error);
    if (selected.has_value()) {
        event.state = "selected";
        event.path = selected->generic_string();
    } else if (!error.empty()) {
        event.state = "error";
        event.error = error;
    } else {
        event.state = "canceled";
    }
    return event;
#else
    (void)window;
    event.state = "error";
    event.error = "当前平台尚未实现原生文件对话框";
    return event;
#endif
}

void syncDraftFromModel(std::string& draft, std::string& lastModel, const std::string& model) {
    if (draft.empty() || lastModel != model) {
        draft = model;
        lastModel = model;
    }
}

void refreshSerialPortOptions(dock::CommDockState& comm) {
    auto ports = transport::listAvailableSerialPorts();
    if (!comm.serial.portName.empty() && std::find(ports.begin(), ports.end(), comm.serial.portName) == ports.end()) {
        ports.push_back(comm.serial.portName);
        ports = transport::normalizeSerialPortNames(std::move(ports));
    }
    if (!ports.empty()) {
        comm.serialPortOptions = std::move(ports);
    }
}

} // namespace

GuiRuntime::GuiRuntime(app::Application& application, const config::ConfigStore& configStore)
    : application_(application),
      configStore_(configStore),
      executableDir_(executableDirectory()),
      waveDockRenderer_(application) {}

GuiRuntime::~GuiRuntime() {
    shutdown();
}

const GuiRuntime::FilteredLogRowsCache& GuiRuntime::filteredLogRowsCached(
    FilteredLogRowsCache& cache,
    const std::vector<dock::ReceiveRow>& rows,
    std::uint64_t version,
    const dock::LogFilterState& filter,
    bool includeBytePreview) {
    if (cache.source != &rows ||
        cache.version != version ||
        cache.filter.keyword != filter.keyword ||
        cache.filter.status != filter.status ||
        cache.includeBytePreview != includeBytePreview) {
        cache.source = &rows;
        cache.version = version;
        cache.filter = filter;
        cache.includeBytePreview = includeBytePreview;
        cache.rows = dock::filteredLogRows(rows, filter, includeBytePreview);
        float endpointWidth = ImGui::CalcTextSize("endpoint").x;
        for (const auto* row : cache.rows) {
            if (row == nullptr) {
                continue;
            }
            endpointWidth = (std::max)(endpointWidth,
                                       ImGui::CalcTextSize(row->endpoint.empty() ? "-" : row->endpoint.c_str()).x);
        }
        cache.endpointWidth = (std::clamp)(endpointWidth, 86.0F, 220.0F);
    }
    return cache;
}

bool GuiRuntime::initialize() {
    if (!initializeWindow()) {
        return false;
    }
    if (!initializeImGui()) {
        return false;
    }
    if (!initializePlotContext()) {
        return false;
    }
    loadCurrentProtocolWorkspace();
    configSnapshot_ = configStore_.snapshot(configStore_.defaultConfigPath());
    running_ = true;
    return true;
}

int GuiRuntime::run() {
    while (running_ && window_ && !glfwWindowShouldClose(window_)) {
        const auto frameStartMs = nowMs();
        glfwPollEvents();

        bool changed = application_.pumpOnce();
        changed = pollConfigFileChanges() || changed;
        changed = maybeAutoSave() || changed;

        if (!changed && lastRenderAtMs_ != 0) {
            const auto nextWakeup = application_.nextWakeupAtMs();
            // 空闲且没有脚本定时器/半双工请求时也要限帧休眠，避免停止传输后 GUI 主循环忙转。
            if (!nextWakeup.has_value() || *nextWakeup > frameStartMs) {
                sleepUntilNextFrame(frameStartMs);
            }
        }

        processPendingProtocolWorkspaceSwitch();
        renderFrame();
        glfwSwapBuffers(window_);
        lastRenderAtMs_ = frameStartMs;
        if (ImGui::GetIO().WantSaveIniSettings && workspaceLayoutMode_ == WorkspaceLayoutMode::Ready) {
            saveCurrentProtocolWorkspace();
        } else if (pendingProtocolWorkspaceSave_ && workspaceLayoutMode_ == WorkspaceLayoutMode::Ready) {
            saveCurrentProtocolWorkspace();
        }
    }
    return 0;
}

void GuiRuntime::shutdown() {
    if (!window_) {
        return;
    }
    running_ = false;
    saveCurrentProtocolWorkspace();
    shutdownImGui();
    shutdownPlotContext();
    shutdownWindow();
}

bool GuiRuntime::initializeWindow() {
    if (!glfwInit()) {
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    const auto& window = application_.captureConfig().gui.window;
    window_ = glfwCreateWindow(window.width, window.height, window.title.c_str(), nullptr, nullptr);
    if (!window_) {
        // 核心流程：优先使用 3.2 Core 以启用 ImGui 顶点偏移能力；失败时回退 3.0，兼容老显卡/驱动。
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        kGlslVersion = "#version 130";
        window_ = glfwCreateWindow(window.width, window.height, window.title.c_str(), nullptr, nullptr);
    }
    if (!window_) {
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);
    if (window.maximized) {
        glfwMaximizeWindow(window_);
    }
    refreshWindowTitle();
    return true;
}

bool GuiRuntime::initializeImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.IniFilename = nullptr;
    ensureChineseFont();

    if (!ImGui_ImplGlfw_InitForOpenGL(window_, true)) {
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(kGlslVersion)) {
        return false;
    }
    return true;
}

bool GuiRuntime::initializePlotContext() {
    ImPlot::CreateContext();
    ImPlot::StyleColorsDark();
    auto& inputMap = ImPlot::GetInputMap();
    inputMap.Pan = ImGuiMouseButton_Left;
    inputMap.Select = ImGuiMouseButton_Right;
    inputMap.SelectCancel = ImGuiMouseButton_Left;
    return true;
}

void GuiRuntime::shutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void GuiRuntime::shutdownPlotContext() {
    ImPlot::DestroyContext();
}

void GuiRuntime::shutdownWindow() {
    glfwDestroyWindow(window_);
    window_ = nullptr;
    glfwTerminate();
}

void GuiRuntime::ensureChineseFont() {
    ImGuiIO& io = ImGui::GetIO();
    for (const auto& candidate : candidateChineseFonts()) {
        if (std::filesystem::exists(candidate)) {
            io.Fonts->AddFontFromFileTTF(candidate.string().c_str(), 18.0F, nullptr, io.Fonts->GetGlyphRangesChineseFull());
            break;
        }
    }
    try {
        const auto resourceFs = cmrc::ui_resources::get_filesystem();
        const auto iconFont = resourceFs.open("assets/fonts/fa-solid-900.ttf");
        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.FontDataOwnedByAtlas = false;
        iconConfig.GlyphMinAdvanceX = 18.0F;
        static constexpr ImWchar kIconRanges[] = {0xf000, 0xf8ff, 0};
        io.Fonts->AddFontFromMemoryTTF(const_cast<char*>(iconFont.begin()),
                                       static_cast<int>(iconFont.size()),
                                       16.0F,
                                       &iconConfig,
                                       kIconRanges);
    } catch (const std::exception&) {
        // 图标字体是体验增强资源，缺失时保留中文 UI 可用，不阻断主窗口启动。
    }
}

void GuiRuntime::renderFrame() {
    refreshWindowTitle();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    drawMainMenu();
    drawAboutDialog();
    drawUpdateCheckDialog();

    ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    if (workspaceLayoutMode_ == WorkspaceLayoutMode::NeedsDefaultBuild) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID mainArea = dockspaceId;
        ImGuiID left = ImGui::DockBuilderSplitNode(mainArea, ImGuiDir_Left, 0.25f, nullptr, &mainArea);
        ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.48F, nullptr, &left);
        ImGuiID rightTop = ImGui::DockBuilderSplitNode(mainArea, ImGuiDir_Up, 0.25f, nullptr, &mainArea);
        ImGuiID rightMid = ImGui::DockBuilderSplitNode(mainArea, ImGuiDir_Up, 0.36F, nullptr, &mainArea);
        const ImGuiID mainBottom = mainArea;

        defaultLuaDockNodes_.clear();
        defaultLuaDockNodes_[LuaDockAnchor::Left] = left;
        defaultLuaDockNodes_[LuaDockAnchor::LeftBottom] = leftBottom;
        defaultLuaDockNodes_[LuaDockAnchor::RightTop] = rightTop;
        defaultLuaDockNodes_[LuaDockAnchor::RightMid] = rightMid;
        defaultLuaDockNodes_[LuaDockAnchor::RightBottom] = mainBottom;
        defaultLuaDockNodes_[LuaDockAnchor::MainBottom] = mainBottom;

        // 核心流程：先切左栏，再从主区向上切辅助区，让 DockBuilder 自然保留唯一的 CentralNode。
        ImGui::DockBuilderDockWindow("通讯配置", left);
        ImGui::DockBuilderDockWindow("协议脚本 / 动态控件", leftBottom);
        ImGui::DockBuilderDockWindow("收发数据", rightTop);
        ImGui::DockBuilderDockWindow("日志", mainBottom);
        ImGui::DockBuilderDockWindow("脚本", mainBottom);
        ImGui::DockBuilderDockWindow("波形", mainBottom);
        ImGui::DockBuilderFinish(dockspaceId);
        pendingLuaDefaultDockLayout_ = true;
        if (shouldRunLuaDefaultDockLayout(workspaceLayoutMode_, pendingLuaDefaultDockLayout_)) {
            // 核心流程：Lua 动态 Dock 的默认停靠只属于默认布局事务，避免用户拖拽后被下一帧拉回。
            updateLuaDockDefaultLayout();
            pendingLuaDefaultDockLayout_ = false;
        }
        workspaceLayoutMode_ = WorkspaceLayoutMode::Ready;
    }

    const bool previousShowCommDock = showCommDock_;
    const bool previousShowProtocolDock = showProtocolDock_;
    const bool previousShowTransferDock = showTransferDock_;
    const bool previousShowLogDock = showLogDock_;
    const bool previousShowScriptDock = showScriptDock_;
    const bool previousShowWaveDock = showWaveDock_;

    drawStatusBar();
    drawCommDock();
    drawProtocolDock();
    drawLuaDockWindows();
    drawTransferDock();
    drawLogDock();
    drawScriptDock();
    waveDockRenderer_.draw(showWaveDock_);
    drawDialogs();
    drawRawCaptureFileDialogs();
    drawLogExportFileDialog();
    drawElfStaticAddressDialog();
    if (previousShowCommDock != showCommDock_
        || previousShowProtocolDock != showProtocolDock_
        || previousShowTransferDock != showTransferDock_
        || previousShowLogDock != showLogDock_
        || previousShowScriptDock != showScriptDock_
        || previousShowWaveDock != showWaveDock_) {
        pendingProtocolWorkspaceSave_ = true;
    }

    ImGui::Render();

    int displayW = 0;
    int displayH = 0;
    glfwGetFramebufferSize(window_, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.10F, 0.11F, 0.12F, 1.00F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GuiRuntime::refreshWindowTitle() {
    if (!window_) {
        return;
    }

    const auto title = std::string("ProtoScope ") + build::kVersion + " - " + currentProtocolTitle(application_.docks().luaState());
    if (title == lastWindowTitle_) {
        return;
    }

    lastWindowTitle_ = title;
    glfwSetWindowTitle(window_, lastWindowTitle_.c_str());
}

} // namespace protoscope::ui

#define PROTOSCOPE_GUI_RUNTIME_COMPONENT_INCLUDE
#include "../dialogs/dialog_host.cpp"
#include "../docks/dock_host.cpp"
#include "../docks/lua_dock_host.cpp"
#include "workspace_controller.cpp"
#include "config_controller.cpp"
#include "platform_controller.cpp"
#undef PROTOSCOPE_GUI_RUNTIME_COMPONENT_INCLUDE
