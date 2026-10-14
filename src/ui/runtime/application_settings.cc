#include "protoscope/ui/gui_runtime.hpp"
#include "../docks/dock_widgets.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cfloat>
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
    help(std::string(description) + " 内建默认：" + defaultText(defaults) + "。");
    return changed;
}
template<class E> void choice(const char* label, E& value, E defaults, const char* items, const char* description) {
    int selected = static_cast<int>(value);
    ImGui::SetNextItemWidth(230);
    if (ImGui::Combo(label, &selected, items)) value = static_cast<E>(selected);
    const char* defaultLabel = items;
    for (int i = 0; i < static_cast<int>(defaults); ++i) defaultLabel += std::strlen(defaultLabel) + 1;
    help(std::string(description) + " 内建默认：" + defaultLabel + "。");
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
            help("选择界面的配色与外观。应用后切换主题，编辑草稿时不预览。内建默认："+d.gui.theme+"。");
            VALUE(gui.showAppHeader,"应用标题区","显示应用顶部标题区。关闭可为面板腾出更多空间，应用后生效。");
            VALUE(gui.interactionFeedback.enabled,"交互反馈","控制操作后的状态提示与动画反馈。关闭会同时停用波形交互动画，应用后生效。");
            VALUE(gui.interactionFeedback.statusDurationMs,"状态提示 ms","控制短暂状态提示的显示时长，单位毫秒。增大后提示停留更久；0 关闭瞬时提示。");
            VALUE(app.autoSave.enabled,"自动保存","自动把已应用的配置修改保存到磁盘。关闭后需手动保存；尚未应用的草稿不会自动保存。");
            VALUE(app.autoSave.intervalMs,"自动保存间隔 ms","限制两次自动保存之间的最短间隔，单位毫秒。增大可减少磁盘写入，但修改保存更晚；0 不限制最短间隔。");
            VALUE(app.configHotReload.enabled,"外部修改提示","检测配置文件被其他程序修改时显示提示。关闭后不再提示，保存时仍检查冲突；外部修改不会自动应用。");
            VALUE(gui.window.width,"启动宽度","设置下次启动时窗口的宽度，单位逻辑像素。增大可容纳更多面板内容；应用并保存后重启生效。");
            VALUE(gui.window.height,"启动高度","设置下次启动时窗口的高度，单位逻辑像素。增大可显示更多纵向内容；应用并保存后重启生效。");
            VALUE(gui.window.maximized,"启动最大化","控制下次启动是否最大化窗口。开启后优先使用屏幕可用空间；应用并保存后重启生效。");
            ENUM(gui.rendererBackend,"渲染后端","OpenGL\0D3D11\0D3D11 WARP\0","选择界面绘制方式，影响显卡兼容性和绘制性能。WARP 使用软件绘制，适合排查显卡问题但通常更慢；应用并保存后重启生效。");
            ENUM(gui.font.chineseGlyphRange,"中文字形","常用简体\0完整中文字形\0","选择可显示的中文字形。完整字形能显示更多生僻字，但增加字体资源占用；应用并保存后重启生效。"); break;
        case config::SettingsPage::Wave:
            ENUM(gui.wave.controlMode,"操作模式","示波器\0兼容全局\0","选择波形交互习惯。示波器模式默认缩放时间轴，并可单独调整当前通道；兼容全局模式默认同时缩放两轴。切换保留数据和当前视口。");
            ENUM(gui.wave.displayFormula,"显示公式","先偏移再缩放\0先缩放再偏移\0","决定通道偏移与缩放的计算顺序。先偏移再缩放时偏移也会随倍率变化，先缩放再偏移时偏移保持独立；不改变采集数据。");
            ENUM(gui.wave.gridDivisionReadoutMode,"格值读数","显示值\0实际值\0原始值\0","选择每格读数的数值含义。显示值反映当前显示变换，实际值使用通道实际数值，原始值使用原始数值；影响读数而非采集数据。");
            VALUE(gui.wave.channelScaleWheelEnabled,"工程档位滚轮","让滚轮按工程档位调整通道缩放，便于选择常用倍率。关闭后改为连续缩放。");
            ENUM(gui.wave.channelScaleWheelAcceleration,"滚轮加速","无\0线性\0对数\0","控制快速滚动时跨越工程档位的速度。加速越强，调整大范围越快，但精细定位更难；仅工程档位滚轮开启时生效。");
            ENUM(gui.wave.channelCardWidthMode,"通道卡片宽度","固定\0自适应\0","控制通道卡片的横向布局。固定模式保持指定像素宽度，自适应模式随可用空间调整宽度。");
            VALUE(gui.wave.channelCardFixedWidth,"固定宽度 px","设置通道卡片宽度，单位像素。增大可显示更完整的信息，但一行容纳的卡片更少；仅固定模式生效。");
            VALUE(gui.wave.channelCardAdaptiveRatio,"宽度比例","设置通道卡片占可用宽度的比例。增大后卡片更宽，最终宽度限制在 160–220 像素；仅自适应模式生效。");
            ENUM(gui.wave.channelDoubleClickAction,"通道双击","复位全部\0复位缩放偏移\0复位缩放\0复位偏移\0","选择双击通道时复位哪些参数。可只复位缩放或偏移以保留另一项调整；下次双击生效。");
            ENUM(gui.wave.xAxisDoubleClickAction,"X轴双击","适配全部历史\0适配可见窗口\0","选择双击 X 轴时适配的时间范围。全部历史便于查看全貌，可见窗口围绕当前窗口调整；下次双击生效。");
            ENUM(gui.wave.yAxisDoubleClickAction,"Y轴双击","适配可见通道\0适配当前通道\0","选择双击 Y 轴时用于适配幅度的通道。可见通道兼顾多条曲线，当前通道便于单独观察；下次双击生效。");
            VALUE(gui.wave.yAxisDoubleClickAdjustOffset,"Y适配偏移（兼容）","保留旧配置的 Y 轴适配偏移选项。当前调整此项不会影响轴适配结果。"); ImGui::TextWrapped("兼容项：当前不影响适配");
            ENUM(gui.wave.hiddenChannelPolicy,"隐藏通道","参与派生视图\0排除派生视图\0","决定隐藏通道是否仍参与派生视图。排除后派生展示只关注显示中的通道；隐藏通道的历史数据仍保留。");
            ENUM(gui.wave.cursorExtremeSnapPolicy,"极值吸附","最近波形\0视口区域\0","选择游标吸附极值时的搜索范围。最近波形关注附近曲线，视口区域在当前可见范围内搜索；下次吸附生效，不移动已有游标。");
            ENUM(gui.wave.mouseYOffsetDragMode,"Y偏移拖动","直接\0Shift\0禁用\0","选择鼠标调整通道 Y 偏移的触发方式。Shift 模式减少误拖动，禁用后鼠标不再调整偏移；后续拖动生效。");
            ENUM(gui.wave.legendOverlayOpenMode,"浮动图例","悬停\0双击\0禁用\0","选择浮动图例的打开方式。悬停便于快速查看，双击减少意外打开，禁用后不显示浮动图例。");
            VALUE(gui.wave.legendOverlayDoubleClickAutoCollapse,"图例自动折叠","控制双击打开的浮动图例是否自动收起。关闭后图例保持展开，便于连续操作；仅双击图例模式生效。"); ImGui::TextWrapped("自动折叠仅双击图例模式生效");
            VALUE(gui.wave.interactionAnimationEnabled,"波形动画","为波形交互增加平滑动画。关闭后变化更直接；开启还需常规页的交互反馈总开关同时开启。");
            ImGui::TextWrapped("动画有效：%s",v.gui.interactionFeedback.enabled && v.gui.wave.interactionAnimationEnabled ? "开启":"关闭（受两开关共同控制）");
            VALUE(gui.wave.zoomSelectionAutoExit,"框选后自动退出","完成框选缩放后自动退出框选模式。关闭后可连续框选；后续框选生效，当前选区保留。");
            ENUM(gui.wave.fullscreenMode,"全屏策略","聚焦\0覆盖\0","选择波形全屏的布局方式。聚焦模式使用工作区聚焦布局，覆盖模式覆盖整个视口；下次进入全屏生效。"); break;
        case config::SettingsPage::Logging: {
            ENUM(logging.level,"日志等级","Trace\0Debug\0Info\0Warn\0Error\0","控制记录哪些严重程度的日志。Trace、Debug 提供更多排查信息，也增加日志量；Warn、Error 更精简。应用后生效，与菜单日志等级同步。");
            text("日志文件",v.logging.filePath,"指定日志输出文件，便于长期保存和排查问题。内建默认为空，表示关闭文件输出；相对路径以可执行文件目录为起点。应用时检查路径，失败则继续使用旧文件。");
            const auto logPath = std::filesystem::path(v.logging.filePath);
            ImGui::TextWrapped("草稿解析路径：%s", v.logging.filePath.empty() ? "文件输出关闭" :
                (logPath.is_absolute() ? logPath : executableDir_ / logPath).string().c_str());
            VALUE(logging.payloadPreview.enabled,"载荷预览","在日志中显示原始数据载荷，便于检查通信内容。开启会增加日志体积，也可能记录敏感数据；采集数据不受影响。");
            VALUE(logging.payloadPreview.maxBytes,"预览 bytes","限制每条日志展示的载荷字节数。增大可看到更多内容，但日志更长；0 不显示载荷字节，并非无限。");
            VALUE(logging.maxFileSizeBytes,"轮转大小 bytes","设置单个日志文件达到多大时轮转备份，单位字节。增大后轮转更少、单文件更大；0 按 1 字节处理。");
            VALUE(logging.maxFiles,"轮转备份数","设置保留多少份轮转备份。增大可保留更长日志历史，也占用更多磁盘；0 按 1 份处理，当前活动文件另计。");
            ImGui::TextWrapped("轮转有效：%llu bytes / %llu 份",static_cast<unsigned long long>((std::max)(std::size_t{1},v.logging.maxFileSizeBytes)),static_cast<unsigned long long>((std::max)(std::size_t{1},v.logging.maxFiles)));
            VALUE(gui.logHistory.transferRawLimit,"原始收发历史","限制面板保留的原始收发记录行数。增大可浏览更多历史，但占用更多内存；0 不保留。缩减并应用前需确认裁剪，已裁剪行无法恢复。");
            VALUE(gui.logHistory.transferFrameLimit,"协议帧历史","限制逐帧列表保留的展示行数，也影响待显示的解析结果。增大保留更多结果，但增加内存占用；0 不保留。缩减并应用前需确认裁剪，部分转换结果可能不再保留。");
            VALUE(gui.logHistory.hostLimit,"宿主日志历史","限制宿主日志面板保留的行数。增大便于回看更早日志，但增加内存占用；0 不保留。缩减并应用前需确认裁剪，旧行无法恢复。");
            VALUE(gui.logHistory.scriptLimit,"脚本日志历史","限制脚本日志面板保留的行数。增大便于排查较早的脚本输出，但增加内存占用；0 不保留。缩减并应用前需确认裁剪，旧行无法恢复。");
            VALUE(gui.logHistory.requestTraceLimit,"请求追踪历史","限制面板保留的请求追踪行数。增大可回看更多请求过程，但增加内存占用；0 不保留。缩减可能裁剪历史，不改变请求队列容量。");
            VALUE(gui.sendHistoryLimit,"发送历史","限制可再次选择的发送历史条数。增大方便重复发送，最多保留 200 条；0 清空。缩减并应用前需确认裁剪。");
            ImGui::Text("发送有效容量：%llu",static_cast<unsigned long long>((std::min)(v.gui.sendHistoryLimit,std::size_t{200})));
            ENUM(gui.importProtocolSource,"导入协议来源","优先现场包协议\0使用当前协议\0","优先包内协议用于按现场定义解析，包内缺少协议时使用当前协议；使用当前协议用于按本机定义解析，不执行包内脚本。导入确认后初始化所选协议；首次浏览不重新计算波形快照。");
            ENUM(gui.historyOnParsedSwitch,"逐帧历史策略","每次询问\0使用历史\0不使用历史\0","决定切换逐帧视图时是否重新解析已有历史。每次询问便于按需选择；使用历史更新逐帧列表；不使用历史保留已有结果，只解析后续数据。仅有可解析协议和所选历史时询问，无需暂停采集；历史转换不执行历史脚本或改变波形。");
            ENUM(gui.historyScope,"逐帧历史范围","面板保留行\0底层保留采集\0","选择逐帧转换使用哪些原始记录。面板保留行只使用当前保留的 RX/TX 行；底层保留采集可使用面板清空后仍保留的采集或导入记录。范围越大，转换可能越久；已裁剪字节无法恢复，不会重新读取文件。");
            break; }
        case config::SettingsPage::Performance: {
            const auto effective=application_.effectiveSettingsPerformance(); const auto& comm=application_.docks().commState();
            ImGui::TextWrapped("运行自适应：%s；压力 %s；有效倍率 %.3f / 追赶 %.3f；%s",comm.adaptivePerformanceEnabled?"开启":"关闭",comm.adaptivePerformanceLevel.c_str(),comm.adaptivePerformanceEffectiveMultiplier,comm.adaptivePerformanceCatchUpMultiplier,comm.adaptivePerformanceReason.c_str());
            ImGui::TextWrapped("静态值可编辑并保存；运行自适应接管8项，关闭后才使用静态值。草稿开关不代表运行开关。");
            VALUE(performance.adaptive.enabled,"自适应（草稿）","根据运行压力自动调整界面帧率、波形绘制和数据处理预算。开启可缓解积压，关闭后使用静态设置；应用后生效，不调整资源队列和连接缓冲容量。");
            VALUE(performance.adaptive.maxMultiplier,"最大倍率 K","限制自适应预算可使用的最大倍率。增大允许使用更高预算，也可能增加处理和绘制开销；允许 0.25–4.0，仅自适应开启时生效。");
            VALUE(app.fpsLimit,"静态 FPS","限制界面每秒刷新次数。增大让显示更流畅，也增加处理和绘制开销；0 按至少 1 FPS 处理。自适应关闭后使用此值，不跟随公共倍率。");
            VALUE(performance.scale,"公共倍率 scale","统一调整跟随倍率的默认预算。增大可提高每轮处理量和部分资源容量，也可能增加内存占用；单独设置的预算不随倍率改变。自适应开启时刷新与处理调度由自适应控制，连接缓冲下次连接生效。");
            VALUE(gui.wave.maxRenderPointsPerChannel,"静态通道点数","限制单个通道每次绘制的点数。增大可保留更多波形细节，也增加绘制开销；0 使用内建点数上限，并非无限。自适应关闭后使用此值，不跟随公共倍率。");
            VALUE(gui.wave.maxRenderVertices,"静态总顶点","限制波形绘制的总顶点数。增大可容纳更多通道和细节，但增加绘制开销；0 使用内建顶点上限，并非无限。自适应关闭后使用此值。");
            VALUE(gui.wave.overviewMaxSamples,"静态概览桶","限制概览汇总的桶数，决定长历史概览的细节量。增大可展示更细的变化，但增加计算和绘制开销；0 取消本项桶数限制。自适应关闭后使用此值。");
            ImGui::TextWrapped("当前有效 FPS %u / 点 %llu / 顶点 %llu / 桶 %llu",effective.fpsLimit,(unsigned long long)effective.maxRenderPointsPerChannel,(unsigned long long)effective.maxRenderVertices,(unsigned long long)effective.overviewMaxSamples);
            if(ImGui::CollapsingHeader("低频预算")) {
#define BUDGET(path, flag, label, desc) if(VALUE(path,label,desc)) v.performance.explicitOverrides.flag=true; \
                ImGui::SameLine(); if(ImGui::SmallButton("跟随倍率##" #flag)) { v.performance.explicitOverrides.flag=false; std::string error; if(!config::recomputePerformanceDefaults(v,error)) settingsError_=error; } \
                help("让此项重新随公共倍率调整，放弃单独设定的预算；自适应开启时仍由自适应控制。"); \
                ImGui::Text("%s",v.performance.explicitOverrides.flag?"显式静态覆盖":"跟随倍率缺省")
                BUDGET(gui.realtimeBacklog.rxChunkBytesPerPump,realtimeBacklogRxChunkBytesPerPump,"RX bytes/pump","限制每轮处理的接收字节数。增大可更快消化积压，但单轮处理更久；0 按至少 1 字节处理。自适应关闭后使用静态预算。");
                BUDGET(gui.realtimeBacklog.transferFrameRowsPerPump,realtimeBacklogTransferFrameRowsPerPump,"帧 rows/pump","限制每轮更新逐帧列表的行数。增大可更快展示结果，但可能降低界面响应；0 按至少 1 行处理。自适应关闭后使用静态预算。");
                BUDGET(gui.realtimeBacklog.plotAppendsPerPump,realtimeBacklogPlotAppendsPerPump,"绘图追加/pump","限制每轮向波形提交的追加操作次数，一次操作可含多个样本。增大可更快更新波形，但单轮处理更久；0 按至少 1 次处理。自适应关闭后使用静态预算。");
                BUDGET(scripting.workerOutputFlushBudgetMs,workerOutputFlushBudgetMs,"flush ms/pump","限制每轮整理脚本输出所用的时间，单位毫秒。增大可更快消化积压，但减少界面响应时间；0 不设时间预算，可能使单轮处理过久。自适应关闭后使用静态预算。");
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
