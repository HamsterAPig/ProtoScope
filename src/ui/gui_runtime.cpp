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
#include <cstring>
#include <filesystem>
#include <cstdio>
#include <sstream>
#include <thread>
#include <vector>

namespace protoscope::ui {

namespace {

const char* kGlslVersion = "#version 130";

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

int hexInputFilter(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag != ImGuiInputTextFlags_CallbackCharFilter) {
        return 0;
    }

    const auto ch = static_cast<unsigned int>(data->EventChar);
    if (ch < 128U && std::isspace(static_cast<unsigned char>(ch))) {
        data->EventChar = ' ';
        return 0;
    }

    if (std::isxdigit(static_cast<unsigned char>(ch))) {
        data->EventChar = static_cast<ImWchar>(std::toupper(static_cast<unsigned char>(ch)));
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
    const auto normalized =
        protocol_utils::normalizeHexEditorInput(current, static_cast<std::size_t>(data->CursorPos));
    if (normalized.text != current) {
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, normalized.text.c_str());
    }

    const int cursorPos =
        static_cast<int>((std::min)(normalized.cursorPos, static_cast<std::size_t>(data->BufTextLen)));
    data->CursorPos = cursorPos;
    data->SelectionStart = cursorPos;
    data->SelectionEnd = cursorPos;
    return 0;
}

std::string formatReceiveRow(const dock::ReceiveRow& row) {
    std::ostringstream builder;
    builder << "[" << row.timestampMs << "] " << row.direction << " " << row.endpoint << " " << row.text;
    return builder.str();
}

std::string formatAllReceiveRows(const std::vector<dock::ReceiveRow>& rows) {
    std::ostringstream builder;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        if (i != 0) {
            builder << '\n';
        }
        builder << formatReceiveRow(rows[i]);
    }
    return builder.str();
}

} // namespace

GuiRuntime::GuiRuntime(app::Application& application, const config::ConfigStore& configStore)
    : application_(application), configStore_(configStore) {}

GuiRuntime::~GuiRuntime() {
    shutdown();
}

bool GuiRuntime::initialize() {
    if (!initializeWindow()) {
        return false;
    }
    if (!initializeImGui()) {
        shutdown();
        return false;
    }
    if (!initializePlotContext()) {
        shutdown();
        return false;
    }

    configSnapshot_ = configStore_.snapshot(application_.docks().configState().loadedFromPath);
    application_.docks().configState().fileTimestampMs = configSnapshot_.timestampMs;
    running_ = true;
    return true;
}

int GuiRuntime::run() {
    while (running_ && window_ && !glfwWindowShouldClose(window_)) {
        const auto frameStartMs = nowMs();
        const bool changed = application_.pumpOnce() || pollConfigFileChanges() || maybeAutoSave();

        if (!changed && lastRenderAtMs_ != 0) {
            double timeoutSeconds = 0.25;
            if (const auto nextWakeup = application_.nextWakeupAtMs()) {
                const auto remainingMs = (*nextWakeup > frameStartMs) ? (*nextWakeup - frameStartMs) : 0;
                timeoutSeconds = (std::min)(timeoutSeconds, static_cast<double>(remainingMs) / 1000.0);
            }
            glfwWaitEventsTimeout(timeoutSeconds);
        }

        glfwPollEvents();
        renderFrame();
        sleepUntilNextFrame(frameStartMs);
    }

    return 0;
}

void GuiRuntime::shutdown() {
    shutdownPlotContext();
    shutdownImGui();
    shutdownWindow();
}

bool GuiRuntime::initializeWindow() {
    if (!glfwInit()) {
        return false;
    }

    const auto captured = application_.captureConfig();
    const auto& window = captured.gui.window;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
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
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
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
    // 核心边界：本阶段只显式保留波形子系统接入口，不提前引入真实 ImPlot 依赖。
    return true;
}

void GuiRuntime::shutdownImGui() {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
}

void GuiRuntime::shutdownPlotContext() {
    // 核心边界：当前无真实 ImPlot 上下文，仅保留对称清理钩子。
}

void GuiRuntime::shutdownWindow() {
    if (window_) {
        glfwDestroyWindow(window_);
        window_ = nullptr;
    }
    glfwTerminate();
}

void GuiRuntime::ensureChineseFont() {
    ImGuiIO& io = ImGui::GetIO();
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseFull();
    for (const auto& candidate : candidateChineseFonts()) {
        if (std::filesystem::exists(candidate)) {
            io.Fonts->AddFontFromFileTTF(candidate.string().c_str(), 18.0F, nullptr, ranges);
            break;
        }
    }
}

void GuiRuntime::renderFrame() {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    ImGuiID dockspaceId = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (!layoutInitialized_) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, ImGui::GetMainViewport()->Size);

        ImGuiID left = dockspaceId;
        ImGuiID right = ImGui::DockBuilderSplitNode(left, ImGuiDir_Right, 0.55F, nullptr, &left);
        ImGuiID leftBottom = ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.40F, nullptr, &left);
        ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.35F, nullptr, &right);

        ImGui::DockBuilderDockWindow("通讯配置", left);
        ImGui::DockBuilderDockWindow("协议脚本 / 动态控件", leftBottom);
        ImGui::DockBuilderDockWindow("收发控制 / 接收日志", right);
        ImGui::DockBuilderDockWindow("波形", rightBottom);
        ImGui::DockBuilderFinish(dockspaceId);
        layoutInitialized_ = true;
    }

    drawStatusBar();
    drawCommDock();
    drawProtocolDock();
    drawLogDock();
    drawWaveDock();

    ImGui::Render();

    int displayW = 0;
    int displayH = 0;
    glfwGetFramebufferSize(window_, &displayW, &displayH);
    glViewport(0, 0, displayW, displayH);
    glClearColor(0.10F, 0.11F, 0.13F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(window_);
    lastRenderAtMs_ = nowMs();
}

void GuiRuntime::drawStatusBar() {
    const auto& config = application_.docks().configState();
    const auto& comm = application_.docks().commState();
    if (ImGui::BeginMainMenuBar()) {
        ImGui::TextUnformatted(config.statusMessage.empty() ? "GUI v1 运行中" : config.statusMessage.c_str());
        if (config.dirty) {
            ImGui::SameLine();
            ImGui::TextUnformatted("· 未保存");
        }
        if (comm.reconnectRequired) {
            ImGui::SameLine();
            ImGui::TextUnformatted("· 需重连");
        }
        if (config.pendingExternalReload) {
            ImGui::SameLine();
            ImGui::TextUnformatted("· 检测到外部配置更新");
        }
        if (config.conflict.detected) {
            ImGui::SameLine();
            ImGui::Text("· 冲突: %s", config.conflict.message.c_str());
        }
        ImGui::EndMainMenuBar();
    }
}

void GuiRuntime::drawCommDock() {
    auto& comm = application_.docks().commState();
    auto& configState = application_.docks().configState();

    ImGui::Begin("通讯配置");
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
            configState.statusMessage = "已刷新串口列表（当前为占位实现）";
        }

        int currentIndex = 0;
        for (int i = 0; i < static_cast<int>(comm.serialPortOptions.size()); ++i) {
            if (comm.serialPortOptions[i] == comm.serial.portName) {
                currentIndex = i;
                break;
            }
        }
        std::vector<const char*> options;
        options.reserve(comm.serialPortOptions.size());
        for (const auto& item : comm.serialPortOptions) {
            options.push_back(item.c_str());
        }
        if (!options.empty() && ImGui::Combo("端口", &currentIndex, options.data(), static_cast<int>(options.size()))) {
            comm.serial.portName = comm.serialPortOptions[currentIndex];
            application_.markCommConfigEdited(true);
        }

        int baudRate = static_cast<int>(comm.serial.baudRate);
        if (ImGui::InputInt("波特率", &baudRate)) {
            comm.serial.baudRate = static_cast<std::uint32_t>((std::max)(baudRate, 0));
            application_.markCommConfigEdited(true);
        }
    }

    ImGui::Separator();
    ImGui::Text("状态：%s", transportKindLabel(comm.kind));
    ImGui::Text("连接状态：%d", static_cast<int>(comm.state));
    ImGui::Text("发送字节：%llu", static_cast<unsigned long long>(comm.txCount));
    ImGui::Text("接收字节：%llu", static_cast<unsigned long long>(comm.rxCount));
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
            configSnapshot_ = configStore_.snapshot(outputPath);
            application_.docks().configState().fileTimestampMs = configSnapshot_.timestampMs;
            configState.pendingExternalReload = false;
            configState.pendingExternalReloadTimestampMs = 0;
            configState.externalReloadMessage.clear();
            application_.docks().clearDirty("配置已保存");
            application_.docks().clearConflict();
        } else {
            application_.docks().configState().statusMessage = "保存配置失败: " + error;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("重载配置")) {
        reloadConfigFromDisk();
    }
    ImGui::SameLine();
    if (ImGui::Button("忽略本次外部更新提示")) {
        configState.pendingExternalReload = false;
        configState.pendingExternalReloadTimestampMs = 0;
        configState.externalReloadMessage.clear();
        application_.docks().clearConflict();
        configState.statusMessage = "已忽略本次外部配置更新";
    }

    if (ImGui::Checkbox("启用外部配置热重载提醒", &configState.configHotReloadEnabled)) {
        application_.docks().markDirty("配置热重载选项已修改");
    }
    ImGui::TextWrapped("配置文件：%s", configState.loadedFromPath.c_str());
    if (configState.pendingExternalReload) {
        ImGui::TextWrapped("%s",
                           configState.externalReloadMessage.empty() ? "检测到外部配置更新，等待手动重载"
                                                                     : configState.externalReloadMessage.c_str());
    }

    ImGui::End();
}

void GuiRuntime::drawProtocolDock() {
    auto& lua = application_.docks().luaState();

    ImGui::Begin("协议脚本 / 动态控件");
    char protocolDir[512]{};
    std::snprintf(protocolDir, sizeof(protocolDir), "%s", lua.protocolDir.c_str());
    if (ImGui::InputText("协议目录", protocolDir, sizeof(protocolDir))) {
        lua.protocolDir = protocolDir;
        application_.markProtocolEdited();
    }

    ImGui::Text("协议名：%s", lua.protocolName.c_str());
    ImGui::Text("入口：%s", lua.scriptPath.c_str());
    if (ImGui::Button("重新加载协议")) {
        application_.reloadProtocolDirectory(lua.protocolDir, true);
        application_.docks().configState().statusMessage = lua.loaded ? "协议已重新加载" : "协议重新加载失败";
    }

    ImGui::Separator();
    for (const auto& control : application_.docks().luaState().controlStates) {
        drawDynamicControl(control);
    }
    ImGui::End();
}

void GuiRuntime::drawLogDock() {
    auto& sendState = application_.docks().sendState();
    auto& receive = application_.docks().receiveState();

    ImGui::Begin("收发控制 / 接收日志");
    if (ImGui::Checkbox("HEX 发送", &sendState.hexMode) && sendState.hexMode) {
        sendState.payload = protocol_utils::normalizeHexText(sendState.payload);
    }

    char payload[512]{};
    std::snprintf(payload, sizeof(payload), "%s", sendState.payload.c_str());
    ImGuiInputTextFlags payloadFlags = ImGuiInputTextFlags_None;
    if (sendState.hexMode) {
        payloadFlags |= ImGuiInputTextFlags_CallbackCharFilter;
        payloadFlags |= ImGuiInputTextFlags_CallbackEdit;
    }
    if (ImGui::InputTextMultiline("发送内容",
                                  payload,
                                  sizeof(payload),
                                  ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5.0f),
                                  payloadFlags,
                                  sendState.hexMode ? hexEditorCallback : nullptr)) {
        sendState.payload = sendState.hexMode ? protocol_utils::normalizeHexText(payload) : std::string(payload);
    }
    const bool hexPayloadComplete = !sendState.hexMode || (protocol_utils::countHexDigits(sendState.payload) % 2 == 0);
    if (sendState.hexMode && !hexPayloadComplete) {
        ImGui::TextWrapped("HEX 文本存在未成对 nibble，当前禁止发送。");
    }

    char actionName[128]{};
    std::snprintf(actionName, sizeof(actionName), "%s", sendState.actionName.c_str());
    if (ImGui::InputText("动作名", actionName, sizeof(actionName))) {
        sendState.actionName = actionName;
    }

    if (!hexPayloadComplete) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("发送")) {
        application_.sendManualPayload(sendState.payload, sendState.hexMode);
    }
    if (!hexPayloadComplete) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("执行业务动作")) {
        application_.triggerAction(sendState.actionName);
    }
    ImGui::SameLine();
    if (ImGui::Button("清空日志")) {
        application_.docks().clearReceiveRows();
    }
    ImGui::SameLine();
    if (ImGui::Button("复制日志")) {
        const auto allText = formatAllReceiveRows(receive.rows);
        ImGui::SetClipboardText(allText.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("暂停跟随", &receive.pauseScroll);

    ImGui::Separator();
    // 核心流程：日志绘制固定在剩余区域内，超出后只通过子窗口滚动，不再把 Dock 自身越撑越高。
    if (ImGui::BeginChild("receive-log-list", ImVec2(0.0f, 0.0f), true, ImGuiWindowFlags_HorizontalScrollbar)) {
        for (const auto& row : receive.rows) {
            const auto line = formatReceiveRow(row);
            ImGui::TextWrapped("%s", line.c_str());
        }
        if (!receive.pauseScroll && !receive.rows.empty()) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void GuiRuntime::drawWaveDock() {
    ImGui::Begin("波形");
    ImGui::TextWrapped("%s", application_.docks().waveState().placeholder.c_str());
    ImGui::End();
}

void GuiRuntime::drawDynamicControl(const scripting::ControlSnapshot& control) {
    const auto& descriptor = control.descriptor;
    switch (descriptor.type) {
    case scripting::ControlType::Button:
        if (ImGui::Button(descriptor.label.c_str())) {
            application_.triggerAction(descriptor.id);
        }
        break;
    case scripting::ControlType::InputText: {
        std::string current = std::get<std::string>(control.value);
        char buffer[256]{};
        std::snprintf(buffer, sizeof(buffer), "%s", current.c_str());
        if (ImGui::InputText(descriptor.label.c_str(), buffer, sizeof(buffer))) {
            application_.updateControlValue(descriptor.id, std::string(buffer));
        }
        break;
    }
    case scripting::ControlType::Checkbox: {
        bool checked = std::get<bool>(control.value);
        if (ImGui::Checkbox(descriptor.label.c_str(), &checked)) {
            application_.updateControlValue(descriptor.id, checked);
        }
        break;
    }
    case scripting::ControlType::Combo: {
        int index = std::get<int>(control.value);
        std::vector<const char*> items;
        items.reserve(descriptor.comboOptions.size());
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
    auto& configState = application_.docks().configState();
    const auto loaded = configStore_.load(std::filesystem::path(configState.loadedFromPath));
    application_.applyConfig(loaded.config);

    configSnapshot_ = configStore_.snapshot(loaded.resolvedPath);
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    configState.pendingExternalReload = false;
    configState.pendingExternalReloadTimestampMs = 0;
    configState.externalReloadMessage.clear();
    application_.docks().clearConflict();

    if (loaded.loadedFromDisk) {
        application_.docks().clearDirty("已从磁盘重载配置");
    } else if (!loaded.error.empty()) {
        application_.docks().markDirty(loaded.error);
    } else {
        application_.docks().markDirty("未找到配置文件，已重新应用默认配置");
    }
    return true;
}

bool GuiRuntime::pollConfigFileChanges() {
    auto& configState = application_.docks().configState();
    if (!configState.configHotReloadEnabled) {
        return false;
    }

    const auto path = std::filesystem::path(configState.loadedFromPath);
    if (!configStore_.hasChanged(configSnapshot_)) {
        return false;
    }

    configSnapshot_ = configStore_.snapshot(path);
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    configState.pendingExternalReload = true;
    configState.pendingExternalReloadTimestampMs = configSnapshot_.timestampMs;
    configState.externalReloadMessage = "检测到外部配置更新，等待手动重载";

    if (configState.dirty) {
        application_.docks().setConflict("当前存在未保存修改，无法自动重载");
        configState.statusMessage = "当前存在未保存修改，无法自动重载";
        return true;
    }

    application_.docks().clearConflict();
    configState.statusMessage = "检测到外部配置更新";
    return true;
}

bool GuiRuntime::maybeAutoSave() {
    auto& configState = application_.docks().configState();
    if (!configState.autoSaveEnabled || !configState.dirty) {
        return false;
    }
    if (configState.pendingExternalReload) {
        configState.statusMessage = "当前存在未保存修改，无法自动重载";
        return false;
    }

    const auto currentMs = nowMs();
    if (lastAutoSaveAtMs_ != 0 && currentMs - lastAutoSaveAtMs_ < configState.autoSaveIntervalMs) {
        return false;
    }

    std::string error;
    const auto path = std::filesystem::path(configState.loadedFromPath);
    if (!configStore_.save(path, application_.captureConfig(), error)) {
        configState.statusMessage = "自动保存失败: " + error;
        lastAutoSaveAtMs_ = currentMs;
        return true;
    }

    configSnapshot_ = configStore_.snapshot(path);
    configState.fileTimestampMs = configSnapshot_.timestampMs;
    application_.docks().clearDirty("已自动保存配置");
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
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

} // namespace protoscope::ui
