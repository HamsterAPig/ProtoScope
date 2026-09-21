#include "../runtime/gui_runtime_detail.hpp"

#include "protoscope/plot/csv_data_file.hpp"
#include "protoscope/ui/algorithm_help.hpp"
#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/keyboard_shortcuts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>

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

    std::size_t activeAlgorithmHelpEntry(std::span<const std::size_t> matches, std::size_t ordinal)
    {
        if (matches.empty() || ordinal >= matches.size()) {
            return kNoAlgorithmHelpMatch;
        }
        return matches[ordinal];
    }

    template <typename Confirm>
    void drawPathModalDialog(bool& open,
                             bool& opened,
                             std::string& path,
                             std::string& error,
                             const char* popupId,
                             const char* prompt,
                             const char* confirmLabel,
                             Confirm&& onConfirm)
    {
        if (!open) {
            return;
        }
        if (!opened) {
            ImGui::OpenPopup(popupId);
            opened = true;
        }

        constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
        if (!ImGui::BeginPopupModal(popupId, nullptr, flags)) {
            return;
        }

        ImGui::TextUnformatted(prompt);
        char buffer[1024]{};
        std::snprintf(buffer, sizeof(buffer), "%s", path.c_str());
        if (ImGui::InputText("路径", buffer, sizeof(buffer))) {
            path = buffer;
        }
        if (!error.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.90F, 0.35F, 0.35F, 1.0F), "%s", error.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button(confirmLabel, ImVec2(90.0F, 0.0F))) {
            onConfirm(path);
            if (!open) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(90.0F, 0.0F))) {
            open = false;
            opened = false;
            error.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
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

void GuiRuntime::requestAlgorithmHelpDialog()
{
    algorithmHelpDialogRequested_ = true;
}

void GuiRuntime::drawAlgorithmHelpDialog()
{
    constexpr const char* popupId = "算法手册";
    if (algorithmHelpDialogRequested_) {
        ImGui::OpenPopup(popupId);
        algorithmHelpDialogRequested_ = false;
    }

    const ImVec2 viewportSize = dialogViewportFallbackSize();
    const ImVec2 maxSize((std::max)(1.0F, viewportSize.x * 0.92F), (std::max)(1.0F, viewportSize.y * 0.92F));
    const ImVec2 minSize((std::min)(560.0F, maxSize.x), (std::min)(360.0F, maxSize.y));
    ImGui::SetNextWindowSizeConstraints(minSize, maxSize);
    ImGui::SetNextWindowSize(ImVec2(760.0F, 560.0F), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(popupId, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    if (ImGui::BeginTable(
            "##algorithm_help_toolbar", 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
        ImGui::TableSetupColumn("搜索", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("上一个", ImGuiTableColumnFlags_WidthFixed, 68.0F);
        ImGui::TableSetupColumn("下一个", ImGuiTableColumnFlags_WidthFixed, 68.0F);
        ImGui::TableSetupColumn("匹配数", ImGuiTableColumnFlags_WidthFixed, 112.0F);
        ImGui::TableSetupColumn("关闭", ImGuiTableColumnFlags_WidthFixed, 56.0F);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::SetNextItemWidth(-1.0F);
        const bool searchChanged = ImGui::InputTextWithHint(
            "##algorithm_help_search", "搜索", algorithmHelpSearchBuffer_.data(), algorithmHelpSearchBuffer_.size());
        const std::string query = algorithmHelpSearchBuffer_.data();
        if (searchChanged || query != algorithmHelpLastQuery_) {
            algorithmHelpLastQuery_ = query;
            algorithmHelpMatches_ = findAlgorithmHelpMatches(query);
            algorithmHelpCurrentMatchOrdinal_ = algorithmHelpMatches_.empty() ? kNoAlgorithmHelpMatch : 0U;
            algorithmHelpScrollToCurrent_ = !algorithmHelpMatches_.empty();
        }

        ImGui::BeginDisabled(algorithmHelpMatches_.empty());
        ImGui::TableSetColumnIndex(1);
        if (ImGui::Button("上一个", ImVec2(-1.0F, 0.0F))) {
            algorithmHelpCurrentMatchOrdinal_ =
                previousAlgorithmHelpMatchOrdinal(algorithmHelpMatches_, algorithmHelpCurrentMatchOrdinal_);
            algorithmHelpScrollToCurrent_ = algorithmHelpCurrentMatchOrdinal_ != kNoAlgorithmHelpMatch;
        }
        ImGui::TableSetColumnIndex(2);
        if (ImGui::Button("下一个", ImVec2(-1.0F, 0.0F))) {
            algorithmHelpCurrentMatchOrdinal_ =
                nextAlgorithmHelpMatchOrdinal(algorithmHelpMatches_, algorithmHelpCurrentMatchOrdinal_);
            algorithmHelpScrollToCurrent_ = algorithmHelpCurrentMatchOrdinal_ != kNoAlgorithmHelpMatch;
        }
        ImGui::EndDisabled();

        ImGui::TableSetColumnIndex(3);
        const std::size_t displayOrdinal =
            algorithmHelpCurrentMatchOrdinal_ == kNoAlgorithmHelpMatch ? 0U : algorithmHelpCurrentMatchOrdinal_ + 1U;
        ImGui::Text("匹配 %zu / %zu", displayOrdinal, algorithmHelpMatches_.size());

        ImGui::TableSetColumnIndex(4);
        if (ImGui::Button("关闭", ImVec2(-1.0F, 0.0F))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    const std::size_t activeEntry = activeAlgorithmHelpEntry(algorithmHelpMatches_, algorithmHelpCurrentMatchOrdinal_);
    if (ImGui::BeginChild("##algorithm_help_content", ImVec2(0.0F, 0.0F), ImGuiChildFlags_Borders)) {
        const auto entries = algorithmHelpEntries();
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const auto& entry = entries[index];
            ImGui::PushID(static_cast<int>(index));
            const bool active = index == activeEntry;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.86F, 0.36F, 1.0F));
            }
            ImGui::TextUnformatted(entry.title.data(), entry.title.data() + entry.title.size());
            if (active) {
                ImGui::PopStyleColor();
                if (algorithmHelpScrollToCurrent_) {
                    ImGui::SetScrollHereY(0.15F);
                    algorithmHelpScrollToCurrent_ = false;
                }
            }
            ImGui::Spacing();
            ImGui::TextWrapped("%.*s", static_cast<int>(entry.body.size()), entry.body.data());
            ImGui::Separator();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();

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
    drawUnifiedDataDialog();
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

void GuiRuntime::rememberFileDialogPath(const std::filesystem::path& path, bool exporting)
{
    auto preferences = application_.runtimeConfig().gui.fileDialogs;
    auto& directory = exporting ? preferences.lastExportDirectory : preferences.lastImportDirectory;
    directory = fileDialogPathText(path.parent_path());
    application_.rememberFileDialogPreferences(preferences);
    saveFileDialogPreferences();
}

void GuiRuntime::saveFileDialogPreferences(const config::DataExportConfig* lastExport)
{
    std::string error;
    const auto configPath = std::filesystem::u8path(application_.docks().configState().loadedFromPath);
    if (!configStore_.saveFileDialogPreferences(
            configPath, application_.runtimeConfig().gui.fileDialogs, error, lastExport)) {
        fileDialogPreferenceError_ = "保存文件目录偏好失败: " + error;
        application_.setStatusMessage(fileDialogPreferenceError_, true);
        return;
    }
    fileDialogPreferenceError_.clear();
    configSnapshot_ = configStore_.snapshot(configPath);
    application_.docks().configState().fileTimestampMs = configSnapshot_.timestampMs;
}

std::optional<std::filesystem::path> GuiRuntime::builtinFileDialog(
    GLFWwindow* window, const wchar_t* title, const wchar_t* filter,
    const std::filesystem::path& defaultPath, bool saveDialog,
    const wchar_t* extension, std::string& error, bool remember)
{
#if defined(_WIN32)
    const auto& preferences = application_.runtimeConfig().gui.fileDialogs;
    const auto directory = resolveFileDialogDirectory(
        remember ? (saveDialog ? preferences.lastExportDirectory : preferences.lastImportDirectory) : "",
        defaultPath.parent_path(), executableDir_);
    const auto fileName = defaultPath.filename();
    const auto selected = nativeCommonItemDialog(
        window, title, filter, directory, saveDialog, false, extension, error, &fileName);
    if (selected && remember) rememberFileDialogPath(*selected, saveDialog);
    return selected;
#else
    return std::nullopt;
#endif
}

void GuiRuntime::openUnifiedDataImport()
{
    if (application_.dataTransferStatus().active) return;
    if (deferBuiltinFileOperation([this] { openUnifiedDataImport(); })) return;
    focusUnifiedDataDialog_ = true;
    unifiedDataError_.clear();
    unifiedExportMode_ = false;
    importParseWaveform_ = false;
    unifiedDataDialogOpen_ = true;
#if defined(_WIN32)
    const auto path = builtinFileDialog(window_, L"导入数据",
        L"ProtoScope Data (*.csv;*.psraw;*.pssession)\0*.csv;*.psraw;*.pssession\0All Files (*.*)\0*.*\0",
        executableDir_ / "captures" / "", false, L"", unifiedDataError_);
    if (path) {
        unifiedDataPath_ = fileDialogPathText(*path);
        application_.startDataImport(*path, unifiedDataError_);
    }
#endif
}

void GuiRuntime::openUnifiedDataExport(int content, bool useLast)
{
    if (application_.dataTransferStatus().active) return;
    if (useLast && !application_.runtimeConfig().gui.lastDataExport.valid) return;
    if (deferBuiltinFileOperation([this, content, useLast] { openUnifiedDataExport(content, useLast); })) return;
    focusUnifiedDataDialog_ = true;
    unifiedExportMode_ = true;
    unifiedDataDialogOpen_ = true;
    unifiedDataError_.clear();
    dataExportDraft_ = application_.runtimeConfig().gui.lastDataExport;
    if (!dataExportDraft_.valid) dataExportDraft_ = {};
    if (content >= 0) dataExportDraft_.content = content;
    if (dataExportDraft_.content == 3) dataExportDraft_.format = 3;
    else if (dataExportDraft_.format == 3 || (dataExportDraft_.content == 0 && dataExportDraft_.format > 1) ||
             (dataExportDraft_.content == 2 && dataExportDraft_.format == 1)) dataExportDraft_.format = 0;
    if (useLast) submitUnifiedDataExport();
}

bool validateWaveCursorExport(const plot::WaveViewState& view, std::string& error)
{
    // 固定仅约束游标交互；可见且时间有效的双游标即可导出闭区间。
    if (!view.showCursors || !view.cursors[0].enabled || !view.cursors[1].enabled) {
        error = "请先显示两个波形游标";
        return false;
    }
    if (!std::isfinite(view.cursors[0].time) || !std::isfinite(view.cursors[1].time)) {
        error = "波形游标时间无效";
        return false;
    }
    return true;
}

void GuiRuntime::submitUnifiedDataExport()
{
    plot::CsvExportRange range;
    range.kind = static_cast<plot::CsvExportRangeKind>(dataExportDraft_.waveRange);
    const auto& view = application_.docks().waveState().view;
    range.currentViewMinTime = view.viewMinTime;
    range.currentViewMaxTime = view.viewMaxTime;
    range.cursorATime = view.cursors[0].time;
    range.cursorBTime = view.cursors[1].time;
    if ((dataExportDraft_.content == 0 || dataExportDraft_.content == 3) && dataExportDraft_.waveRange == 2 &&
        !validateWaveCursorExport(view, unifiedDataError_)) {
        return;
    }
    if (dataExportDraft_.content != 0 && dataExportDraft_.recordRange == 2 &&
        dataRecordBeginMs_ == 0 && dataRecordEndMs_ == 0) {
        unifiedDataError_ = "请先指定收发时间段";
        return;
    }
    if (deferBuiltinFileOperation([this] { submitUnifiedDataExport(); })) return;
    const char* extension = dataExportDraft_.format == 0 ? ".csv" : dataExportDraft_.format == 1 ? ".psraw" :
                            dataExportDraft_.format == 2 ? ".log" : ".pssession";
#if defined(_WIN32)
    const auto directory = executableDir_ / "captures";
    const auto path = builtinFileDialog(window_, L"导出数据", L"All Files (*.*)\0*.*\0",
        directory / ("data" + std::string(extension)), true,
        dataExportDraft_.format == 0 ? L"csv" : dataExportDraft_.format == 1 ? L"psraw" :
        dataExportDraft_.format == 2 ? L"log" : L"pssession", unifiedDataError_);
    if (!path) return;
#else
    const auto path = std::optional<std::filesystem::path>(unifiedDataPath_);
#endif
    if (application_.startDataExport(*path, dataExportDraft_.content, dataExportDraft_.format, range,
        dataExportDraft_.recordRange, dataRecordBeginMs_, dataRecordEndMs_,
        dataExportDraft_.csvShape == 0 ? plot::WaveCsvShape::Wide : plot::WaveCsvShape::Long, unifiedDataError_)) {
        pendingDataExport_ = dataExportDraft_;
        pendingDataExport_.valid = true;
        pendingDataExport_.directory = fileDialogPathText(path->parent_path());
        pendingDataExportTask_ = application_.dataTransferStatus().id;
        unifiedDataPath_ = fileDialogPathText(*path);
    }
}

void GuiRuntime::drawUnifiedDataDialog()
{
    const auto status = application_.dataTransferStatus();
    if (pendingDataExportTask_ == status.id && status.complete && !status.active) {
        if (!status.canceled && status.error.empty()) {
            application_.rememberDataExport(pendingDataExport_);
            saveFileDialogPreferences(&pendingDataExport_);
        }
        pendingDataExportTask_ = 0;
    }
    if (!unifiedDataDialogOpen_) return;
    ImGui::SetNextWindowSize(ImVec2(520, 400), ImGuiCond_FirstUseEver);
    if (focusUnifiedDataDialog_) {
        ImGui::SetNextWindowFocus();
        focusUnifiedDataDialog_ = false;
    }
    if (!ImGui::Begin("数据导入导出", &unifiedDataDialogOpen_)) { ImGui::End(); return; }
    ImGui::TextWrapped("%s", unifiedDataPath_.c_str());
    if (status.active || (!unifiedExportMode_ && status.id != 0)) {
        if (status.awaitingConfirmation) {
            ImGui::Text("内容: %s%s", status.metadata.waveform ? "波形数据 " : "",
                        status.includesRecords ? "收发记录" : "");
            ImGui::TextWrapped("来源: %s", status.metadata.waveform ?
                status.metadata.waveform->source.c_str() : status.metadata.source.c_str());
            ImGui::TextWrapped("协议: %s", status.metadata.protocolName.c_str());
            ImGui::Text("完整性: %s%s%s", status.metadata.incomplete ? "不完整 " : "完整 ",
                status.metadata.truncated ? "历史已截断 " : "", status.metadata.filtered ? "范围筛选 " : "");
            if (status.includesRecords) ImGui::TextUnformatted(status.metadata.rxOnly ? "RX-only" : "RX / TX");
            if (status.metadata.waveform)
                ImGui::TextWrapped("波形范围: %s", status.metadata.waveform->rangeDescription.c_str());
            if (status.includesRecords) ImGui::TextWrapped("收发范围: %s", status.metadata.rangeDescription.c_str());
            if (status.includesRecords && !status.metadata.waveform) {
                ImGui::BeginDisabled(!application_.docks().luaState().loaded);
                ImGui::Checkbox("使用当前协议解析波形", &importParseWaveform_);
                ImGui::EndDisabled();
            }
            if (ImGui::Button("确认替换并导入")) application_.confirmDataImport(importParseWaveform_);
        } else {
            const float progress = status.total ? static_cast<float>(status.submitted) / static_cast<float>(status.total) :
                                   status.complete ? 1.0F : 0.0F;
            ImGui::ProgressBar(progress);
            ImGui::Text("%s: %llu / %llu", status.importing ? "已提交 / 已解析" : "已编码 / 总数",
                        static_cast<unsigned long long>(status.submitted),
                        static_cast<unsigned long long>(status.total));
            if (status.complete) ImGui::TextUnformatted(status.canceled ? "已取消" :
                status.error.empty() ? "任务完成" : "任务失败");
        }
        if (status.active && ImGui::Button("取消任务")) application_.cancelDataTransfer();
        if (!status.error.empty()) ImGui::TextWrapped("%s", status.error.c_str());
    } else if (unifiedExportMode_) {
        if (ImGui::Combo("内容", &dataExportDraft_.content, "波形数据\0收发原始记录\0逐帧分析结果\0完整现场\0")) {
            dataExportDraft_.format = dataExportDraft_.content == 3 ? 3 : 0;
        }
        if (dataExportDraft_.content == 3) ImGui::TextUnformatted("格式: .pssession");
        else {
            const char* formats[] = {"CSV", ".psraw", "可读日志"};
            if (ImGui::BeginCombo("格式", formats[dataExportDraft_.format])) {
                for (int f = 0; f < 3; ++f) {
                    if ((dataExportDraft_.content == 0 && f == 2) || (dataExportDraft_.content == 2 && f == 1)) continue;
                    if (ImGui::Selectable(formats[f], dataExportDraft_.format == f)) dataExportDraft_.format = f;
                }
                ImGui::EndCombo();
            }
        }
        if (dataExportDraft_.content == 0 || dataExportDraft_.content == 3) {
            ImGui::Combo("波形范围", &dataExportDraft_.waveRange, "全部保留历史\0当前横轴视图\0双游标闭区间\0");
            if (dataExportDraft_.content == 0 && dataExportDraft_.format == 0)
                ImGui::Combo("CSV 表形", &dataExportDraft_.csvShape, "宽表\0长表\0");
        }
        if (dataExportDraft_.content != 0) {
            ImGui::Combo("收发范围", &dataExportDraft_.recordRange, "全部保留历史\0当前筛选结果\0指定收发时间段\0");
            if (dataExportDraft_.recordRange == 2) {
                ImGui::InputScalar("开始时间 (ms)", ImGuiDataType_U64, &dataRecordBeginMs_);
                ImGui::InputScalar("结束时间 (ms)", ImGuiDataType_U64, &dataRecordEndMs_);
            }
        }
        if (ImGui::Button("选择文件并导出")) submitUnifiedDataExport();
    }
    if (unifiedExportMode_ && status.complete && !status.active) {
        ImGui::TextUnformatted(status.canceled ? "导出已取消" : status.error.empty() ? "导出完成" : "导出失败");
        if (!status.error.empty()) ImGui::TextWrapped("%s", status.error.c_str());
        ImGui::Text("数量: %llu", static_cast<unsigned long long>(status.submitted));
    }
    if (!unifiedDataError_.empty()) ImGui::TextWrapped("%s", unifiedDataError_.c_str());
    ImGui::End();
}

void GuiRuntime::openRawCaptureImportDialog()
{
    if (deferBuiltinFileOperation([this] { openRawCaptureImportDialog(); })) return;
#if defined(_WIN32)
    const auto defaultPath = rawCaptureImportPath_.empty() ? executableDir_ / "captures" / "capture.psraw"
                                                           : std::filesystem::u8path(rawCaptureImportPath_);
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
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

void GuiRuntime::openCsvDataImportDialog()
{
    if (deferBuiltinFileOperation([this] { openCsvDataImportDialog(); })) return;
#if defined(_WIN32)
    const auto defaultPath = csvDataImportPath_.empty() ? executableDir_ / "captures" / "data.csv"
                                                        : std::filesystem::u8path(csvDataImportPath_);
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"导入 CSV 数据",
                                       L"ProtoScope CSV (*.csv)\0*.csv\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       false,
                                       L"csv",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        importCsvDataFromPath(*path);
    }
#else
    csvDataImportError_.clear();
    if (csvDataImportPath_.empty()) {
        csvDataImportPath_ = (executableDir_ / "captures" / "data.csv").generic_string();
    }
    application_.setStatusMessage("当前平台暂未实现 CSV 数据文件对话框");
#endif
}

void GuiRuntime::openRawCaptureReplayTimelineDialog()
{
    if (deferBuiltinFileOperation([this] { openRawCaptureReplayTimelineDialog(); })) return;
#if defined(_WIN32)
    const auto defaultPath = rawCaptureReplayTimelinePath_.empty()
                                 ? executableDir_ / "captures" / "capture.psraw"
                                 : std::filesystem::u8path(rawCaptureReplayTimelinePath_);
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"载入原始回放时间轴",
                                       L"ProtoScope Replay (*.psraw;*.csv)\0*.psraw;*.csv\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       false,
                                       nullptr,
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        loadRawCaptureReplayTimelineFromPath(*path);
    }
#else
    rawCaptureReplayTimelineDialogOpen_ = true;
    rawCaptureReplayTimelineDialogOpened_ = false;
    rawCaptureReplayTimelineError_.clear();
    if (rawCaptureReplayTimelinePath_.empty()) {
        rawCaptureReplayTimelinePath_ = (executableDir_ / "captures" / "capture.psraw").generic_string();
    }
#endif
}

void GuiRuntime::openRawCaptureExportDialog()
{
    if (deferBuiltinFileOperation([this] { openRawCaptureExportDialog(); })) return;
    const auto& lua = application_.docks().luaState();
    const std::string baseName = lua.protocolName.empty() ? std::string("wave-capture") : lua.protocolName + "-wave";
    const auto defaultPath = rawCaptureExportPath_.empty() ? executableDir_ / "captures" / (baseName + ".psraw")
                                                           : std::filesystem::u8path(rawCaptureExportPath_);
#if defined(_WIN32)
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"导出当前缓存快照",
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

void GuiRuntime::openWaveCsvExportDialog()
{
    if (deferBuiltinFileOperation([this] { openWaveCsvExportDialog(); })) return;
    const auto& lua = application_.docks().luaState();
    const std::string baseName = lua.protocolName.empty() ? std::string("wave") : lua.protocolName + "-wave";
    const auto defaultPath = waveCsvExportPath_.empty() ? executableDir_ / "captures" / (baseName + ".csv")
                                                        : std::filesystem::u8path(waveCsvExportPath_);
#if defined(_WIN32)
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"导出波形 CSV",
                                       L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       true,
                                       L"csv",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        exportWaveCsvToPath(*path);
    }
#else
    waveCsvExportPath_ = defaultPath.generic_string();
    waveCsvExportError_.clear();
    application_.setStatusMessage("当前平台暂未实现波形 CSV 文件对话框");
#endif
}

void GuiRuntime::openRawCaptureCsvExportDialog()
{
    if (deferBuiltinFileOperation([this] { openRawCaptureCsvExportDialog(); })) return;
    const auto& lua = application_.docks().luaState();
    const std::string baseName =
        lua.protocolName.empty() ? std::string("raw-events") : lua.protocolName + "-raw-events";
    const auto defaultPath = rawCaptureCsvExportPath_.empty() ? executableDir_ / "captures" / (baseName + ".csv")
                                                              : std::filesystem::u8path(rawCaptureCsvExportPath_);
#if defined(_WIN32)
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"导出原始事件 CSV",
                                       L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       true,
                                       L"csv",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        exportRawCaptureCsvToPath(*path);
    }
#else
    rawCaptureCsvExportPath_ = defaultPath.generic_string();
    rawCaptureCsvExportError_.clear();
    application_.setStatusMessage("当前平台暂未实现原始事件 CSV 文件对话框");
#endif
}

void GuiRuntime::openRawCaptureRecordingDialog()
{
    if (application_.isRawCaptureRecording()) return;
    if (deferBuiltinFileOperation([this] { openRawCaptureRecordingDialog(); })) return;
    const auto& lua = application_.docks().luaState();
    const std::string baseName =
        lua.protocolName.empty() ? std::string("raw-recording") : lua.protocolName + "-raw-recording";
    const auto defaultPath = rawCaptureRecordingPath_.empty() ? executableDir_ / "captures" / (baseName + ".psraw")
                                                              : std::filesystem::u8path(rawCaptureRecordingPath_);
#if defined(_WIN32)
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
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

void GuiRuntime::openSessionPackageImportDialog()
{
    if (deferBuiltinFileOperation([this] { openSessionPackageImportDialog(); })) return;
#if defined(_WIN32)
    const auto defaultPath = sessionPackageImportPath_.empty() ? executableDir_ / "captures" / "session.pssession"
                                                               : std::filesystem::u8path(sessionPackageImportPath_);
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"导入现场会话包",
                                       L"ProtoScope Session Package (*.pssession)\0*.pssession\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       false,
                                       L"pssession",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        importSessionPackageFromPath(*path);
    }
#else
    sessionPackageImportDialogOpen_ = true;
    sessionPackageImportDialogOpened_ = false;
    sessionPackageImportError_.clear();
    if (sessionPackageImportPath_.empty()) {
        sessionPackageImportPath_ = (executableDir_ / "captures" / "session.pssession").generic_string();
    }
#endif
}

void GuiRuntime::openSessionPackageExportDialog()
{
    if (deferBuiltinFileOperation([this] { openSessionPackageExportDialog(); })) return;
    const auto& lua = application_.docks().luaState();
    const std::string baseName = lua.protocolName.empty() ? std::string("session") : lua.protocolName + "-session";
    const auto defaultPath = sessionPackageExportPath_.empty() ? executableDir_ / "captures" / (baseName + ".pssession")
                                                               : std::filesystem::u8path(sessionPackageExportPath_);
#if defined(_WIN32)
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"导出现场会话包",
                                       L"ProtoScope Session Package (*.pssession)\0*.pssession\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       true,
                                       L"pssession",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        exportSessionPackageToPath(*path);
    }
#else
    sessionPackageExportDialogOpen_ = true;
    sessionPackageExportDialogOpened_ = false;
    sessionPackageExportError_.clear();
    sessionPackageExportPath_ = defaultPath.generic_string();
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

void GuiRuntime::openRequestTraceExportDialog()
{
    if (deferBuiltinFileOperation([this] { openRequestTraceExportDialog(); })) return;
    const auto defaultPath = requestTraceExportPath_.empty() ? executableDir_ / "logs" / "request-trace.csv"
                                                             : std::filesystem::u8path(requestTraceExportPath_);
#if defined(_WIN32)
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"导出请求追踪",
                                       L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       true,
                                       L"csv",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        exportRequestTraceToPath(*path);
    }
#else
    requestTraceExportPath_ = defaultPath.generic_string();
    requestTraceExportError_.clear();
    requestTraceExportDialogOpen_ = true;
    requestTraceExportDialogOpened_ = false;
#endif
}

void GuiRuntime::openLogExportDialog(LogExportTarget target)
{
    if (deferBuiltinFileOperation([this, target] { openLogExportDialog(target); })) return;
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
    const auto path = builtinFileDialog(window_,
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
    if (deferBuiltinFileOperation([this] { openElfStaticAddressDialog(); })) return;
#if defined(_WIN32)
    const auto defaultPath =
        elfStaticAddressPath_.empty() ? executableDir_ / "" : std::filesystem::u8path(elfStaticAddressPath_);
    std::string dialogError;
    const auto path =
        builtinFileDialog(window_,
                         L"打开 ELF/ElfStaticView 数据文件",
                         L"ELF/ElfStaticView Files "
                         L"(*.elf;*.out;*.axf;*.json;*.esv)\0*.elf;*.out;*.axf;*.json;*.esv\0All Files (*.*)\0*.*\0",
                         defaultPath,
                         false,
                         nullptr,
                         dialogError, false);
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
    rawCaptureImportPath_ = fileDialogPathText(path);
    std::string error;
    const auto capture = plot::readRawCaptureFile(path, error);
    if (!capture.has_value()) {
        rawCaptureImportError_ = error;
        application_.setStatusMessage("原始波形导入失败: " + error);
        return;
    }
    std::error_code protocolEntryError;
    if (!std::filesystem::exists(configStore_.mainLuaPath(capture->protocolDir), protocolEntryError)) {
        rawCaptureImportError_ = "导入文件引用的协议目录不存在: " + capture->protocolDir;
        if (protocolEntryError) {
            rawCaptureImportError_ += " (" + protocolEntryError.message() + ")";
        }
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

void GuiRuntime::importCsvDataFromPath(const std::filesystem::path& path)
{
    csvDataImportPath_ = fileDialogPathText(path);
    std::string error;
    const auto kind = plot::detectCsvKind(path, error);
    if (kind == plot::CsvKind::Wave) {
        const auto data = plot::readWaveCsvFile(path, error);
        if (!data.has_value()) {
            csvDataImportError_ = error;
            application_.setStatusMessage("CSV 数据导入失败: " + error);
            return;
        }
        if (!application_.importWaveCsvData(*data, error)) {
            csvDataImportError_ = error;
            application_.setStatusMessage("CSV 数据导入失败: " + error);
            return;
        }
        application_.setStatusMessage("波形 CSV 导入成功");
        csvDataImportError_.clear();
        return;
    }

    if (kind != plot::CsvKind::RawEvents) {
        csvDataImportError_ = error.empty() ? "未知 CSV 类型" : error;
        application_.setStatusMessage("CSV 数据导入失败: " + csvDataImportError_);
        return;
    }

    const auto capture = plot::readRawCaptureCsvFile(path, error);
    if (!capture.has_value()) {
        csvDataImportError_ = error;
        application_.setStatusMessage("CSV 数据导入失败: " + error);
        return;
    }
    std::error_code protocolEntryError;
    if (!capture->protocolDir.empty() &&
        !std::filesystem::exists(configStore_.mainLuaPath(capture->protocolDir), protocolEntryError)) {
        csvDataImportError_ = "导入文件引用的协议目录不存在: " + capture->protocolDir;
        if (protocolEntryError) {
            csvDataImportError_ += " (" + protocolEntryError.message() + ")";
        }
        application_.setStatusMessage("CSV 数据导入失败: " + csvDataImportError_);
        return;
    }

    const auto& currentLua = application_.docks().luaState();
    if (!capture->protocolDir.empty() && currentLua.protocolDir != capture->protocolDir &&
        !switchProtocolWorkspace(capture->protocolDir, false)) {
        csvDataImportError_ = "切换导入协议失败";
        application_.setStatusMessage("CSV 数据导入失败: " + csvDataImportError_);
    } else if (!application_.importWaveRawCapture(*capture, error)) {
        csvDataImportError_ = error;
        application_.setStatusMessage("CSV 数据导入失败: " + error);
    } else {
        application_.setStatusMessage("原始事件 CSV 导入成功");
        csvDataImportError_.clear();
    }
}

void GuiRuntime::exportRawCaptureToPath(const std::filesystem::path& path)
{
    // 核心流程：导出路径只在 UI 层选择，实际写入仍交给 Application 统一处理。
    rawCaptureExportPath_ = fileDialogPathText(path);
    std::string error;
    if (!application_.exportWaveRawCapture(path, error)) {
        rawCaptureExportError_ = error;
        application_.setStatusMessage("当前缓存快照导出失败: " + error);
        return;
    }
    const auto& rawCapture = application_.docks().waveState().rawCapture;
    application_.setStatusMessage(rawCapture.truncated ? "当前缓存快照导出成功（实时缓存已截断，仅包含最近原始字节）"
                                                       : "当前缓存快照导出成功");
    rawCaptureExportDialogOpen_ = false;
    rawCaptureExportDialogOpened_ = false;
    rawCaptureExportError_.clear();
}

void GuiRuntime::exportWaveCsvToPath(const std::filesystem::path& path)
{
    waveCsvExportPath_ = fileDialogPathText(path);
    std::string error;
    if (!application_.exportWaveCsv(path, plot::WaveCsvShape::Wide, plot::CsvExportRange{}, error)) {
        waveCsvExportError_ = error;
        application_.setStatusMessage("波形 CSV 导出失败: " + error);
        return;
    }
    application_.setStatusMessage("波形 CSV 导出成功");
    waveCsvExportError_.clear();
}

void GuiRuntime::exportRawCaptureCsvToPath(const std::filesystem::path& path)
{
    rawCaptureCsvExportPath_ = fileDialogPathText(path);
    std::string error;
    if (!application_.exportRawCaptureCsv(path, plot::CsvExportRange{}, error)) {
        rawCaptureCsvExportError_ = error;
        application_.setStatusMessage("原始事件 CSV 导出失败: " + error);
        return;
    }
    application_.setStatusMessage("原始事件 CSV 导出成功");
    rawCaptureCsvExportError_.clear();
}

void GuiRuntime::loadRawCaptureReplayTimelineFromPath(const std::filesystem::path& path)
{
    rawCaptureReplayTimelinePath_ = fileDialogPathText(path);
    std::string error;
    std::optional<plot::RawCaptureFileData> capture;
    const auto csvKind = plot::detectCsvKind(path, error);
    if (csvKind == plot::CsvKind::Wave) {
        rawCaptureReplayTimelineError_ = "波形 CSV 不能作为原始回放时间轴载入";
        application_.setStatusMessage("原始回放时间轴载入失败: " + rawCaptureReplayTimelineError_);
        return;
    }
    if (csvKind == plot::CsvKind::RawEvents) {
        capture = plot::readRawCaptureCsvFile(path, error);
    } else {
        error.clear();
        capture = plot::readRawCaptureFile(path, error);
    }
    if (!capture.has_value()) {
        rawCaptureReplayTimelineError_ = error;
        application_.setStatusMessage("原始回放时间轴载入失败: " + error);
        return;
    }
    std::error_code protocolEntryError;
    if (!capture->protocolDir.empty() &&
        !std::filesystem::exists(configStore_.mainLuaPath(capture->protocolDir), protocolEntryError)) {
        rawCaptureReplayTimelineError_ = "回放文件引用的协议目录不存在: " + capture->protocolDir;
        if (protocolEntryError) {
            rawCaptureReplayTimelineError_ += " (" + protocolEntryError.message() + ")";
        }
        application_.setStatusMessage("原始回放时间轴载入失败: " + rawCaptureReplayTimelineError_);
        return;
    }

    const auto& currentLua = application_.docks().luaState();
    if (!capture->protocolDir.empty() && currentLua.protocolDir != capture->protocolDir &&
        !switchProtocolWorkspace(capture->protocolDir, false)) {
        rawCaptureReplayTimelineError_ = "切换回放协议失败";
        application_.setStatusMessage("原始回放时间轴载入失败: " + rawCaptureReplayTimelineError_);
    } else if (!application_.loadRawCaptureReplayTimeline(*capture, error)) {
        rawCaptureReplayTimelineError_ = error;
        application_.setStatusMessage("原始回放时间轴载入失败: " + error);
    } else {
        application_.setStatusMessage("原始回放时间轴已载入");
        rawCaptureReplayTimelineDialogOpen_ = false;
        rawCaptureReplayTimelineDialogOpened_ = false;
        rawCaptureReplayTimelineError_.clear();
    }
}

void GuiRuntime::startRawCaptureRecordingToPath(const std::filesystem::path& path)
{
    // 核心流程：菜单只负责选择完整录制路径，录制状态和写入错误统一收口到 Application。
    rawCaptureRecordingPath_ = fileDialogPathText(path);
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

void GuiRuntime::importSessionPackageFromPath(const std::filesystem::path& path)
{
    sessionPackageImportPath_ = fileDialogPathText(path);
    std::string error;
    if (!application_.importSessionPackage(path, error)) {
        sessionPackageImportError_ = error;
        application_.setStatusMessage("现场会话包导入失败: " + error);
        return;
    }
    showOfflineReplayDock_ = true;
    pendingProtocolWorkspaceSave_ = true;
    application_.setStatusMessage("现场会话包已导入，原始回放已暂停在起点");
    sessionPackageImportDialogOpen_ = false;
    sessionPackageImportDialogOpened_ = false;
    sessionPackageImportError_.clear();
}

void GuiRuntime::exportSessionPackageToPath(const std::filesystem::path& path)
{
    sessionPackageExportPath_ = fileDialogPathText(path);
    std::string error;
    if (!application_.exportSessionPackage(path, error)) {
        sessionPackageExportError_ = error;
        application_.setStatusMessage("现场会话包导出失败: " + error);
        return;
    }
    application_.setStatusMessage("现场会话包导出成功");
    sessionPackageExportDialogOpen_ = false;
    sessionPackageExportDialogOpened_ = false;
    sessionPackageExportError_.clear();
}

void GuiRuntime::openWaveAnalysisExportDialog()
{
    if (deferBuiltinFileOperation([this] { openWaveAnalysisExportDialog(); })) return;
#if defined(_WIN32)
    const auto& lua = application_.docks().luaState();
    const std::string baseName =
        lua.protocolName.empty() ? std::string("wave-analysis") : lua.protocolName + "-analysis";
    const auto defaultPath = executableDir_ / "captures" / (baseName + ".csv");
    std::string dialogError;
    const auto path = builtinFileDialog(window_,
                                       L"导出波形分析报告",
                                       L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0",
                                       defaultPath,
                                       true,
                                       L"csv",
                                       dialogError);
    if (!dialogError.empty()) {
        application_.setStatusMessage(dialogError);
    }
    if (path.has_value()) {
        exportWaveAnalysisReportToPath(*path);
    }
#else
    application_.setStatusMessage("当前平台暂未实现波形分析报告文件对话框");
#endif
}

void GuiRuntime::exportWaveAnalysisReportToPath(const std::filesystem::path& path)
{
    std::string error;
    if (!application_.exportWaveAnalysisReport(path, error)) {
        application_.setStatusMessage("波形分析报告导出失败: " + error);
        return;
    }
    application_.setStatusMessage("波形分析报告导出成功");
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
        logExportPath_ = fileDialogPathText(path);
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

std::vector<dock::RequestTraceRow> GuiRuntime::requestTraceExportRows()
{
    const auto& trace = application_.docks().requestTraceState();
    const auto filteredRows = dock::filteredRequestTraceRows(trace.rows, trace.filter);
    std::vector<dock::RequestTraceRow> rows;
    rows.reserve(filteredRows.size());
    for (const auto* row : filteredRows) {
        rows.push_back(*row);
    }
    return rows;
}

bool GuiRuntime::exportRequestTraceToPath(const std::filesystem::path& path)
{
    const auto& trace = application_.docks().requestTraceState();
    const auto rows = requestTraceExportRows();
    const bool exported = exportRequestTraceRowsToPath(path, rows, trace.showTimestamps);
    if (exported) {
        requestTraceExportPath_ = fileDialogPathText(path);
        requestTraceExportDialogOpen_ = false;
        requestTraceExportDialogOpened_ = false;
    }
    return exported;
}

bool GuiRuntime::exportRequestTraceRowsToPath(const std::filesystem::path& path,
                                              std::span<const dock::RequestTraceRow> rows,
                                              bool showTimestamps)
{
    auto fail = [&](std::string message) {
        requestTraceExportError_ = "请求追踪导出失败: " + message;
        application_.setStatusMessage(requestTraceExportError_);
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

        output << dock::formatRequestTraceRowsCsv(rows, showTimestamps);
        if (!output.good()) {
            return fail("写入文件失败");
        }

        std::error_code absoluteError;
        const auto savedPath = std::filesystem::absolute(path, absoluteError);
        const auto displayPath = absoluteError ? path.generic_string() : savedPath.generic_string();
        if (rows.empty()) {
            application_.setStatusMessage("已导出空请求追踪: " + displayPath);
        } else {
            application_.setStatusMessage("请求追踪已导出: " + displayPath);
        }
        requestTraceExportError_.clear();
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
    drawPathModalDialog(rawCaptureImportDialogOpen_,
                        rawCaptureImportDialogOpened_,
                        rawCaptureImportPath_,
                        rawCaptureImportError_,
                        "导入原始波形##psraw_import",
                        "请输入 .psraw 文件路径",
                        "导入",
                        [this](const std::string& path) { importRawCaptureFromPath(path); });
    drawPathModalDialog(rawCaptureReplayTimelineDialogOpen_,
                        rawCaptureReplayTimelineDialogOpened_,
                        rawCaptureReplayTimelinePath_,
                        rawCaptureReplayTimelineError_,
                        "载入原始回放时间轴##psraw_replay_timeline",
                        "请输入 .psraw 文件路径",
                        "载入",
                        [this](const std::string& path) { loadRawCaptureReplayTimelineFromPath(path); });
    drawPathModalDialog(rawCaptureExportDialogOpen_,
                        rawCaptureExportDialogOpened_,
                        rawCaptureExportPath_,
                        rawCaptureExportError_,
                        "导出当前缓存快照##psraw_export",
                        "请输入导出 .psraw 文件路径",
                        "导出",
                        [this](const std::string& path) { exportRawCaptureToPath(path); });
    drawPathModalDialog(rawCaptureRecordingDialogOpen_,
                        rawCaptureRecordingDialogOpened_,
                        rawCaptureRecordingPath_,
                        rawCaptureRecordingError_,
                        "开始完整原始数据录制##psraw_record",
                        "请输入完整录制 .psraw 文件路径",
                        "开始录制",
                        [this](const std::string& path) { startRawCaptureRecordingToPath(path); });
    drawPathModalDialog(sessionPackageImportDialogOpen_,
                        sessionPackageImportDialogOpened_,
                        sessionPackageImportPath_,
                        sessionPackageImportError_,
                        "导入现场会话包##pssession_import",
                        "请输入 .pssession 文件路径",
                        "导入",
                        [this](const std::string& path) { importSessionPackageFromPath(path); });
    drawPathModalDialog(sessionPackageExportDialogOpen_,
                        sessionPackageExportDialogOpened_,
                        sessionPackageExportPath_,
                        sessionPackageExportError_,
                        "导出现场会话包##pssession_export",
                        "请输入导出 .pssession 文件路径",
                        "导出",
                        [this](const std::string& path) { exportSessionPackageToPath(path); });
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

void GuiRuntime::drawRequestTraceExportFileDialog()
{
    if (!requestTraceExportDialogOpen_) {
        return;
    }

    const char* popupId = "导出请求追踪##request_trace_export";
    if (!requestTraceExportDialogOpened_) {
        ImGui::OpenPopup(popupId);
        requestTraceExportDialogOpened_ = true;
    }

    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::BeginPopupModal(popupId, nullptr, flags)) {
        ImGui::TextUnformatted("请输入请求追踪 CSV 导出路径");
        char buffer[1024]{};
        std::snprintf(buffer, sizeof(buffer), "%s", requestTraceExportPath_.c_str());
        if (ImGui::InputText("路径", buffer, sizeof(buffer))) {
            requestTraceExportPath_ = buffer;
        }
        if (!requestTraceExportError_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.90F, 0.35F, 0.35F, 1.0F), "%s", requestTraceExportError_.c_str());
        }
        ImGui::Spacing();
        if (ImGui::Button("导出", ImVec2(90.0F, 0.0F))) {
            if (exportRequestTraceToPath(requestTraceExportPath_)) {
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("取消", ImVec2(90.0F, 0.0F))) {
            requestTraceExportDialogOpen_ = false;
            requestTraceExportDialogOpened_ = false;
            requestTraceExportError_.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}


} // namespace protoscope::ui
