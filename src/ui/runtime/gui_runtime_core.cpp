#include "protoscope/ui/gui_runtime.hpp"

#include "protoscope/build/version.hpp"
#include "protoscope/plot/raw_capture_file.hpp"
#include "protoscope/protocol_utils/codec.hpp"
#include "protoscope/transport/transport.hpp"
#include "protoscope/ui/dock_layout.hpp"
#include "protoscope/ui/editable_combo.hpp"
#include "protoscope/ui/icons.hpp"
#include "protoscope/ui/protocol_ui_state.hpp"

#include "protoscope/ui/ui_component.hpp"
#include "workspace_controller.hpp"

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
namespace protoscope::ui {

namespace {

const char* kGlslVersion = "#version 150";

std::string currentProtocolTitle(const dock::LuaDockState& lua) {
    if (!lua.protocolName.empty()) {
        return lua.protocolName;
    }
    return lua.protocolDir.empty() ? "未加载协议" : lua.protocolDir;
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

} // namespace


GuiRuntime::GuiRuntime(app::Application& application, const config::ConfigStore& configStore)
    : application_(application),
      configStore_(configStore),
      workspaceController_(std::make_unique<WorkspaceController>(*this)),
      executableDir_(executableDirectory()),
      waveDockRenderer_(application) {}

GuiRuntime::~GuiRuntime() {
    shutdown();
}

const GuiRuntime::FilteredLogRowsCache& GuiRuntime::filteredLogRowsCached(
    FilteredLogRowsCache& cache,
    const std::deque<dock::ReceiveRow>& rows,
    std::uint64_t version,
    const dock::LogFilterState& filter,
    bool includeBytePreview) {
    const bool sameSourceAndFilter =
        cache.source == &rows &&
        cache.filter.keyword == filter.keyword &&
        cache.filter.status == filter.status &&
        cache.includeBytePreview == includeBytePreview;
    const bool appendOnly =
        sameSourceAndFilter &&
        cache.version != version &&
        cache.rowCount <= rows.size() &&
        (cache.rowCount == 0U || (!rows.empty() && cache.firstRow == &rows.front()));

    if (appendOnly) {
        // 核心流程：高速收包时日志只追加未裁剪，过滤缓存增量处理新行，避免每帧全量扫描历史。
        for (std::size_t index = cache.rowCount; index < rows.size(); ++index) {
            const auto& row = rows[index];
            if (!dock::matchesLogFilter(row, filter, includeBytePreview)) {
                continue;
            }
            cache.rows.push_back(&row);
            cache.endpointWidth =
                (std::clamp)((std::max)(cache.endpointWidth,
                                        ImGui::CalcTextSize(row.endpoint.empty() ? "-" : row.endpoint.c_str()).x),
                             86.0F,
                             220.0F);
        }
        cache.version = version;
        cache.rowCount = rows.size();
        cache.firstRow = rows.empty() ? nullptr : &rows.front();
    } else if (!sameSourceAndFilter || cache.version != version) {
        cache.source = &rows;
        cache.version = version;
        cache.filter = filter;
        cache.includeBytePreview = includeBytePreview;
        cache.rowCount = rows.size();
        cache.firstRow = rows.empty() ? nullptr : &rows.front();
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
    registerUiComponents();
    attachUiComponents();
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

        workspaceController_->processPendingProtocolWorkspaceSwitch();
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
    detachUiComponents();
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

void GuiRuntime::registerUiComponents() {
    uiComponents_ = UiComponentRegistry::createRuntimeComponents(*this);
    menuContributors_.clear();
    dialogComponents_.clear();
    dockComponents_.clear();
    menuContributors_.reserve(uiComponents_.size());
    dialogComponents_.reserve(uiComponents_.size());
    dockComponents_.reserve(uiComponents_.size());
    for (const auto& component : uiComponents_) {
        if (auto* menu = dynamic_cast<IMenuContributor*>(component.get())) {
            menuContributors_.push_back(menu);
        }
        if (auto* dialog = dynamic_cast<IDialogComponent*>(component.get())) {
            dialogComponents_.push_back(dialog);
        }
        if (auto* dock = dynamic_cast<IDockComponent*>(component.get())) {
            dockComponents_.push_back(dock);
        }
    }
}

void GuiRuntime::syncRuntimeState() {
    runtimeState_.window = window_;
    runtimeState_.running = running_;
    runtimeState_.showWaveDock = showWaveDock_;
    runtimeState_.activeWorkspaceProtocolKey = activeWorkspaceProtocolKey_;
    runtimeState_.pendingProtocolDir = pendingProtocolDir_;
    runtimeState_.pendingProtocolForceReload = pendingProtocolForceReload_;
    runtimeState_.lastRenderAtMs = lastRenderAtMs_;
    runtimeState_.dockVisibility = {
        {"comm", showCommDock_},
        {"protocol", showProtocolDock_},
        {"transfer", showTransferDock_},
        {"log", showLogDock_},
        {"script", showScriptDock_},
        {"wave", showWaveDock_},
    };
}

RuntimeUiContext GuiRuntime::makeUiContext() {
    syncRuntimeState();
    return RuntimeUiContext{
        .application = application_,
        .configStore = configStore_,
        .window = window_,
        .runtimeState = runtimeState_,
        .workspace = *workspaceController_,
    };
}

void GuiRuntime::attachUiComponents() {
    auto context = makeUiContext();
    for (const auto& component : uiComponents_) {
        component->onAttach(context);
    }
}

void GuiRuntime::detachUiComponents() {
    auto context = makeUiContext();
    for (auto componentIter = uiComponents_.rbegin(); componentIter != uiComponents_.rend(); ++componentIter) {
        (*componentIter)->onDetach(context);
    }
}

void GuiRuntime::drawRegisteredMenus() {
    auto context = makeUiContext();
    for (auto* menu : menuContributors_) {
        menu->drawMainMenuItems(context);
    }
}

void GuiRuntime::syncRegisteredDialogs() {
    auto context = makeUiContext();
    for (auto* dialog : dialogComponents_) {
        dialog->syncRequests(context);
    }
}

void GuiRuntime::drawRegisteredDialogs() {
    auto context = makeUiContext();
    for (auto* dialog : dialogComponents_) {
        dialog->drawDialogs(context);
    }
}

void GuiRuntime::drawRegisteredDocks() {
    auto context = makeUiContext();
    for (auto* dock : dockComponents_) {
        dock->drawDock(context);
    }
}

void GuiRuntime::renderFrame() {
    refreshWindowTitle();

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    drawRegisteredMenus();
    syncRegisteredDialogs();

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
    drawRegisteredDocks();
    waveDockRenderer_.draw(showWaveDock_);
    drawRegisteredDialogs();
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
