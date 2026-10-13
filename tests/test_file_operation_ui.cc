#include "protoscope/app/application.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include "../src/ui/wave/wave_render_service.hpp"
#include "../src/ui/docks/dock_widgets.hpp"
#include <imgui_internal.h>
#include <implot_internal.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
std::filesystem::path captureDirectory;
bool withGl = false;
void beginFrame()
{
    if (withGl) ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
}
void endFrame(const std::string& name = {})
{
    ImGui::Render();
    if (!withGl) return;
    const int width = static_cast<int>(ImGui::GetIO().DisplaySize.x);
    const int height = static_cast<int>(ImGui::GetIO().DisplaySize.y);
    glViewport(0, 0, width, height);
    glClearColor(0.05F, 0.05F, 0.05F, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glFinish();
    if (name.empty()) return;
    // BMP 行对齐到四字节，支持窄窗口验证截图。
    const auto stride = (width * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(stride * height));
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());
    const auto extrema = std::minmax_element(pixels.begin(), pixels.end());
    if (*extrema.first == *extrema.second || glGetError() != GL_NO_ERROR)
        throw std::runtime_error("blank UI framebuffer");
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) std::swap(pixels[y * stride + x * 3], pixels[y * stride + x * 3 + 2]);
    std::array<unsigned char, 54> header{};
    header[0] = 'B'; header[1] = 'M';
    const auto put = [&](std::size_t offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) header[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    put(2, static_cast<std::uint32_t>(54 + pixels.size()));
    put(10, 54); put(14, 40); put(18, width); put(22, height);
    header[26] = 1; header[28] = 24;
    std::filesystem::create_directories(captureDirectory);
    std::ofstream output(captureDirectory / (name + ".bmp"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(header.data()), header.size());
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    if (!output) throw std::runtime_error("UI screenshot write failure");
}
}

namespace protoscope::ui {
struct GuiRuntimeTestAccess {
    static void require(bool value, const char* message)
    {
        if (!value) throw std::runtime_error(message);
    }
    static void verifySettings(app::Application& application, GuiRuntime& runtime)
    {
        const auto original = application.captureConfig();
        runtime.openApplicationSettings();
        auto frame = [&] {
            runtime.processSettingsActions();
            beginFrame(); runtime.drawApplicationSettings(); endFrame();
        };        frame(); frame();
        auto* settings = ImGui::FindWindowByName("应用设置");
        require(settings && settings->Active && settings->DockId == 0, "settings floating single instance");
        require((settings->Flags & ImGuiWindowFlags_NoSavedSettings) != 0, "settings excluded from saved layouts");
        runtime.settingsDraft_->value.gui.showAppHeader = !original.gui.showAppHeader;
        runtime.openApplicationSettings(); frame();
        require(application.captureConfig().gui.showAppHeader == original.gui.showAppHeader, "settings draft isolation and reopen retention");
        require(runtime.settingsOwnInput(), "settings focus owns shortcuts");
        for (const auto mode : {config::GuiWaveFullscreenMode::Focus, config::GuiWaveFullscreenMode::Overlay}) {
            auto fullscreenConfig = application.captureConfig();
            fullscreenConfig.gui.wave.fullscreenMode = mode;
            application.applyConfig(fullscreenConfig);
            runtime.enterWaveFullscreen();
            runtime.openApplicationSettings(); frame();
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
            beginFrame(); runtime.processWaveFullscreenInput(); runtime.drawApplicationSettings(); endFrame();
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); frame();
            // 第一次Escape打开草稿关闭弹窗，第二次Escape取消，草稿继续保留。
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true); frame();
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false); frame();
            require(runtime.waveFullscreenActive_, "settings Escape does not leave either fullscreen strategy");
            runtime.exitWaveFullscreen();
        }
        // 用真实鼠标点击标题栏关闭按钮，而不是直接调用关闭业务函数。
        const ImVec2 close(settings->Pos.x + settings->Size.x - ImGui::GetFontSize(),
                           settings->Pos.y + settings->TitleBarHeight * 0.5F);
        auto& io = ImGui::GetIO();
        io.AddMousePosEvent(close.x, close.y); io.AddMouseButtonEvent(0, true); frame();
        io.AddMouseButtonEvent(0, false); frame(); frame();
        require(runtime.settingsCloseRequested_ && runtime.settingsOpen_, "title close protects unapplied draft");
        io.AddKeyEvent(ImGuiKey_Escape, true); frame();
        io.AddKeyEvent(ImGuiKey_Escape, false); frame();
        require(runtime.settingsOpen_ && !runtime.settingsCloseRequested_, "escape continues editing modal draft");
        runtime.settingsDraft_->refresh(original);
        for (const auto size : {ImVec2(820, 680), ImVec2(360, 400)}) {
            ImGui::SetWindowSize(settings, size);
            for (int page = 0; page < 4; ++page) {
                runtime.settingsPage_ = static_cast<config::SettingsPage>(page);
                frame(); frame();
                require(settings->Active && settings->Size.x >= 320, "all settings pages render at narrow size");
            }
        }
        runtime.settingsDraft_->value.gui.showAppHeader = !original.gui.showAppHeader;
        runtime.applySettingsDraft(false, false);
        frame();
        require(application.captureConfig().gui.showAppHeader != original.gui.showAppHeader, "settings apply executed at frame boundary");
        require(!runtime.settingsDraft_->hasChanges() && application.docks().configState().dirty, "apply refreshes draft without claiming saved");
        // 设置窗口存在但失焦时，波形重新取得键盘所有权。
        beginFrame();
        ImGui::Begin("波形焦点测试"); ImGui::SetWindowFocus(); ImGui::End();
        runtime.drawApplicationSettings(); endFrame();
        require(!runtime.settingsOwnInput(), "wave refocus restores shortcut ownership");
        io.AddMousePosEvent(settings->Pos.x + 30, settings->Pos.y + 65);
        io.AddMouseButtonEvent(0, true);
        io.AddKeyEvent(ImGuiKey_Space, true);
        beginFrame();
        require(runtime.settingsOwnInput(), "same-frame mouse target protects settings keyboard input before Begin");
        runtime.drawApplicationSettings(); endFrame();
        io.AddMouseButtonEvent(0, false); io.AddKeyEvent(ImGuiKey_Space, false); frame();
        const auto root = std::filesystem::temp_directory_path() /
            ("protoscope-settings-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root);
        const auto path = root / "config.yaml";
        std::string error;
        require(runtime.configStore_.save(path, application.captureConfig(), error), "save transaction fixture");
        application.docks().configState().loadedFromPath = path.string();
        runtime.configSnapshot_ = runtime.configStore_.snapshot(path);
        const auto savedBaseline = runtime.configSnapshot_.contents;
        { std::ofstream external(path); external << "app: {fps_limit: 321}\n"; }
        const auto externalContents = runtime.configStore_.snapshot(path).contents;
        runtime.settingsOverwriteSnapshot_ = runtime.configStore_.snapshot(path);
        runtime.settingsDraft_->refresh(application.captureConfig());
        runtime.settingsDraft_->value.gui.window.width = 0;
        application.docks().configState().autoSaveEnabled = true;
        application.docks().markDirty("test");
        runtime.applySettingsDraft(true, false);
        require(!runtime.maybeAutoSave() && runtime.configSnapshot_.contents == savedBaseline,
                "queued overwrite cannot authorize autosave or advance baseline");
        runtime.processSettingsActions();
        require(runtime.configStore_.snapshot(path).contents == externalContents &&
                runtime.settingsDraft_->value.gui.window.width == 0 && !runtime.settingsCloseAfterApply_ &&
                !runtime.settingsSaveAfterApply_ && !runtime.settingsOverwriteSnapshot_,
                "invalid overwrite preserves external file and ends transaction intentions");
        // 有效应用后使用错误快照触发落盘冲突，原草稿和编辑基线必须仍在。
        runtime.settingsDraft_->refresh(application.captureConfig());
        const auto beforeSave = runtime.settingsDraft_->baseline.gui.showAppHeader;
        runtime.settingsDraft_->value.gui.showAppHeader = !beforeSave;
        runtime.settingsOverwriteSnapshot_ = runtime.configSnapshot_; // 旧快照：模拟应用期间磁盘变化。
        runtime.applySettingsDraft(true, true); runtime.processSettingsActions();
        require(runtime.settingsPreserveSaveDraft_ && runtime.settingsDraft_->hasChanges() &&
                runtime.settingsDraft_->baseline.gui.showAppHeader == beforeSave && runtime.settingsOpen_ &&
                !runtime.settingsCloseAfterApply_, "persist failure keeps original draft baseline and does not close");
        runtime.settingsExternalConflict_ = false;
        runtime.settingsSaveAfterApply_ = runtime.settingsCloseAfterApply_ = false;
        application.docks().configState().autoSaveEnabled = false;
        runtime.settingsOpen_ = false;
        ImRect marker;
        for (int n = 0; n < 60; ++n) {
            if (n > 0) io.AddMousePosEvent(marker.GetCenter().x, marker.GetCenter().y);
            beginFrame();
            ImGui::Begin("帮助标记验证");
            ImGui::TextUnformatted("字段");
            drawDockHelpMarker("单位：毫秒；默认：5000；范围：非负；0无最小间隔。长文本自动换行，不影响草稿。");
            marker = GImGui->LastItemData.Rect;
            ImGui::End();
            if (n == 59) {
                const auto* tooltip = ImGui::FindWindowByName("##Tooltip_00");
                require(tooltip && tooltip->Active && tooltip->Size.x <= ImGui::GetFontSize() * 34,
                        "real hover opens wrapped help tooltip");
            }
            endFrame();
        }
        application.applyConfig(original);
    }
    // Logging 设置页必须提供逐帧历史策略与范围枚举控件：白名单/校验/恢复默认已就位，仅 UI 入口缺失会阻断用户配置。
    static void verifyLoggingHistorySwitchSettings(app::Application& application, GuiRuntime& runtime)
    {
        const auto original = application.captureConfig();
        runtime.openApplicationSettings();
        runtime.settingsPage_ = config::SettingsPage::Logging;
        auto frame = [&] {
            runtime.processSettingsActions();
            beginFrame(); runtime.drawApplicationSettings(); endFrame();
        };
        frame(); frame();
        require(runtime.settingsDraft_.has_value(), "logging settings draft must exist");
        auto& draft = runtime.settingsDraft_->value;
        // 直接改草稿枚举，再应用；断言应用路径与白名单生效。
        draft.gui.historyOnParsedSwitch = config::TransferLogHistoryOnParsedSwitch::UseHistory;
        draft.gui.historyScope = config::TransferLogHistoryScope::RetainedCapture;
        runtime.applySettingsDraft(false, false);
        frame();
        require(application.captureConfig().gui.historyOnParsedSwitch ==
                    config::TransferLogHistoryOnParsedSwitch::UseHistory &&
                application.captureConfig().gui.historyScope == config::TransferLogHistoryScope::RetainedCapture,
            "logging page history switch enums must apply to runtime config");
        // 恢复默认：枚举回到 ask / retained_rows。
        runtime.settingsDraft_->value.gui.historyOnParsedSwitch =
            config::TransferLogHistoryOnParsedSwitch::Ask;
        runtime.settingsDraft_->value.gui.historyScope = config::TransferLogHistoryScope::RetainedRows;
        runtime.applySettingsDraft(false, false);
        frame();
        require(application.captureConfig().gui.historyOnParsedSwitch ==
                    config::TransferLogHistoryOnParsedSwitch::Ask &&
                application.captureConfig().gui.historyScope == config::TransferLogHistoryScope::RetainedRows,
            "logging page history switch enums must restore to defaults");
        runtime.settingsOpen_ = false;
        application.applyConfig(original);
    }
    // 真实鼠标路径验证三态历史切换：原始->逐帧按钮打开宿主确认 modal，
    // 使用历史/不使用历史/取消（Escape）分别走通 Application 状态机与分批重建。
    static void verifyTransferHistorySwitchModal(app::Application& application, GuiRuntime& runtime)
    {
        using protoscope::config::TransferLogHistoryOnParsedSwitch;
        require(application.initialize(), "transfer history UI test init failed");
        const auto protocol = std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path() /
            "fixtures/protocols/stream_frame_only";
        require(application.reloadProtocolDirectory(protocol.generic_string(), true), "stream fixture load failed");
        // 载入含 RX 原始字节的 timeline，让“有历史可重建”路径可点。
        plot::RawCaptureFileData capture;
        capture.source = "probe";
        capture.events = {
            {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 10, .bytes = {0xAA, 0x55, 0x00, 0x07, 0x00, 0x00}, .endpoint = "probe"},
            {.type = plot::RawCaptureEventType::RxBytes, .timestampMs = 20, .bytes = {0xAA, 0x55, 0x00, 0x08, 0x00, 0x00}, .endpoint = "probe"},
        };
        std::string error;
        require(application.loadRawCaptureReplayTimeline(capture, error), error.c_str());
        require(application.seekRawCaptureReplay(2, error), error.c_str());
        for (int i = 0; i < 200 && application.rawCaptureReplayStatus().eventIndex < 2; ++i) application.pumpOnce();
        require(application.rawCaptureReplayStatus().eventIndex == 2, "timeline prefix did not reach eventIndex=2");
        require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
                "timeline browse starts in raw mode");

        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2(1280, 700);
        auto frame = [&](std::string* renderedText = nullptr) {
            application.pumpOnce();
            beginFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);
            ImGui::Begin("TransferTest", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
            if (renderedText) ImGui::LogToBuffer();
            runtime.drawTransferLogSection(400.0F);
            if (renderedText) {
                *renderedText = GImGui->LogBuffer.c_str();
                ImGui::LogFinish();
            }
            ImGui::End();
            endFrame();
        };
        // 通过鼠标坐标点击指定矩形中心：位置与按下同一帧输入，释放再一帧。
        auto clickRect = [&frame](const ImRect& rect) {
            const auto center = rect.GetCenter();
            ImGui::GetIO().AddMousePosEvent(center.x, center.y);
            frame(); // 先让 HoveredWindow 更新；移动与按下不能依赖上一帧的命中窗口。
            ImGui::GetIO().AddMouseButtonEvent(0, true);
            frame();
            ImGui::GetIO().AddMouseButtonEvent(0, false);
            frame();
        };
        // 通过现有 ImGui 导航矩形观察实际绘制项，不猜测表格列宽或固定像素。
        // 仅定位焦点，激活仍只由真实鼠标按下/释放完成。
        auto itemRect = [&](ImGuiWindow* window, ImGuiID id) {
            GImGui->NavWindow = window;
            ImGui::SetNavID(id, ImGuiNavLayer_Main, window->NavRootFocusScopeId, ImRect());
            frame();
            require(GImGui->NavIdIsAlive, "target button must be submitted in rendered frame");
            return ImGui::WindowRectRelToAbs(window, window->NavRectRel[ImGuiNavLayer_Main]);
        };
        auto historyPopupOpen = [] {
            for (const auto& popup : GImGui->OpenPopupStack) {
                if (popup.Window && std::strcmp(popup.Window->Name, "##transfer_history_switch") == 0)
                    return true;
            }
            return false;
        };
        auto clickItem = [&](const char* label) {
            frame();
            auto* modal = ImGui::FindWindowByName("##transfer_history_switch");
            const bool toolbar = std::strcmp(label, "原始") == 0 || std::strcmp(label, "逐帧") == 0;
            // release 帧关闭 popup 后 WasActive 仍可能为真；实际弹窗栈才表示当前阻挡状态。
            const bool popupOpen = historyPopupOpen();
            const bool confirmationPending = application.historySwitchConfirmationPending();
            if (toolbar && (popupOpen || confirmationPending)) return false;
            if (std::strcmp(label, "关闭转换状态") == 0) {
                for (int n = 0; n < GImGui->Windows.Size; ++n) {
                    auto* window = GImGui->Windows[n];
                    if (window->WasActive && std::strstr(window->Name, "##transfer_history_status")) {
                        clickRect(itemRect(window, window->GetID(label)));
                        return true;
                    }
                }
                return false;
            }
            if (!toolbar) {
                if (!modal || !modal->Active || !popupOpen || !confirmationPending) return false;
                clickRect(itemRect(modal, modal->GetID(label)));
                return true;
            }
            // 工具栏按钮 ID 包含 BeginTable 的作用域。

            ImGuiWindow* section = nullptr;
            for (int n = 0; n < GImGui->Windows.Size; ++n) {
                ImGuiWindow* window = GImGui->Windows[n];
                if (window->WasActive && std::strstr(window->Name, "##transfer_log_section") != nullptr) {
                    section = window;
                    break;
                }
            }
            if (section == nullptr) return false;
            const auto tableId = section->GetID("##transfer_log_toolbar");
            const auto buttonId = ImGui::GetIDWithSeed(label, nullptr, tableId);
            const auto rect = itemRect(section, buttonId);
            require(rect.Min.x >= section->ClipRect.Min.x && rect.Min.y >= section->ClipRect.Min.y &&
                rect.Max.x <= section->ClipRect.Max.x && rect.Max.y <= section->ClipRect.Max.y,
                "mode switch must be fully inside visible clipping region");
            clickRect(rect);
            return true;
        };

        // 用真实绘制文字区分筛选无结果与空数据，不通过清空偏好让数据重新出现。
        auto& receive = application.docks().receiveState();
        const auto originalFilter = receive.filter;
        std::string renderedText;
        const auto requireNoMatches = [&] {
            frame(&renderedText);
            require(runtime.transferLogRowsCache_.rows.empty(), "filter must have no visible matches");
            require(renderedText.find("没有匹配当前筛选条件的记录") != std::string::npos,
                    "filtered nonempty history must render the no-matches message");
            require(renderedText.find("暂无 TX/RX 原始数据") == std::string::npos &&
                        renderedText.find("暂无已解析帧") == std::string::npos,
                    "filtered history must not render a no-data message");
        };
        receive.filter = {.keyword = "missing-history-keyword", .status = dock::LogStatusFilter::All};
        requireNoMatches();
        require(receive.filter.keyword == "missing-history-keyword", "rendering must preserve keyword filter");
        receive.filter = {.keyword = "", .status = dock::LogStatusFilter::Tx};
        requireNoMatches();
        require(receive.filter.status == dock::LogStatusFilter::Tx, "rendering must preserve direction filter");
        receive.filter = originalFilter;

        // 第一次点击“原始”工具栏按钮：ask 策略打开宿主确认 modal，确认前保持原始模式。
        for (int i = 0; i < 3; ++i) frame();
        require(clickItem("原始"), "raw/parsed toolbar button must be clickable");
        require(application.historySwitchConfirmationPending(), "toolbar click must open history switch confirmation");
        require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
                "confirmation must keep raw mode before resolve");
        require(ImGui::FindWindowByName("##transfer_history_switch") != nullptr,
                "history switch modal must be visible");
        // 打开状态下直接点“原始”不得重复入队或关闭 modal。
        require(!clickItem("原始"), "background toolbar click must not reach while modal is open");
        require(application.historySwitchConfirmationPending(), "modal must stay open after background click");

        // Escape 等同取消：保持原始模式。
        io.AddKeyEvent(ImGuiKey_Escape, true);
        frame();
        io.AddKeyEvent(ImGuiKey_Escape, false);
        frame();
        require(!application.historySwitchConfirmationPending() &&
                    application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
                "Escape must cancel and keep raw mode");

        // 再次点击按钮并选择“不使用历史”：切到逐帧但不重建。
        require(clickItem("原始"), "second toolbar click must reopen confirmation");
        require(application.historySwitchConfirmationPending(), "confirmation must reopen");
        const auto framesBeforeReject = application.docks().receiveState().frameRows.size();
        require(clickItem("不使用历史"), "skip-history button must be clickable");
        require(!application.historySwitchConfirmationPending(), "skip-history must close modal");
        require(!historyPopupOpen(), "skip-history release frame must close the real popup stack");
        require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::ParsedFrames,
                "skip-history must switch to parsed view");
        require(application.docks().receiveState().frameRows.size() == framesBeforeReject,
                "skip-history must preserve existing frames without rebuilding");

        // 切回原始后再点击按钮并选择“使用历史”：按 timeline 前缀分批重建并替换逐帧结果。
        require(clickItem("逐帧"), "parsed->raw switch must go through directly");
        require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
                "switch back to raw must be direct");
        require(!application.historySwitchConfirmationPending() && !historyPopupOpen(),
                "parsed->raw mouse click must not open a confirmation");
        require(clickItem("原始"), "third toolbar click must reopen confirmation");
        require(application.historySwitchConfirmationPending(), "confirmation must reopen for accept path");
        require(clickItem("使用历史"), "use-history button must be clickable");
        require(!application.historySwitchConfirmationPending(), "use-history must close modal");
        for (int i = 0; i < 50 && application.docks().receiveState().frameRows.empty(); ++i) application.pumpOnce();
        require(!application.docks().receiveState().frameRows.empty(), "use-history must rebuild frames");
        for (const auto& row : application.docks().receiveState().frameRows) {
            require(row.timestampMs == 10 || row.timestampMs == 20, "future events must not enter history frames");
        }

        receive.filter = {.keyword = "missing-history-keyword", .status = dock::LogStatusFilter::All};
        requireNoMatches();
        receive.filter = originalFilter;

        // seek 必须使旧确认失效并回原始视图；有历史时再次请求逐帧仍需询问。
        require(application.seekRawCaptureReplay(1, error), error.c_str());
        require(!application.historySwitchConfirmationPending(), "seek must invalidate pending confirmation");
        require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
                "seek must return to raw mode");
        for (int i = 0; i < 200 && application.rawCaptureReplayStatus().eventIndex < 1; ++i) application.pumpOnce();
        require(!application.docks().receiveState().rows.empty(), "seek prefix must retain raw history");
        require(clickItem("原始"), "post-seek toolbar click must reopen confirmation");
        require(application.historySwitchConfirmationPending() &&
                    application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
                "有历史时每次切换都必须询问，seek 不能绕过询问");
        require(clickItem("取消"), "cancel button must be clickable");
        require(!application.historySwitchConfirmationPending(), "cancel button must close confirmation");

        // 小视口仍通过真实鼠标打开/关闭 modal，尺寸基于实际工作区而非固定像素。
        for (const auto size : {ImVec2(1280, 700), ImVec2(480, 360), ImVec2(320, 240)}) {
            io.DisplaySize = size;
            frame(); frame();
            require(clickItem("原始"), "small viewport toolbar must open modal by mouse");
            frame(); frame();
            auto* modal = ImGui::FindWindowByName("##transfer_history_switch");
            require(modal && modal->Active && historyPopupOpen(), "small viewport modal must be visible");
            const auto* viewport = ImGui::GetMainViewport();
            require(modal->Size.x <= viewport->WorkSize.x * 0.90F + 1.0F &&
                    modal->Size.y <= viewport->WorkSize.y * 0.80F + 1.0F,
                    "history modal must fit viewport work-area constraints");
            require((modal->Flags & ImGuiWindowFlags_AlwaysAutoResize) != 0,
                    "history modal must use local auto resize");
            for (const auto* label : {"使用历史", "不使用历史", "取消"}) {
                const auto rect = itemRect(modal, modal->GetID(label));
                require(rect.Min.x >= viewport->WorkPos.x && rect.Min.y >= viewport->WorkPos.y &&
                        rect.Max.x <= viewport->WorkPos.x + viewport->WorkSize.x &&
                        rect.Max.y <= viewport->WorkPos.y + viewport->WorkSize.y,
                        "history modal buttons must remain inside viewport");
            }
            require(clickItem("取消"), "small viewport cancel button must accept a real mouse click");
            require(!application.historySwitchConfirmationPending(), "small viewport click must resolve cancellation");
        }
        io.DisplaySize = ImVec2(1280, 700);
        frame(); frame();

        // modal 已显示时来源失效，不能只检查 Application 标记：必须关闭 ImGui 自身弹窗栈。
        require(clickItem("原始") && historyPopupOpen(), "invalidation test must open the real modal");
        require(application.seekRawCaptureReplay(1, error), error.c_str());
        frame();
        require(!historyPopupOpen(), "seek must close the displayed ImGui history popup");
        frame();
        auto* invalidatedModal = ImGui::FindWindowByName("##transfer_history_switch");
        require(invalidatedModal && !invalidatedModal->Active && !historyPopupOpen(),
                "invalidated history modal must stop submitting immediately after popup closure");
        // 失效帧已 Begin 后才 CloseCurrentPopup；下一帧 WasActive 仍记录该提交帧。
        // 再推进一帧核验上一帧也未提交窗口，保留原非活跃断言而非放宽它。
        frame();
        require(invalidatedModal && !invalidatedModal->WasActive,
                "invalidated history modal must no longer be active");
        require(!clickItem("使用历史"), "invalidated modal must not submit an old action");
        require(application.docks().receiveState().displayMode == dock::TransferLogDisplayMode::RawChunks,
                "old modal action must not change the new source mode");
        for (int i = 0; i < 200 && application.rawCaptureReplayStatus().eventIndex < 1; ++i) application.pumpOnce();
        require(clickItem("原始") && historyPopupOpen(), "new source must reopen a fresh real confirmation");
        // 两次 GUI 帧之间重新导入并产生新请求：旧窗口先关闭，不消费新请求，再下一帧打开。
        const auto oldConfirmation = application.historySwitchConfirmationId();
        plot::RawCaptureFileData replacement;
        replacement.events = {{.type = plot::RawCaptureEventType::RxBytes,
            .bytes = {0xAA, 0x55, 0x00, 0x07, 0x00, 0x00}}};
        application.unloadRawCaptureReplayTimeline();
        require(application.importRawRecords(replacement, error), error.c_str());
        application.requestParsedHistorySwitch();
        require(application.historySwitchConfirmationId() != oldConfirmation,
                "new confirmation must have a distinct identity");
        frame();
        require(!historyPopupOpen() && application.historySwitchConfirmationPending(),
                "stale popup must close without consuming the replacement request");
        frame();
        require(historyPopupOpen(), "replacement request must open its own confirmation");
        require(clickItem("取消"), "fresh replacement modal must remain clickable");
        require(!application.historySwitchConfirmationPending(), "fresh cancel must resolve only the fresh request");

        // 全量导入替换在同一 deque、同一行数下仍须重新筛选，不能保留旧行指针的匹配结果。
        receive.filter = {.keyword = "replacement-match", .status = dock::LogStatusFilter::Rx};
        replacement.events.front().endpoint = "replacement-match";
        require(application.importRawRecords(replacement, error), error.c_str());
        frame(&renderedText);
        require(runtime.transferLogRowsCache_.rows.size() == 1 &&
                    runtime.transferLogRowsCache_.rows.front() == &receive.rows.front() &&
                    runtime.transferLogRowsCache_.rows.front()->endpoint == "replacement-match",
                "matching replacement import must refresh the filtered row");
        replacement.events.front().endpoint = "replacement-unmatched";
        require(application.importRawRecords(replacement, error), error.c_str());
        requireNoMatches();
        require(receive.filter.keyword == "replacement-match" && receive.filter.status == dock::LogStatusFilter::Rx,
                "replacement import must preserve keyword and direction preferences");
        replacement.events.front().endpoint = "replacement-match";
        require(application.importRawRecords(replacement, error), error.c_str());
        frame(&renderedText);
        require(runtime.transferLogRowsCache_.rows.size() == 1 &&
                    runtime.transferLogRowsCache_.rows.front()->endpoint == "replacement-match",
                "matching data after no matches must reappear without changing filters");

        replacement.events.clear();
        require(application.importRawRecords(replacement, error), error.c_str());
        frame(&renderedText);
        require(receive.rows.empty() && runtime.transferLogRowsCache_.rows.empty(), "empty import must clear visible data");
        require(renderedText.find("暂无 TX/RX 原始数据") != std::string::npos &&
                    renderedText.find("没有匹配当前筛选条件的记录") == std::string::npos,
                "truly empty raw history must render the no-data message");
        receive.filter = originalFilter;
        const auto noStream = std::filesystem::absolute(std::filesystem::path(__FILE__)).parent_path() /
            "fixtures/protocols/import_source_label";
        require(application.reloadProtocolDirectory(noStream.generic_string(), true, false), "no-stream UI fixture failed");
        receive.frameRows.push_back({.message = "cached-frame-without-schema"});
        receive.displayMode = dock::TransferLogDisplayMode::ParsedFrames;
        frame(&renderedText);
        require(renderedText.find("当前协议没有 stream schema") != std::string::npos &&
                !receive.frameRows.empty(), "cached frames must not hide unavailable schema reason");
        receive.filter = {.keyword = "missing-history-keyword", .status = dock::LogStatusFilter::All};
        requireNoMatches();
        require(renderedText.find("当前协议没有 stream schema") != std::string::npos,
                "schema reason and filter no-matches message must coexist");
        receive.filter = originalFilter;
        require(application.reloadProtocolDirectory(protocol.generic_string(), true), "progress UI schema reload failed");
        auto progressConfig = application.captureConfig();
        progressConfig.gui.historyOnParsedSwitch = TransferLogHistoryOnParsedSwitch::UseHistory;
        progressConfig.gui.historyScope = config::TransferLogHistoryScope::RetainedRows;
        application.applyConfig(progressConfig);
        replacement.events = {{.type = plot::RawCaptureEventType::RxBytes,
            .bytes = std::vector<std::uint8_t>(4 * 1024 * 1024, 0), .endpoint = "progress"}};
        require(application.importRawRecords(replacement, error), error.c_str());
        receive.pauseScroll = true;
        receive.frameRows.push_back({.message = "previous-progress-result"});
        application.requestParsedHistorySwitch();
        require(application.transferHistoryStatus() && application.transferHistoryStatus()->running(),
            "UI first status must be available before pump");
        io.DisplaySize = ImVec2(360, 480);
        frame(&renderedText);
        require(renderedText.find("转换状态：解析历史") != std::string::npos &&
            renderedText.find("上次结果") != std::string::npos && renderedText.find("当前记录字节") != std::string::npos,
            "small viewport must render phase, previous results and byte progress");
        const auto bytesBefore = application.transferHistoryStatus()->currentBytesDone;
        frame(&renderedText);
        require(receive.pauseScroll && application.transferHistoryStatus()->currentBytesDone > bytesBefore,
            "pause scrolling must not freeze conversion progress");
        receive.frameRows.clear();
        ++receive.frameRowsVersion;
        frame(&renderedText);
        require(renderedText.find("正在转换，尚无本次逐帧结果") != std::string::npos &&
            renderedText.find("暂无已解析帧") == std::string::npos, "running empty list must not report no frames");
        const auto requireStatusVisible = [&] {
            ImGuiWindow* statusWindow = nullptr;
            for (auto* window : GImGui->Windows)
                if (window->Active && std::strstr(window->Name, "##transfer_history_status")) statusWindow = window;
            require(statusWindow != nullptr, "status child must be active");
            const auto clip = statusWindow->ClipRect;
            const auto contentMin = statusWindow->DC.CursorStartPos;
            const auto contentMax = statusWindow->DC.CursorMaxPos;
            require(contentMin.x >= clip.Min.x && contentMin.y >= clip.Min.y &&
                contentMax.x <= clip.Max.x + 1.0F && contentMax.y <= clip.Max.y + 1.0F,
                "actual status content including phase and reason must fit visible clipping region");
        };
        frame(&renderedText);
        requireStatusVisible();
        require(clickItem("逐帧"), "360px mode switch must accept real mouse cancellation");
        require(receive.displayMode == dock::TransferLogDisplayMode::RawChunks,
            "narrow viewport real click must switch back to raw");
        frame(&renderedText);
        requireStatusVisible();
        require(renderedText.find("转换状态：取消") != std::string::npos &&
            renderedText.find("切回原始") != std::string::npos, "cancelled status must show its actual reason");
        require(clickItem("关闭转换状态"), "terminal close button must accept real mouse interaction");
        require(!application.transferHistoryStatus(), "closing terminal UI must dismiss only status");
        io.DisplaySize = ImVec2(1280, 700);
        receive.pauseScroll = false;
        receive.filter = originalFilter;
        application.shutdown();
    }
    static void verify(app::Application& application, GuiRuntime& runtime)
    {
        for (const auto mode : {config::GuiWaveFullscreenMode::Overlay, config::GuiWaveFullscreenMode::Focus}) {
            auto config = application.captureConfig();
            config.gui.wave.fullscreenMode = mode;
            application.applyConfig(config);
            runtime.showCommDock_ = true;
            runtime.showLogDock_ = false;
            runtime.luaDockVisibility_["test"] = true;
            runtime.enterWaveFullscreen();
            require(runtime.waveFullscreenActive_, "enter fullscreen");
            runtime.openUnifiedDataExport(-1, true);
            require(runtime.waveFullscreenActive_ && !runtime.pendingBuiltinFileOperation_, "rejected operation stays fullscreen");
            auto exportSettings = config.gui.lastDataExport;
            exportSettings.valid = true;
            exportSettings.waveRange = 2;
            application.rememberDataExport(exportSettings);
            application.docks().waveState().view.showCursors = false;
            runtime.openUnifiedDataExport(-1, true);
            require(runtime.waveFullscreenActive_ && !runtime.pendingBuiltinFileOperation_,
                    "invalid cursor export stays fullscreen");
            application.rememberDataExport({});
            application.docks().waveState().view.showCursors = true;
            bool called = false;
            runtime.deferBuiltinFileOperation([&] {
                require(!runtime.waveFullscreenActive_, "operation requires normal layout");
                require(runtime.showCommDock_ && !runtime.showLogDock_ && runtime.luaDockVisibility_["test"],
                        "restore visibility");
                called = true;
            });
            runtime.dispatchBuiltinFileOperation();
            require(!called && runtime.waveFullscreenActive_, "not dispatched in request frame");
            runtime.prepareBuiltinFileOperation();
            require(!called && !runtime.waveFullscreenActive_, "restore before normal frame");
            beginFrame();
            ImGui::Begin("normal layout");
            ImGui::TextUnformatted("normal");
            ImGui::End();
            endFrame();
            runtime.dispatchBuiltinFileOperation();
            require(called && !runtime.waveFullscreenActive_, "dispatch after normal frame");
            runtime.dispatchBuiltinFileOperation();
            require(!runtime.pendingBuiltinFileOperation_, "operation consumed");

            runtime.enterWaveFullscreen();
            runtime.openUnifiedDataExport();
            require(runtime.pendingBuiltinFileOperation_ && runtime.waveFullscreenActive_, "unified entry is deferred");
            runtime.prepareBuiltinFileOperation();
            beginFrame();
            endFrame();
            runtime.dispatchBuiltinFileOperation();
            require(runtime.unifiedDataDialogOpen_ && runtime.focusUnifiedDataDialog_, "open and focus once");
            beginFrame();
            runtime.drawUnifiedDataDialog();
            const auto* dataWindow = ImGui::FindWindowByName("数据导入导出");
            require(dataWindow && GImGui->NavWindow == dataWindow, "focus operation window");
            require(!runtime.focusUnifiedDataDialog_, "focus request consumed");
            endFrame();
            runtime.unifiedDataDialogOpen_ = false;
            require(!runtime.waveFullscreenActive_, "closing stays normal");
        }
    }
    static void verifyStatus(app::Application& application, GuiRuntime& runtime)
    {
        auto& config = application.docks().configState();
        auto& wave = application.docks().waveState();
        application.setStatusMessage("import result");
        config.dirty = false;
        wave.view.fft.enabled = true;
        wave.analysisEpoch = wave.buffer.historyEpoch();
        wave.cachedFftKeyValid = true;
        wave.cachedFftFrame.message = "FFT calculation failed with a long diagnostic message";
        for (float width : {1280.0F, 480.0F, 320.0F}) {
            ImGui::GetIO().DisplaySize = ImVec2(width, 700);
            for (int frame = 0; frame < 4; ++frame) {
                beginFrame();
                wave.view.fftUpdatePending = frame % 2 == 0;
                runtime.drawStatusBar();
                const auto* bar = ImGui::FindWindowByName("状态栏");
                require(bar && bar->Size.y == kStatusBarHeight && bar->ScrollMax.y == 0,
                        "fixed status height without vertical scroll");
                require(config.statusMessage == "import result" && !config.dirty,
                        "wave status must not overwrite general message or dirty configuration");
                require(runtime.waveStatusPresenter_.display().fftError, "FFT error shown in status bar");
                endFrame(frame == 3 ? "status-" + std::to_string(static_cast<int>(width)) : "");
            }
        }
        wave.view.fft.enabled = false;
        beginFrame();
        runtime.drawStatusBar();
        require(runtime.waveStatusPresenter_.display().fft.empty(), "disable clears status in same frame");
        endFrame();
        ImGui::GetIO().DisplaySize = ImVec2(1000, 700);
        for (int frame = 0; frame < 2; ++frame) {
            beginFrame();
            runtime.waveDockRenderer_.drawOverlay(true, nullptr);
            const auto* overlay = ImGui::FindWindowByName("波形全屏##wave_fullscreen_overlay");
            require(overlay && overlay->Pos.y + overlay->Size.y <= 700 - kStatusBarHeight,
                    "overlay reserves status bar");
            endFrame();
        }
    }
    static void verifyRememberedPaths(app::Application& application, GuiRuntime& runtime,
                                     const config::ConfigStore& store)
    {
        const auto root = std::filesystem::temp_directory_path() /
            ("protoscope-ui-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const auto importDir = root / std::filesystem::u8path("中文 import");
        std::filesystem::create_directories(importDir);
        const auto configPath = root / "config.yaml";
        auto& state = application.docks().configState();
        state.loadedFromPath = fileDialogPathText(configPath);
        runtime.configSnapshot_ = store.snapshot(configPath);
        state.dirty = false;
        runtime.importCsvDataFromPath(importDir / "missing.csv");
        require(!runtime.csvDataImportError_.empty(), "import fixture must fail");
        require(application.runtimeConfig().gui.fileDialogs.lastImportDirectory == fileDialogPathText(importDir),
                "confirmed directory survives import failure");
        require(store.load(configPath).config.gui.fileDialogs.lastImportDirectory == fileDialogPathText(importDir),
                "confirmed directory survives restart");
        require(!state.dirty, "directory persistence must not mark other settings dirty");
        const auto beforeCancel = application.runtimeConfig().gui.fileDialogs;
        runtime.deferBuiltinFileOperation([] {});
        runtime.prepareBuiltinFileOperation();
        beginFrame();
        endFrame();
        runtime.dispatchBuiltinFileOperation();
        require(application.runtimeConfig().gui.fileDialogs.lastImportDirectory == beforeCancel.lastImportDirectory,
                "canceled operation does not remember browsing");
        // 损坏配置保存失败仍允许继续读取用户确认的文件，并保留本次会话目录。
        { std::ofstream output(configPath); output << "gui: ["; }
        runtime.importCsvDataFromPath(root / "missing-again.csv");
        require(!runtime.fileDialogPreferenceError_.empty() && !runtime.csvDataImportError_.empty(),
                "preference save failure does not block file operation");
        require(application.runtimeConfig().gui.fileDialogs.lastImportDirectory == fileDialogPathText(root),
                "save failure preserves in-memory preference");
        require(!state.dirty, "save error must not mark configuration dirty");
        std::ifstream input(configPath);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        require(text == "gui: [", "broken configuration preserved");
        runtime.fileDialogPreferenceError_.clear();
    }
    static void verifyFullscreenFft(app::Application& application, GuiRuntime& runtime)
    {
        for (const auto mode : {config::GuiWaveFullscreenMode::Overlay, config::GuiWaveFullscreenMode::Focus}) {
            auto config = application.captureConfig();
            config.gui.wave.fullscreenMode = mode;
            application.applyConfig(config);
            auto& wave = application.docks().waveState();
            wave.buffer.clear();
            wave.view.initialized = true;
            wave.view.defaultViewportPending = false;
            wave.view.autoFollowLatest = false;
            wave.view.viewMinTime = 0;
            wave.view.viewMaxTime = 1;
            wave.view.sampleFrequencyHz = 1024;
            wave.view.fft.enabled = true;
            wave.view.overviewWindowDragging = true;
            std::vector<plot::WaveSample> samples;
            for (int i = 0; i < 1024; ++i)
                samples.push_back({double(i) / 1024, std::sin(double(i) * 0.1)});
            wave.buffer.append(0, {{}, samples});
            const auto prepared = prepareWaveFrame(wave, 1200);
            wave.cachedFftFrame = plot::buildWaveFftFrame(*prepared.fullSnapshot, *prepared.displayData,
                                                        wave.view.fft, {1}, 0, 1, 1024);
            require(wave.cachedFftFrame.valid, "fullscreen FFT fixture");
            // 拖动期间尚未提交任务，夹具应使用最新目标键而非上次执行任务的键。
            wave.cachedFftKey = wave.fftTargetKey;
            wave.cachedFftKeyValid = true;
            runtime.enterWaveFullscreen();
            for (const auto size : {ImVec2(1280, 900), ImVec2(480, 600)}) {
                ImGui::GetIO().DisplaySize = size;
                std::vector<ImRect> baseline;
                for (int n = 0; n < 8; ++n) {
                    beginFrame();
                    const bool pending = n % 2 == 0;
                    wave.view.overviewWindowDragging = true;
                    wave.cachedFftKey.dataRevision = wave.buffer.analysisRevision() + (pending ? 1 : 0);
                    if (mode == config::GuiWaveFullscreenMode::Overlay) {
                        runtime.waveDockRenderer_.drawOverlay(true, nullptr);
                    } else {
                        ImGui::SetNextWindowPos(ImVec2(0, 0));
                        ImGui::SetNextWindowSize(ImVec2(size.x, size.y - kStatusBarHeight));
                        runtime.waveDockRenderer_.draw(runtime.showWaveDock_, true, nullptr, true);
                    }
                    require(wave.view.fftUpdatePending == pending, "fullscreen pending state exercised");
                    std::vector<ImRect> current;
                    for (int i = 0; i < GImPlot->Plots.GetBufSize(); ++i)
                        current.push_back(GImPlot->Plots.GetByIndex(i)->FrameRect);
                    require(current.size() >= 2, "fullscreen magnitude and phase present");
                    if (n == 3) baseline = current;
                    if (n > 3) {
                        require(current.size() == baseline.size(), "plot count stable");
                        for (std::size_t i = 0; i < current.size(); ++i)
                            require(current[i].Min.x == baseline[i].Min.x && current[i].Min.y == baseline[i].Min.y &&
                                    current[i].Max.x == baseline[i].Max.x && current[i].Max.y == baseline[i].Max.y,
                                    "actual fullscreen FFT geometry stable");
                    }
                    runtime.drawStatusBar();
                    const std::string label = mode == config::GuiWaveFullscreenMode::Overlay ? "overlay" : "focus";
                    endFrame(n == 7 ? label + "-" + std::to_string(static_cast<int>(size.x)) : "");
                }
            }
            runtime.exitWaveFullscreen();
        }
    }
};
}

int main(int argc, char** argv)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string_view(argv[i]) == "--capture") captureDirectory = argv[i + 1];
    withGl = !captureDirectory.empty();
    GLFWwindow* window = nullptr;
    if (withGl) {
        if (!glfwInit()) return 2;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        window = glfwCreateWindow(1280, 900, "UI verification", nullptr, nullptr);
        if (!window) return 2;
        glfwMakeContextCurrent(window);
    }
    ImGui::CreateContext();
    ImPlot::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.DisplaySize = ImVec2(1000, 700);
    io.DeltaTime = 1.0F / 60.0F;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    unsigned char* pixels;
    int width, height;
    if (const auto* windows = std::getenv("SystemRoot")) {
        const auto font = std::filesystem::path(windows) / "Fonts" / "msyh.ttc";
        if (std::filesystem::exists(font))
            io.Fonts->AddFontFromFileTTF(font.string().c_str(), 16.0F, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    }
    if (withGl) {
        if (!ImGui_ImplOpenGL3_Init("#version 130")) return 2;
    } else {
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    int result = 0;
    try {
        protoscope::app::Application app;
        protoscope::config::ConfigStore store;
        protoscope::ui::GuiRuntime runtime(app, store);
          protoscope::ui::GuiRuntimeTestAccess::verifySettings(app, runtime);
          protoscope::ui::GuiRuntimeTestAccess::verify(app, runtime);
          protoscope::ui::GuiRuntimeTestAccess::verifyRememberedPaths(app, runtime, store);
          protoscope::ui::GuiRuntimeTestAccess::verifyStatus(app, runtime);
          protoscope::ui::GuiRuntimeTestAccess::verifyFullscreenFft(app, runtime);
          protoscope::ui::GuiRuntimeTestAccess::verifyTransferHistorySwitchModal(app, runtime);
          protoscope::ui::GuiRuntimeTestAccess::verifyLoggingHistorySwitchSettings(app, runtime);
        std::cout << "file operation frame boundary and focus passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (withGl) ImGui_ImplOpenGL3_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    if (window) { glfwDestroyWindow(window); glfwTerminate(); }
    return result;
}
