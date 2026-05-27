#include "protoscope/ui/gui_runtime.hpp"

#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/ui/dock_layout.hpp"

#if defined(_WIN32)
#include <windows.h>
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

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <functional>
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

struct EditableComboResult {
    bool edited{false};
    bool selectedFromList{false};
    bool valid{true};
    std::string value;
};

std::vector<std::filesystem::path> candidateChineseFonts() {
    return {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
        "3rdparty/imgui/misc/fonts/DroidSans.ttf",
    };
}

const char* transportKindLabel(transport::TransportKind kind) {
    switch (kind) {
    case transport::TransportKind::TcpClient:
        return "TCP 客户端";
    case transport::TransportKind::TcpServer:
        return "TCP 服务端";
    case transport::TransportKind::Serial:
        return "串口";
    }
    return "未知";
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

std::string bytesToAsciiPreview(const std::vector<std::uint8_t>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto byte : bytes) {
        const char ch = static_cast<char>(byte);
        text.push_back(std::isprint(static_cast<unsigned char>(ch)) ? ch : '.');
    }
    return text;
}

std::string formatReceiveRowText(const dock::ReceiveRow& row, bool showHex) {
    if (!row.message.empty()) {
        return row.message;
    }
    if (row.bytes.empty()) {
        return {};
    }
    return showHex ? protocol_utils::bytesToHex(row.bytes, true) : bytesToAsciiPreview(row.bytes);
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
    const auto stableId = stableWindowId(windowName);
    const ImGuiID windowId = ImHashStr(stableId.c_str());
    const auto* settings = ImGui::FindWindowSettingsByID(windowId);
    if (targetNode == 0 || ImGui::FindWindowByName(name.c_str()) != nullptr || settings != nullptr) {
        return settings != nullptr;
    }
    ImGui::DockBuilderDockWindow(name.c_str(), targetNode);
    return true;
}

void keepOnlyCurrentLuaDockSettings(
    std::string_view layoutKey,
    const std::unordered_set<std::string>& activeLuaDockIdSet = {}) {
    auto& settings = ImGui::GetCurrentContext()->SettingsWindows;
    const std::vector<std::string> activeLuaDockIds(activeLuaDockIdSet.begin(), activeLuaDockIdSet.end());

    // 核心逻辑：清理其它协议或已删除 Lua Dock 的窗口状态，避免失效窗口继续污染当前协议布局。
    for (ImGuiWindowSettings* setting = settings.begin(); setting != nullptr; setting = settings.next_chunk(setting)) {
        const std::string_view name = setting->GetName();
        const auto stableIdPos = name.find("###");
        const auto stableId = stableIdPos == std::string_view::npos ? name : name.substr(stableIdPos + 3);
        if (!shouldKeepLuaWindowSettings(stableId, layoutKey, activeLuaDockIds)) {
            setting->WantDelete = true;
            setting->ID = 0;
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
    case scripting::ControlType::Button:
        break;
    }
}

std::string formatTimestampText(std::uint64_t timestampMs) {
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

void drawRowList(const char* childId,
                 const std::vector<dock::ReceiveRow>& rows,
                 bool showTimestamps,
                 bool showHex,
                 bool& pauseScroll,
                 const std::string& emptyText) {
    if (ImGui::BeginChild(childId, ImVec2(0.0F, 0.0F), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        if (rows.empty()) {
            ImGui::TextDisabled("%s", emptyText.c_str());
        } else {
            for (const auto& row : rows) {
                if (showTimestamps) {
                    ImGui::Text("[%s] %s %s",
                                formatTimestampText(row.timestampMs).c_str(),
                                row.direction.c_str(),
                                row.endpoint.c_str());
                } else {
                    ImGui::Text("%s %s", row.direction.c_str(), row.endpoint.c_str());
                }
                const auto content = formatReceiveRowText(row, showHex);
                if (!content.empty()) {
                    ImGui::TextWrapped("  %s", content.c_str());
                }
            }
            if (!pauseScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0F) {
                ImGui::SetScrollHereY(1.0F);
            }
        }
    }
    ImGui::EndChild();
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

std::string wideToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::optional<std::string> chooseProtocolRootDirectory(const std::string& currentDir) {
    BROWSEINFOW browseInfo{};
    browseInfo.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    browseInfo.lpszTitle = L"选择协议根目录";

    const auto initialDir = utf8ToWide(currentDir);
    browseInfo.lParam = reinterpret_cast<LPARAM>(initialDir.c_str());
    browseInfo.lpfn = browseInitialDirCallback;

    PIDLIST_ABSOLUTE selected = SHBrowseForFolderW(&browseInfo);
    if (selected == nullptr) {
        return std::nullopt;
    }

    wchar_t path[MAX_PATH]{};
    const bool ok = SHGetPathFromIDListW(selected, path) == TRUE;
    CoTaskMemFree(selected);
    if (!ok) {
        return std::nullopt;
    }

    return wideToUtf8(path);
}
#endif

EditableComboResult drawEditableCombo(const char* label,
                                      std::string& draft,
                                      const std::vector<std::string>& options,
                                      const std::function<bool(const std::string&)>& validator = {}) {
    EditableComboResult result;
    result.value = draft;

    ImGui::PushID(label);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    const float arrowWidth = ImGui::GetFrameHeight();
    const float inputWidth = (std::max)(120.0F, ImGui::GetContentRegionAvail().x - arrowWidth - ImGui::GetStyle().ItemSpacing.x);
    ImGui::SetNextItemWidth(inputWidth);
    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer), "%s", draft.c_str());
    if (ImGui::InputText("##value", buffer, sizeof(buffer))) {
        draft = buffer;
        result.edited = true;
        result.value = draft;
    }

    ImGui::SameLine(0.0F, ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::BeginCombo("##options", "", ImGuiComboFlags_NoPreview)) {
        for (const auto& option : options) {
            const bool selected = option == draft;
            if (ImGui::Selectable(option.c_str(), selected)) {
                draft = option;
                result.edited = true;
                result.selectedFromList = true;
                result.value = draft;
            }
            if (selected) {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopID();

    if (validator) {
        result.valid = validator(draft);
    }
    result.value = draft;
    return result;
}

void syncDraftFromModel(std::string& draft, std::string& lastModel, const std::string& model) {
    if (draft.empty() || lastModel != model) {
        draft = model;
        lastModel = model;
    }
}

} // namespace

GuiRuntime::GuiRuntime(app::Application& application, const config::ConfigStore& configStore)
    : application_(application),
      configStore_(configStore),
      executableDir_(executableDirectory()),
      waveDockRenderer_(application) {
    customBaudRateDraft_ = std::to_string(kCommonBaudRates[7]);
    customBaudRateDraftModel_ = customBaudRateDraft_;
}

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
            if (const auto nextWakeup = application_.nextWakeupAtMs()) {
                if (*nextWakeup > frameStartMs) {
                    sleepUntilNextFrame(frameStartMs);
                }
            }
        }

        processPendingProtocolWorkspaceSwitch();
        renderFrame();
        glfwSwapBuffers(window_);
        lastRenderAtMs_ = frameStartMs;
        if (ImGui::GetIO().WantSaveIniSettings) {
            saveCurrentProtocolWorkspace();
        } else if (pendingProtocolWorkspaceSave_) {
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
}

void GuiRuntime::renderFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    drawMainMenu();

    ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

    if (workspaceLayoutMode_ == WorkspaceLayoutMode::NeedsDefaultBuild) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID left = dockspaceId;
        ImGuiID mainBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.28F, nullptr, &left);
        ImGuiID right = ImGui::DockBuilderSplitNode(left, ImGuiDir_Right, 0.58F, nullptr, &left);
        ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.48F, nullptr, &left);
        ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.42F, nullptr, &right);
        ImGuiID rightMid = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.36F, nullptr, &right);
        ImGuiID rightLogs = ImGui::DockBuilderSplitNode(rightMid, ImGuiDir_Down, 0.5F, nullptr, &rightMid);

        defaultLuaDockNodes_.clear();
        defaultLuaDockNodes_[LuaDockAnchor::Left] = left;
        defaultLuaDockNodes_[LuaDockAnchor::LeftBottom] = leftBottom;
        defaultLuaDockNodes_[LuaDockAnchor::RightTop] = right;
        defaultLuaDockNodes_[LuaDockAnchor::RightMid] = rightMid;
        defaultLuaDockNodes_[LuaDockAnchor::RightBottom] = rightBottom;
        defaultLuaDockNodes_[LuaDockAnchor::MainBottom] = mainBottom;

        ImGui::DockBuilderDockWindow("通讯配置", left);
        ImGui::DockBuilderDockWindow("协议脚本 / 动态控件", leftBottom);
        ImGui::DockBuilderDockWindow("发送", right);
        ImGui::DockBuilderDockWindow("接收数据", rightMid);
        ImGui::DockBuilderDockWindow("日志", rightLogs);
        ImGui::DockBuilderDockWindow("脚本", rightBottom);
        ImGui::DockBuilderDockWindow("波形", rightBottom);
        ImGui::DockBuilderFinish(dockspaceId);
        workspaceLayoutMode_ = WorkspaceLayoutMode::Ready;
        pendingLuaDefaultDockLayout_ = true;
    }
    if (pendingLuaDefaultDockLayout_) {
        // 核心流程：Lua 动态 Dock 的默认停靠只属于默认布局事务，避免用户拖拽后被下一帧拉回。
        updateLuaDockDefaultLayout();
        pendingLuaDefaultDockLayout_ = false;
    }

    drawStatusBar();
    drawCommDock();
    drawProtocolDock();
    drawSendDock();
    drawReceiveDock();
    drawLogDock();
    drawScriptDock();
    waveDockRenderer_.draw(showWaveDock_);

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
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("视图")) {
        ImGui::MenuItem("通讯配置", nullptr, &showCommDock_);
        ImGui::MenuItem("协议脚本 / 动态控件", nullptr, &showProtocolDock_);
        ImGui::MenuItem("发送", nullptr, &showSendDock_);
        ImGui::MenuItem("接收数据", nullptr, &showReceiveDock_);
        ImGui::MenuItem("日志", nullptr, &showLogDock_);
        ImGui::MenuItem("脚本", nullptr, &showScriptDock_);
        ImGui::MenuItem("波形", nullptr, &showWaveDock_);
        ImGui::EndMenu();
    }

    ImGui::EndMainMenuBar();
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

    int kindIndex = static_cast<int>(comm.kind);
    const char* items[] = {"TCP 客户端", "TCP 服务端", "串口"};
    if (ImGui::Combo("模式", &kindIndex, items, IM_ARRAYSIZE(items))) {
        comm.kind = static_cast<transport::TransportKind>(kindIndex);
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
    } else {
        if (ImGui::Button("刷新串口列表")) {
            comm.serialPortOptions = {"COM1", "COM2", "COM3", "COM4"};
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
        if (std::find(baudRateOptions.begin(), baudRateOptions.end(), currentBaudText) != baudRateOptions.end()) {
            syncDraftFromModel(commonBaudRateDraft_, commonBaudRateDraftModel_, currentBaudText);
        }
        if (const auto baudEdit = drawEditableCombo("常用波特率", commonBaudRateDraft_, baudRateOptions, digitsOnly); baudEdit.edited && baudEdit.valid) {
            const auto baudRate = static_cast<std::uint32_t>(std::stoul(baudEdit.value));
            if (baudRate != comm.serial.baudRate) {
                comm.serial.baudRate = baudRate;
                customBaudRateDraft_ = baudEdit.value;
                commonBaudRateDraftModel_ = baudEdit.value;
                customBaudRateDraftModel_ = baudEdit.value;
                application_.markCommConfigEdited(true);
            }
        }

        syncDraftFromModel(customBaudRateDraft_, customBaudRateDraftModel_, currentBaudText);
        if (const auto customBaudEdit = drawEditableCombo("自定义波特率", customBaudRateDraft_, {}, digitsOnly); customBaudEdit.edited) {
            if (customBaudEdit.valid) {
                const auto baudRate = static_cast<std::uint32_t>(std::stoul(customBaudEdit.value));
                if (baudRate != comm.serial.baudRate) {
                    comm.serial.baudRate = baudRate;
                    customBaudRateDraftModel_ = customBaudEdit.value;
                    if (std::find(baudRateOptions.begin(), baudRateOptions.end(), customBaudEdit.value) != baudRateOptions.end()) {
                        commonBaudRateDraft_ = customBaudEdit.value;
                        commonBaudRateDraftModel_ = customBaudEdit.value;
                    }
                    application_.markCommConfigEdited(true);
                }
            } else {
                application_.setStatusMessage("自定义波特率仅接受纯数字");
            }
        }
        if (!digitsOnly(customBaudRateDraft_)) {
            ImGui::TextDisabled("自定义波特率仅接受纯数字");
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
    }

    ImGui::Separator();
    ImGui::Text("当前模式: %s", transportKindLabel(comm.kind));
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
        if (const auto selectedDir = chooseProtocolRootDirectory(lua.protocolRootDir)) {
            lua.protocolRootDir = *selectedDir;
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
        for (const auto& control : controls) {
            drawDynamicControl(control);
        }
        ImGui::End();
        return;
    }
    ImGui::End();

    // 核心流程：动态 Dock 中的控件点击会同步改写 `lua.docks`。
    // 这里按值复制当前帧快照，保证本帧渲染遍历期间底层容器不会被重入修改。
    const auto dockSnapshots = lua.docks;
    const auto layoutKey = luaDockLayoutKey(lua.protocolDir, lua.scriptPath);
    for (const auto& dockSnapshot : dockSnapshots) {
        const auto windowName = luaDockWindowName(dockSnapshot.descriptor, layoutKey);
        const bool windowVisible = ImGui::Begin(windowName.c_str());
        if (windowVisible) {
            for (const auto& control : dockSnapshot.controls) {
                drawDynamicControl(control);
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
        if (defaultDockedLuaWindows_.contains(request.windowName)) {
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
        if (dockWindowIfMissing(request.windowName, targetNode)) {
            defaultDockedLuaWindows_.insert(request.windowName);
        }
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
        defaultDockedLuaWindows_.clear();
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
        pruneCurrentLuaDockSettings();
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
    defaultDockedLuaWindows_.clear();
    defaultLuaDockNodes_.clear();
    workspaceLayoutMode_ = workspaceLayoutModeAfterLoad(layoutPaths);
    pendingLuaDefaultDockLayout_ = false;

    ImGui::ClearIniSettings();
    if (layoutPaths.hasUserLayout && !layoutPaths.isLegacyLayout) {
        ImGui::LoadIniSettingsFromDisk(layoutPaths.layoutPath.string().c_str());
    } else if (layoutPaths.hasLegacyLayout) {
        ImGui::LoadIniSettingsFromDisk(layoutPaths.legacyLayoutPath.string().c_str());
        pendingProtocolWorkspaceSave_ = true;
    } else if (layoutPaths.hasUserLayout) {
        ImGui::LoadIniSettingsFromDisk(layoutPaths.layoutPath.string().c_str());
        pendingProtocolWorkspaceSave_ = true;
    } else {
        pendingProtocolWorkspaceSave_ = true;
    }
    pruneCurrentLuaDockSettings();
    ImGui::GetIO().WantSaveIniSettings = false;
    loadCurrentProtocolControlState();
}

void GuiRuntime::saveCurrentProtocolWorkspace() {
    if (!protocolWorkspaceLoaded_ || activeWorkspaceProtocolKey_.empty()) {
        return;
    }

    const auto layoutPath = currentProtocolLayoutPath();
    std::filesystem::create_directories(layoutPath.parent_path());
    ImGui::SaveIniSettingsToDisk(layoutPath.string().c_str());
    try {
        writeLuaDockLayoutMeta(luaDockLayoutMetaPath(executableDir_, activeWorkspaceProtocolKey_), 2);
    } catch (const std::exception& ex) {
        application_.setStatusMessage(std::string("保存协议布局 meta 失败: ") + ex.what(), true);
    }
    ImGui::GetIO().WantSaveIniSettings = false;
    pendingProtocolWorkspaceSave_ = false;
    saveCurrentProtocolControlState();
}

void GuiRuntime::pruneCurrentLuaDockSettings() {
    if (activeWorkspaceProtocolKey_.empty()) {
        return;
    }

    const auto& lua = application_.docks().luaState();
    std::unordered_set<std::string> activeLuaDockIds;
    activeLuaDockIds.reserve(lua.docks.size());
    for (const auto& dock : lua.docks) {
        activeLuaDockIds.insert(std::string("LuaDock:") + activeWorkspaceProtocolKey_ + ':' + dock.descriptor.id);
    }
    keepOnlyCurrentLuaDockSettings(activeWorkspaceProtocolKey_, activeLuaDockIds);
}

void GuiRuntime::loadCurrentProtocolControlState() {
    const auto statePath = protocolControlStatePath();
    if (!std::filesystem::exists(statePath)) {
        return;
    }

    try {
        const auto root = YAML::LoadFile(statePath.string());
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

void GuiRuntime::drawSendDock() {
    if (!showSendDock_) {
        return;
    }

    auto& sendState = application_.docks().sendState();
    auto& comm = application_.docks().commState();
    if (!ImGui::Begin("发送", &showSendDock_)) {
        ImGui::End();
        return;
    }

    bool hexMode = sendState.hexMode;
    if (ImGui::Checkbox("HEX 发送", &hexMode)) {
        if (!application_.setSendHexMode(hexMode)) {
            hexMode = sendState.hexMode;
        }
    }

    if (sendState.hexMode) {
        char buffer[2048]{};
        std::snprintf(buffer, sizeof(buffer), "%s", sendState.payload.c_str());
        if (ImGui::InputTextMultiline("原始载荷", buffer, sizeof(buffer), ImVec2(-FLT_MIN, 120.0F),
                                      ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackCharFilter,
                                      hexEditorCallback)) {
            sendState.payload = buffer;
        }
    } else {
        char buffer[2048]{};
        std::snprintf(buffer, sizeof(buffer), "%s", sendState.payload.c_str());
        if (ImGui::InputTextMultiline("原始载荷", buffer, sizeof(buffer), ImVec2(-FLT_MIN, 120.0F))) {
            sendState.payload = buffer;
        }
    }

    if (ImGui::Button("发送原始载荷")) {
        if (!application_.sendManualPayload(sendState.payload, sendState.hexMode)) {
            application_.setStatusMessage(comm.lastError, true);
        }
    }

    ImGui::End();
}

void GuiRuntime::drawReceiveDock() {
    if (!showReceiveDock_) {
        return;
    }

    auto& receive = application_.docks().receiveState();
    if (!ImGui::Begin("接收数据", &showReceiveDock_)) {
        ImGui::End();
        return;
    }

    ImGui::Checkbox("显示 HEX", &receive.showHex);
    ImGui::SameLine();
    ImGui::Checkbox("显示时间戳", &receive.showTimestamps);
    ImGui::SameLine();
    ImGui::Checkbox("暂停滚动", &receive.pauseScroll);
    ImGui::SameLine();
    if (ImGui::Button("清空")) {
        application_.docks().clearReceiveRows();
    }

    drawRowList("receive_rows", receive.rows, receive.showTimestamps, receive.showHex, receive.pauseScroll, "暂无 TX/RX 原始数据");
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
    return formatTimestampText(timestampMs);
}

} // namespace protoscope::ui
