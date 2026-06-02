#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include "../runtime/gui_runtime_detail.hpp"

namespace protoscope::ui {

namespace {

std::string luaControlImGuiLabel(const scripting::ControlDescriptor& descriptor) {
    return descriptor.label + "##lua_control_" + descriptor.id;
}

float compactLogToolbarHeight() {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float controlHeight = ImGui::GetFrameHeight();
    const float extraPadding = style.WindowPadding.y * 0.5F;
    return controlHeight + extraPadding + 16.0f;
}

} // namespace

void GuiRuntime::drawStatusBar() {
    const auto& tokens = defaultUiStyleTokens();
    auto& comm = application_.docks().commState();
    auto& config = application_.docks().configState();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - 44.0F));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 44.0F));
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 8.0F));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07F, 0.09F, 0.13F, 0.98F));
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.panelBorder);
    if (ImGui::Begin("状态栏", nullptr, flags)) {
        drawHeaderBadge(transportStateLabel(comm.state), comm.state == transport::TransportState::Open ? tokens.success : tokens.warning, false);
        if (config.dirty) {
            ImGui::SameLine();
            drawHeaderBadge("配置未保存", tokens.warning, false);
        }
        if (config.pendingExternalReload) {
            ImGui::SameLine();
            drawHeaderBadge(config.externalReloadMessage.empty() ? "检测到外部更新" : config.externalReloadMessage.c_str(), tokens.warning, false);
            ImGui::SameLine();
            if (drawGhostIconButton("重载配置", "从磁盘重载当前配置")) {
                if (!reloadConfigFromDisk()) {
                    application_.setStatusMessage("从磁盘重载配置失败", true);
                }
            }
        }
        if (comm.reconnectRequired) {
            ImGui::SameLine();
            drawHeaderBadge("通讯参数变更待重连", tokens.warning, false);
        }
        if (application_.isRawCaptureRecording()) {
            const auto fileName = application_.rawCaptureRecordingPath().filename().generic_string();
            const std::string recordingText = "录制 " + (fileName.empty() ? std::string("(未命名)") : fileName)
                + " " + std::to_string(static_cast<unsigned long long>(application_.rawCaptureRecordingBytes())) + " bytes";
            ImGui::SameLine();
            drawHeaderBadge(recordingText.c_str(), tokens.danger, true);
        }
        if (comm.pendingRxBytes > 0U || comm.pendingTransferFrameRows > 0U || comm.pendingPlotAppends > 0U) {
            const std::string pendingText =
                "待处理 RX " + std::to_string(comm.pendingRxBytes)
                + " / 帧 " + std::to_string(comm.pendingTransferFrameRows)
                + " / 绘图 " + std::to_string(comm.pendingPlotAppends);
            ImGui::SameLine();
            drawHeaderBadge(pendingText.c_str(), tokens.accent, false);
        }
        if (!config.statusMessage.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", config.statusMessage.c_str());
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
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

    const auto& transportItems = transport::transportDescriptors();
    int kindIndex = 0;
    for (std::size_t index = 0; index < transportItems.size(); ++index) {
        if (transportItems[index].kind == comm.kind) {
            kindIndex = static_cast<int>(index);
            break;
        }
    }
    std::vector<const char*> itemLabels;
    itemLabels.reserve(transportItems.size());
    for (const auto& item : transportItems) {
        itemLabels.push_back(item.label.data());
    }
    if (ImGui::Combo("模式", &kindIndex, itemLabels.data(), static_cast<int>(itemLabels.size()))) {
        comm.kind = transportItems[static_cast<std::size_t>(kindIndex)].kind;
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
    } else if (comm.kind == transport::TransportKind::Serial) {
        if (!serialPortsScanned_) {
            refreshSerialPortOptions(comm);
            serialPortsScanned_ = true;
        }
        if (ImGui::Button("刷新串口列表")) {
            refreshSerialPortOptions(comm);
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
        syncDraftFromModel(commonBaudRateDraft_, commonBaudRateDraftModel_, currentBaudText);
        if (const auto baudEdit = drawEditableCombo("波特率", commonBaudRateDraft_, baudRateOptions, digitsOnly); baudEdit.edited) {
            if (!baudEdit.valid) {
                application_.setStatusMessage("波特率仅接受纯数字");
            } else {
                const auto baudRate = static_cast<std::uint32_t>(std::stoul(baudEdit.value));
                if (baudRate != comm.serial.baudRate) {
                    comm.serial.baudRate = baudRate;
                    commonBaudRateDraftModel_ = baudEdit.value;
                    application_.markCommConfigEdited(true);
                }
            }
        }
        if (!digitsOnly(commonBaudRateDraft_)) {
            ImGui::TextDisabled("波特率仅接受纯数字");
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
    } else if (comm.kind == transport::TransportKind::UdpPeer) {
        char bindAddress[256]{};
        std::snprintf(bindAddress, sizeof(bindAddress), "%s", comm.udpPeer.bindAddress.c_str());
        if (ImGui::InputText("本地地址", bindAddress, sizeof(bindAddress))) {
            comm.udpPeer.bindAddress = bindAddress;
            application_.markCommConfigEdited(true);
        }
        int bindPort = comm.udpPeer.bindPort;
        if (ImGui::InputInt("本地端口", &bindPort)) {
            comm.udpPeer.bindPort = static_cast<std::uint16_t>(std::clamp(bindPort, 0, 65535));
            application_.markCommConfigEdited(true);
        }
        char remoteHost[256]{};
        std::snprintf(remoteHost, sizeof(remoteHost), "%s", comm.udpPeer.remoteHost.c_str());
        if (ImGui::InputText("远端地址", remoteHost, sizeof(remoteHost))) {
            comm.udpPeer.remoteHost = remoteHost;
            application_.markCommConfigEdited(true);
        }
        int remotePort = comm.udpPeer.remotePort;
        if (ImGui::InputInt("远端端口", &remotePort)) {
            comm.udpPeer.remotePort = static_cast<std::uint16_t>(std::clamp(remotePort, 0, 65535));
            application_.markCommConfigEdited(true);
        }
    }

    ImGui::Separator();
    ImGui::Text("当前模式: %s", transport::transportKindLabel(comm.kind).data());
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

    if (comm.pendingRxBytes > 0U || comm.pendingTransferFrameRows > 0U || comm.pendingPlotAppends > 0U) {
        ImGui::Text("实时待处理: RX %zu bytes / 逐帧 %zu / 波形 %zu",
                    comm.pendingRxBytes,
                    comm.pendingTransferFrameRows,
                    comm.pendingPlotAppends);
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
        std::string dialogError;
        const auto selectedDir = nativeDirectoryDialog(window_, L"选择协议根目录", std::filesystem::path(lua.protocolRootDir), dialogError);
        if (!dialogError.empty()) {
            application_.setStatusMessage(dialogError);
        }
        if (selectedDir.has_value()) {
            lua.protocolRootDir = selectedDir->generic_string();
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
        // 这里直接按引用遍历当前帧控件，配合 bool 冒泡在触发更新后立刻停止本帧遍历。
        const auto& controls = lua.controlStates;
        drawLuaDockFlow(controls);
        ImGui::End();
        return;
    }
    ImGui::End();

}

bool GuiRuntime::drawLuaDockFlow(const std::vector<scripting::ControlSnapshot>& controls) {
    for (const auto& control : controls) {
        if (drawDynamicControl(control)) {
            return true;
        }
    }
    return false;
}


void GuiRuntime::drawTransferDock()
{
    if (!showTransferDock_)
    {
        return;
    }

    auto& sendState = application_.docks().sendState();
    const auto& comm = application_.docks().commState();
    auto& receive = application_.docks().receiveState();

    if (!ImGui::Begin("收发数据", &showTransferDock_))
    {
        ImGui::End();
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();

    const float availableHeight = ImGui::GetContentRegionAvail().y;
    const float splitterThickness = style.FramePadding.y * 2.0F;
    const float minPayloadHeight = ImGui::GetFrameHeight();
    const float sendBorderSlack = 2.0F;
    // 发送区最小高度要覆盖 Child 边距、表格 CellPadding 和边框，否则窗口较小时输入框会被挤没。
    const float minSendHeight =
        minPayloadHeight +
        style.WindowPadding.y * 2.0F +
        style.CellPadding.y * 2.0F +
        sendBorderSlack;
    if (transferSendSectionHeight_ <= 0.0F) {
        transferSendSectionHeight_ = minSendHeight;
    }

    const float maxSendHeight =
        (std::max)(minSendHeight,
                   availableHeight - splitterThickness);
    transferSendSectionHeight_ = (std::clamp)(
        transferSendSectionHeight_,
        minSendHeight,
        maxSendHeight);

    float logHeight =
        (std::max)(0.0F,
                   availableHeight - transferSendSectionHeight_ - splitterThickness);

    // =========================
    // 上方：收发记录区
    // =========================
    if (ImGui::BeginChild(
            "##transfer_log_section",
            ImVec2(0.0F, logHeight),
            true)) {
        if (ImGui::BeginTable(
                "##transfer_log_toolbar",
                12,
                ImGuiTableFlags_SizingFixedFit |
                ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn(
                "title",
                ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableSetupColumn("keyword", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("status", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("all", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("rx", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("tx", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("mode", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("hex", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("time", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("pause", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("export", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("clear", ImGuiTableColumnFlags_WidthFixed);

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(PROTOSCOPE_ICON_EXCHANGE " 收发记录");

            ImGui::TableSetColumnIndex(1);
            drawLogKeywordFilterInput("关键字##transfer_log_keyword", receive.filter, 150.0F);

            ImGui::TableSetColumnIndex(2);
            drawLogStatusFilterCombo("STATUS##transfer_log_status", receive.filter);

            ImGui::TableSetColumnIndex(3);
            drawLogStatusFilterButton(
                "全部",
                dock::LogStatusFilter::All,
                receive.filter);

            ImGui::TableSetColumnIndex(4);
            drawLogStatusFilterButton(
                "RX",
                dock::LogStatusFilter::Rx,
                receive.filter);

            ImGui::TableSetColumnIndex(5);
            drawLogStatusFilterButton(
                "TX",
                dock::LogStatusFilter::Tx,
                receive.filter);

            ImGui::TableSetColumnIndex(6);
            const bool parsedFrames = receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames;
            if (ImGui::SmallButton(parsedFrames ? "逐帧" : "原始")) {
                receive.displayMode = parsedFrames ? dock::TransferLogDisplayMode::RawChunks
                                                   : dock::TransferLogDisplayMode::ParsedFrames;
                if (receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames) {
                    application_.activateParsedTransferLogView();
                }
            }
            drawIconTooltip(parsedFrames ? "按 Lua stream() schema 逐帧显示" : "按运输层原始分块显示");

            ImGui::TableSetColumnIndex(7);
            drawIconCheckbox(
                PROTOSCOPE_ICON_HEX,
                &receive.showHex,
                "显示 HEX");

            ImGui::TableSetColumnIndex(8);
            drawIconCheckbox(
                PROTOSCOPE_ICON_CLOCK,
                &receive.showTimestamps,
                "显示时间戳");

            ImGui::TableSetColumnIndex(9);
            drawIconCheckbox(
                receive.pauseScroll ? PROTOSCOPE_ICON_PLAY : PROTOSCOPE_ICON_PAUSE,
                &receive.pauseScroll,
                "暂停滚动");

            ImGui::TableSetColumnIndex(10);
            if (ImGui::Button("导出##transfer_log_export")) {
                openTransferLogExportDialog();
            }
            drawIconTooltip("导出当前过滤后的收发记录");

            ImGui::TableSetColumnIndex(11);
            if (drawIconButton(PROTOSCOPE_ICON_TRASH, "清空收发记录")) {
                application_.docks().clearReceiveRows();
                application_.rebuildTransferFrameRows();
            }

            ImGui::EndTable();
        }

        const auto& visibleRows = receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames ? receive.frameRows : receive.rows;
        const auto visibleRowsVersion = receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames
                                            ? receive.frameRowsVersion
                                            : receive.rowsVersion;
        const auto& filteredRows =
            filteredLogRowsCached(transferLogRowsCache_, visibleRows, visibleRowsVersion, receive.filter, true);

        drawTransferLogRows(
            "transfer_rows",
            filteredRows.rows,
            receive.showTimestamps,
            receive.showHex,
            receive.pauseScroll,
            receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames ? "暂无已解析帧" : "暂无 TX/RX 原始数据",
            filteredRows.endpointWidth);
    }
    ImGui::EndChild();

    // =========================
    // splitter
    // drawHorizontalSplitter 按 top height 工作
    // 所以这里传 logHeight
    // =========================
    drawHorizontalSplitter(
        "##transfer_splitter",
        logHeight,
        0.0F,
        minSendHeight,
        availableHeight,
        splitterThickness);

    transferSendSectionHeight_ =
        (std::max)(minSendHeight,
                   availableHeight - logHeight - splitterThickness);

    // =========================
    // 下方：发送区
    // HEX | Payload | SEND
    // =========================
    if (ImGui::BeginChild(
            "##transfer_send_section",
            ImVec2(0.0F, transferSendSectionHeight_),
            true)) {
        char buffer[2048]{};
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%s",
            sendState.payload.c_str());

        ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap;
        if (sendState.hexMode) {
            flags |= ImGuiInputTextFlags_CallbackEdit |
                     ImGuiInputTextFlags_CallbackCharFilter;
        }

        const float payloadHeight =
            (std::max)(minPayloadHeight,
                       ImGui::GetContentRegionAvail().y - style.CellPadding.y * 2.0F);
        const float frameHeight = ImGui::GetFrameHeight();
        const float hexWidth = (std::max)(ImGui::CalcTextSize("HEX").x + style.FramePadding.x * 2.0F, frameHeight * 2.4F);
        const float sendWidth = (std::max)(ImGui::CalcTextSize("发送").x + style.FramePadding.x * 2.0F, frameHeight * 2.8F);
        const float minHistoryWidth = frameHeight * 4.0F;
        sendState.historyComboWidth = (std::clamp)(sendState.historyComboWidth, minHistoryWidth, (std::max)(minHistoryWidth, ImGui::GetContentRegionAvail().x * 0.45F));
        const auto historyLimit = configuredSendHistoryLimit(application_.captureConfig());
        dock::trimSendHistory(sendState, historyLimit);

        if (ImGui::BeginTable(
                "##transfer_send_table",
                4,
                ImGuiTableFlags_SizingStretchProp |
                ImGuiTableFlags_Resizable |
                ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn(
                "hex",
                ImGuiTableColumnFlags_WidthFixed,
                hexWidth);

            ImGui::TableSetupColumn(
                "payload",
                ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableSetupColumn(
                "history",
                ImGuiTableColumnFlags_WidthFixed,
                sendState.historyComboWidth);

            ImGui::TableSetupColumn(
                "send",
                ImGuiTableColumnFlags_WidthFixed,
                sendWidth);

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            {
                const float oldY = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(
                    oldY + (payloadHeight - frameHeight) * 0.5F);

                if (sendState.hexMode) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                if (ImGui::Button("HEX", ImVec2(-FLT_MIN, 0.0F))) {
                    if (!application_.setSendHexMode(!sendState.hexMode)) {
                        application_.setStatusMessage("HEX 模式切换失败", true);
                    }
                }
                if (sendState.hexMode) {
                    ImGui::PopStyleColor(2);
                }
                drawIconTooltip(sendState.hexMode ? "HEX 发送已开启" : "HEX 发送已关闭");

                ImGui::SetCursorPosY(oldY);
            }

            ImGui::TableSetColumnIndex(1);
            {
                ImGui::SetNextItemWidth(-FLT_MIN);

                if (ImGui::InputTextMultiline(
                        "##raw_payload",
                        buffer,
                        sizeof(buffer),
                        ImVec2(-FLT_MIN, payloadHeight),
                        flags,
                        sendState.hexMode ? hexEditorCallback : nullptr)) {
                    sendState.payload = buffer;
                }
            }

            ImGui::TableSetColumnIndex(2);
            {
                const float oldY = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(
                    oldY + (payloadHeight - frameHeight) * 0.5F);

                ImGui::SetNextItemWidth(-FLT_MIN);
                const std::string preview = sendState.history.empty() ? "无历史记录" : "发送历史";
                if (ImGui::BeginCombo("##send_history", preview.c_str())) {
                    if (sendState.history.empty()) {
                        ImGui::TextDisabled("暂无发送历史");
                    } else {
                        for (const auto& item : sendState.history) {
                            if (ImGui::Selectable(item.c_str())) {
                                sendState.payload = item;
                            }
                        }
                        ImGui::Separator();
                        if (ImGui::Selectable("清空历史")) {
                            sendState.history.clear();
                            saveCurrentProtocolControlState();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SetCursorPosY(oldY);
            }

            ImGui::TableSetColumnIndex(3);
            {
                const float oldY = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(
                    oldY + (payloadHeight - frameHeight) * 0.5F);

                if (ImGui::Button("发送", ImVec2(-FLT_MIN, 0.0F))) {
                    if (application_.sendManualPayload(sendState.payload, sendState.hexMode)) {
                        dock::rememberSendHistory(sendState, sendState.payload, historyLimit);
                        saveCurrentProtocolControlState();
                    } else {
                        application_.setStatusMessage(comm.lastError, true);
                    }
                }
                drawIconTooltip("发送原始载荷");

                ImGui::SetCursorPosY(oldY);
            }

            ImGui::EndTable();
        }
    }
    ImGui::EndChild();

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

    const float toolbarHeight = compactLogToolbarHeight();
    if (beginToolbarGroup("host_log_toolbar", nullptr, toolbarHeight)) {
        drawLogKeywordFilterInput("关键字##host_log_keyword", logState.filter, 190.0F);
        ImGui::SameLine();
        drawLogStatusFilterCombo("STATUS##host_log_status", logState.filter);
        ImGui::SameLine();
        ImGui::Checkbox("显示时间戳", &logState.showTimestamps);
        ImGui::SameLine();
        ImGui::Checkbox("暂停滚动", &logState.pauseScroll);
        ImGui::SameLine();
        if (drawDangerIconButton(PROTOSCOPE_ICON_TRASH " 清空", "清空当前宿主日志")) {
            application_.docks().clearLogRows();
        }
        ImGui::SameLine();
        if (drawGhostIconButton("导出", "导出当前筛选后的宿主日志")) {
            openHostLogExportDialog();
        }
    }
    endToolbarGroup();

    const auto& filteredRows =
        filteredLogRowsCached(hostLogRowsCache_, logState.rows, logState.rowsVersion, logState.filter, false);
    drawRowList("log_rows", filteredRows.rows, logState.showTimestamps, false, logState.pauseScroll, "暂无宿主日志", filteredRows.endpointWidth);
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

    const float toolbarHeight = compactLogToolbarHeight();
    if (beginToolbarGroup("script_log_toolbar", nullptr, toolbarHeight)) {
        drawLogKeywordFilterInput("关键字##script_log_keyword", scriptState.filter, 190.0F);
        ImGui::SameLine();
        drawLogStatusFilterCombo("STATUS##script_log_status", scriptState.filter);
        ImGui::SameLine();
        ImGui::Checkbox("显示时间戳", &scriptState.showTimestamps);
        ImGui::SameLine();
        ImGui::Checkbox("暂停滚动", &scriptState.pauseScroll);
        ImGui::SameLine();
        if (drawDangerIconButton(PROTOSCOPE_ICON_TRASH " 清空", "清空当前 Lua 日志与事件")) {
            application_.docks().clearScriptRows();
        }
        ImGui::SameLine();
        if (drawGhostIconButton("导出", "导出当前筛选后的 Lua 日志")) {
            openScriptLogExportDialog();
        }
    }
    endToolbarGroup();

    const auto& filteredRows =
        filteredLogRowsCached(scriptLogRowsCache_, scriptState.rows, scriptState.rowsVersion, scriptState.filter, false);
    drawRowList("script_rows", filteredRows.rows, scriptState.showTimestamps, false, scriptState.pauseScroll, "暂无 Lua 日志或事件", filteredRows.endpointWidth);
    ImGui::End();
}
bool GuiRuntime::drawDynamicControl(const scripting::ControlSnapshot& control) {
    const auto& descriptor = control.descriptor;
    const std::string imguiLabel = luaControlImGuiLabel(descriptor);
    switch (descriptor.type) {
    case scripting::ControlType::Button:
        if (ImGui::Button(imguiLabel.c_str())) {
            application_.updateControlValue(descriptor.id, true);
            return true;
        }
        break;
    case scripting::ControlType::Checkbox: {
        bool checked = std::get<bool>(control.value);
        if (ImGui::Checkbox(imguiLabel.c_str(), &checked)) {
            application_.updateControlValue(descriptor.id, checked);
            return true;
        }
        break;
    }
    case scripting::ControlType::InputText: {
        char buffer[512]{};
        std::snprintf(buffer, sizeof(buffer), "%s", std::get<std::string>(control.value).c_str());
        if (ImGui::InputText(imguiLabel.c_str(), buffer, sizeof(buffer))) {
            application_.updateControlValue(descriptor.id, std::string(buffer));
            return true;
        }
        break;
    }
    case scripting::ControlType::Combo: {
        int index = std::get<int>(control.value);
        std::vector<const char*> items;
        for (const auto& option : descriptor.comboOptions) {
            items.push_back(option.c_str());
        }
        if (!items.empty() && ImGui::Combo(imguiLabel.c_str(), &index, items.data(), static_cast<int>(items.size()))) {
            application_.updateControlValue(descriptor.id, index);
            return true;
        }
        break;
    }
    case scripting::ControlType::ElfSymbolCombo: {
        auto& state = elfSymbolComboStates_[descriptor.id];
        const auto& current = std::get<scripting::ElfSymbolValue>(control.value);
        if (state.draft.empty() && !current.label.empty()) {
            state.draft = current.label;
        }

        const auto comboConfig = application_.captureConfig().gui.elfSymbolCombo;
        const std::size_t effectiveLimit = descriptor.limitConfigured ? descriptor.limit : comboConfig.limit;
        const int effectiveDebounceMs = descriptor.debounceMsConfigured ? descriptor.debounceMs : comboConfig.debounceMs;
        const auto loadedRevision = application_.elfStaticAddressRevision();
        const auto currentMs = nowMs();
        if (state.editedAtMs == 0) {
            state.editedAtMs = currentMs;
        }
        const bool elfReloaded = state.loadedRevision != loadedRevision;
        const bool queryLimitChanged = state.queriedLimit != effectiveLimit;
        const bool debounceElapsed =
            currentMs >= state.editedAtMs + static_cast<std::uint64_t>(effectiveDebounceMs);
        if (elfReloaded || queryLimitChanged || (state.queriedDraft != state.draft && debounceElapsed)) {
            // 核心流程：ELF 成功加载后用空查询预热候选；输入变化后按配置消抖实时刷新候选列表。
            state.options = application_.queryElfStaticAddresses(state.draft, effectiveLimit);
            state.queriedDraft = state.draft;
            state.queriedLimit = effectiveLimit;
            state.loadedRevision = loadedRevision;
        }

        std::vector<std::string> labels;
        labels.reserve(state.options.size());
        for (const auto& option : state.options) {
            labels.push_back(option.label);
        }

        const auto edit = drawEditableCombo(imguiLabel.c_str(),
                                            state.draft,
                                            labels,
                                            EditableComboOptions{.keepPopupOpenWhileEditing = true});
        if (edit.edited) {
            state.draft = edit.value;
            state.editedAtMs = currentMs;
        }
        if (edit.selectedFromList) {
            const auto selected = std::find_if(state.options.begin(), state.options.end(), [&](const auto& option) {
                return option.label == edit.value;
            });
            if (selected != state.options.end()) {
                application_.updateControlValue(descriptor.id, *selected);
                return true;
            }
        }
        break;
    }
    case scripting::ControlType::InputInt: {
        int value = std::get<int>(control.value);
        if (ImGui::InputInt(imguiLabel.c_str(), &value)) {
            application_.updateControlValue(descriptor.id, value);
            return true;
        }
        break;
    }
    case scripting::ControlType::InputFloat: {
        float value = std::get<float>(control.value);
        if (ImGui::InputFloat(imguiLabel.c_str(), &value)) {
            application_.updateControlValue(descriptor.id, value);
            return true;
        }
        break;
    }
    }
    return false;
}

} // namespace protoscope::ui
