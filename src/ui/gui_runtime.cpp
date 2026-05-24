#include "protoscope/ui/gui_runtime.hpp"

#include "protoscope/protocol_utils/codec.hpp"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#if defined(_WIN32)
#include <windows.h>
#endif
#include <GL/gl.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <thread>
#include <vector>

namespace protoscope::ui {

namespace {

const char* kGlslVersion = "#version 130";
constexpr std::uint32_t kCommonBaudRates[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};

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

const char* serialOptionIndex(const std::vector<std::string>& options, const std::string& value, int& index) {
    index = 0;
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        if (options[i] == value) {
            index = i;
            break;
        }
    }
    return options.empty() ? nullptr : options[index].c_str();
}

} // namespace

GuiRuntime::GuiRuntime(app::Application& application, const config::ConfigStore& configStore)
    : application_(application),
      configStore_(configStore) {}

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

        renderFrame();
        glfwSwapBuffers(window_);
        lastRenderAtMs_ = frameStartMs;
    }
    return 0;
}

void GuiRuntime::shutdown() {
    if (!window_) {
        return;
    }
    running_ = false;
    shutdownImGui();
    shutdownPlotContext();
    shutdownWindow();
}

bool GuiRuntime::initializeWindow() {
    if (!glfwInit()) {
        return false;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    const auto& window = application_.captureConfig().gui.window;
    window_ = glfwCreateWindow(window.width, window.height, window.title.c_str(), nullptr, nullptr);
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
    return true;
}

void GuiRuntime::shutdownImGui() {
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void GuiRuntime::shutdownPlotContext() {
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
    if (!layoutInitialized_) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID left = dockspaceId;
        ImGuiID right = ImGui::DockBuilderSplitNode(left, ImGuiDir_Right, 0.58F, nullptr, &left);
        ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.48F, nullptr, &left);
        ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.42F, nullptr, &right);
        ImGuiID rightMid = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.36F, nullptr, &right);
        ImGuiID rightLogs = ImGui::DockBuilderSplitNode(rightMid, ImGuiDir_Down, 0.5F, nullptr, &rightMid);

        ImGui::DockBuilderDockWindow("通讯配置", left);
        ImGui::DockBuilderDockWindow("协议脚本 / 动态控件", leftBottom);
        ImGui::DockBuilderDockWindow("发送", right);
        ImGui::DockBuilderDockWindow("接收数据", rightMid);
        ImGui::DockBuilderDockWindow("日志", rightLogs);
        ImGui::DockBuilderDockWindow("脚本", rightBottom);
        ImGui::DockBuilderDockWindow("波形", rightBottom);
        ImGui::DockBuilderFinish(dockspaceId);
        layoutInitialized_ = true;
    }

    drawStatusBar();
    drawCommDock();
    drawProtocolDock();
    drawSendDock();
    drawReceiveDock();
    drawLogDock();
    drawScriptDock();
    drawWaveDock();

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
            if (!application_.reloadProtocolDirectory(application_.docks().luaState().protocolDir, true)) {
                application_.setStatusMessage("协议重载失败", true);
            }
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

        int currentIndex = 0;
        serialOptionIndex(comm.serialPortOptions, comm.serial.portName, currentIndex);
        std::vector<const char*> options;
        for (const auto& item : comm.serialPortOptions) {
            options.push_back(item.c_str());
        }
        if (!options.empty() && ImGui::Combo("端口", &currentIndex, options.data(), static_cast<int>(options.size()))) {
            comm.serial.portName = comm.serialPortOptions[currentIndex];
            application_.markCommConfigEdited(true);
        }

        std::vector<std::string> baudRateOptions;
        int baudIndex = -1;
        for (std::size_t i = 0; i < std::size(kCommonBaudRates); ++i) {
            baudRateOptions.push_back(std::to_string(kCommonBaudRates[i]));
            if (kCommonBaudRates[i] == comm.serial.baudRate) {
                baudIndex = static_cast<int>(i);
            }
        }
        std::vector<const char*> baudItems;
        for (const auto& item : baudRateOptions) {
            baudItems.push_back(item.c_str());
        }
        if (!baudItems.empty() && ImGui::Combo("常用波特率", &baudIndex, baudItems.data(), static_cast<int>(baudItems.size()))) {
            if (baudIndex >= 0) {
                comm.serial.baudRate = kCommonBaudRates[baudIndex];
                application_.markCommConfigEdited(true);
            }
        }

        int baudRate = static_cast<int>(comm.serial.baudRate);
        if (ImGui::InputInt("自定义波特率", &baudRate)) {
            comm.serial.baudRate = static_cast<std::uint32_t>(std::max(0, baudRate));
            application_.markCommConfigEdited(true);
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
    if (ImGui::InputText("协议根目录", protocolRoot, sizeof(protocolRoot))) {
        lua.protocolRootDir = protocolRoot;
        lua.protocolDirOptions = configStore_.scanProtocolDirectories(lua.protocolRootDir);
        application_.markProtocolEdited();
    }

    std::vector<const char*> protocolItems;
    int protocolIndex = -1;
    for (int i = 0; i < static_cast<int>(lua.protocolDirOptions.size()); ++i) {
        protocolItems.push_back(lua.protocolDirOptions[i].c_str());
        if (lua.protocolDirOptions[i] == lua.protocolDir) {
            protocolIndex = i;
        }
    }
    if (!protocolItems.empty() && ImGui::Combo("扫描结果", &protocolIndex, protocolItems.data(), static_cast<int>(protocolItems.size()))) {
        if (protocolIndex >= 0) {
            lua.protocolDir = lua.protocolDirOptions[protocolIndex];
            application_.markProtocolEdited();
        }
    }

    char protocolDir[512]{};
    std::snprintf(protocolDir, sizeof(protocolDir), "%s", lua.protocolDir.c_str());
    if (ImGui::InputText("协议目录", protocolDir, sizeof(protocolDir))) {
        lua.protocolDir = protocolDir;
        application_.markProtocolEdited();
    }

    if (ImGui::Button("重新扫描协议目录")) {
        lua.protocolDirOptions = configStore_.scanProtocolDirectories(lua.protocolRootDir);
        application_.setStatusMessage("协议目录扫描已刷新");
    }
    ImGui::SameLine();
    if (ImGui::Button("重新加载协议")) {
        if (!application_.reloadProtocolDirectory(lua.protocolDir, true)) {
            application_.setStatusMessage("协议重载失败", true);
        }
    }

    ImGui::Text("协议名称: %s", lua.protocolName.c_str());
    ImGui::TextWrapped("入口脚本: %s", lua.scriptPath.c_str());
    if (!lua.lastError.empty()) {
        ImGui::TextWrapped("脚本错误：%s", lua.lastError.c_str());
    }

    ImGui::Separator();
    if (lua.docks.empty()) {
        for (const auto& control : lua.controlStates) {
            drawDynamicControl(control);
        }
        ImGui::End();
        return;
    }
    ImGui::End();

    for (const auto& dockSnapshot : lua.docks) {
        if (ImGui::Begin(dockSnapshot.descriptor.title.c_str())) {
            for (const auto& control : dockSnapshot.controls) {
                drawDynamicControl(control);
            }
        }
        ImGui::End();
    }
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

    if (!sendState.actionOptions.empty()) {
        std::vector<const char*> items;
        int actionIndex = 0;
        for (int i = 0; i < static_cast<int>(sendState.actionOptions.size()); ++i) {
            items.push_back(sendState.actionOptions[i].c_str());
            if (sendState.actionOptions[i] == sendState.actionName) {
                actionIndex = i;
            }
        }
        if (ImGui::Combo("业务动作", &actionIndex, items.data(), static_cast<int>(items.size()))) {
            sendState.actionName = sendState.actionOptions[actionIndex];
        }
    } else {
        ImGui::TextDisabled("当前协议未声明业务动作");
    }

    if (ImGui::Button("发送原始载荷")) {
        if (!application_.sendManualPayload(sendState.payload, sendState.hexMode)) {
            application_.setStatusMessage(comm.lastError, true);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("触发业务动作") && !sendState.actionName.empty()) {
        application_.triggerAction(sendState.actionName);
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

void GuiRuntime::drawWaveDock() {
    if (!showWaveDock_) {
        return;
    }

    const auto& wave = application_.docks().waveState();
    if (ImGui::Begin("波形", &showWaveDock_)) {
        ImGui::TextWrapped("%s", wave.placeholder.c_str());
    }
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
    const auto loaded = configStore_.load(configStore_.defaultConfigPath());
    if (!application_.applyConfig(loaded.config)) {
        return false;
    }
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
    const auto fpsLimit = std::max<std::uint32_t>(1, application_.docks().configState().fpsLimit);
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
