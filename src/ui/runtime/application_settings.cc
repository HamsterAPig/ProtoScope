#include "protoscope/ui/gui_runtime.hpp"
#include "../docks/dock_widgets.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cfloat>
#include <limits>
#include <type_traits>

namespace protoscope::ui {
namespace {
constexpr const char* windowName = "应用设置";
template<class T> std::string defaultText(T value) {
    if constexpr (std::is_same_v<T, bool>) return value ? "开启" : "关闭";
    else if constexpr (std::is_enum_v<T>) return std::to_string(static_cast<int>(value));
    else return std::to_string(value);
}
std::string defaultText(const std::string& value) { return value.empty() ? "空" : value; }
void help(const std::string& value) { drawDockHelpMarker(value.c_str()); }
template<class T> bool scalar(const char* label, T& value, T defaults, const char* description) {
    bool changed;
    ImGui::SetNextItemWidth(180);
    if constexpr (std::is_same_v<T,bool>) changed = ImGui::Checkbox(label, &value);
    else {
        auto type = ImGuiDataType_U64;
        if constexpr (std::is_same_v<T,int>) type = ImGuiDataType_S32;
        else if constexpr (std::is_same_v<T,std::uint32_t>) type = ImGuiDataType_U32;
        else if constexpr (std::is_same_v<T,double>) type = ImGuiDataType_Double;
        changed = ImGui::InputScalar(label, type, &value);
    }
    std::string range;
    if constexpr (std::is_integral_v<T> && !std::is_same_v<T,bool>)
        range = " 存储范围：0–" + std::to_string((std::numeric_limits<T>::max)()) + "；应用还校验本项更严格边界。";
    help(std::string(description) + " 内建默认：" + defaultText(defaults) + "。" + range);
    return changed;
}
template<class E> void choice(const char* label, E& value, E defaults, const char* items, const char* description) {
    int selected = static_cast<int>(value);
    ImGui::SetNextItemWidth(230);
    if (ImGui::Combo(label, &selected, items)) value = static_cast<E>(selected);
    const char* defaultLabel = items;
    for (int i = 0; i < static_cast<int>(defaults); ++i) defaultLabel += std::strlen(defaultLabel) + 1;
    help(std::string(description) + " 内建默认：" + defaultLabel + "；只能选择列出的值。");
}
void text(const char* label, std::string& value, const char* description) {
    // 输入缓冲每帧从草稿建立；超过缓冲的已有路径原样保留，未编辑不截断。
    std::vector<char> buffer((std::max)(std::size_t{4096}, value.size()+1024), 0);
    std::memcpy(buffer.data(), value.data(), value.size());
    ImGui::SetNextItemWidth(280);
    if (ImGui::InputText(label, buffer.data(), buffer.size())) value = buffer.data();
    help(description);
}
}

void GuiRuntime::openApplicationSettings() {
    if (!settingsStartupConfig_) settingsStartupConfig_ = application_.captureConfig();
    if (!settingsOpen_) { settingsDraft_.emplace(application_.captureConfig()); settingsPreserveSaveDraft_ = false; }
    settingsOpen_ = true; settingsFocusRequested_ = true;
}
bool GuiRuntime::settingsOwnInput() const {
    if (!settingsOpen_) return false;
    if (settingsFocusRequested_) return true;
    // NewFrame 已更新鼠标目标，NavWindow仍可能属于波形；点击设置的同帧先保护按键。
    if (GImGui && ImGui::IsMouseDown(ImGuiMouseButton_Left) && GImGui->HoveredWindow) {
        const auto* target = GImGui->HoveredWindow->RootWindow;
        const auto* settings = ImGui::FindWindowByName(windowName);
        if (settings && target == settings->RootWindow) return true;
    }
    const auto* focused = GImGui ? GImGui->NavWindow : nullptr;
    const auto* root = ImGui::FindWindowByName(windowName);
    return focused && root && (focused->RootWindow == root->RootWindow ||
        (focused->Flags & ImGuiWindowFlags_Popup && settingsFocused_));
}
void GuiRuntime::requestSettingsClose() {
    if (settingsApplyRequested_ || settingsResult_.status == app::SettingsApplyStatus::Pending) { settingsError_ = "应用完成前不能关闭"; return; }
    if (settingsDraft_ && settingsDraft_->hasChanges()) settingsCloseRequested_ = true;
    else settingsOpen_ = false;
}
void GuiRuntime::applySettingsDraft(bool save, bool close) {
    if (application_.dataTransferStatus().active || settingsResult_.status == app::SettingsApplyStatus::Pending ||
        (save && application_.settingsSaveBlocked())) {
        settingsError_ = "传输/应用活动中或部分应用未修复，不能执行本次操作";
        settingsSaveAfterApply_ = settingsCloseAfterApply_ = false;
        settingsOverwriteSnapshot_.reset();
        return;
    }
    settingsSaveAfterApply_ = save; settingsCloseAfterApply_ = close;
    settingsApplyRequested_ = true;
}

void GuiRuntime::processSettingsActions() {
    // 在 NewFrame 前执行真实应用及主题提交，避免绘制中改变样式和工作区资源。
    if (!settingsDraft_) return;
    if (settingsApplyRequested_) {
    settingsApplyRequested_ = false;
    if (settingsSaveAfterApply_ && !settingsOverwriteSnapshot_ && configStore_.hasChanged(configSnapshot_)) { settingsExternalConflict_ = true; return; }
    app::SettingsApplyOptions options;
    options.confirmationId = settingsResult_.confirmationId;
    options.confirmHistoryTrim = options.overwriteConflicts = options.confirmationId != 0;
    options.prepareTheme = [this](std::string_view id, std::string& error) {
        if (themeManager_.find(id)) return true;
        error = "主题不存在，请重载主题或重新选择"; return false;
    };
    options.applyTheme = [this](std::string_view id, std::string& error) {
        if (!themeManager_.request(id, error)) return false;
        // 必须在安全帧真正提交，不能 request 后就冒充成功。
        themeManager_.applyPending(); return true;
    };
    settingsResult_ = application_.applySettingsChanges(settingsDraft_->baseline, settingsDraft_->value, std::move(options));
    settingsError_ = settingsResult_.error;
    }
    if (settingsResult_.status == app::SettingsApplyStatus::Pending) {
        settingsResult_ = application_.pollSettingsApply(); settingsError_ = settingsResult_.error;
    }
    if (settingsResult_.status == app::SettingsApplyStatus::Applied) {
        const bool saved = !settingsSaveAfterApply_ || saveCurrentConfigToDisk();
        settingsPreserveSaveDraft_ = !saved;
        if (saved) settingsDraft_->refresh(application_.captureConfig());
        if (saved && settingsCloseAfterApply_) settingsOpen_ = false;
        settingsSaveAfterApply_ = settingsCloseAfterApply_ = false;
        settingsOverwriteSnapshot_.reset();
        settingsResult_.status = app::SettingsApplyStatus::Failed; // 空闲，不重复消费完成结果。
    } else if (settingsResult_.status == app::SettingsApplyStatus::Failed) {
        settingsSaveAfterApply_ = settingsCloseAfterApply_ = false;
        settingsOverwriteSnapshot_.reset();
    }
}

void GuiRuntime::drawApplicationSettings() {
    if (!settingsOpen_ || !settingsDraft_) return;
    if (!settingsPreserveSaveDraft_ && !settingsApplyRequested_ && !application_.settingsSaveBlocked())
        settingsDraft_->followUnedited(application_.captureConfig());
    ImGui::SetNextWindowSize(ImVec2(820,680), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(320,360), ImVec2(FLT_MAX,FLT_MAX));
    if (settingsFocusRequested_) { ImGui::SetNextWindowFocus(); settingsFocusRequested_ = false; }
    const bool popupEscape = ImGui::IsKeyPressed(ImGuiKey_Escape) &&
        (settingsCloseRequested_ || settingsExternalConflict_);
    if (popupEscape) { settingsCloseRequested_ = false; settingsExternalConflict_ = false; settingsSaveAfterApply_=settingsCloseAfterApply_=false; settingsOverwriteSnapshot_.reset(); }
    bool keepOpen = true;
    const bool visible = ImGui::Begin(windowName, &keepOpen, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings);
    settingsFocused_ = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    if (!keepOpen || (!popupEscape && settingsFocused_ && ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))) requestSettingsClose();
    if (visible) {
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::BeginChild("设置内容", ImVec2(0,-190), false);
        const char* pages[] = {"常规外观","波形偏好","日志历史","性能"};
        if (width >= 620) {
            ImGui::BeginChild("分类",ImVec2(125,0),true);
            for (int i=0;i<4;++i)
                if (ImGui::Selectable(pages[i], static_cast<int>(settingsPage_)==i)) settingsPage_=static_cast<config::SettingsPage>(i);
            ImGui::EndChild(); ImGui::SameLine();
        } else {
            int page = static_cast<int>(settingsPage_);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##设置分类", &page, pages, 4)) settingsPage_ = static_cast<config::SettingsPage>(page);
        }
        ImGui::BeginChild("表单",ImVec2(0,0),false);
        const bool pending = settingsApplyRequested_ || settingsResult_.status == app::SettingsApplyStatus::Pending;
        ImGui::BeginDisabled(pending);
        auto& v=settingsDraft_->value; const config::AppConfig d;
#define VALUE(path, label, desc) scalar(label,v.path,d.path,desc)
#define ENUM(path, label, items, desc) choice(label,v.path,d.path,items,desc)
        switch(settingsPage_) {
        case config::SettingsPage::General:
            if (ImGui::BeginCombo("主题",v.gui.theme.c_str())) {
                for (const auto& theme:themeManager_.themes()) if(ImGui::Selectable(theme.name.c_str(),theme.id==v.gui.theme)) v.gui.theme=theme.id;
                ImGui::EndCombo();
            }
            help("外观主题；内建默认："+d.gui.theme+"。注册主题可选，仅应用时更换，不实时预览。");
            VALUE(gui.showAppHeader,"应用标题区","即时显示标题区；开关。");
            VALUE(gui.interactionFeedback.enabled,"交互反馈","即时开关；也控制波形动画有效状态。");
            VALUE(gui.interactionFeedback.statusDurationMs,"状态提示 ms","毫秒，非负；0关闭瞬时提示，持续时间加当前时间不得溢出。");
            VALUE(app.autoSave.enabled,"自动保存","应用后dirty按既有间隔保存；未应用草稿不保存。");
            VALUE(app.autoSave.intervalMs,"自动保存间隔 ms","非负毫秒；0无最小间隔。");
            VALUE(app.configHotReload.enabled,"外部修改提示","只提示，不自动应用；关闭仍检查保存冲突。");
            VALUE(gui.window.width,"启动宽度","正整数逻辑像素，待重启。");
            VALUE(gui.window.height,"启动高度","正整数逻辑像素，待重启。");
            VALUE(gui.window.maximized,"启动最大化","开关，待重启，不重建当前窗口。");
            ENUM(gui.rendererBackend,"渲染后端","OpenGL\0D3D11\0D3D11 WARP\0","平台支持项，待重启，不重建后端。");
            ENUM(gui.font.chineseGlyphRange,"中文字形","常用简体\0完整中文字形\0","完整字形增加字库资源；待重启。"); break;
        case config::SettingsPage::Wave:
            ENUM(gui.wave.controlMode,"操作模式","示波器\0兼容全局\0","仅影响交互，不清数据/视口。");
            ENUM(gui.wave.displayFormula,"显示公式","先偏移再缩放\0先缩放再偏移\0","工程显示转换，不清数据。");
            ENUM(gui.wave.gridDivisionReadoutMode,"格值读数","显示值\0实际值\0原始值\0","只改变读数解释。");
            VALUE(gui.wave.channelScaleWheelEnabled,"工程档位滚轮","关闭回退连续缩放。");
            ENUM(gui.wave.channelScaleWheelAcceleration,"滚轮加速","无\0线性\0对数\0","仅工程档位滚轮开启时使用。");
            ENUM(gui.wave.channelCardWidthMode,"通道卡片宽度","固定\0自适应\0","固定像素或可用宽度比例。");
            VALUE(gui.wave.channelCardFixedWidth,"固定宽度 px","正有限数，安全转float；固定模式生效。");
            VALUE(gui.wave.channelCardAdaptiveRatio,"宽度比例","正有限比例，乘可用宽不溢出；最终160–220px；仅自适应模式。");
            ENUM(gui.wave.channelDoubleClickAction,"通道双击","复位全部\0复位缩放偏移\0复位缩放\0复位偏移\0","仅后续双击操作生效。");
            ENUM(gui.wave.xAxisDoubleClickAction,"X轴双击","适配全部历史\0适配可见窗口\0","仅后续双击操作生效。");
            ENUM(gui.wave.yAxisDoubleClickAction,"Y轴双击","适配可见通道\0适配当前通道\0","仅后续双击操作生效。");
            VALUE(gui.wave.yAxisDoubleClickAdjustOffset,"Y适配偏移（兼容）","兼容配置，当前不影响适配。"); ImGui::TextWrapped("兼容项：当前不影响适配");
            ENUM(gui.wave.hiddenChannelPolicy,"隐藏通道","参与派生视图\0排除派生视图\0","改变派生展示，不删除历史。");
            ENUM(gui.wave.cursorExtremeSnapPolicy,"极值吸附","最近波形\0视口区域\0","不主动移动现有游标。");
            ENUM(gui.wave.mouseYOffsetDragMode,"Y偏移拖动","直接\0Shift\0禁用\0","控制后续鼠标拖动。");
            ENUM(gui.wave.legendOverlayOpenMode,"浮动图例","悬停\0双击\0禁用\0","控制浮动图例触发。");
            VALUE(gui.wave.legendOverlayDoubleClickAutoCollapse,"图例自动折叠","只在双击图例模式生效。"); ImGui::TextWrapped("自动折叠仅双击图例模式生效");
            VALUE(gui.wave.interactionAnimationEnabled,"波形动画","开关；还受常规交互反馈总开关控制。");
            ImGui::TextWrapped("动画有效：%s",v.gui.interactionFeedback.enabled && v.gui.wave.interactionAnimationEnabled ? "开启":"关闭（受两开关共同控制）");
            VALUE(gui.wave.zoomSelectionAutoExit,"框选后自动退出","只影响后续框选，不取消当前选区。");
            ENUM(gui.wave.fullscreenMode,"全屏策略","聚焦\0覆盖\0","下次进入全屏生效，不切换当前布局。"); break;
        case config::SettingsPage::Logging: {
            ENUM(logging.level,"日志等级","Trace\0Debug\0Info\0Warn\0Error\0","即时门限；与菜单快捷入口共享运行态。");
            text("日志文件",v.logging.filePath,"默认空=关闭文件输出；相对可执行目录。应用先预检，失败保旧；不会在预检旋转或截断。");
            const auto logPath = std::filesystem::path(v.logging.filePath);
            ImGui::TextWrapped("草稿解析路径：%s", v.logging.filePath.empty() ? "文件输出关闭" :
                (logPath.is_absolute() ? logPath : executableDir_ / logPath).string().c_str());
            VALUE(logging.payloadPreview.enabled,"载荷预览","可能泄露原始载荷、增加日志体积；不改变采集数据。");
            VALUE(logging.payloadPreview.maxBytes,"预览 bytes","非负字节；0预览零字节，不是无限。");
            VALUE(logging.maxFileSizeBytes,"轮转大小 bytes","非负字节；0有效回退1byte，轮转影响磁盘文件。");
            VALUE(logging.maxFiles,"轮转备份数","0–200000，0有效回退1；活动文件另外存在。");
            ImGui::TextWrapped("轮转有效：%llu bytes / %llu 份",static_cast<unsigned long long>((std::max)(std::size_t{1},v.logging.maxFileSizeBytes)),static_cast<unsigned long long>((std::max)(std::size_t{1},v.logging.maxFiles)));
            VALUE(gui.logHistory.transferRawLimit,"原始收发历史","非负行数；0不保留；缩减应用前确认裁剪。");
            VALUE(gui.logHistory.transferFrameLimit,"协议帧历史","非负行数；0不保留；包括pending派生行。");
            VALUE(gui.logHistory.hostLimit,"宿主日志历史","非负行数；0不保留；缩减会裁剪。");
            VALUE(gui.logHistory.scriptLimit,"脚本日志历史","非负行数；0不保留；缩减会裁剪。");
            VALUE(gui.logHistory.requestTraceLimit,"请求追踪历史","非负行数；0不保留，不是请求队列预算。");
            VALUE(gui.sendHistoryLimit,"发送历史","非负条数；有效min(配置,200)，0清空。");
            ImGui::Text("发送有效容量：%llu",static_cast<unsigned long long>((std::min)(v.gui.sendHistoryLimit,std::size_t{200})));
            ENUM(gui.importProtocolSource,"导入协议来源","优先现场包协议\0使用当前协议\0","优先包内协议（缺失时使用当前），导入确认后执行所选 Lua 初始化；使用当前不会执行包内 Lua。初次仅浏览原始记录，不重新计算波形快照。此本机偏好不随导入覆盖。");
            ENUM(gui.historyOnParsedSwitch,"逐帧历史策略","每次询问\0使用历史\0不使用历史\0","原始转逐帧时如何处理已有原始历史：ask 弹宿主确认；use_history 按范围重建替换；skip_history 只清残留半帧，不重放旧字节。");
            ENUM(gui.historyScope,"逐帧历史范围","面板保留行\0底层保留采集\0","use_history 重建来源范围：retained_rows 只用收发面板保留的 RX/TX 行；retained_capture 用底层仍保留的完整采集/导入历史。清空面板不删除底层数据，可经同意恢复；不保证找回已裁剪字节，不自动重读文件。");
            break; }
        case config::SettingsPage::Performance: {
            const auto effective=application_.effectiveSettingsPerformance(); const auto& comm=application_.docks().commState();
            ImGui::TextWrapped("运行自适应：%s；压力 %s；有效倍率 %.3f / 追赶 %.3f；%s",comm.adaptivePerformanceEnabled?"开启":"关闭",comm.adaptivePerformanceLevel.c_str(),comm.adaptivePerformanceEffectiveMultiplier,comm.adaptivePerformanceCatchUpMultiplier,comm.adaptivePerformanceReason.c_str());
            ImGui::TextWrapped("静态值可编辑并保存；运行自适应接管8项，关闭后才使用静态值。草稿开关不代表运行开关。");
            VALUE(performance.adaptive.enabled,"自适应（草稿）","接管FPS、三波形预算、三perPump和flush；不接管资源队列/内存/背压/连接缓冲。");
            VALUE(performance.adaptive.maxMultiplier,"最大倍率 K","有限0.25–4.0；保持已有自适应算法。");
            VALUE(app.fpsLimit,"静态 FPS","非负整数；0至少1FPS；超过1000调度最小1ms；自适应接管，不跟随scale。");
            VALUE(performance.scale,"公共倍率 scale","有限正数；只缩放未显式的10类缺省预算，不累乘；自适应开启不参与调度。资源预算可能增加内存/背压，连接缓冲下次连接生效。");
            VALUE(gui.wave.maxRenderPointsPerChannel,"静态通道点数","非负；0回退内建点数，不是无限；自适应接管，不跟随scale。");
            VALUE(gui.wave.maxRenderVertices,"静态总顶点","非负；0回退内建顶点，不是无限；自适应接管。");
            VALUE(gui.wave.overviewMaxSamples,"静态概览桶","非负；0仅取消本项桶数限制；自适应接管。");
            ImGui::TextWrapped("当前有效 FPS %u / 点 %llu / 顶点 %llu / 桶 %llu",effective.fpsLimit,(unsigned long long)effective.maxRenderPointsPerChannel,(unsigned long long)effective.maxRenderVertices,(unsigned long long)effective.overviewMaxSamples);
            if(ImGui::CollapsingHeader("低频预算")) {
#define BUDGET(path, flag, label, desc) if(VALUE(path,label,desc)) v.performance.explicitOverrides.flag=true; \
                ImGui::SameLine(); if(ImGui::SmallButton("跟随倍率##" #flag)) { v.performance.explicitOverrides.flag=false; std::string error; if(!config::recomputePerformanceDefaults(v,error)) settingsError_=error; } \
                help("清除加载/保存显式标记，从内建缺省基线重算；不改变自适应优先级。"); \
                ImGui::Text("%s",v.performance.explicitOverrides.flag?"显式静态覆盖":"跟随倍率缺省")
                BUDGET(gui.realtimeBacklog.rxChunkBytesPerPump,realtimeBacklogRxChunkBytesPerPump,"RX bytes/pump","非负；0至少1；自适应接管，静态按标记跟随scale。");
                BUDGET(gui.realtimeBacklog.transferFrameRowsPerPump,realtimeBacklogTransferFrameRowsPerPump,"帧 rows/pump","非负；0至少1；可能影响积压/响应；自适应接管。");
                BUDGET(gui.realtimeBacklog.plotAppendsPerPump,realtimeBacklogPlotAppendsPerPump,"绘图追加/pump","追加操作不是样本数；0至少1；自适应接管。");
                BUDGET(scripting.workerOutputFlushBudgetMs,workerOutputFlushBudgetMs,"flush ms/pump","有限非负毫秒，乘1000不得溢出；0无时间预算可能降低响应性；自适应接管。");
#undef BUDGET
            }
            ImGui::TextWrapped("当前有效 RX %llu / 帧 %llu / 追加 %llu / flush %.3fms",(unsigned long long)effective.rxChunkBytesPerPump,(unsigned long long)effective.transferFrameRowsPerPump,(unsigned long long)effective.plotAppendsPerPump,effective.workerOutputFlushBudgetMs);
            break; }
        }
#undef VALUE
#undef ENUM
        ImGui::EndDisabled();
        ImGui::EndChild(); ImGui::EndChild();
        ImGui::BeginChild("底部",ImVec2(0,0),false);
        const auto& state=application_.docks().configState();
        ImGui::TextWrapped("配置：%s",state.loadedFromPath.c_str());
        ImGui::TextWrapped("%s；%s",settingsDraft_->hasChanges()?"草稿未应用":"草稿无变化",state.dirty?"已应用未保存":"运行态已保存");
        ImGui::TextWrapped("保存也包含其它面板已应用的修改；自动保存开启时应用可能自动落盘，草稿永不落盘。");
        const auto current = application_.captureConfig();
        if (settingsStartupConfig_) {
            const auto& start = *settingsStartupConfig_;
            if (current.gui.window.width != start.gui.window.width || current.gui.window.height != start.gui.window.height ||
                current.gui.window.maximized != start.gui.window.maximized || current.gui.rendererBackend != start.gui.rendererBackend ||
                current.gui.font.chineseGlyphRange != start.gui.font.chineseGlyphRange)
                ImGui::TextUnformatted("已应用启动项：待重启");
        }
        if(settingsResult_.nextConnectionRequired) ImGui::TextUnformatted("连接缓冲：下次连接生效（不会自动重连）");
        if(application_.settingsSaveBlocked() && settingsResult_.status != app::SettingsApplyStatus::Pending) {
            ImGui::TextWrapped("部分应用：保留真实预算和dirty，禁止保存。修正剩余草稿并成功应用后解除；可明确解除本次worker资源覆盖，不恢复整页。");
            if (ImGui::Button("本次worker资源重新跟随倍率")) {
                application_.preparePartialWorkerRecovery(*settingsDraft_);
                settingsResult_.confirmationId = 0;
                settingsError_ = "仅解除worker覆盖草稿；请修正其它失败项后应用，未保存";
            }
            ImGui::BeginDisabled(application_.dataTransferStatus().active || pending);
            if (ImGui::Button("明确接受当前实际部分运行态（不保存）")) {
                if (application_.acceptPartialSettings(settingsError_)) {
                    settingsResult_ = {};
                    settingsSaveAfterApply_ = settingsCloseAfterApply_ = false;
                    settingsOverwriteSnapshot_.reset();
                    settingsError_ = "已接受实际预算，剩余草稿仍保留；自动保存开启时运行dirty可按间隔落盘";
                }
            }
            ImGui::EndDisabled();
        }
        if(!settingsError_.empty()) ImGui::TextWrapped("错误：%s",settingsError_.c_str());
        ImGui::BeginDisabled(pending);
        if(ImGui::Button("恢复本页默认")) settingsDraft_->restoreDefaults(settingsPage_);
        ImGui::EndDisabled();
        const bool blocked=application_.dataTransferStatus().active || pending;
        if(blocked) ImGui::TextWrapped("导入导出或设置应用中：应用/保存暂不可用；应用中草稿锁定，避免完成后丢失编辑。");
        ImGui::BeginDisabled(blocked);
        if(ImGui::Button("应用")) { settingsResult_.confirmationId=0; applySettingsDraft(false,false); }
        ImGui::SameLine(); ImGui::BeginDisabled(application_.settingsSaveBlocked());
        if(ImGui::Button("保存")) { settingsResult_.confirmationId=0; applySettingsDraft(true,false); }
        ImGui::EndDisabled();
        ImGui::EndDisabled(); ImGui::SameLine(); if(ImGui::Button("关闭")) requestSettingsClose();
        if(settingsResult_.status==app::SettingsApplyStatus::ConfirmationRequired) {
            ImGui::TextWrapped("确认：可能裁剪历史或覆盖同字段冲突。请检查草稿目标；取消不会改变历史。");
            const auto& docks = application_.docks();
            const auto& target = settingsDraft_->value.gui;
            ImGui::TextWrapped("当前行数 → 目标上限：原始 %llu → %llu；帧 %llu (+pending %llu) → %llu；宿主 %llu → %llu；脚本 %llu → %llu；请求 %llu → %llu；发送 %llu → %llu。超限历史不可恢复。",
                (unsigned long long)docks.receiveState().rows.size(), (unsigned long long)target.logHistory.transferRawLimit,
                (unsigned long long)docks.receiveState().frameRows.size(), (unsigned long long)docks.commState().pendingTransferFrameRows, (unsigned long long)target.logHistory.transferFrameLimit,
                (unsigned long long)docks.logState().rows.size(), (unsigned long long)target.logHistory.hostLimit,
                (unsigned long long)docks.scriptState().rows.size(), (unsigned long long)target.logHistory.scriptLimit,
                (unsigned long long)docks.requestTraceState().rows.size(), (unsigned long long)target.logHistory.requestTraceLimit,
                (unsigned long long)docks.sendState().history.size(), (unsigned long long)(std::min)(target.sendHistoryLimit, std::size_t{200}));
            for(const auto& field:settingsResult_.conflicts) ImGui::TextWrapped("冲突：%s",field.c_str());
            ImGui::BeginDisabled(blocked);
            if(ImGui::Button("明确确认并应用")) applySettingsDraft(settingsSaveAfterApply_,settingsCloseAfterApply_);
            ImGui::EndDisabled();
            ImGui::SameLine(); if(ImGui::Button("刷新运行值")) { settingsDraft_->refresh(application_.captureConfig()); settingsResult_={}; settingsError_.clear(); settingsSaveAfterApply_=settingsCloseAfterApply_=false; settingsOverwriteSnapshot_.reset(); settingsPreserveSaveDraft_=false; }
            ImGui::SameLine(); if(ImGui::Button("取消确认")) { settingsResult_.status=app::SettingsApplyStatus::Failed; settingsResult_.confirmationId=0; settingsSaveAfterApply_=settingsCloseAfterApply_=false; settingsOverwriteSnapshot_.reset(); }
        }
        ImGui::EndChild();
    }
    if(settingsCloseRequested_) ImGui::OpenPopup("关闭设置草稿");
    if(ImGui::BeginPopupModal("关闭设置草稿",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("草稿尚未应用。放弃草稿不撤销之前已应用的配置。");
        ImGui::BeginDisabled(application_.dataTransferStatus().active || settingsApplyRequested_ || settingsResult_.status == app::SettingsApplyStatus::Pending);
        if(ImGui::Button("应用后关闭")) { settingsCloseRequested_=false; settingsResult_.confirmationId=0; ImGui::CloseCurrentPopup(); applySettingsDraft(false,true); }
        ImGui::EndDisabled();
        if(ImGui::Button("放弃草稿")) { settingsCloseRequested_=false; settingsOpen_=false; ImGui::CloseCurrentPopup(); }
        if(ImGui::Button("继续编辑") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { settingsCloseRequested_=false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    if(settingsExternalConflict_) ImGui::OpenPopup("外部配置冲突");
    if(ImGui::BeginPopupModal("外部配置冲突",nullptr,ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("磁盘已变化或读取失败；草稿保留。覆盖会保存全部最新运行态。重载会放弃草稿及未保存运行修改，并重载协议。");
        if(ImGui::Button("明确覆盖外部文件")) { settingsOverwriteSnapshot_=configStore_.snapshot(application_.docks().configState().loadedFromPath); settingsExternalConflict_=false; ImGui::CloseCurrentPopup(); applySettingsDraft(true,false); }
        if(ImGui::Button("放弃并重载")) { if(reloadConfigFromDisk()) { settingsDraft_->refresh(application_.captureConfig()); settingsPreserveSaveDraft_=false; } settingsExternalConflict_=false; settingsSaveAfterApply_=settingsCloseAfterApply_=false; settingsOverwriteSnapshot_.reset(); ImGui::CloseCurrentPopup(); }
        if(ImGui::Button("继续编辑") || ImGui::IsKeyPressed(ImGuiKey_Escape)) { settingsExternalConflict_=false; settingsSaveAfterApply_=settingsCloseAfterApply_=false; settingsOverwriteSnapshot_.reset(); ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    ImGui::End();
}
} // namespace protoscope::ui
