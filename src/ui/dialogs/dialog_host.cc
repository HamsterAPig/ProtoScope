#include "../runtime/gui_runtime_detail.hpp"

#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/keyboard_shortcuts.hpp"

namespace protoscope::ui {

namespace {

    bool dialogUsesCustomWindowOptions(const scripting::DialogRequest& dialog)
    {
        return dialog.window.width.has_value() || dialog.window.height.has_value() || dialog.window.x.has_value() ||
               dialog.window.y.has_value() || !dialog.window.resizable || !dialog.window.movable ||
               dialog.window.autoResize;
    }

    ImVec2 dialogViewportFallbackSize()
    {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        if (viewport == nullptr || viewport->Size.x <= 0.0F || viewport->Size.y <= 0.0F) {
            return ImVec2(1280.0F, 720.0F);
        }
        return viewport->Size;
    }

    ImVec2 dialogDefaultSize(const scripting::DialogRequest& dialog)
    {
        const ImVec2 viewportSize = dialogViewportFallbackSize();
        const float minWidth = 420.0F;
        const float minHeight = 200.0F;
        const float width = dialog.window.width.has_value() ? static_cast<float>(*dialog.window.width)
                                                            : std::max(minWidth, viewportSize.x * 0.40F);
        const float height = dialog.window.height.has_value() ? static_cast<float>(*dialog.window.height)
                                                              : std::max(minHeight, viewportSize.y * 0.25F);
        return ImVec2(width, height);
    }

    void applyDialogWindowOptions(const scripting::DialogRequest& dialog)
    {
        const ImVec2 initialSize = dialogDefaultSize(dialog);
        if (!dialog.window.autoResize) {
            ImGui::SetNextWindowSize(initialSize, ImGuiCond_Appearing);
        }
        if (dialog.window.x.has_value() || dialog.window.y.has_value()) {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImVec2 viewportPos = viewport != nullptr ? viewport->Pos : ImVec2(0.0F, 0.0F);
            const ImVec2 viewportSize = viewport != nullptr ? viewport->Size : dialogViewportFallbackSize();
            const float defaultX = viewportPos.x + std::max(0.0F, (viewportSize.x - initialSize.x) * 0.5F);
            const float defaultY = viewportPos.y + std::max(0.0F, (viewportSize.y - initialSize.y) * 0.3F);
            const float x =
                dialog.window.x.has_value() ? viewportPos.x + static_cast<float>(*dialog.window.x) : defaultX;
            const float y =
                dialog.window.y.has_value() ? viewportPos.y + static_cast<float>(*dialog.window.y) : defaultY;
            ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Appearing);
        }
    }

    ImGuiWindowFlags dialogWindowFlags(const scripting::DialogRequest& dialog)
    {
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
        if (dialog.window.autoResize) {
            flags |= ImGuiWindowFlags_AlwaysAutoResize;
        }
        if (!dialog.window.resizable) {
            flags |= ImGuiWindowFlags_NoResize;
        }
        if (!dialog.window.movable) {
            flags |= ImGuiWindowFlags_NoMove;
        }
        return flags;
    }

} // namespace

void GuiRuntime::requestAboutDialog()
{
    aboutDialogRequested_ = true;
}

void GuiRuntime::drawAboutDialog()
{
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

void GuiRuntime::requestShortcutHelpDialog()
{
    shortcutHelpDialogRequested_ = true;
}

void GuiRuntime::drawShortcutHelpDialog()
{
    constexpr const char* popupId = "快捷键说明";
    if (shortcutHelpDialogRequested_) {
        ImGui::OpenPopup(popupId);
        shortcutHelpDialogRequested_ = false;
    }

    ImGui::SetNextWindowSize(ImVec2(560.0F, 420.0F), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    ImGui::TextUnformatted("全局快捷键会在输入框和弹窗中自动让路。");
    ImGui::Separator();

    const auto drawSection = [](const char* title, const ShortcutScope scope) {
        if (!ImGui::CollapsingHeader(title, ImGuiTreeNodeFlags_DefaultOpen)) {
            return;
        }
        if (ImGui::BeginTable(title, 2, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("按键", ImGuiTableColumnFlags_WidthFixed, 120.0F);
            ImGui::TableSetupColumn("动作");
            ImGui::TableHeadersRow();
            for (const auto& shortcut : shortcutDescriptors()) {
                if (shortcut.scope != scope) {
                    continue;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(shortcut.label);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(shortcut.description);
            }
            ImGui::EndTable();
        }
    };

    drawSection("全局", ShortcutScope::Global);
    drawSection("波形 Dock", ShortcutScope::WaveDock);

    ImGui::Spacing();
    if (ImGui::Button("关闭")) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void GuiRuntime::startUpdateCheck()
{
    updateCheckDialogRequested_ = true;
    if (updateCheckInProgress_) {
        return;
    }

    updateCheckResult_.reset();
    updateCheckInProgress_ = true;
    updateCheckFuture_ = std::async(std::launch::async, [] { return checkForUpdates(); });
}

void GuiRuntime::drawUpdateCheckDialog()
{
    constexpr const char* popupId = "检查更新";
    if (updateCheckDialogRequested_) {
        ImGui::OpenPopup(popupId);
        updateCheckDialogRequested_ = false;
    }

    if (updateCheckInProgress_ && updateCheckFuture_.valid() &&
        updateCheckFuture_.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
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

void GuiRuntime::syncDialogQueue()
{
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

void GuiRuntime::drawDialogs()
{
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

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoSavedSettings;
    if (dialogUsesCustomWindowOptions(dialog)) {
        applyDialogWindowOptions(dialog);
        flags |= dialogWindowFlags(dialog);
    } else {
        flags |= ImGuiWindowFlags_AlwaysAutoResize;
    }
    if (!ImGui::BeginPopupModal(popupId.c_str(), nullptr, flags)) {
        return;
    }

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

void GuiRuntime::openRawCaptureImportDialog()
{
#if defined(_WIN32)
    const auto defaultPath = rawCaptureImportPath_.empty() ? executableDir_ / "captures" / "capture.psraw"
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

void GuiRuntime::openRawCaptureExportDialog()
{
    const auto& lua = application_.docks().luaState();
    const std::string baseName = lua.protocolName.empty() ? std::string("wave-capture") : lua.protocolName + "-wave";
    const auto defaultPath = rawCaptureExportPath_.empty() ? executableDir_ / "captures" / (baseName + ".psraw")
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

void GuiRuntime::openRawCaptureRecordingDialog()
{
    const auto& lua = application_.docks().luaState();
    const std::string baseName =
        lua.protocolName.empty() ? std::string("raw-recording") : lua.protocolName + "-raw-recording";
    const auto defaultPath = rawCaptureRecordingPath_.empty() ? executableDir_ / "captures" / (baseName + ".psraw")
                                                              : std::filesystem::path(rawCaptureRecordingPath_);
#if defined(_WIN32)
    std::string dialogError;
    const auto path = nativeFileDialog(window_,
                                       L"开始完整原始数据录制",
                                       L"ProtoScope Raw Capture (*.psraw)\0*.psraw\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       true,
                                       L"psraw",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        startRawCaptureRecordingToPath(*path);
    }
#else
    rawCaptureRecordingDialogOpen_ = true;
    rawCaptureRecordingDialogOpened_ = false;
    rawCaptureRecordingError_.clear();
    rawCaptureRecordingPath_ = defaultPath.generic_string();
#endif
}

void GuiRuntime::openTransferLogExportDialog()
{
    openLogExportDialog(LogExportTarget::Transfer);
}

void GuiRuntime::openHostLogExportDialog()
{
    openLogExportDialog(LogExportTarget::Host);
}

void GuiRuntime::openScriptLogExportDialog()
{
    openLogExportDialog(LogExportTarget::Script);
}

void GuiRuntime::openLogExportDialog(LogExportTarget target)
{
    const char* title = "收发数据日志";
    const char* defaultFileName = "transfer-log.log";
#if defined(_WIN32)
    const wchar_t* windowsTitle = L"导出收发数据日志";
#endif
    switch (target) {
        case LogExportTarget::Host:
            title = "系统日志";
            defaultFileName = "host-log.log";
#if defined(_WIN32)
            windowsTitle = L"导出系统日志";
#endif
            break;
        case LogExportTarget::Script:
            title = "Lua 日志";
            defaultFileName = "lua-log.log";
#if defined(_WIN32)
            windowsTitle = L"导出 Lua 日志";
#endif
            break;
        case LogExportTarget::Transfer:
        default:
            break;
    }

#if defined(_WIN32)
    (void) title;
#endif
    const auto defaultPath = executableDir_ / "logs" / defaultFileName;
#if defined(_WIN32)
    std::string dialogError;
    const auto path = nativeFileDialog(window_,
                                       windowsTitle,
                                       L"ProtoScope Log (*.log)\0*.log\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       true,
                                       L"log",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        exportLogTargetToPath(target, *path);
    }
#else
    logExportTarget_ = target;
    logExportPath_ = defaultPath.generic_string();
    logExportDialogTitle_ = title;
    logExportError_.clear();
    logExportDialogOpen_ = true;
    logExportDialogOpened_ = false;
#endif
}

void GuiRuntime::openElfStaticAddressDialog()
{
#if defined(_WIN32)
    const auto defaultPath =
        elfStaticAddressPath_.empty() ? executableDir_ : std::filesystem::path(elfStaticAddressPath_);
    std::string dialogError;
    const auto path =
        nativeFileDialog(window_,
                         L"打开 ELF/ElfStaticView 数据文件",
                         L"ELF/ElfStaticView Files "
                         L"(*.elf;*.out;*.axf;*.json;*.esv)\0*.elf;*.out;*.axf;*.json;*.esv\0All Files (*.*)\0*.*\0",
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

void GuiRuntime::importRawCaptureFromPath(const std::filesystem::path& path)
{
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

void GuiRuntime::exportRawCaptureToPath(const std::filesystem::path& path)
{
    // 核心流程：导出路径只在 UI 层选择，实际写入仍交给 Application 统一处理。
    rawCaptureExportPath_ = path.generic_string();
    std::string error;
    if (!application_.exportWaveRawCapture(path, error)) {
        rawCaptureExportError_ = error;
        application_.setStatusMessage("原始波形导出失败: " + error);
        return;
    }
    const auto& rawCapture = application_.docks().waveState().rawCapture;
    application_.setStatusMessage(rawCapture.truncated ? "原始波形导出成功（实时缓存已截断，仅包含最近原始字节）"
                                                       : "原始波形导出成功");
    rawCaptureExportDialogOpen_ = false;
    rawCaptureExportDialogOpened_ = false;
    rawCaptureExportError_.clear();
}

void GuiRuntime::startRawCaptureRecordingToPath(const std::filesystem::path& path)
{
    // 核心流程：菜单只负责选择完整录制路径，录制状态和写入错误统一收口到 Application。
    rawCaptureRecordingPath_ = path.generic_string();
    std::string error;
    if (!application_.startRawCaptureRecording(path, error)) {
        rawCaptureRecordingError_ = error;
        application_.setStatusMessage("完整原始数据录制启动失败: " + error);
        return;
    }
    rawCaptureRecordingDialogOpen_ = false;
    rawCaptureRecordingDialogOpened_ = false;
    rawCaptureRecordingError_.clear();
}

std::vector<dock::ReceiveRow> GuiRuntime::logExportRows(LogExportTarget target)
{
    auto& docks = application_.docks();
    switch (target) {
        case LogExportTarget::Transfer: {
            const auto& receive = docks.receiveState();
            const auto filteredRows = dock::filteredLogRows(receive.rows, receive.filter, true);
            std::vector<dock::ReceiveRow> rows;
            rows.reserve(filteredRows.size());
            for (const auto* row : filteredRows) {
                rows.push_back(*row);
            }
            return rows;
        }
        case LogExportTarget::Host: {
            const auto& logState = docks.logState();
            const auto filteredRows = dock::filteredLogRows(logState.rows, logState.filter, false);
            std::vector<dock::ReceiveRow> rows;
            rows.reserve(filteredRows.size());
            for (const auto* row : filteredRows) {
                rows.push_back(*row);
            }
            return rows;
        }
        case LogExportTarget::Script: {
            const auto& scriptState = docks.scriptState();
            const auto filteredRows = dock::filteredLogRows(scriptState.rows, scriptState.filter, false);
            std::vector<dock::ReceiveRow> rows;
            rows.reserve(filteredRows.size());
            for (const auto* row : filteredRows) {
                rows.push_back(*row);
            }
            return rows;
        }
    }
    return {};
}

bool GuiRuntime::exportLogTargetToPath(LogExportTarget target, const std::filesystem::path& path)
{
    auto& docks = application_.docks();
    bool showTimestamps = true;
    bool showHex = false;
    std::string_view title = "收发数据日志";

    switch (target) {
        case LogExportTarget::Transfer: {
            const auto& receive = docks.receiveState();
            showTimestamps = receive.showTimestamps;
            showHex = receive.showHex;
            title = "收发数据日志";
            break;
        }
        case LogExportTarget::Host: {
            const auto& logState = docks.logState();
            showTimestamps = logState.showTimestamps;
            showHex = false;
            title = "系统日志";
            break;
        }
        case LogExportTarget::Script: {
            const auto& scriptState = docks.scriptState();
            showTimestamps = scriptState.showTimestamps;
            showHex = false;
            title = "Lua 日志";
            break;
        }
    }

    const auto rows = logExportRows(target);
    const bool exported = exportLogRowsToPath(path, rows, showTimestamps, showHex, title);
    if (exported) {
        logExportPath_ = path.generic_string();
        logExportDialogOpen_ = false;
        logExportDialogOpened_ = false;
    }
    return exported;
}

bool GuiRuntime::exportLogRowsToPath(const std::filesystem::path& path,
                                     std::span<const dock::ReceiveRow> rows,
                                     bool showTimestamps,
                                     bool showHex,
                                     std::string_view title)
{
    auto fail = [&](std::string message) {
        logExportError_ = std::string(title) + "导出失败: " + message;
        application_.setStatusMessage(logExportError_);
        return false;
    };

    if (path.empty()) {
        return fail("导出路径为空");
    }

    try {
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code directoryError;
            std::filesystem::create_directories(parent, directoryError);
            if (directoryError) {
                return fail("创建目录失败: " + directoryError.message());
            }
        }

        std::ofstream output(path, std::ios::binary);
        if (!output.is_open()) {
            return fail("无法打开文件");
        }

        output << dock::formatReceiveRowsText(rows, showTimestamps, showHex);
        if (!output.good()) {
            return fail("写入文件失败");
        }

        std::error_code absoluteError;
        const auto savedPath = std::filesystem::absolute(path, absoluteError);
        const auto displayPath = absoluteError ? path.generic_string() : savedPath.generic_string();
        if (rows.empty()) {
            application_.setStatusMessage("已导出空日志: " + displayPath);
        } else {
            application_.setStatusMessage(std::string(title) + "已导出: " + displayPath);
        }
        logExportError_.clear();
        return true;
    } catch (const std::exception& exception) {
        return fail(exception.what());
    }
}

void GuiRuntime::loadElfStaticAddressFromPath(const std::filesystem::path& path)
{
    static_cast<void>(loadElfStaticAddressFromPath(path, false, true));
}

bool GuiRuntime::loadElfStaticAddressFromPath(const std::filesystem::path& path,
                                              bool clearLoadedContextOnFailure,
                                              bool saveProtocolStateOnSuccess)
{
    // 核心流程：加载成功后清空符号下拉缓存，让 Lua 控件基于新模型重新查询。
    std::error_code absoluteError;
    const auto resolvedPath = std::filesystem::absolute(path, absoluteError);
    const auto loadPath = absoluteError ? path : resolvedPath;
    std::string error;
    if (!application_.loadElfStaticAddressFile(loadPath, error)) {
        elfStaticAddressError_ = error;
        application_.setStatusMessage("ELF/ElfStaticView 数据文件加载失败: " + error);
        if (clearLoadedContextOnFailure) {
            clearElfStaticAddressContext(false);
            elfStaticAddressError_ = error;
        }
        return false;
    }
    elfStaticAddressPath_ = loadPath.generic_string();
    std::error_code watchError;
    const bool exists = std::filesystem::exists(loadPath, watchError);
    elfStaticAddressWatch_.path = loadPath;
    elfStaticAddressWatch_.watching = true;
    elfStaticAddressWatch_.lastExists = exists && !watchError;
    elfStaticAddressWatch_.lastPollAtMs = 0;
    elfStaticAddressWatch_.pendingReload = false;
    elfStaticAddressWatch_.pendingReloadSinceMs = 0;
    elfStaticAddressWatch_.pendingStatusMessage.clear();
    if (elfStaticAddressWatch_.lastExists) {
        const auto lastWriteTime = std::filesystem::last_write_time(loadPath, watchError);
        if (!watchError) {
            elfStaticAddressWatch_.lastWriteTimeNs = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(lastWriteTime.time_since_epoch()).count());
        }
        const auto fileSize = std::filesystem::file_size(loadPath, watchError);
        if (!watchError) {
            elfStaticAddressWatch_.fileSize = fileSize;
        }
    }
    elfSymbolComboStates_.clear();
    elfStaticAddressDialogOpen_ = false;
    elfStaticAddressDialogOpened_ = false;
    elfStaticAddressError_.clear();
    if (saveProtocolStateOnSuccess && protocolWorkspaceLoaded_ && !activeWorkspaceProtocolKey_.empty()) {
        saveCurrentProtocolControlState();
    }
    return true;
}

void GuiRuntime::clearElfStaticAddressContext(bool clearDialogPath)
{
    // 核心流程：协议没有绑定 ELF 时必须清掉已加载模型和候选缓存，避免符号下拉串用上一个 Lua。
    application_.clearElfStaticAddressFile();
    elfStaticAddressWatch_ = ElfStaticAddressFileWatchState{};
    elfSymbolComboStates_.clear();
    elfStaticAddressError_.clear();
    if (clearDialogPath) {
        elfStaticAddressPath_.clear();
    }
}

void GuiRuntime::restoreElfStaticAddressForCurrentProtocol(const std::string& savedPath)
{
    if (savedPath.empty()) {
        clearElfStaticAddressContext(true);
        return;
    }

    // 核心流程：自动恢复失败时保留 YAML 路径作为下次对话框默认值，但清空旧模型防止跨 Lua 泄漏。
    elfStaticAddressPath_ = savedPath;
    static_cast<void>(loadElfStaticAddressFromPath(std::filesystem::path(savedPath), true, false));
}

void GuiRuntime::drawElfStaticAddressDialog()
{
    if (!elfStaticAddressDialogOpen_) {
        return;
    }

    const char* popupId = "打开 ELF/ElfStaticView 数据文件##elf_static_view";
    if (!elfStaticAddressDialogOpened_) {
        ImGui::OpenPopup(popupId);
        elfStaticAddressDialogOpened_ = true;
    }
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(popupId, nullptr, flags)) {
        ImGui::TextUnformatted("请输入 ELF 或 ElfStaticView 数据文件路径");
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

void GuiRuntime::drawRawCaptureFileDialogs()
{
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

    if (rawCaptureRecordingDialogOpen_) {
        const char* popupId = "开始完整原始数据录制##psraw_record";
        if (!rawCaptureRecordingDialogOpened_) {
            ImGui::OpenPopup(popupId);
            rawCaptureRecordingDialogOpened_ = true;
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::BeginPopupModal(popupId, nullptr, flags)) {
            ImGui::TextUnformatted("请输入完整录制 .psraw 文件路径");
            char buffer[1024]{};
            std::snprintf(buffer, sizeof(buffer), "%s", rawCaptureRecordingPath_.c_str());
            if (ImGui::InputText("路径", buffer, sizeof(buffer))) {
                rawCaptureRecordingPath_ = buffer;
            }
            if (!rawCaptureRecordingError_.empty()) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.90F, 0.35F, 0.35F, 1.0F), "%s", rawCaptureRecordingError_.c_str());
            }
            ImGui::Spacing();
            if (ImGui::Button("开始录制", ImVec2(90.0F, 0.0F))) {
                startRawCaptureRecordingToPath(rawCaptureRecordingPath_);
                if (!rawCaptureRecordingDialogOpen_) {
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("取消", ImVec2(90.0F, 0.0F))) {
                rawCaptureRecordingDialogOpen_ = false;
                rawCaptureRecordingDialogOpened_ = false;
                rawCaptureRecordingError_.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }
}

void GuiRuntime::drawLogExportFileDialog()
{
    if (!logExportDialogOpen_) {
        return;
    }

    const char* popupId = "导出日志##log_export";
    if (!logExportDialogOpened_) {
        ImGui::OpenPopup(popupId);
        logExportDialogOpened_ = true;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(popupId, nullptr, flags)) {
        ImGui::Text("请输入 %s 导出路径", logExportDialogTitle_.c_str());
        char buffer[1024]{};
        std::snprintf(buffer, sizeof(buffer), "%s", logExportPath_.c_str());
        if (ImGui::InputText("路径", buffer, sizeof(buffer))) {
            logExportPath_ = buffer;
        }
        if (!logExportError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.90F, 0.35F, 0.35F, 1.0F), "%s", logExportError_.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("导出", ImVec2(90.0F, 0.0F))) {
            if (exportLogTargetToPath(logExportTarget_, logExportPath_)) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(90.0F, 0.0F))) {
            logExportDialogOpen_ = false;
            logExportDialogOpened_ = false;
            logExportError_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}


} // namespace protoscope::ui
