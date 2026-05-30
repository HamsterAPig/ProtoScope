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

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <GL/gl.h>
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

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

std::vector<std::filesystem::path> candidateIconFonts() {
    return {
        "assets/fonts/fa-solid-900.ttf",
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

std::string buildRowListText(const std::vector<dock::ReceiveRow>& rows, bool showTimestamps, bool showHex) {
    std::string text;
    text.reserve(rows.size() * 64);
    for (const auto& row : rows) {
        if (!text.empty()) {
            text.push_back('\n');
        }
        text.append(dock::formatReceiveRowSingleLine(row, showTimestamps, showHex));
    }
    return text;
}

std::string formatShortTimestampText(std::uint64_t timestampMs) {
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

std::string bytesToAsciiPreview(const std::vector<std::uint8_t>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto byte : bytes) {
        const char ch = static_cast<char>(byte);
        text.push_back(std::isprint(static_cast<unsigned char>(ch)) ? ch : '.');
    }
    return text;
}

std::string flattenTransferText(std::string_view text) {
    std::string flattened;
    flattened.reserve(text.size());
    for (const char ch : text) {
        flattened.push_back(ch == '\r' || ch == '\n' || ch == '\t' ? ' ' : ch);
    }
    return flattened;
}

std::string transferRowContent(const dock::ReceiveRow& row, bool showHex) {
    if (!row.message.empty()) {
        return flattenTransferText(row.message);
    }
    if (row.bytes.empty()) {
        return {};
    }
    return showHex ? protocol_utils::bytesToHex(row.bytes, true) : bytesToAsciiPreview(row.bytes);
}

bool matchesTransferFilter(const dock::ReceiveRow& row, dock::TransferLogFilter filter) {
    switch (filter) {
    case dock::TransferLogFilter::Rx:
        return row.direction == "RX";
    case dock::TransferLogFilter::Tx:
        return row.direction == "TX";
    case dock::TransferLogFilter::All:
    default:
        return true;
    }
}

std::vector<const dock::ReceiveRow*> filteredTransferRows(const std::vector<dock::ReceiveRow>& rows, dock::TransferLogFilter filter) {
    std::vector<const dock::ReceiveRow*> filtered;
    filtered.reserve(rows.size());
    for (const auto& row : rows) {
        if (matchesTransferFilter(row, filter)) {
            filtered.push_back(&row);
        }
    }
    return filtered;
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

bool drawTransferLogFilterButton(const char* label, dock::TransferLogFilter value, dock::TransferLogFilter& filter) {
    const bool active = filter == value;
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    }
    const bool clicked = ImGui::SmallButton(label);
    if (active) {
        ImGui::PopStyleColor();
    }
    if (clicked) {
        filter = value;
    }
    return clicked;
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
                         const std::string& emptyText) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 childSize(available.x, (std::max)(available.y, ImGui::GetTextLineHeightWithSpacing() * 4.0F));
    if (ImGui::BeginChild(childId, childSize, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
        ImGuiWindow* childWindow = ImGui::GetCurrentWindow();
        const bool stickToBottom = childWindow->Scroll.y >= childWindow->ScrollMax.y - 4.0F;
        if (rows.empty()) {
            ImGui::TextDisabled("%s", emptyText.c_str());
        } else if (ImGui::BeginTable("##transfer_log_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable)) {
            ImGui::TableSetupColumn("方向", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize(" TX ").x + 18.0F);
            ImGui::TableSetupColumn("时间", ImGuiTableColumnFlags_WidthFixed, showTimestamps ? 92.0F : 1.0F);
            ImGui::TableSetupColumn("端点", ImGuiTableColumnFlags_WidthFixed, 130.0F);
            ImGui::TableSetupColumn("内容", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (const auto* row : rows) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                const bool isTx = row->direction == "TX";
                const ImVec4 color = isTx ? ImVec4(0.95F, 0.68F, 0.22F, 1.0F) : ImVec4(0.38F, 0.82F, 0.52F, 1.0F);
                ImGui::TextColored(color, "%s", row->direction.empty() ? "-" : row->direction.c_str());

                ImGui::TableSetColumnIndex(1);
                if (showTimestamps) {
                    const auto timestamp = formatShortTimestampText(row->timestampMs);
                    ImGui::TextUnformatted(timestamp.c_str());
                }

                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(row->endpoint.c_str());

                ImGui::TableSetColumnIndex(3);
                const auto content = transferRowContent(*row, showHex);
                ImGui::TextUnformatted(content.c_str());
            }
            ImGui::EndTable();
        }

        // 核心流程：记录区只在用户停留底部时自动跟随，避免查看历史 TX/RX 时被滚动打断。
        if (!pauseScroll && stickToBottom) {
            ImGui::SetScrollY(childWindow, childWindow->ScrollMax.y);
        }
    }
    ImGui::EndChild();
}

void drawRowList(const char* childId,
                 const std::vector<dock::ReceiveRow>& rows,
                 bool showTimestamps,
                 bool showHex,
                 bool& pauseScroll,
                 const std::string& emptyText) {
    const ImVec2 available = ImGui::GetContentRegionAvail();
    const ImVec2 textBoxSize(available.x, (std::max)(available.y, ImGui::GetTextLineHeightWithSpacing() * 4.0F));
    if (rows.empty()) {
        if (ImGui::BeginChild(childId, textBoxSize, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::TextDisabled("%s", emptyText.c_str());
        }
        ImGui::EndChild();
        return;
    }

    constexpr auto kTextBoxLabel = "##row_list_text";
    const ImGuiWindow* existingWindow = findMultilineChildWindow(kTextBoxLabel);
    const bool stickToBottom = existingWindow == nullptr || existingWindow->Scroll.y >= existingWindow->ScrollMax.y - 4.0F;

    auto text = buildRowListText(rows, showTimestamps, showHex);
    std::vector<char> buffer(text.begin(), text.end());
    buffer.push_back('\0');

    // 核心流程：统一改为只读多行文本框，保持每条记录单行显示，同时支持拖选复制与横向滚动。
    ImGui::InputTextMultiline(kTextBoxLabel,
                              buffer.data(),
                              buffer.size(),
                              textBoxSize,
                              ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_NoUndoRedo);

    // 核心流程：仅在用户原本停留在底部时自动滚到底，避免查看历史记录时被打断。
    if (!pauseScroll && stickToBottom) {
        if (ImGuiWindow* childWindow = findMultilineChildWindow(kTextBoxLabel)) {
            ImGui::SetScrollY(childWindow, childWindow->ScrollMax.y);
        }
    }
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
    for (const auto& candidate : candidateIconFonts()) {
        if (!std::filesystem::exists(candidate)) {
            continue;
        }
        ImFontConfig iconConfig;
        iconConfig.MergeMode = true;
        iconConfig.PixelSnapH = true;
        iconConfig.GlyphMinAdvanceX = 18.0F;
        static constexpr ImWchar kIconRanges[] = {0xf000, 0xf8ff, 0};
        io.Fonts->AddFontFromFileTTF(candidate.string().c_str(), 16.0F, &iconConfig, kIconRanges);
        break;
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
    drawElfStaticAddressDialog();

    ImGui::Render();

    int displayW = 0;
    int displayH = 0;
    glfwGetFramebufferSize(window_, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.10F, 0.11F, 0.12F, 1.00F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

void GuiRuntime::drawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu("文件")) {
        if (ImGui::MenuItem("保存配置")) {
            std::string error;
            const auto path = std::filesystem::path(application_.docks().configState().loadedFromPath);
            if (configStore_.save(path, application_.captureConfig(), error)) {
                application_.docks().clearDirty("配置已保存");
                configSnapshot_ = configStore_.snapshot(path);
                application_.docks().configState().fileTimestampMs = configSnapshot_.timestampMs;
            } else {
                application_.setStatusMessage("保存配置失败: " + error, true);
            }
        }
        if (ImGui::MenuItem("重新加载配置")) {
            if (!reloadConfigFromDisk()) {
                application_.setStatusMessage("从磁盘重载配置失败", true);
            }
        }
        if (ImGui::MenuItem("重新加载协议")) {
            requestProtocolWorkspaceSwitch(application_.docks().luaState().protocolDir, true);
        }
        if (ImGui::MenuItem("打开 ELF/JSON...")) {
            openElfStaticAddressDialog();
        }
        if (ImGui::MenuItem("导入原始波形...")) {
            openRawCaptureImportDialog();
        }
        if (ImGui::MenuItem("导出原始波形...")) {
            openRawCaptureExportDialog();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("视图")) {
        ImGui::MenuItem("通讯配置", nullptr, &showCommDock_);
        ImGui::MenuItem("协议脚本 / 动态控件", nullptr, &showProtocolDock_);
        ImGui::MenuItem("收发数据", nullptr, &showTransferDock_);
        ImGui::MenuItem("日志", nullptr, &showLogDock_);
        ImGui::MenuItem("脚本", nullptr, &showScriptDock_);
        ImGui::MenuItem("波形", nullptr, &showWaveDock_);
        ImGui::Separator();
        if (ImGui::MenuItem(
                "重置当前协议 Dock 布局",
                nullptr,
                false,
                canResetProtocolWorkspaceLayout(protocolWorkspaceLoaded_, activeWorkspaceProtocolKey_))) {
            resetCurrentProtocolWorkspaceLayout();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("帮助")) {
        if (ImGui::MenuItem("检查更新")) {
            startUpdateCheck();
        }
        if (ImGui::MenuItem("关于 ProtoScope")) {
            requestAboutDialog();
        }
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
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

void GuiRuntime::requestAboutDialog() {
    aboutDialogRequested_ = true;
}

void GuiRuntime::drawAboutDialog() {
    constexpr const char* popupId = "关于 ProtoScope";
    if (aboutDialogRequested_) {
        ImGui::OpenPopup(popupId);
        aboutDialogRequested_ = false;
    }

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal(popupId, nullptr, flags)) {
        return;
    }

    const auto& lua = application_.docks().luaState();
    ImGui::TextUnformatted("ProtoScope");
    ImGui::Separator();
    ImGui::Text("版本: %s", build::kVersion);
    ImGui::Text("当前协议: %s", currentProtocolTitle(lua).c_str());
    ImGui::Text("项目地址: %s", build::kProjectUrl);
    ImGui::Text("作者: %s", build::kAuthor);
    ImGui::Text("邮箱: %s", build::kAuthorEmail);
    ImGui::Spacing();

    if (ImGui::Button("复制项目地址")) {
        ImGui::SetClipboardText(build::kProjectUrl);
        application_.setStatusMessage("项目地址已复制", false);
    }
    ImGui::SameLine();
    if (ImGui::Button("打开项目地址")) {
#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", build::kProjectUrl, nullptr, nullptr, SW_SHOWNORMAL);
#else
        application_.setStatusMessage("当前平台暂未实现打开外部链接", true);
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭")) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void GuiRuntime::startUpdateCheck() {
    updateCheckDialogRequested_ = true;
    if (updateCheckInProgress_) {
        return;
    }

    updateCheckResult_.reset();
    updateCheckInProgress_ = true;
    updateCheckFuture_ = std::async(std::launch::async, [] {
        return checkForUpdates();
    });
}

void GuiRuntime::drawUpdateCheckDialog() {
    constexpr const char* popupId = "检查更新";
    if (updateCheckDialogRequested_) {
        ImGui::OpenPopup(popupId);
        updateCheckDialogRequested_ = false;
    }

    if (updateCheckInProgress_ && updateCheckFuture_.valid()
        && updateCheckFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
        updateCheckResult_ = updateCheckFuture_.get();
        updateCheckInProgress_ = false;
    }

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal(popupId, nullptr, flags)) {
        return;
    }

    ImGui::Text("当前版本: %s", build::kVersion);
    if (!std::string(build::kBaseTag).empty()) {
        ImGui::Text("版本基准: %s", build::kBaseTag);
    }
    ImGui::Separator();

    if (updateCheckInProgress_) {
        ImGui::TextUnformatted("正在连接 GitHub 检查更新...");
    } else if (updateCheckResult_.has_value()) {
        ImGui::TextUnformatted(updateCheckResult_->title.c_str());
        ImGui::TextWrapped("%s", updateCheckResult_->message.c_str());
        if (!updateCheckResult_->latestTag.empty()) {
            ImGui::Text("远端版本: %s", updateCheckResult_->latestTag.c_str());
        }
    } else {
        ImGui::TextUnformatted("尚未开始检查。");
    }

    ImGui::Spacing();
    if (!updateCheckInProgress_ && ImGui::Button("重新检查")) {
        startUpdateCheck();
    }
    if (!updateCheckInProgress_) {
        ImGui::SameLine();
    }
    if (ImGui::Button("打开项目地址")) {
#if defined(_WIN32)
        ShellExecuteA(nullptr, "open", build::kProjectUrl, nullptr, nullptr, SW_SHOWNORMAL);
#else
        application_.setStatusMessage("当前平台暂未实现打开外部链接", true);
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("关闭")) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void GuiRuntime::drawStatusBar() {
    auto& comm = application_.docks().commState();
    auto& config = application_.docks().configState();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - 28.0F));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 28.0F));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("状态栏", nullptr, flags)) {
        ImGui::Text("状态: %s", transportStateLabel(comm.state));
        ImGui::SameLine();
        ImGui::Separator();
        ImGui::SameLine();
        if (config.dirty) {
            ImGui::TextUnformatted("未保存");
            ImGui::SameLine();
            ImGui::Separator();
            ImGui::SameLine();
        }
        if (config.pendingExternalReload) {
            ImGui::TextUnformatted(config.externalReloadMessage.empty() ? "检测到外部配置更新" : config.externalReloadMessage.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("重载配置")) {
                if (!reloadConfigFromDisk()) {
                    application_.setStatusMessage("从磁盘重载配置失败", true);
                }
            }
            ImGui::SameLine();
            ImGui::Separator();
            ImGui::SameLine();
        }
        if (comm.reconnectRequired) {
            ImGui::TextUnformatted("通讯参数变更待重连");
            ImGui::SameLine();
            ImGui::Separator();
            ImGui::SameLine();
        }
        if (!config.statusMessage.empty()) {
            ImGui::TextUnformatted(config.statusMessage.c_str());
        }
    }
    ImGui::End();
}

void GuiRuntime::drawCommDock() {
    if (!showCommDock_) {
        return;
    }

    auto& comm = application_.docks().commState();
    auto& configState = application_.docks().configState();

    if (!ImGui::Begin("通讯配置", &showCommDock_)) {
        ImGui::End();
        return;
    }

    const auto& transportItems = transport::transportDescriptors();
    int kindIndex = 0;
    for (std::size_t index = 0; index < transportItems.size(); ++index) {
        if (transportItems[index].kind == comm.kind) {
            kindIndex = static_cast<int>(index);
            break;
        }
    }
    std::vector<const char*> itemLabels;
    itemLabels.reserve(transportItems.size());
    for (const auto& item : transportItems) {
        itemLabels.push_back(item.label.data());
    }
    if (ImGui::Combo("模式", &kindIndex, itemLabels.data(), static_cast<int>(itemLabels.size()))) {
        comm.kind = transportItems[static_cast<std::size_t>(kindIndex)].kind;
        application_.markCommConfigEdited(true);
    }

    if (comm.kind == transport::TransportKind::TcpClient) {
        char host[256]{};
        std::snprintf(host, sizeof(host), "%s", comm.tcpClient.host.c_str());
        if (ImGui::InputText("主机", host, sizeof(host))) {
            comm.tcpClient.host = host;
            application_.markCommConfigEdited(true);
        }
        int port = comm.tcpClient.port;
        if (ImGui::InputInt("端口", &port)) {
            comm.tcpClient.port = static_cast<std::uint16_t>(std::clamp(port, 0, 65535));
            application_.markCommConfigEdited(true);
        }
    } else if (comm.kind == transport::TransportKind::TcpServer) {
        char bindAddress[256]{};
        std::snprintf(bindAddress, sizeof(bindAddress), "%s", comm.tcpServer.bindAddress.c_str());
        if (ImGui::InputText("监听地址", bindAddress, sizeof(bindAddress))) {
            comm.tcpServer.bindAddress = bindAddress;
            application_.markCommConfigEdited(true);
        }
        int port = comm.tcpServer.port;
        if (ImGui::InputInt("监听端口", &port)) {
            comm.tcpServer.port = static_cast<std::uint16_t>(std::clamp(port, 0, 65535));
            application_.markCommConfigEdited(true);
        }
        if (ImGui::Checkbox("拒绝新连接", &comm.tcpServer.rejectNewConnection)) {
            application_.markCommConfigEdited(true);
        }
    } else if (comm.kind == transport::TransportKind::Serial) {
        if (!serialPortsScanned_) {
            refreshSerialPortOptions(comm);
            serialPortsScanned_ = true;
        }
        if (ImGui::Button("刷新串口列表")) {
            refreshSerialPortOptions(comm);
        }

        syncDraftFromModel(serialPortDraft_, serialPortDraftModel_, comm.serial.portName);
        if (const auto portEdit = drawEditableCombo("端口", serialPortDraft_, comm.serialPortOptions); portEdit.edited && portEdit.value != comm.serial.portName) {
            comm.serial.portName = portEdit.value;
            serialPortDraftModel_ = comm.serial.portName;
            application_.markCommConfigEdited(true);
        }

        std::vector<std::string> baudRateOptions;
        for (std::size_t i = 0; i < std::size(kCommonBaudRates); ++i) {
            baudRateOptions.push_back(std::to_string(kCommonBaudRates[i]));
        }

        const std::string currentBaudText = std::to_string(comm.serial.baudRate);
        syncDraftFromModel(commonBaudRateDraft_, commonBaudRateDraftModel_, currentBaudText);
        if (const auto baudEdit = drawEditableCombo("波特率", commonBaudRateDraft_, baudRateOptions, digitsOnly); baudEdit.edited) {
            if (!baudEdit.valid) {
                application_.setStatusMessage("波特率仅接受纯数字");
            } else {
                const auto baudRate = static_cast<std::uint32_t>(std::stoul(baudEdit.value));
                if (baudRate != comm.serial.baudRate) {
                    comm.serial.baudRate = baudRate;
                    commonBaudRateDraftModel_ = baudEdit.value;
                    application_.markCommConfigEdited(true);
                }
            }
        }
        if (!digitsOnly(commonBaudRateDraft_)) {
            ImGui::TextDisabled("波特率仅接受纯数字");
        }

        int dataBits = static_cast<int>(comm.serial.dataBits);
        if (ImGui::InputInt("数据位", &dataBits)) {
            comm.serial.dataBits = static_cast<std::uint32_t>(std::clamp(dataBits, 5, 8));
            application_.markCommConfigEdited(true);
        }

        const char* parityItems[] = {"none", "odd", "even"};
        int parityIndex = comm.serial.parity == "odd" ? 1 : (comm.serial.parity == "even" ? 2 : 0);
        if (ImGui::Combo("奇偶校验", &parityIndex, parityItems, IM_ARRAYSIZE(parityItems))) {
            comm.serial.parity = parityItems[parityIndex];
            application_.markCommConfigEdited(true);
        }

        const char* stopBitItems[] = {"one", "one_point_five", "two"};
        int stopBitIndex = comm.serial.stopBits == "two" ? 2 : (comm.serial.stopBits == "one_point_five" ? 1 : 0);
        if (ImGui::Combo("停止位", &stopBitIndex, stopBitItems, IM_ARRAYSIZE(stopBitItems))) {
            comm.serial.stopBits = stopBitItems[stopBitIndex];
            application_.markCommConfigEdited(true);
        }

        const char* flowItems[] = {"none", "software", "hardware"};
        int flowIndex = comm.serial.flowControl == "software" ? 1 : (comm.serial.flowControl == "hardware" ? 2 : 0);
        if (ImGui::Combo("流控", &flowIndex, flowItems, IM_ARRAYSIZE(flowItems))) {
            comm.serial.flowControl = flowItems[flowIndex];
            application_.markCommConfigEdited(true);
        }
    } else if (comm.kind == transport::TransportKind::UdpPeer) {
        char bindAddress[256]{};
        std::snprintf(bindAddress, sizeof(bindAddress), "%s", comm.udpPeer.bindAddress.c_str());
        if (ImGui::InputText("本地地址", bindAddress, sizeof(bindAddress))) {
            comm.udpPeer.bindAddress = bindAddress;
            application_.markCommConfigEdited(true);
        }
        int bindPort = comm.udpPeer.bindPort;
        if (ImGui::InputInt("本地端口", &bindPort)) {
            comm.udpPeer.bindPort = static_cast<std::uint16_t>(std::clamp(bindPort, 0, 65535));
            application_.markCommConfigEdited(true);
        }
        char remoteHost[256]{};
        std::snprintf(remoteHost, sizeof(remoteHost), "%s", comm.udpPeer.remoteHost.c_str());
        if (ImGui::InputText("远端地址", remoteHost, sizeof(remoteHost))) {
            comm.udpPeer.remoteHost = remoteHost;
            application_.markCommConfigEdited(true);
        }
        int remotePort = comm.udpPeer.remotePort;
        if (ImGui::InputInt("远端端口", &remotePort)) {
            comm.udpPeer.remotePort = static_cast<std::uint16_t>(std::clamp(remotePort, 0, 65535));
            application_.markCommConfigEdited(true);
        }
    }

    ImGui::Separator();
    ImGui::Text("当前模式: %s", transport::transportKindLabel(comm.kind).data());
    ImGui::Text("连接状态: %s", transportStateLabel(comm.state));
    ImGui::Text("TX=%llu RX=%llu",
                static_cast<unsigned long long>(comm.txCount),
                static_cast<unsigned long long>(comm.rxCount));
    if (!comm.lastError.empty()) {
        ImGui::TextWrapped("错误：%s", comm.lastError.c_str());
    }

    if (ImGui::Button("连接")) {
        application_.openTransport();
    }
    ImGui::SameLine();
    if (ImGui::Button("断开")) {
        application_.closeTransport();
    }
    ImGui::SameLine();
    if (ImGui::Button("保存配置")) {
        std::string error;
        const auto outputPath = std::filesystem::path(configState.loadedFromPath);
        if (configStore_.save(outputPath, application_.captureConfig(), error)) {
            application_.docks().clearDirty("配置已保存");
            configSnapshot_ = configStore_.snapshot(outputPath);
            configState.fileTimestampMs = configSnapshot_.timestampMs;
        } else {
            application_.setStatusMessage("保存配置失败: " + error, true);
        }
    }

    ImGui::End();
}

void GuiRuntime::drawProtocolDock() {
    if (!showProtocolDock_) {
        return;
    }

    auto& lua = application_.docks().luaState();
    if (!ImGui::Begin("协议脚本 / 动态控件", &showProtocolDock_)) {
        ImGui::End();
        return;
    }

    char protocolRoot[512]{};
    std::snprintf(protocolRoot, sizeof(protocolRoot), "%s", lua.protocolRootDir.c_str());
    ImGui::PushID("协议根目录");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("协议根目录");
    ImGui::SameLine();
    const float browseButtonWidth = ImGui::CalcTextSize("浏览...").x + ImGui::GetStyle().FramePadding.x * 2.0F;
    const float rootInputWidth =
        (std::max)(120.0F, ImGui::GetContentRegionAvail().x - browseButtonWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SetNextItemWidth(rootInputWidth);
    if (ImGui::InputText("##value", protocolRoot, sizeof(protocolRoot))) {
        lua.protocolRootDir = protocolRoot;
        refreshProtocolRoot(configStore_, lua, protocolDirDraft_, protocolDirDraftModel_);
        application_.markProtocolEdited();
    }
    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
#if defined(_WIN32)
    if (ImGui::Button("浏览...")) {
        std::string dialogError;
        const auto selectedDir = nativeDirectoryDialog(window_, L"选择协议根目录", std::filesystem::path(lua.protocolRootDir), dialogError);
        if (!dialogError.empty()) {
            application_.setStatusMessage(dialogError);
        }
        if (selectedDir.has_value()) {
            lua.protocolRootDir = selectedDir->generic_string();
            refreshProtocolRoot(configStore_, lua, protocolDirDraft_, protocolDirDraftModel_);
            application_.markProtocolEdited();
            application_.setStatusMessage("协议根目录已更新");
        }
    }
#else
    ImGui::BeginDisabled();
    ImGui::Button("浏览...");
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("当前平台暂不支持原生目录选择，请直接输入路径。");
    }
#endif
    ImGui::PopID();

    syncDraftFromModel(protocolDirDraft_, protocolDirDraftModel_, lua.protocolDir);
    if (const auto protocolDirEdit = drawEditableCombo("协议目录", protocolDirDraft_, lua.protocolDirOptions); protocolDirEdit.edited) {
        protocolDirDraft_ = normalizeProtocolDraft(configStore_, lua.protocolRootDir, protocolDirEdit.value);
    }

    if (ImGui::Button("重新扫描协议目录")) {
        refreshProtocolRoot(configStore_, lua, protocolDirDraft_, protocolDirDraftModel_);
        application_.setStatusMessage("协议目录扫描已刷新");
    }
    ImGui::SameLine();
    if (ImGui::Button("重新加载协议")) {
        const auto decision = decideProtocolWorkspaceSwitch(lua.protocolDir, protocolDirDraft_, true);
        if (decision.reloadProtocolDir.has_value()) {
            requestProtocolWorkspaceSwitch(*decision.reloadProtocolDir, true);
        }
    }

    ImGui::Text("协议名称: %s", lua.protocolName.c_str());
    ImGui::TextWrapped("入口脚本: %s", lua.scriptPath.c_str());
    if (decideProtocolWorkspaceSwitch(lua.protocolDir, protocolDirDraft_, false).draftChanged) {
        ImGui::TextDisabled("待加载协议: %s", protocolDirDraft_.c_str());
    }
    if (!lua.lastError.empty()) {
        ImGui::TextWrapped("脚本错误：%s", lua.lastError.c_str());
    }

    ImGui::Separator();
    if (lua.docks.empty()) {
        // 核心流程：Lua 按钮可能在点击回调里同步刷新脚本控件快照。
        // 这里先复制当前帧的控件列表，避免遍历 `lua.controlStates` 时引用失效导致闪退。
        const auto controls = lua.controlStates;
        drawLuaDockFlow(controls);
        ImGui::End();
        return;
    }
    ImGui::End();

}

void GuiRuntime::drawLuaDockFlow(const std::vector<scripting::ControlSnapshot>& controls) {
    for (const auto& control : controls) {
        drawDynamicControl(control);
    }
}

void GuiRuntime::syncDialogQueue() {
    for (auto& request : application_.drainDialogRequests()) {
        dialogQueue_.push_back(std::move(request));
    }
    for (auto& request : application_.drainFileDialogRequests()) {
        application_.respondFileDialog(runLuaFileDialog(window_, request));
    }
    if (!activeDialog_.has_value() && !dialogQueue_.empty()) {
        activeDialog_ = std::move(dialogQueue_.front());
        dialogQueue_.pop_front();
        activeDialogOpened_ = false;
    }
}

void GuiRuntime::drawDialogs() {
    syncDialogQueue();
    if (!activeDialog_.has_value()) {
        return;
    }

    auto dialog = *activeDialog_;
    const std::string popupId = dialog.title + "##proto_dialog_" + std::to_string(dialog.id);
    if (!activeDialogOpened_) {
        ImGui::OpenPopup(popupId.c_str());
        activeDialogOpened_ = true;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::BeginPopupModal(popupId.c_str(), nullptr, flags)) {
        return;
    }

    ImGui::TextUnformatted(dialog.title.c_str());
    ImGui::Separator();
    ImGui::TextWrapped("%s", dialog.message.c_str());
    ImGui::Spacing();

    auto respond = [&](std::string state, std::optional<bool> confirmed) {
        application_.respondDialog(scripting::DialogEvent{
            .id = dialog.id,
            .kind = dialog.kind,
            .state = std::move(state),
            .confirmed = confirmed,
            .title = dialog.title,
            .message = dialog.message,
            .level = dialog.level,
            .dedupeKey = dialog.dedupeKey,
            .timestampMs = nowMs(),
        });
        activeDialog_.reset();
        activeDialogOpened_ = false;
        ImGui::CloseCurrentPopup();
    };

    if (dialog.kind == scripting::DialogKind::Confirm) {
        if (ImGui::Button("确认", ImVec2(90.0F, 0.0F))) {
            respond("confirmed", true);
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(90.0F, 0.0F))) {
            respond("canceled", false);
        }
    } else {
        if (ImGui::Button("关闭", ImVec2(90.0F, 0.0F))) {
            respond("closed", std::nullopt);
        }
    }

    ImGui::EndPopup();
}

void GuiRuntime::openRawCaptureImportDialog() {
#if defined(_WIN32)
    const auto defaultPath = rawCaptureImportPath_.empty()
                               ? executableDir_ / "captures" / "capture.psraw"
                               : std::filesystem::path(rawCaptureImportPath_);
    std::string dialogError;
    const auto path = nativeFileDialog(window_,
                                       L"导入原始波形",
                                       L"ProtoScope Raw Capture (*.psraw)\0*.psraw\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       false,
                                       L"psraw",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        importRawCaptureFromPath(*path);
    }
#else
    rawCaptureImportDialogOpen_ = true;
    rawCaptureImportDialogOpened_ = false;
    rawCaptureImportError_.clear();
    if (rawCaptureImportPath_.empty()) {
        rawCaptureImportPath_ = (executableDir_ / "captures" / "capture.psraw").generic_string();
    }
#endif
}

void GuiRuntime::openRawCaptureExportDialog() {
    const auto& lua = application_.docks().luaState();
    const std::string baseName = lua.protocolName.empty() ? std::string("wave-capture") : lua.protocolName + "-wave";
    const auto defaultPath = rawCaptureExportPath_.empty()
                               ? executableDir_ / "captures" / (baseName + ".psraw")
                               : std::filesystem::path(rawCaptureExportPath_);
#if defined(_WIN32)
    std::string dialogError;
    const auto path = nativeFileDialog(window_,
                                       L"导出原始波形",
                                       L"ProtoScope Raw Capture (*.psraw)\0*.psraw\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       true,
                                       L"psraw",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        exportRawCaptureToPath(*path);
    }
#else
    rawCaptureExportDialogOpen_ = true;
    rawCaptureExportDialogOpened_ = false;
    rawCaptureExportError_.clear();
    rawCaptureExportPath_ = defaultPath.generic_string();
#endif
}

void GuiRuntime::openElfStaticAddressDialog() {
#if defined(_WIN32)
    const auto defaultPath =
        elfStaticAddressPath_.empty() ? executableDir_ : std::filesystem::path(elfStaticAddressPath_);
    std::string dialogError;
    const auto path = nativeFileDialog(window_,
                                       L"打开 ELF/JSON",
                                       L"ELF/JSON Files (*.elf;*.out;*.axf;*.json)\0*.elf;*.out;*.axf;*.json\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       false,
                                       nullptr,
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        loadElfStaticAddressFromPath(*path);
    }
#else
    elfStaticAddressDialogOpen_ = true;
    elfStaticAddressDialogOpened_ = false;
    elfStaticAddressError_.clear();
    if (elfStaticAddressPath_.empty()) {
        elfStaticAddressPath_ = executableDir_.generic_string();
    }
#endif
}

void GuiRuntime::importRawCaptureFromPath(const std::filesystem::path& path) {
    // 核心流程：原生对话框和非 Windows 回退弹窗共用同一条导入链路，避免两套行为分叉。
    rawCaptureImportPath_ = path.generic_string();
    std::string error;
    const auto capture = plot::readRawCaptureFile(path, error);
    if (!capture.has_value()) {
        rawCaptureImportError_ = error;
        application_.setStatusMessage("原始波形导入失败: " + error);
        return;
    }
    if (!std::filesystem::exists(configStore_.mainLuaPath(capture->protocolDir))) {
        rawCaptureImportError_ = "导入文件引用的协议目录不存在: " + capture->protocolDir;
        application_.setStatusMessage("原始波形导入失败: " + rawCaptureImportError_);
        return;
    }

    const auto& currentLua = application_.docks().luaState();
    if (currentLua.protocolDir != capture->protocolDir && !switchProtocolWorkspace(capture->protocolDir, false)) {
        rawCaptureImportError_ = "切换导入协议失败";
        application_.setStatusMessage("原始波形导入失败: " + rawCaptureImportError_);
    } else if (!application_.importWaveRawCapture(*capture, error)) {
        rawCaptureImportError_ = error;
        application_.setStatusMessage("原始波形导入失败: " + error);
    } else {
        application_.setStatusMessage("原始波形导入成功");
        rawCaptureImportDialogOpen_ = false;
        rawCaptureImportDialogOpened_ = false;
        rawCaptureImportError_.clear();
    }
}

void GuiRuntime::exportRawCaptureToPath(const std::filesystem::path& path) {
    // 核心流程：导出路径只在 UI 层选择，实际写入仍交给 Application 统一处理。
    rawCaptureExportPath_ = path.generic_string();
    std::string error;
    if (!application_.exportWaveRawCapture(path, error)) {
        rawCaptureExportError_ = error;
        application_.setStatusMessage("原始波形导出失败: " + error);
        return;
    }
    application_.setStatusMessage("原始波形导出成功");
    rawCaptureExportDialogOpen_ = false;
    rawCaptureExportDialogOpened_ = false;
    rawCaptureExportError_.clear();
}

void GuiRuntime::loadElfStaticAddressFromPath(const std::filesystem::path& path) {
    // 核心流程：加载成功后清空符号下拉缓存，让 Lua 控件基于新模型重新查询。
    elfStaticAddressPath_ = path.generic_string();
    std::string error;
    if (!application_.loadElfStaticAddressFile(path, error)) {
        elfStaticAddressError_ = error;
        application_.setStatusMessage("ELF/JSON 加载失败: " + error);
        return;
    }
    elfSymbolComboStates_.clear();
    elfStaticAddressDialogOpen_ = false;
    elfStaticAddressDialogOpened_ = false;
    elfStaticAddressError_.clear();
}

void GuiRuntime::drawElfStaticAddressDialog() {
    if (!elfStaticAddressDialogOpen_) {
        return;
    }

    const char* popupId = "打开 ELF/JSON##elf_static_view";
    if (!elfStaticAddressDialogOpened_) {
        ImGui::OpenPopup(popupId);
        elfStaticAddressDialogOpened_ = true;
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(popupId, nullptr, flags)) {
        ImGui::TextUnformatted("请输入 ELF 或 ElfStaticView JSON 文件路径");
        char buffer[1024]{};
        std::snprintf(buffer, sizeof(buffer), "%s", elfStaticAddressPath_.c_str());
        if (ImGui::InputText("路径", buffer, sizeof(buffer))) {
            elfStaticAddressPath_ = buffer;
        }
        if (!elfStaticAddressError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.90F, 0.35F, 0.35F, 1.0F), "%s", elfStaticAddressError_.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("打开", ImVec2(90.0F, 0.0F))) {
            loadElfStaticAddressFromPath(elfStaticAddressPath_);
            if (!elfStaticAddressDialogOpen_) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(90.0F, 0.0F))) {
            elfStaticAddressDialogOpen_ = false;
            elfStaticAddressDialogOpened_ = false;
            elfStaticAddressError_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void GuiRuntime::drawRawCaptureFileDialogs() {
    if (rawCaptureImportDialogOpen_) {
        const char* popupId = "导入原始波形##psraw_import";
        if (!rawCaptureImportDialogOpened_) {
            ImGui::OpenPopup(popupId);
            rawCaptureImportDialogOpened_ = true;
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::BeginPopupModal(popupId, nullptr, flags)) {
            ImGui::TextUnformatted("请输入 .psraw 文件路径");
            char buffer[1024]{};
            std::snprintf(buffer, sizeof(buffer), "%s", rawCaptureImportPath_.c_str());
            if (ImGui::InputText("路径", buffer, sizeof(buffer))) {
                rawCaptureImportPath_ = buffer;
            }
            if (!rawCaptureImportError_.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.90F, 0.35F, 0.35F, 1.0F), "%s", rawCaptureImportError_.c_str());
            }
            ImGui::Spacing();
            if (ImGui::Button("导入", ImVec2(90.0F, 0.0F))) {
                importRawCaptureFromPath(rawCaptureImportPath_);
                if (!rawCaptureImportDialogOpen_) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(90.0F, 0.0F))) {
                rawCaptureImportDialogOpen_ = false;
                rawCaptureImportDialogOpened_ = false;
                rawCaptureImportError_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

    if (rawCaptureExportDialogOpen_) {
        const char* popupId = "导出原始波形##psraw_export";
        if (!rawCaptureExportDialogOpened_) {
            ImGui::OpenPopup(popupId);
            rawCaptureExportDialogOpened_ = true;
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::BeginPopupModal(popupId, nullptr, flags)) {
            ImGui::TextUnformatted("请输入导出 .psraw 文件路径");
            char buffer[1024]{};
            std::snprintf(buffer, sizeof(buffer), "%s", rawCaptureExportPath_.c_str());
            if (ImGui::InputText("路径", buffer, sizeof(buffer))) {
                rawCaptureExportPath_ = buffer;
            }
            if (!rawCaptureExportError_.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.90F, 0.35F, 0.35F, 1.0F), "%s", rawCaptureExportError_.c_str());
            }
            ImGui::Spacing();
            if (ImGui::Button("导出", ImVec2(90.0F, 0.0F))) {
                exportRawCaptureToPath(rawCaptureExportPath_);
                if (!rawCaptureExportDialogOpen_) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(90.0F, 0.0F))) {
                rawCaptureExportDialogOpen_ = false;
                rawCaptureExportDialogOpened_ = false;
                rawCaptureExportError_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void GuiRuntime::drawLuaDockTable(const scripting::DockSnapshot& dockSnapshot,
                                  const scripting::TableLayoutDescriptor& layout,
                                  std::string_view stableId) {
    std::unordered_map<std::string, const scripting::ControlSnapshot*> controlsById;
    controlsById.reserve(dockSnapshot.controls.size());
    for (const auto& control : dockSnapshot.controls) {
        controlsById.emplace(control.descriptor.id, &control);
    }

    ImGuiTableFlags flags = ImGuiTableFlags_None;
    if (layout.borders) {
        flags |= ImGuiTableFlags_Borders;
    }
    if (layout.resizable) {
        flags |= ImGuiTableFlags_Resizable;
    }
    if (layout.rowBg) {
        flags |= ImGuiTableFlags_RowBg;
    }
    if (layout.sizing == "stretch") {
        flags |= ImGuiTableFlags_SizingStretchSame;
    }

    const std::string tableId = "##lua_dock_table_" + std::string(stableId);
    if (!ImGui::BeginTable(tableId.c_str(), static_cast<int>(layout.columns), flags)) {
        return;
    }

    for (std::size_t columnIndex = 0; columnIndex < layout.columns; ++columnIndex) {
        ImGui::TableSetupColumn(nullptr, ImGuiTableColumnFlags_WidthStretch);
    }

    // 核心流程：表格布局只负责按声明式 rows/cells 摆放控件，
    // 具体控件行为仍然复用 drawDynamicControl，避免改动现有控件契约。
    for (const auto& row : layout.rows) {
        ImGui::TableNextRow();
        for (std::size_t columnIndex = 0; columnIndex < layout.columns; ++columnIndex) {
            ImGui::TableSetColumnIndex(static_cast<int>(columnIndex));
            if (columnIndex >= row.cells.size()) {
                continue;
            }

            const auto& cell = row.cells[columnIndex];
            if (cell.spacer) {
                continue;
            }

            const auto controlIter = controlsById.find(cell.controlId);
            if (controlIter == controlsById.end()) {
                continue;
            }

            ImGui::PushID(controlIter->second->descriptor.id.c_str());
            drawDynamicControl(*controlIter->second);
            ImGui::PopID();
        }
    }
    ImGui::EndTable();
}

void GuiRuntime::drawLuaDockFormItems(
    const std::vector<scripting::FormLayoutItemDescriptor>& items,
    const std::unordered_map<std::string, const scripting::ControlSnapshot*>& controlsById,
    std::string_view stableId,
    std::size_t& widgetIndex) {
    for (const auto& item : items) {
        switch (item.kind) {
        case scripting::FormLayoutItemKind::Control: {
            const auto controlIter = controlsById.find(item.controlId);
            if (controlIter != controlsById.end()) {
                drawDynamicControl(*controlIter->second);
            }
            break;
        }
        case scripting::FormLayoutItemKind::Controls: {
            bool firstControl = true;
            for (const auto& controlId : item.controls.controlIds) {
                const auto controlIter = controlsById.find(controlId);
                if (controlIter == controlsById.end()) {
                    continue;
                }
                if (!firstControl) {
                    ImGui::SameLine();
                }
                drawDynamicControl(*controlIter->second);
                firstControl = false;
            }
            break;
        }
        case scripting::FormLayoutItemKind::Group:
            if (item.group) {
                ImGui::SeparatorText(item.group->title.c_str());
                drawLuaDockFormItems(item.group->items, controlsById, stableId, widgetIndex);
            }
            break;
        case scripting::FormLayoutItemKind::Collapse:
            if (item.collapse) {
                ImGuiTreeNodeFlags flags = item.collapse->defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : ImGuiTreeNodeFlags_None;
                const std::string headerId = item.collapse->title + "##lua_form_collapse_" + std::string(stableId)
                    + "_" + std::to_string(widgetIndex++);
                if (ImGui::CollapsingHeader(headerId.c_str(), flags)) {
                    drawLuaDockFormItems(item.collapse->items, controlsById, stableId, widgetIndex);
                }
            }
            break;
        case scripting::FormLayoutItemKind::Separator:
            ImGui::Separator();
            break;
        case scripting::FormLayoutItemKind::Text:
            ImGui::TextWrapped("%s", item.text.text.c_str());
            break;
        }
    }
}

void GuiRuntime::drawLuaDockForm(const scripting::DockSnapshot& dockSnapshot,
                                 const scripting::FormLayoutDescriptor& layout,
                                 std::string_view stableId) {
    std::unordered_map<std::string, const scripting::ControlSnapshot*> controlsById;
    controlsById.reserve(dockSnapshot.controls.size());
    for (const auto& control : dockSnapshot.controls) {
        controlsById.emplace(control.descriptor.id, &control);
    }

    // 核心流程：form 布局只负责分组、折叠和同排摆放，
    // 具体控件交互继续复用 drawDynamicControl，保证旧控件行为不变。
    std::size_t widgetIndex = 0;
    drawLuaDockFormItems(layout.items, controlsById, stableId, widgetIndex);
}

void GuiRuntime::drawLuaDockWindows() {
    auto& lua = application_.docks().luaState();
    if (lua.docks.empty()) {
        return;
    }

    // 这里按值复制当前帧快照，保证本帧渲染遍历期间底层容器不会被重入修改。
    const auto dockSnapshots = lua.docks;
    const auto layoutKey = luaDockLayoutKey(lua.protocolDir, lua.scriptPath);
    for (const auto& dockSnapshot : dockSnapshots) {
        const auto stableId = luaDockStableId(dockSnapshot.descriptor, layoutKey);
        const auto windowName = luaDockWindowName(dockSnapshot.descriptor, layoutKey);
        const bool windowVisible = ImGui::Begin(windowName.c_str());
        if (windowVisible) {
            if (dockSnapshot.descriptor.layout.has_value()) {
                if (dockSnapshot.descriptor.layout->kind == scripting::DockLayoutKind::Table) {
                    drawLuaDockTable(dockSnapshot, dockSnapshot.descriptor.layout->table, stableId);
                } else if (dockSnapshot.descriptor.layout->kind == scripting::DockLayoutKind::Form) {
                    drawLuaDockForm(dockSnapshot, dockSnapshot.descriptor.layout->form, stableId);
                } else {
                    drawLuaDockFlow(dockSnapshot.controls);
                }
            } else {
                drawLuaDockFlow(dockSnapshot.controls);
            }
        }
        ImGui::End();
    }
}

void GuiRuntime::updateLuaDockDefaultLayout() {
    if (defaultLuaDockNodes_.empty()) {
        return;
    }

    const auto& lua = application_.docks().luaState();
    const auto layoutKey = luaDockLayoutKey(lua.protocolDir, lua.scriptPath);
    const auto requests = buildLuaDockLayoutRequests(lua.docks, layoutKey);
    std::unordered_map<std::string, ImGuiID> tabGroupNodes;

    for (const auto& request : requests) {
        if (defaultDockedLuaStableIds_.contains(stableWindowId(request.windowName))) {
            continue;
        }

        const auto anchor = parseLuaDockAnchor(request.anchor);
        if (!anchor.has_value()) {
            application_.setStatusMessage("Lua Dock 默认停靠点无效: " + request.anchor, true);
            continue;
        }

        ImGuiID targetNode = 0;
        if (const auto groupIter = tabGroupNodes.find(request.tabGroup); groupIter != tabGroupNodes.end()) {
            targetNode = groupIter->second;
        } else if (const auto nodeIter = defaultLuaDockNodes_.find(*anchor); nodeIter != defaultLuaDockNodes_.end()) {
            targetNode = nodeIter->second;
            tabGroupNodes.emplace(request.tabGroup, targetNode);
        }

        if (targetNode == 0) {
            application_.setStatusMessage("Lua Dock 默认停靠节点不存在: " + visibleWindowTitle(request.windowName), true);
            continue;
        }

        // 核心流程：只给首次出现、ini 尚未创建过的 Lua Dock 提供默认停靠，不覆盖用户拖拽后的布局。
        const bool debugLayout = application_.docks().configState().luaDockLayoutDebug;
        if (debugLayout) {
            application_.setStatusMessage("LuaDockLayout: stableId=" + stableWindowId(request.windowName) + " anchor=" + request.anchor + " tabGroup=" + request.tabGroup + " targetNode=" + std::to_string(targetNode));
        }
        const bool docked = dockWindowIfMissing(request.windowName, targetNode);
        if (debugLayout) {
            application_.setStatusMessage("LuaDockLayout: stableId=" + stableWindowId(request.windowName) + " docked=" + (docked ? "true" : "false") + " schemaRebuild=" + (workspaceLayoutMode_ == WorkspaceLayoutMode::NeedsDefaultBuild ? "true" : "false"));
        }
        defaultDockedLuaStableIds_.insert(stableWindowId(request.windowName));
    }
}

void GuiRuntime::requestProtocolWorkspaceSwitch(std::string protocolDir, bool forceReload) {
    pendingProtocolDir_ = std::move(protocolDir);
    pendingProtocolForceReload_ = forceReload;
}

void GuiRuntime::processPendingProtocolWorkspaceSwitch() {
    if (!pendingProtocolDir_.has_value()) {
        return;
    }

    const auto protocolDir = std::move(*pendingProtocolDir_);
    pendingProtocolDir_.reset();
    if (!switchProtocolWorkspace(protocolDir, pendingProtocolForceReload_)) {
        application_.setStatusMessage("协议重载失败", true);
    }
}

bool GuiRuntime::switchProtocolWorkspace(const std::string& protocolDir, bool forceReload) {
    const auto& previousLua = application_.docks().luaState();
    const auto requestedDir = configStore_.normalizeProtocolDir(previousLua.protocolRootDir, protocolDir).generic_string();
    const bool sameProtocol = protocolWorkspaceLoaded_
        && previousLua.loaded
        && previousLua.protocolDir == requestedDir
        && activeWorkspaceProtocolKey_ == luaDockLayoutKey(requestedDir, configStore_.mainLuaPath(requestedDir).generic_string());

    if (shouldResetLuaDefaultDockStateOnProtocolSwitch(sameProtocol)) {
        saveCurrentProtocolWorkspace();
        defaultDockedLuaStableIds_.clear();
        defaultLuaDockNodes_.clear();
        protocolWorkspaceLoaded_ = false;
        workspaceLayoutMode_ = WorkspaceLayoutMode::NeedsDefaultBuild;
        pendingLuaDefaultDockLayout_ = false;
        pendingProtocolWorkspaceSave_ = false;
    }

    if (!application_.reloadProtocolDirectory(protocolDir, forceReload)) {
        if (!sameProtocol) {
            loadCurrentProtocolWorkspace();
        }
        return false;
    }

    if (sameProtocol) {
        loadCurrentProtocolControlState();
    } else {
        loadCurrentProtocolWorkspace();
    }
    return true;
}

void GuiRuntime::loadCurrentProtocolWorkspace() {
    const auto& lua = application_.docks().luaState();
    const auto layoutPaths = resolveLuaDockLayoutPaths(executableDir_, lua.protocolDir, lua.scriptPath);
    activeWorkspaceProtocolKey_ = layoutPaths.protocolKey;
    protocolWorkspaceLoaded_ = true;
    defaultDockedLuaStableIds_.clear();
    defaultLuaDockNodes_.clear();
    workspaceLayoutMode_ = workspaceLayoutModeAfterLoad(layoutPaths);
    if (application_.docks().configState().luaDockLayoutDebug) {
        const char* modeLabel = workspaceLayoutMode_ == WorkspaceLayoutMode::NeedsDefaultBuild ? "rebuilding" : "ready";
        application_.setStatusMessage("LuaDockLayout: load protocol=" + activeWorkspaceProtocolKey_ + " schemaVersion=" + std::to_string(layoutPaths.schemaVersion) + " isLegacy=" + (layoutPaths.isLegacyLayout ? "true" : "false") + " mode=" + modeLabel);
    }
    pendingLuaDefaultDockLayout_ = false;
    pendingProtocolWorkspaceSave_ = false;

    ImGui::ClearIniSettings();
    if (layoutPaths.hasUserLayout && !layoutPaths.isLegacyLayout) {
        const auto savedLayoutHealth = readDockLayoutIniHealth(layoutPaths.layoutPath);
        if (savedLayoutHealth.has_value() && shouldRebuildDockLayout(*savedLayoutHealth)) {
            workspaceLayoutMode_ = WorkspaceLayoutMode::NeedsDefaultBuild;
            pendingProtocolWorkspaceSave_ = true;
            application_.setStatusMessage(
                std::string("检测到损坏的协议 Dock 布局，已回退默认布局: CentralNode=")
                + std::to_string(savedLayoutHealth->centralNodeCount)
                + (savedLayoutHealth->centralNodeInLegacyLeftPane ? " left-pane=true" : " left-pane=false"));
        } else {
            ImGui::LoadIniSettingsFromDisk(layoutPaths.layoutPath.string().c_str());
        }
    } else if (layoutPaths.hasLegacyLayout) {
        ImGui::LoadIniSettingsFromDisk(layoutPaths.legacyLayoutPath.string().c_str());
        pendingProtocolWorkspaceSave_ = true;
    } else if (layoutPaths.hasUserLayout) {
        ImGui::LoadIniSettingsFromDisk(layoutPaths.layoutPath.string().c_str());
        pendingProtocolWorkspaceSave_ = true;
    } else {
        pendingProtocolWorkspaceSave_ = true;
    }
    ImGui::GetIO().WantSaveIniSettings = false;
    loadCurrentProtocolControlState();
}

void GuiRuntime::saveCurrentProtocolWorkspace() {
    if (!protocolWorkspaceLoaded_ || activeWorkspaceProtocolKey_.empty()) {
        return;
    }

    const auto layoutPath = currentProtocolLayoutPath();
    std::filesystem::create_directories(layoutPath.parent_path());
    pruneCurrentLuaDockSettings();
    ImGui::SaveIniSettingsToDisk(layoutPath.string().c_str());
    try {
        writeLuaDockLayoutMeta(luaDockLayoutMetaPath(executableDir_, activeWorkspaceProtocolKey_), 3);
    } catch (const std::exception& ex) {
        application_.setStatusMessage(std::string("保存协议布局 meta 失败: ") + ex.what(), true);
    }
    ImGui::GetIO().WantSaveIniSettings = false;
    pendingProtocolWorkspaceSave_ = false;
    saveCurrentProtocolControlState();
}

void GuiRuntime::resetCurrentProtocolWorkspaceLayout() {
    if (!canResetProtocolWorkspaceLayout(protocolWorkspaceLoaded_, activeWorkspaceProtocolKey_)) {
        application_.setStatusMessage("当前没有可重置的协议 Dock 布局", true);
        return;
    }

    // 核心流程：手动重置只清空当前运行时 Dock 状态，下一帧按新默认布局重建并覆盖当前协议布局文件。
    ImGui::ClearIniSettings();
    defaultDockedLuaStableIds_.clear();
    defaultLuaDockNodes_.clear();
    workspaceLayoutMode_ = WorkspaceLayoutMode::NeedsDefaultBuild;
    pendingLuaDefaultDockLayout_ = false;
    pendingProtocolWorkspaceSave_ = true;
    ImGui::GetIO().WantSaveIniSettings = false;
    application_.setStatusMessage("当前协议 Dock 布局将在下一帧重置");
}

void GuiRuntime::pruneCurrentLuaDockSettings() {
    if (activeWorkspaceProtocolKey_.empty()) {
        return;
    }

    keepOnlyCurrentLuaDockSettings(activeWorkspaceProtocolKey_);
}

void GuiRuntime::loadCurrentProtocolControlState() {
    const auto statePath = protocolControlStatePath();
    if (!std::filesystem::exists(statePath)) {
        return;
    }

    try {
        const auto root = YAML::LoadFile(statePath.string());
        restoreWaveProtocolState(root, activeWorkspaceProtocolKey_, application_.docks().waveState());
        const auto protocolState = root["protocols"][activeWorkspaceProtocolKey_]["controls"];
        if (!protocolState) {
            return;
        }

        const auto controls = application_.docks().luaState().controlStates;
        for (const auto& control : controls) {
            const auto& descriptor = control.descriptor;
            if (!isPersistedControlType(descriptor.type)) {
                continue;
            }
            const auto saved = protocolState[descriptor.id];
            if (!saved || saved["type"].as<std::string>("") != controlTypeName(descriptor.type)) {
                continue;
            }
            if (const auto value = readControlValue(saved["value"], descriptor.type)) {
                application_.restoreControlValue(descriptor.id, *value);
            }
        }
        application_.docks().waveState().statusMessage = "协议波形状态已恢复";
    } catch (const std::exception& ex) {
        application_.setStatusMessage(std::string("加载协议控件状态失败: ") + ex.what(), true);
    }
}

void GuiRuntime::saveCurrentProtocolControlState() {
    YAML::Node root;
    const auto statePath = protocolControlStatePath();
    try {
        if (std::filesystem::exists(statePath)) {
            root = YAML::LoadFile(statePath.string());
        }

        YAML::Node controlsNode;
        const auto controls = application_.docks().luaState().controlStates;
        for (const auto& control : controls) {
            const auto& descriptor = control.descriptor;
            if (!isPersistedControlType(descriptor.type)) {
                continue;
            }

            YAML::Node controlNode;
            controlNode["type"] = controlTypeName(descriptor.type);
            writeControlValue(controlNode["value"], control);
            controlsNode[descriptor.id] = controlNode;
        }

        root["protocols"][activeWorkspaceProtocolKey_]["controls"] = controlsNode;
        storeWaveProtocolState(root, activeWorkspaceProtocolKey_, application_.docks().waveState());
        std::filesystem::create_directories(statePath.parent_path());
        std::ofstream out(statePath);
        if (!out.good()) {
            application_.setStatusMessage("保存协议控件状态失败: 无法写入文件", true);
            return;
        }
        out << root;
    } catch (const std::exception& ex) {
        application_.setStatusMessage(std::string("保存协议控件状态失败: ") + ex.what(), true);
    }
}

std::filesystem::path GuiRuntime::currentProtocolLayoutPath() const {
    return luaDockLayoutPath(executableDir_, activeWorkspaceProtocolKey_);
}

std::filesystem::path GuiRuntime::legacyProtocolLayoutPath() const {
    const auto& lua = application_.docks().luaState();
    return luaDockLayoutPath(executableDir_, legacyLuaDockLayoutKey(lua.protocolDir, lua.scriptPath));
}

std::filesystem::path GuiRuntime::protocolControlStatePath() const {
    return std::filesystem::path("config") / "ui" / "protocol-control-state.yaml";
}

void GuiRuntime::drawTransferDock() {
    if (!showTransferDock_) {
        return;
    }

    auto& sendState = application_.docks().sendState();
    auto& comm = application_.docks().commState();
    auto& receive = application_.docks().receiveState();
    if (!ImGui::Begin("收发数据", &showTransferDock_)) {
        ImGui::End();
        return;
    }

    const float availableHeight = ImGui::GetContentRegionAvail().y;
    const float splitterThickness = ImGui::GetStyle().FramePadding.y * 2.0F;
    const float minSendHeight = ImGui::GetFrameHeightWithSpacing() * 4.0F;
    const float minLogHeight = ImGui::GetTextLineHeightWithSpacing() * 6.0F;
    transferSendSectionHeight_ = (std::clamp)(
        transferSendSectionHeight_, minSendHeight, (std::max)(minSendHeight, availableHeight - minLogHeight - splitterThickness));

    if (ImGui::BeginChild("##transfer_send_section", ImVec2(0.0F, transferSendSectionHeight_), true)) {
        ImGui::TextUnformatted(PROTOSCOPE_ICON_SEND " 发送区");
        bool hexMode = sendState.hexMode;
        if (drawIconCheckbox(PROTOSCOPE_ICON_HEX, &hexMode, "HEX 发送")) {
            if (!application_.setSendHexMode(hexMode)) {
                hexMode = sendState.hexMode;
            }
        }

        char buffer[2048]{};
        std::snprintf(buffer, sizeof(buffer), "%s", sendState.payload.c_str());
        const auto flags = sendState.hexMode ? (ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackCharFilter) : ImGuiInputTextFlags_None;
        if (ImGui::InputTextMultiline("原始载荷",
                                      buffer,
                                      sizeof(buffer),
                                      ImVec2(-FLT_MIN, -ImGui::GetFrameHeightWithSpacing() * 1.4F),
                                      flags,
                                      sendState.hexMode ? hexEditorCallback : nullptr)) {
            sendState.payload = buffer;
        }
        if (drawIconButton(PROTOSCOPE_ICON_SEND, "发送原始载荷")) {
            if (!application_.sendManualPayload(sendState.payload, sendState.hexMode)) {
                application_.setStatusMessage(comm.lastError, true);
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("发送原始载荷");
    }
    ImGui::EndChild();

    // 核心流程：发送区与记录区之间用 splitter 调节高度，避免两块内容互相挤占。
    drawHorizontalSplitter("##transfer_splitter", transferSendSectionHeight_, minSendHeight, minLogHeight, availableHeight, splitterThickness);

    if (ImGui::BeginChild("##transfer_log_section", ImVec2(0.0F, 0.0F), true)) {
        ImGui::TextUnformatted(PROTOSCOPE_ICON_EXCHANGE " 收发记录");
        ImGui::SameLine();
        drawTransferLogFilterButton("全部", dock::TransferLogFilter::All, receive.filter);
        ImGui::SameLine();
        drawTransferLogFilterButton("RX", dock::TransferLogFilter::Rx, receive.filter);
        ImGui::SameLine();
        drawTransferLogFilterButton("TX", dock::TransferLogFilter::Tx, receive.filter);
        ImGui::SameLine();
        drawIconCheckbox(PROTOSCOPE_ICON_HEX, &receive.showHex, "显示 HEX");
        ImGui::SameLine();
        drawIconCheckbox(PROTOSCOPE_ICON_CLOCK, &receive.showTimestamps, "显示时间戳");
        ImGui::SameLine();
        drawIconCheckbox(receive.pauseScroll ? PROTOSCOPE_ICON_PLAY : PROTOSCOPE_ICON_PAUSE, &receive.pauseScroll, "暂停滚动");
        ImGui::SameLine();
        if (drawIconButton(PROTOSCOPE_ICON_TRASH, "清空收发记录")) {
            application_.docks().clearReceiveRows();
        }

        const auto filteredRows = filteredTransferRows(receive.rows, receive.filter);
        drawTransferLogRows("transfer_rows", filteredRows, receive.showTimestamps, receive.showHex, receive.pauseScroll, "暂无 TX/RX 原始数据");
    }
    ImGui::EndChild();

    ImGui::End();
}

void GuiRuntime::drawLogDock() {
    if (!showLogDock_) {
        return;
    }

    auto& logState = application_.docks().logState();
    if (!ImGui::Begin("日志", &showLogDock_)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("显示时间戳", &logState.showTimestamps);
    ImGui::SameLine();
    ImGui::Checkbox("暂停滚动", &logState.pauseScroll);
    ImGui::SameLine();
    if (ImGui::Button("清空")) {
        application_.docks().clearLogRows();
    }

    drawRowList("log_rows", logState.rows, logState.showTimestamps, false, logState.pauseScroll, "暂无宿主日志");
    ImGui::End();
}

void GuiRuntime::drawScriptDock() {
    if (!showScriptDock_) {
        return;
    }

    auto& scriptState = application_.docks().scriptState();
    if (!ImGui::Begin("脚本", &showScriptDock_)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("显示时间戳", &scriptState.showTimestamps);
    ImGui::SameLine();
    ImGui::Checkbox("暂停滚动", &scriptState.pauseScroll);
    ImGui::SameLine();
    if (ImGui::Button("清空")) {
        application_.docks().clearScriptRows();
    }

    drawRowList("script_rows", scriptState.rows, scriptState.showTimestamps, false, scriptState.pauseScroll, "暂无 Lua 日志或事件");
    ImGui::End();
}

void GuiRuntime::drawDynamicControl(const scripting::ControlSnapshot& control) {
    const auto& descriptor = control.descriptor;
    switch (descriptor.type) {
    case scripting::ControlType::Button:
        if (ImGui::Button(descriptor.label.c_str())) {
            application_.updateControlValue(descriptor.id, true);
        }
        break;
    case scripting::ControlType::Checkbox: {
        bool checked = std::get<bool>(control.value);
        if (ImGui::Checkbox(descriptor.label.c_str(), &checked)) {
            application_.updateControlValue(descriptor.id, checked);
        }
        break;
    }
    case scripting::ControlType::InputText: {
        char buffer[512]{};
        std::snprintf(buffer, sizeof(buffer), "%s", std::get<std::string>(control.value).c_str());
        if (ImGui::InputText(descriptor.label.c_str(), buffer, sizeof(buffer))) {
            application_.updateControlValue(descriptor.id, std::string(buffer));
        }
        break;
    }
    case scripting::ControlType::Combo: {
        int index = std::get<int>(control.value);
        std::vector<const char*> items;
        for (const auto& option : descriptor.comboOptions) {
            items.push_back(option.c_str());
        }
        if (!items.empty() && ImGui::Combo(descriptor.label.c_str(), &index, items.data(), static_cast<int>(items.size()))) {
            application_.updateControlValue(descriptor.id, index);
        }
        break;
    }
    case scripting::ControlType::ElfSymbolCombo: {
        auto& state = elfSymbolComboStates_[descriptor.id];
        const auto& current = std::get<scripting::ElfSymbolValue>(control.value);
        if (state.draft.empty() && !current.label.empty()) {
            state.draft = current.label;
        }

        const auto currentMs = nowMs();
        if (state.editedAtMs == 0) {
            state.editedAtMs = currentMs;
        }
        if (state.queriedDraft != state.draft
            && currentMs >= state.editedAtMs + static_cast<std::uint64_t>(descriptor.debounceMs)) {
            state.options = application_.queryElfStaticAddresses(state.draft, descriptor.limit);
            state.queriedDraft = state.draft;
        }

        std::vector<std::string> labels;
        labels.reserve(state.options.size());
        for (const auto& option : state.options) {
            labels.push_back(option.label);
        }

        const auto edit = drawEditableCombo(descriptor.label.c_str(), state.draft, labels);
        if (edit.edited) {
            state.draft = edit.value;
            state.editedAtMs = currentMs;
        }
        if (edit.selectedFromList) {
            const auto selected = std::find_if(state.options.begin(), state.options.end(), [&](const auto& option) {
                return option.label == edit.value;
            });
            if (selected != state.options.end()) {
                application_.updateControlValue(descriptor.id, *selected);
            }
        }
        break;
    }
    case scripting::ControlType::InputInt: {
        int value = std::get<int>(control.value);
        if (ImGui::InputInt(descriptor.label.c_str(), &value)) {
            application_.updateControlValue(descriptor.id, value);
        }
        break;
    }
    case scripting::ControlType::InputFloat: {
        float value = std::get<float>(control.value);
        if (ImGui::InputFloat(descriptor.label.c_str(), &value)) {
            application_.updateControlValue(descriptor.id, value);
        }
        break;
    }
    }
}

bool GuiRuntime::reloadConfigFromDisk() {
    saveCurrentProtocolWorkspace();
    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    if (!application_.applyConfig(loaded.config)) {
        return false;
    }
    loadCurrentProtocolWorkspace();
    configSnapshot_ = configStore_.snapshot(configStore_.defaultConfigPath());
    auto& configState = application_.docks().configState();
    application_.docks().clearPendingExternalReload();
    application_.docks().clearDirty("已从磁盘重载配置");
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    return true;
}

bool GuiRuntime::pollConfigFileChanges() {
    auto& configState = application_.docks().configState();
    if (!configState.configHotReloadEnabled) {
        return false;
    }
    if (!configSnapshot_.path.empty() && !configStore_.hasChanged(configSnapshot_)) {
        return false;
    }
    configSnapshot_ = configStore_.snapshot(configStore_.defaultConfigPath());
    application_.docks().setPendingExternalReload(configSnapshot_.timestampMs, "检测到外部配置更新，请手动保存或重载");
    return false;
}

bool GuiRuntime::maybeAutoSave() {
    auto& configState = application_.docks().configState();
    if (!configState.autoSaveEnabled || !configState.dirty) {
        return false;
    }
    if (configState.pendingExternalReload) {
        return false;
    }

    const auto currentMs = nowMs();
    if (lastAutoSaveAtMs_ != 0 && currentMs - lastAutoSaveAtMs_ < configState.autoSaveIntervalMs) {
        return false;
    }

    std::string error;
    const auto path = std::filesystem::path(configState.loadedFromPath);
    if (!configStore_.save(path, application_.captureConfig(), error)) {
        application_.setStatusMessage("自动保存失败: " + error, true);
        return false;
    }
    configSnapshot_ = configStore_.snapshot(path);
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    application_.docks().clearDirty("自动保存成功");
    lastAutoSaveAtMs_ = currentMs;
    return true;
}

void GuiRuntime::sleepUntilNextFrame(std::uint64_t frameStartMs) const {
    const auto fpsLimit = (std::max)(std::uint32_t{1}, application_.docks().configState().fpsLimit);
    const auto minFrameMs = 1000ULL / fpsLimit;
    const auto elapsed = nowMs() - frameStartMs;
    if (elapsed < minFrameMs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(minFrameMs - elapsed));
    }
}

std::uint64_t GuiRuntime::nowMs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string GuiRuntime::formatTimestamp(std::uint64_t timestampMs) {
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

    char buffer[64]{};
    std::snprintf(buffer,
                  sizeof(buffer),
                  "%04d-%02d-%02d %02d:%02d:%02d:%03d",
                  localTm.tm_year + 1900,
                  localTm.tm_mon + 1,
                  localTm.tm_mday,
                  localTm.tm_hour,
                  localTm.tm_min,
                  localTm.tm_sec,
                  static_cast<int>(millis));
    return buffer;
}

} // namespace protoscope::ui
