#include "protoscope/ui/gui_runtime.hpp"

#include "../runtime/gui_runtime_detail.hpp"

namespace protoscope::ui {

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

void GuiRuntime::openTransferLogExportDialog() {
    openLogExportDialog(LogExportTarget::Transfer);
}

void GuiRuntime::openHostLogExportDialog() {
    openLogExportDialog(LogExportTarget::Host);
}

void GuiRuntime::openScriptLogExportDialog() {
    openLogExportDialog(LogExportTarget::Script);
}

void GuiRuntime::openLogExportDialog(LogExportTarget target) {
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
    (void)title;
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

std::vector<dock::ReceiveRow> GuiRuntime::logExportRows(LogExportTarget target) {
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

bool GuiRuntime::exportLogTargetToPath(LogExportTarget target, const std::filesystem::path& path) {
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
                                     std::string_view title) {
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

void GuiRuntime::drawLogExportFileDialog() {
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
