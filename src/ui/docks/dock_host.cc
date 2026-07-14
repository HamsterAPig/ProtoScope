#include "../runtime/gui_runtime_detail.hpp"
#include "lua_control_label.hpp"

#include "protoscope/ui/gui_runtime.hpp"
#include "protoscope/ui/ui_theme.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

namespace protoscope::ui {

namespace {

    std::string luaControlImGuiLabel(const scripting::ControlDescriptor& descriptor, std::string_view visibleLabel)
    {
        return std::string(visibleLabel) + "##lua_control_" + descriptor.id;
    }

    std::string luaControlHiddenImGuiLabel(const scripting::ControlDescriptor& descriptor)
    {
        return "##lua_control_" + descriptor.id;
    }

    std::string luaControlInputLabel(const scripting::ControlDescriptor& descriptor, std::string_view visibleLabel)
    {
        if (visibleLabel.empty() || descriptor.labelPosition == scripting::ControlLabelPosition::Right) {
            return luaControlImGuiLabel(descriptor, visibleLabel);
        }
        return luaControlHiddenImGuiLabel(descriptor);
    }

    void drawLuaControlCompactTooltip(const scripting::ControlDescriptor& descriptor, std::string_view visibleLabel)
    {
        if (luaControlUsesCompactLabel(descriptor, visibleLabel) &&
            ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
            ImGui::SetTooltip("%s", descriptor.label.c_str());
        }
    }

    void drawLuaControlLeftLabel(const scripting::ControlDescriptor& descriptor, std::string_view visibleLabel)
    {
        if (visibleLabel.empty() || descriptor.labelPosition != scripting::ControlLabelPosition::Left) {
            return;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(visibleLabel.data(), visibleLabel.data() + visibleLabel.size());
        drawLuaControlCompactTooltip(descriptor, visibleLabel);
        ImGui::SameLine();
    }

    class ScopedImGuiItemWidth final {
    public:
        explicit ScopedImGuiItemWidth(std::optional<float> width) : active_(width.has_value())
        {
            if (active_) {
                ImGui::PushItemWidth(*width);
            }
        }

        ~ScopedImGuiItemWidth()
        {
            if (active_) {
                ImGui::PopItemWidth();
            }
        }

    private:
        bool active_{false};
    };

    bool isLuaDynamicInputControl(scripting::ControlType type)
    {
        return type == scripting::ControlType::InputText || type == scripting::ControlType::InputInt ||
               type == scripting::ControlType::InputFloat || type == scripting::ControlType::Combo ||
               type == scripting::ControlType::ElfSymbolCombo;
    }

    std::uint32_t nextTxSequenceFrameId(const scripting::TxSequenceValue& value)
    {
        std::uint32_t maxId = 0;
        for (const auto& frame : value.frames) {
            maxId = std::max(maxId, frame.id);
        }
        return maxId == std::numeric_limits<std::uint32_t>::max() ? maxId : maxId + 1U;
    }

    scripting::TxSequenceFieldValue defaultTxSequenceFieldValue(
        const scripting::TxSequenceFieldDescriptor& field)
    {
        if (field.type == scripting::TxSequenceFieldType::String) {
            if (const auto* text = std::get_if<std::string>(&field.defaultValue)) {
                return *text;
            }
            return std::string();
        }
        if (const auto* number = std::get_if<std::int64_t>(&field.defaultValue)) {
            return *number;
        }
        return std::int64_t{0};
    }

    scripting::TxSequenceFrameValue makeTxSequenceUiFrame(const scripting::ControlDescriptor& descriptor,
                                                          std::uint32_t id)
    {
        scripting::TxSequenceFrameValue frame;
        frame.id = id;
        frame.enabled = true;
        frame.name = "Frame " + std::to_string(id);
        for (const auto& field : descriptor.txSequenceFields) {
            frame.fields[field.id] = defaultTxSequenceFieldValue(field);
        }
        return frame;
    }

    std::int64_t clampTxSequenceUiInteger(const scripting::TxSequenceFieldDescriptor& field, std::int64_t value)
    {
        switch (field.type) {
            case scripting::TxSequenceFieldType::U8:
                return std::clamp<std::int64_t>(value, 0, 0xFF);
            case scripting::TxSequenceFieldType::U16:
                return std::clamp<std::int64_t>(value, 0, 0xFFFF);
            case scripting::TxSequenceFieldType::I16:
                return std::clamp<std::int64_t>(value, -32768, 32767);
            case scripting::TxSequenceFieldType::U32:
                return std::clamp<std::int64_t>(
                    value, 0, static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max()));
            case scripting::TxSequenceFieldType::String:
                break;
        }
        return value;
    }

    bool drawTxSequenceIntegerInput(const char* label,
                                    const scripting::TxSequenceFieldDescriptor& field,
                                    std::int64_t& value)
    {
        if (field.radix != scripting::TxSequenceFieldRadix::Hex) {
            std::int64_t edited = value;
            if (ImGui::InputScalar(label, ImGuiDataType_S64, &edited, nullptr, nullptr, "%lld")) {
                value = edited;
                return true;
            }
            return false;
        }

        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%llX", static_cast<unsigned long long>(std::max<std::int64_t>(0, value)));
        if (!ImGui::InputText(label, buffer, sizeof(buffer), ImGuiInputTextFlags_CharsHexadecimal)) {
            return false;
        }
        std::uint64_t parsed = 0;
        const auto* begin = buffer;
        const auto* end = buffer + std::strlen(buffer);
        const auto result = std::from_chars(begin, end, parsed, 16);
        if (result.ec == std::errc{} && result.ptr == end) {
            value = static_cast<std::int64_t>(std::min<std::uint64_t>(
                parsed, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
        } else if (begin == end) {
            value = 0;
        }
        return true;
    }

    scripting::TxSequenceFieldValue txSequenceFieldValueOrDefault(
        const scripting::TxSequenceFieldDescriptor& field,
        const scripting::TxSequenceFrameValue& frame)
    {
        const auto iter = frame.fields.find(field.id);
        if (iter != frame.fields.end()) {
            return iter->second;
        }
        return defaultTxSequenceFieldValue(field);
    }

    std::string txSequenceFieldPreviewText(const scripting::TxSequenceFieldDescriptor& field,
                                           const scripting::TxSequenceFieldValue& value)
    {
        if (const auto* text = std::get_if<std::string>(&value)) {
            return *text;
        }
        const auto number = std::get_if<std::int64_t>(&value);
        if (number == nullptr) {
            return {};
        }
        char buffer[32]{};
        if (field.radix == scripting::TxSequenceFieldRadix::Hex) {
            std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(*number));
        } else {
            std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(*number));
        }
        return buffer;
    }

    bool drawTxSequenceOptionCombo(const char* label,
                                   const scripting::TxSequenceFieldDescriptor& field,
                                   scripting::TxSequenceFieldValue& value)
    {
        if (field.options.empty()) {
            return false;
        }

        const auto selected = std::find_if(field.options.begin(), field.options.end(), [&](const auto& option) {
            return option.value == value;
        });
        const std::string fallbackPreview = txSequenceFieldPreviewText(field, value);
        const char* preview = selected == field.options.end() ? fallbackPreview.c_str() : selected->label.c_str();
        bool updated = false;
        if (ImGui::BeginCombo(label, preview)) {
            for (const auto& option : field.options) {
                const bool isSelected = option.value == value;
                if (ImGui::Selectable(option.label.c_str(), isSelected)) {
                    value = option.value;
                    updated = true;
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return updated;
    }

    bool drawTxSequenceFieldEditor(const scripting::TxSequenceFieldDescriptor& field,
                                   scripting::TxSequenceFrameValue& frame)
    {
        constexpr float kFieldWidth = 118.0F;
        ImGui::SetNextItemWidth(kFieldWidth);
        auto value = txSequenceFieldValueOrDefault(field, frame);
        if (drawTxSequenceOptionCombo(("##" + field.id).c_str(), field, value)) {
            frame.fields[field.id] = value;
            return true;
        }

        if (field.type == scripting::TxSequenceFieldType::String) {
            std::string text;
            if (const auto* stored = std::get_if<std::string>(&value)) {
                text = *stored;
            }
            char buffer[256]{};
            std::snprintf(buffer, sizeof(buffer), "%s", text.c_str());
            if (ImGui::InputText(("##" + field.id).c_str(), buffer, sizeof(buffer))) {
                frame.fields[field.id] = std::string(buffer);
                return true;
            }
            return false;
        }

        std::int64_t integerValue = 0;
        if (const auto* number = std::get_if<std::int64_t>(&value)) {
            integerValue = *number;
        }
        if (drawTxSequenceIntegerInput(("##" + field.id).c_str(), field, integerValue)) {
            frame.fields[field.id] = clampTxSequenceUiInteger(field, integerValue);
            return true;
        }
        return false;
    }

    std::size_t enabledTxSequenceFrameCount(const scripting::TxSequenceValue& value)
    {
        return static_cast<std::size_t>(std::count_if(value.frames.begin(), value.frames.end(), [](const auto& frame) {
            return frame.enabled;
        }));
    }

    float luaDynamicControlLabelPartWidth(std::string_view visibleLabel)
    {
        if (visibleLabel.empty()) {
            return 0.0F;
        }
        return ImGui::CalcTextSize(visibleLabel.data(), visibleLabel.data() + visibleLabel.size()).x +
               ImGui::GetStyle().ItemInnerSpacing.x;
    }

    std::optional<float> luaDynamicControlItemWidth(const scripting::ControlDescriptor& descriptor,
                                                    std::string_view visibleLabel,
                                                    std::optional<float> layoutWidth)
    {
        if (!layoutWidth.has_value() || !isLuaDynamicInputControl(descriptor.type)) {
            return std::nullopt;
        }
        return std::max(1.0F, *layoutWidth - luaDynamicControlLabelPartWidth(visibleLabel));
    }

    void reserveLuaDynamicControlWidth(float startX, float layoutWidth)
    {
        const float drawnWidth = ImGui::GetItemRectMax().x - startX;
        if (layoutWidth <= drawnWidth) {
            return;
        }
        ImGui::SameLine(0.0F, 0.0F);
        ImGui::Dummy(ImVec2(layoutWidth - drawnWidth, 0.0F));
    }

    float compactLogToolbarHeight()
    {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float controlHeight = ImGui::GetFrameHeight();
        const float extraPadding = style.WindowPadding.y * 0.5F;
        return controlHeight + extraPadding + 16.0f;
    }

    float transferLogToolbarButtonHeight()
    {
        return ImGui::GetFrameHeight();
    }

    bool drawTransferToolbarButton(const char* label, const char* tooltip, bool active, float width = 0.0F)
    {
        return drawToolbarSectionButton(label, tooltip, active, ImVec2(width, transferLogToolbarButtonHeight()));
    }

    bool drawTransferToolbarToggleButton(const char* label, bool& value, const char* tooltip, float width = 0.0F)
    {
        if (!drawTransferToolbarButton(label, tooltip, value, width)) {
            return false;
        }
        value = !value;
        return true;
    }

    bool drawTransferToolbarDangerButton(const char* label, const char* tooltip, float width = 0.0F)
    {
        const auto& tokens = defaultUiStyleTokens();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(tokens.danger.x, tokens.danger.y, tokens.danger.z, 0.18F));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(tokens.danger.x, tokens.danger.y, tokens.danger.z, 0.32F));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(tokens.danger.x, tokens.danger.y, tokens.danger.z, 0.48F));
        ImGui::PushStyleColor(ImGuiCol_Text, tokens.danger);
        const bool clicked = ImGui::Button(label, ImVec2(width, transferLogToolbarButtonHeight()));
        ImGui::PopStyleColor(4);
        drawIconTooltip(tooltip);
        return clicked;
    }

    bool drawRequestTraceFilterButton(const char* label,
                                      const char* tooltip,
                                      dock::RequestTraceStatusFilter value,
                                      dock::RequestTraceFilterState& filter)
    {
        if (!drawTransferToolbarButton(label, tooltip, filter.status == value)) {
            return false;
        }
        filter.status = value;
        return true;
    }

    void drawRequestTraceKeywordFilterInput(const char* id, dock::RequestTraceFilterState& filter, float width)
    {
        char buffer[128]{};
        std::snprintf(buffer, sizeof(buffer), "%s", filter.keyword.c_str());
        ImGui::SetNextItemWidth(width);
        if (ImGui::InputText(id, buffer, sizeof(buffer))) {
            filter.keyword = buffer;
        }
        drawIconTooltip("按请求 ID、类型、状态、tag、端点、guard 状态和错误筛选");
    }

} // namespace

void GuiRuntime::drawStatusBar()
{
    const auto& tokens = defaultUiStyleTokens();
    auto& comm = application_.docks().commState();
    auto& config = application_.docks().configState();

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + viewport->Size.y - 44.0F));
    ImGui::SetNextWindowSize(ImVec2(viewport->Size.x, 44.0F));
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0F, 8.0F));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.07F, 0.09F, 0.13F, 0.98F));
    ImGui::PushStyleColor(ImGuiCol_Border, tokens.panelBorder);
    if (ImGui::Begin("状态栏", nullptr, flags)) {
        drawHeaderBadge(transportStateLabel(comm.state),
                        comm.state == transport::TransportState::Open ? tokens.success : tokens.warning,
                        false);
        if (config.dirty) {
            ImGui::SameLine();
            drawHeaderBadge("配置未保存", tokens.warning, false);
        }
        if (config.pendingExternalReload) {
            ImGui::SameLine();
            drawHeaderBadge(
                config.externalReloadMessage.empty() ? "检测到外部更新" : config.externalReloadMessage.c_str(),
                tokens.warning,
                false);
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
            const std::string recordingText =
                "录制 " + (fileName.empty() ? std::string("(未命名)") : fileName) + " " +
                std::to_string(static_cast<unsigned long long>(application_.rawCaptureRecordingBytes())) + " bytes";
            ImGui::SameLine();
            drawHeaderBadge(recordingText.c_str(), tokens.danger, true);
        }
        const std::string_view statusText = !config.transientStatusMessage.empty()
                                                ? std::string_view{config.transientStatusMessage}
                                                : std::string_view{config.statusMessage};
        if (!statusText.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%.*s", static_cast<int>(statusText.size()), statusText.data());
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

void GuiRuntime::drawCommTransportModeSelector(dock::CommDockState& comm)
{
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
}

void GuiRuntime::drawTcpClientCommConfig(dock::CommDockState& comm)
{
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
}

void GuiRuntime::drawTcpServerCommConfig(dock::CommDockState& comm)
{
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
}

void GuiRuntime::drawSerialCommConfig(dock::CommDockState& comm)
{
    if (!serialPortsScanned_) {
        refreshSerialPortOptions(comm);
        serialPortsScanned_ = true;
    }
    if (ImGui::Button("刷新串口列表")) {
        refreshSerialPortOptions(comm);
    }

    syncDraftFromModel(serialPortDraft_, serialPortDraftModel_, comm.serial.portName);
    if (const auto portEdit = drawEditableCombo("端口", serialPortDraft_, comm.serialPortOptions);
        portEdit.edited && portEdit.value != comm.serial.portName) {
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
    if (const auto baudEdit = drawEditableCombo("波特率", commonBaudRateDraft_, baudRateOptions, digitsOnly);
        baudEdit.edited) {
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

    const char* parityValues[] = {"none", "odd", "even"};
    const char* parityItems[] = {"无校验", "奇校验", "偶校验"};
    int parityIndex = comm.serial.parity == "odd" ? 1 : (comm.serial.parity == "even" ? 2 : 0);
    if (ImGui::Combo("奇偶校验", &parityIndex, parityItems, IM_ARRAYSIZE(parityItems))) {
        comm.serial.parity = parityValues[parityIndex];
        application_.markCommConfigEdited(true);
    }
    ImGui::SetItemTooltip("仅改变界面显示名称；配置仍保存为 none / odd / even。");

    const char* stopBitValues[] = {"one", "one_point_five", "two"};
    const char* stopBitItems[] = {"1 位", "1.5 位", "2 位"};
    int stopBitIndex = comm.serial.stopBits == "two" ? 2 : (comm.serial.stopBits == "one_point_five" ? 1 : 0);
    if (ImGui::Combo("停止位", &stopBitIndex, stopBitItems, IM_ARRAYSIZE(stopBitItems))) {
        comm.serial.stopBits = stopBitValues[stopBitIndex];
        application_.markCommConfigEdited(true);
    }
    ImGui::SetItemTooltip("仅改变界面显示名称；配置仍保存为 one / one_point_five / two。");

    const char* flowValues[] = {"none", "software", "hardware"};
    const char* flowItems[] = {"无流控", "软件流控 (XON/XOFF)", "硬件流控 (RTS/CTS)"};
    int flowIndex = comm.serial.flowControl == "software" ? 1 : (comm.serial.flowControl == "hardware" ? 2 : 0);
    if (ImGui::Combo("流控", &flowIndex, flowItems, IM_ARRAYSIZE(flowItems))) {
        comm.serial.flowControl = flowValues[flowIndex];
        application_.markCommConfigEdited(true);
    }
    ImGui::SetItemTooltip("仅改变界面显示名称；配置仍保存为 none / software / hardware。");
}

void GuiRuntime::drawUdpPeerCommConfig(dock::CommDockState& comm)
{
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

void GuiRuntime::drawCommTransportConfig(dock::CommDockState& comm)
{
    if (comm.kind == transport::TransportKind::TcpClient) {
        drawTcpClientCommConfig(comm);
    } else if (comm.kind == transport::TransportKind::TcpServer) {
        drawTcpServerCommConfig(comm);
    } else if (comm.kind == transport::TransportKind::Serial) {
        drawSerialCommConfig(comm);
    } else if (comm.kind == transport::TransportKind::UdpPeer) {
        drawUdpPeerCommConfig(comm);
    }
}

void GuiRuntime::drawCommStatus(const dock::CommDockState& comm)
{
    ImGui::Text("当前模式: %s", transport::transportKindLabel(comm.kind).data());
    ImGui::Text("连接状态: %s", transportStateLabel(comm.state));
    ImGui::Text("TX=%llu RX=%llu",
                static_cast<unsigned long long>(comm.txCount),
                static_cast<unsigned long long>(comm.rxCount));
    if (comm.adaptivePerformanceEnabled) {
        ImGui::Text("自适应性能: K=%.2f 当前=%.2f 等级=%s",
                    comm.adaptivePerformanceMaxMultiplier,
                    comm.adaptivePerformanceEffectiveMultiplier,
                    comm.adaptivePerformanceLevel.c_str());
        ImGui::TextWrapped("自适应原因: %s%s",
                           comm.adaptivePerformanceReason.c_str(),
                           comm.adaptivePerformanceSystemMetricsAvailable ? "" : " (仅软件指标)");
    }
    if (!comm.lastError.empty()) {
        ImGui::TextWrapped("错误：%s", comm.lastError.c_str());
    }
}

void GuiRuntime::drawCommActions(dock::ConfigDockState& configState)
{
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
}

void GuiRuntime::drawCommParserStatus(const dock::CommDockState& comm)
{
    if (!comm.lastErrorSummary.empty()) {
        ImGui::TextColored(ImVec4(1.0F, 0.5F, 0.0F, 1.0F), "解析错误: %s", comm.lastErrorSummary.c_str());
    }
}

void GuiRuntime::drawCommDock()
{
    if (!showCommDock_) {
        return;
    }

    auto& comm = application_.docks().commState();
    auto& configState = application_.docks().configState();

    if (!ImGui::Begin("通讯配置", &showCommDock_)) {
        ImGui::End();
        return;
    }

    drawCommTransportModeSelector(comm);
    drawCommTransportConfig(comm);
    ImGui::Separator();
    drawCommStatus(comm);
    drawCommActions(configState);
    drawCommParserStatus(comm);

    ImGui::End();
}

void GuiRuntime::drawProtocolDock()
{
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
        const auto selectedDir =
            nativeDirectoryDialog(window_, L"选择协议根目录", std::filesystem::path(lua.protocolRootDir), dialogError);
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
    if (const auto protocolDirEdit = drawEditableCombo("协议目录", protocolDirDraft_, lua.protocolDirOptions);
        protocolDirEdit.edited) {
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
        const bool copyMode = application_.docks().configState().luaDockRenderCopyMode;
        if (copyMode) {
            // 拷贝模式：深拷贝控件列表，避免回调修改导致迭代器失效
            const auto controls = lua.controlStates;
            drawLuaDockFlow(controls, false);
        } else {
            // 引用模式：直接按引用遍历当前帧控件
            const auto& controls = lua.controlStates;
            drawLuaDockFlow(controls);
        }
        ImGui::End();
        return;
    }
    ImGui::End();
}

bool GuiRuntime::drawLuaDockFlow(const std::vector<scripting::ControlSnapshot>& controls, bool earlyExit)
{
    for (const auto& control : controls) {
        if (drawDynamicControl(control)) {
            if (earlyExit)
                return true;
        }
    }
    return false;
}

void GuiRuntime::drawTransferLogSection(float logHeight)
{
    auto& receive = application_.docks().receiveState();
    if (ImGui::BeginChild("##transfer_log_section", ImVec2(0.0F, logHeight), true)) {
        if (ImGui::BeginTable(
                "##transfer_log_toolbar", 12, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("title", ImGuiTableColumnFlags_WidthStretch);

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
            if (drawTransferToolbarButton(
                    "全部", "显示 RX、TX 和系统状态等全部收发记录。", receive.filter.status == dock::LogStatusFilter::All)) {
                receive.filter.status = dock::LogStatusFilter::All;
            }

            ImGui::TableSetColumnIndex(4);
            if (drawTransferToolbarButton(
                    "RX", "只显示设备发来的 RX 数据记录。", receive.filter.status == dock::LogStatusFilter::Rx)) {
                receive.filter.status = dock::LogStatusFilter::Rx;
            }

            ImGui::TableSetColumnIndex(5);
            if (drawTransferToolbarButton(
                    "TX", "只显示宿主发出的 TX 数据记录。", receive.filter.status == dock::LogStatusFilter::Tx)) {
                receive.filter.status = dock::LogStatusFilter::Tx;
            }

            ImGui::TableSetColumnIndex(6);
            const bool parsedFrames = receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames;
            if (drawTransferToolbarButton(parsedFrames ? "逐帧" : "原始",
                                          parsedFrames ? "当前按 Lua stream() schema 逐帧显示；点击切回运输层原始分块。"
                                                       : "当前按运输层原始分块显示；点击解析为 Lua stream() 帧列表。",
                                          parsedFrames)) {
                receive.displayMode =
                    parsedFrames ? dock::TransferLogDisplayMode::RawChunks : dock::TransferLogDisplayMode::ParsedFrames;
                if (receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames) {
                    application_.activateParsedTransferLogView();
                }
            }

            ImGui::TableSetColumnIndex(7);
            drawTransferToolbarToggleButton(
                "HEX", receive.showHex, receive.showHex ? "当前显示 HEX 字节；点击后隐藏字节列。"
                                                         : "当前隐藏 HEX 字节；点击后显示字节列。");

            ImGui::TableSetColumnIndex(8);
            drawTransferToolbarToggleButton("时间",
                                            receive.showTimestamps,
                                            receive.showTimestamps ? "当前显示时间戳；点击后隐藏时间列。"
                                                                   : "当前隐藏时间戳；点击后显示时间列。");

            ImGui::TableSetColumnIndex(9);
            drawTransferToolbarToggleButton(receive.pauseScroll ? "继续" : "暂停",
                                            receive.pauseScroll,
                                            receive.pauseScroll ? "恢复自动滚动，最新收发记录会回到底部可见。"
                                                                : "暂停自动滚动，便于停在当前位置查看历史记录。");

            ImGui::TableSetColumnIndex(10);
            if (drawTransferToolbarButton("导出", "导出当前过滤结果中的收发记录。", false)) {
                openTransferLogExportDialog();
            }

            ImGui::TableSetColumnIndex(11);
            if (drawTransferToolbarDangerButton(PROTOSCOPE_ICON_TRASH " 清空",
                                                "清空收发记录和已解析帧缓存；不会断开连接。")) {
                application_.docks().clearReceiveRows();
                application_.rebuildTransferFrameRows();
            }

            ImGui::EndTable();
        }

        const auto& visibleRows =
            receive.displayMode == dock::TransferLogDisplayMode::ParsedFrames ? receive.frameRows : receive.rows;
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
}

void GuiRuntime::drawTransferSendSection(float minPayloadHeight, const ImGuiStyle& style)
{
    auto& sendState = application_.docks().sendState();
    const auto& comm = application_.docks().commState();
    if (ImGui::BeginChild("##transfer_send_section", ImVec2(0.0F, transferSendSectionHeight_), true)) {
        char buffer[2048]{};
        std::snprintf(buffer, sizeof(buffer), "%s", sendState.payload.c_str());

        ImGuiInputTextFlags flags = ImGuiInputTextFlags_WordWrap;
        if (sendState.hexMode) {
            flags |= ImGuiInputTextFlags_CallbackEdit | ImGuiInputTextFlags_CallbackCharFilter;
        }

        const float payloadHeight =
            (std::max)(minPayloadHeight, ImGui::GetContentRegionAvail().y - style.CellPadding.y * 2.0F);
        const float frameHeight = ImGui::GetFrameHeight();
        const float hexWidth =
            (std::max)(ImGui::CalcTextSize("HEX").x + style.FramePadding.x * 2.0F, frameHeight * 2.4F);
        const float sendWidth =
            (std::max)(ImGui::CalcTextSize("发送").x + style.FramePadding.x * 2.0F, frameHeight * 2.8F);
        const float minHistoryWidth = frameHeight * 4.0F;
        sendState.historyComboWidth =
            (std::clamp)(sendState.historyComboWidth,
                         minHistoryWidth,
                         (std::max)(minHistoryWidth, ImGui::GetContentRegionAvail().x * 0.45F));
        const auto historyLimit = configuredSendHistoryLimit(application_.captureConfig());
        dock::trimSendHistory(sendState, historyLimit);

        if (ImGui::BeginTable(
                "##transfer_send_table",
                4,
                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable | ImGuiTableFlags_NoSavedSettings)) {
            ImGui::TableSetupColumn("hex", ImGuiTableColumnFlags_WidthFixed, hexWidth);

            ImGui::TableSetupColumn("payload", ImGuiTableColumnFlags_WidthStretch);

            ImGui::TableSetupColumn("history", ImGuiTableColumnFlags_WidthFixed, sendState.historyComboWidth);

            ImGui::TableSetupColumn("send", ImGuiTableColumnFlags_WidthFixed, sendWidth);

            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            {
                const float oldY = ImGui::GetCursorPosY();
                ImGui::SetCursorPosY(oldY + (payloadHeight - frameHeight) * 0.5F);

                const bool hexModeBeforeClick = sendState.hexMode;
                if (hexModeBeforeClick) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
                }
                const bool hexModeToggleRequested = ImGui::Button("HEX##transfer_send_hex", ImVec2(-FLT_MIN, 0.0F));
                if (hexModeBeforeClick) {
                    ImGui::PopStyleColor(2);
                }
                if (hexModeToggleRequested && !application_.setSendHexMode(!hexModeBeforeClick)) {
                    application_.setStatusMessage("HEX 模式切换失败", true);
                }
                drawIconTooltip(sendState.hexMode ? "HEX 发送已开启" : "HEX 发送已关闭");

                ImGui::SetCursorPosY(oldY);
            }

            ImGui::TableSetColumnIndex(1);
            {
                ImGui::SetNextItemWidth(-FLT_MIN);

                if (ImGui::InputTextMultiline("##raw_payload",
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
                ImGui::SetCursorPosY(oldY + (payloadHeight - frameHeight) * 0.5F);

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
                ImGui::SetCursorPosY(oldY + (payloadHeight - frameHeight) * 0.5F);

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
}

void GuiRuntime::drawTransferDock()
{
    if (!showTransferDock_) {
        return;
    }

    if (!ImGui::Begin("收发数据", &showTransferDock_)) {
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
        minPayloadHeight + style.WindowPadding.y * 2.0F + style.CellPadding.y * 2.0F + sendBorderSlack;
    // 扣除 ItemSpacing 间隙，避免 3 个部件（日志区、分割条、发送区）总高度超出可用区域导致纵向滚动条
    const float childSpacing = style.ItemSpacing.y * 2.0F;
    if (transferSendSectionHeight_ <= 0.0F) {
        transferSendSectionHeight_ = minSendHeight;
    }

    const float maxSendHeight = (std::max)(minSendHeight, availableHeight - splitterThickness - childSpacing);
    transferSendSectionHeight_ = (std::clamp)(transferSendSectionHeight_, minSendHeight, maxSendHeight);

    float logHeight = (std::max)(0.0F, availableHeight - transferSendSectionHeight_ - splitterThickness - childSpacing);

    drawTransferLogSection(logHeight);

    // =========================
    // splitter
    // drawHorizontalSplitter 按 top height 工作
    // 所以这里传 logHeight
    // =========================
    drawHorizontalSplitter("##transfer_splitter", logHeight, 0.0F, minSendHeight, availableHeight, splitterThickness);

    transferSendSectionHeight_ =
        (std::max)(minSendHeight, availableHeight - logHeight - splitterThickness - childSpacing);

    drawTransferSendSection(minPayloadHeight, style);

    ImGui::End();
}

void GuiRuntime::drawRequestTraceDock()
{
    if (!showRequestTraceDock_) {
        return;
    }

    if (!ImGui::Begin("请求追踪", &showRequestTraceDock_)) {
        ImGui::End();
        return;
    }

    auto& trace = application_.docks().requestTraceState();
    const float toolbarHeight = compactLogToolbarHeight();
    if (beginToolbarGroup("request_trace_toolbar", nullptr, toolbarHeight)) {
        ImGui::TextUnformatted(PROTOSCOPE_ICON_EXCHANGE " 请求时间线");
        ImGui::SameLine();
        drawRequestTraceKeywordFilterInput("关键字##request_trace_keyword", trace.filter, 180.0F);
        ImGui::SameLine();
        drawRequestTraceFilterButton("全部", "显示全部请求事件", dock::RequestTraceStatusFilter::All, trace.filter);
        ImGui::SameLine();
        drawRequestTraceFilterButton(
            "进行中", "仅显示排队和已发送事件", dock::RequestTraceStatusFilter::Active, trace.filter);
        ImGui::SameLine();
        drawRequestTraceFilterButton(
            "成功", "仅显示完成和熔断重置事件", dock::RequestTraceStatusFilter::Success, trace.filter);
        ImGui::SameLine();
        drawRequestTraceFilterButton(
            "失败", "仅显示失败、超时、拒绝、丢弃和取消事件", dock::RequestTraceStatusFilter::Failure, trace.filter);
        ImGui::SameLine();
        drawTransferToolbarToggleButton(PROTOSCOPE_ICON_CLOCK " 时间", trace.showTimestamps, "显示或隐藏时间列");
        ImGui::SameLine();
        drawTransferToolbarToggleButton(PROTOSCOPE_ICON_PAUSE " 暂停", trace.pauseScroll, "暂停自动滚动");
        ImGui::SameLine();
        if (drawTransferToolbarButton("导出", "导出当前筛选后的请求追踪 CSV", false)) {
            openRequestTraceExportDialog();
        }
        ImGui::SameLine();
        if (drawTransferToolbarDangerButton(PROTOSCOPE_ICON_TRASH " 清空", "清空请求追踪时间线")) {
            application_.docks().clearRequestTraceRows();
        }
        endToolbarGroup();
    }

    const auto rows = dock::filteredRequestTraceRows(trace.rows, trace.filter);
    if (rows.empty()) {
        ImGui::TextDisabled("暂无请求事件");
    } else if (ImGui::BeginTable("##request_trace_table",
                                 trace.showTimestamps ? 10 : 9,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable |
                                     ImGuiTableFlags_Reorderable | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_SizingStretchProp,
                                 ImVec2(0.0F, 0.0F))) {
        if (trace.showTimestamps) {
            ImGui::TableSetupColumn("时间", ImGuiTableColumnFlags_WidthFixed, 96.0F);
        }
        ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 64.0F);
        ImGui::TableSetupColumn("类型", ImGuiTableColumnFlags_WidthFixed, 72.0F);
        ImGui::TableSetupColumn("状态", ImGuiTableColumnFlags_WidthFixed, 86.0F);
        ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthStretch, 1.0F);
        ImGui::TableSetupColumn("端点", ImGuiTableColumnFlags_WidthStretch, 1.2F);
        ImGui::TableSetupColumn("尝试", ImGuiTableColumnFlags_WidthFixed, 70.0F);
        ImGui::TableSetupColumn("字节", ImGuiTableColumnFlags_WidthFixed, 64.0F);
        ImGui::TableSetupColumn("耗时", ImGuiTableColumnFlags_WidthFixed, 82.0F);
        ImGui::TableSetupColumn("详情", ImGuiTableColumnFlags_WidthStretch, 1.6F);
        ImGui::TableHeadersRow();

        std::size_t rowIndex = 0;
        for (const auto* row : rows) {
            ImGui::TableNextRow();
            const float rowMinY = ImGui::GetCursorScreenPos().y;
            int column = 0;
            if (trace.showTimestamps) {
                ImGui::TableSetColumnIndex(column++);
                ImGui::TextUnformatted(formatTimestamp(row->timestampMs).c_str());
            }
            ImGui::TableSetColumnIndex(column++);
            if (row->id == 0U) {
                ImGui::TextUnformatted("-");
            } else {
                ImGui::Text("%llu", static_cast<unsigned long long>(row->id));
            }
            ImGui::TableSetColumnIndex(column++);
            ImGui::TextUnformatted(dock::requestTraceKindLabel(row->kind));
            ImGui::TableSetColumnIndex(column++);
            ImGui::TextUnformatted(dock::requestTraceStateLabel(row->state));
            ImGui::TableSetColumnIndex(column++);
            ImGui::TextUnformatted(row->tag.empty() ? "-" : row->tag.c_str());
            ImGui::TableSetColumnIndex(column++);
            ImGui::TextUnformatted(row->endpoint.empty() ? "-" : row->endpoint.c_str());
            ImGui::TableSetColumnIndex(column++);
            ImGui::Text("%u/%u", row->attempt, row->maxAttempts);
            ImGui::TableSetColumnIndex(column++);
            ImGui::Text("%zu", row->bytes);
            ImGui::TableSetColumnIndex(column++);
            const auto duration = dock::formatRequestTraceDuration(*row);
            ImGui::TextUnformatted(duration.c_str());
            ImGui::TableSetColumnIndex(column++);
            std::string detail = dock::formatRequestTraceDetail(*row);
            if (detail.empty()) {
                detail = "-";
            }
            ImGui::TextUnformatted(detail.c_str());

            // 右键菜单不改表格形态，只在整行热区内打开复制操作。
            ImGui::PushID(static_cast<int>(rowIndex++));
            const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
            const ImVec2 rowMin(ImGui::GetWindowPos().x, rowMinY);
            const ImVec2 rowMax(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), rowMinY + rowHeight);
            if (ImGui::IsMouseHoveringRect(rowMin, rowMax) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                ImGui::OpenPopup("##request_trace_row_menu");
            }
            if (ImGui::BeginPopup("##request_trace_row_menu")) {
                const auto copyLine = dock::formatRequestTraceRowCsv(*row, trace.showTimestamps);
                if (ImGui::MenuItem("复制本行")) {
                    ImGui::SetClipboardText(copyLine.c_str());
                }
                if (ImGui::MenuItem("复制详情")) {
                    ImGui::SetClipboardText(detail.c_str());
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        if (!trace.pauseScroll && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2.0F) {
            ImGui::SetScrollHereY(1.0F);
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

void GuiRuntime::drawLogDock()
{
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
    drawRowList("log_rows",
                filteredRows.rows,
                logState.showTimestamps,
                false,
                logState.pauseScroll,
                "暂无宿主日志",
                filteredRows.endpointWidth);
    ImGui::End();
}

void GuiRuntime::drawScriptDock()
{
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

    const auto& filteredRows = filteredLogRowsCached(
        scriptLogRowsCache_, scriptState.rows, scriptState.rowsVersion, scriptState.filter, false);
    drawRowList("script_rows",
                filteredRows.rows,
                scriptState.showTimestamps,
                false,
                scriptState.pauseScroll,
                "暂无 Lua 日志或事件",
                filteredRows.endpointWidth);
    ImGui::End();
}

bool GuiRuntime::drawDynamicControl(const scripting::ControlSnapshot& control)
{
    return drawDynamicControl(control, std::nullopt);
}

bool GuiRuntime::drawDynamicLayoutControl(const scripting::ControlSnapshot& control, float layoutWidth)
{
    return drawDynamicControl(control, layoutWidth);
}

bool GuiRuntime::drawDynamicControl(const scripting::ControlSnapshot& control, std::optional<float> layoutWidth)
{
    const auto& descriptor = control.descriptor;
    const std::string visibleLabel = resolveLuaControlVisibleLabel(descriptor, layoutWidth);
    const std::string imguiLabel = luaControlImGuiLabel(descriptor, visibleLabel);
    const std::string inputLabel = luaControlInputLabel(descriptor, visibleLabel);
    const float startX = ImGui::GetCursorScreenPos().x;
    const ScopedImGuiItemWidth itemWidth(luaDynamicControlItemWidth(descriptor, visibleLabel, layoutWidth));
    bool updated = false;
    const int feedbackStyleColors = pushLuaControlFeedbackStyle(descriptor);
    switch (descriptor.type) {
        case scripting::ControlType::Button:
            updated = drawDynamicButtonControl(control, imguiLabel, layoutWidth);
            drawLuaControlCompactTooltip(descriptor, visibleLabel);
            break;
        case scripting::ControlType::Checkbox:
            updated = drawDynamicCheckboxControl(control, inputLabel, visibleLabel);
            drawLuaControlCompactTooltip(descriptor, visibleLabel);
            break;
        case scripting::ControlType::InputText:
            updated = drawDynamicTextControl(control, inputLabel, visibleLabel);
            drawLuaControlCompactTooltip(descriptor, visibleLabel);
            break;
        case scripting::ControlType::Combo:
            updated = drawDynamicComboControl(control, inputLabel, visibleLabel);
            drawLuaControlCompactTooltip(descriptor, visibleLabel);
            break;
        case scripting::ControlType::ElfSymbolCombo:
            updated = drawDynamicElfSymbolComboControl(control, inputLabel, visibleLabel);
            drawLuaControlCompactTooltip(descriptor, visibleLabel);
            break;
        case scripting::ControlType::ValueTable:
            updated = drawValueTableControl(control, visibleLabel);
            break;
        case scripting::ControlType::TxSequence:
            updated = drawTxSequenceControl(control, visibleLabel);
            break;
        case scripting::ControlType::InputInt:
            updated = drawDynamicIntControl(control, inputLabel, visibleLabel);
            drawLuaControlCompactTooltip(descriptor, visibleLabel);
            break;
        case scripting::ControlType::InputFloat:
            updated = drawDynamicFloatControl(control, inputLabel, visibleLabel);
            drawLuaControlCompactTooltip(descriptor, visibleLabel);
            break;
    }
    if (feedbackStyleColors > 0) {
        ImGui::PopStyleColor(feedbackStyleColors);
    }
    if (layoutWidth.has_value()) {
        reserveLuaDynamicControlWidth(startX, *layoutWidth);
    }
    return updated;
}

int GuiRuntime::pushLuaControlFeedbackStyle(const scripting::ControlDescriptor& descriptor)
{
    if (!application_.runtimeConfig().gui.interactionFeedback.enabled) {
        return 0;
    }

    constexpr std::uint64_t kVisualFeedbackDurationMs = 420;
    const auto stateIter = luaControlFeedbackStates_.find(descriptor.id);
    if (stateIter == luaControlFeedbackStates_.end() || stateIter->second.triggeredAtMs == 0) {
        return 0;
    }

    const std::uint64_t currentMs = nowMs();
    const std::uint64_t elapsedMs =
        currentMs > stateIter->second.triggeredAtMs ? currentMs - stateIter->second.triggeredAtMs : 0;
    if (elapsedMs >= kVisualFeedbackDurationMs) {
        luaControlFeedbackStates_.erase(stateIter);
        return 0;
    }

    const float alpha =
        1.0F - static_cast<float>(elapsedMs) / static_cast<float>(kVisualFeedbackDurationMs);
    const ImVec4 accent(0.28F, 0.62F, 1.00F, 1.0F);
    const auto blend = [alpha](ImVec4 base, ImVec4 highlight, float strength) {
        const float amount = (std::clamp)(alpha * strength, 0.0F, 1.0F);
        return ImVec4(base.x + (highlight.x - base.x) * amount,
                      base.y + (highlight.y - base.y) * amount,
                      base.z + (highlight.z - base.z) * amount,
                      base.w);
    };

    if (descriptor.type == scripting::ControlType::Button) {
        ImGui::PushStyleColor(ImGuiCol_Button, blend(ImGui::GetStyleColorVec4(ImGuiCol_Button), accent, 0.42F));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered, blend(ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered), accent, 0.50F));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive, blend(ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive), accent, 0.58F));
        return 3;
    }

    ImGui::PushStyleColor(ImGuiCol_FrameBg, blend(ImGui::GetStyleColorVec4(ImGuiCol_FrameBg), accent, 0.32F));
    ImGui::PushStyleColor(
        ImGuiCol_FrameBgHovered, blend(ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered), accent, 0.40F));
    ImGui::PushStyleColor(
        ImGuiCol_FrameBgActive, blend(ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive), accent, 0.48F));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(accent.x, accent.y, accent.z, 0.45F * alpha));
    return 4;
}

void GuiRuntime::updateDynamicControlValueWithFeedback(const scripting::ControlDescriptor& descriptor,
                                                       const scripting::ControlValue& value)
{
    const bool feedbackEnabled = application_.runtimeConfig().gui.interactionFeedback.enabled;
    const std::string previousStatus = application_.docks().configState().statusMessage;
    application_.updateControlValue(descriptor.id, value);
    if (!feedbackEnabled) {
        return;
    }

    luaControlFeedbackStates_[descriptor.id].triggeredAtMs = nowMs();
    if (application_.docks().configState().statusMessage != previousStatus) {
        return;
    }

    const std::string label = descriptor.label.empty() ? descriptor.id : descriptor.label;
    application_.setTransientStatusMessage(
        (descriptor.type == scripting::ControlType::Button ? "已触发：" : "已更新：") + label);
}

bool GuiRuntime::drawDynamicButtonControl(const scripting::ControlSnapshot& control,
                                          const std::string& imguiLabel,
                                          std::optional<float> layoutWidth)
{
    const ImVec2 size(layoutWidth.value_or(0.0F), 0.0F);
    if (ImGui::Button(imguiLabel.c_str(), size)) {
        updateDynamicControlValueWithFeedback(control.descriptor, true);
        return true;
    }
    return false;
}

bool GuiRuntime::drawDynamicCheckboxControl(const scripting::ControlSnapshot& control,
                                            const std::string& inputLabel,
                                            std::string_view visibleLabel)
{
    const auto& descriptor = control.descriptor;
    bool checked = std::get<bool>(control.value);
    drawLuaControlLeftLabel(descriptor, visibleLabel);
    if (ImGui::Checkbox(inputLabel.c_str(), &checked)) {
        updateDynamicControlValueWithFeedback(descriptor, checked);
        return true;
    }
    return false;
}

bool GuiRuntime::drawDynamicTextControl(const scripting::ControlSnapshot& control,
                                        const std::string& inputLabel,
                                        std::string_view visibleLabel)
{
    const auto& descriptor = control.descriptor;
    char buffer[512]{};
    std::snprintf(buffer, sizeof(buffer), "%s", std::get<std::string>(control.value).c_str());
    drawLuaControlLeftLabel(descriptor, visibleLabel);
    if (ImGui::InputText(inputLabel.c_str(), buffer, sizeof(buffer))) {
        updateDynamicControlValueWithFeedback(descriptor, std::string(buffer));
        return true;
    }
    return false;
}

bool GuiRuntime::drawDynamicComboControl(const scripting::ControlSnapshot& control,
                                         const std::string& inputLabel,
                                         std::string_view visibleLabel)
{
    const auto& descriptor = control.descriptor;
    int index = std::get<int>(control.value);
    std::vector<const char*> items;
    for (const auto& option : descriptor.comboOptions) {
        items.push_back(option.c_str());
    }
    drawLuaControlLeftLabel(descriptor, visibleLabel);
    if (!items.empty() && ImGui::Combo(inputLabel.c_str(), &index, items.data(), static_cast<int>(items.size()))) {
        updateDynamicControlValueWithFeedback(descriptor, index);
        return true;
    }
    return false;
}

bool GuiRuntime::drawDynamicElfSymbolComboControl(const scripting::ControlSnapshot& control,
                                                  const std::string& inputLabel,
                                                  std::string_view visibleLabel)
{
    const auto& descriptor = control.descriptor;
    auto& state = elfSymbolComboStates_[descriptor.id];
    const auto& current = std::get<scripting::ElfSymbolValue>(control.value);
    seedElfSymbolComboDraft(state, current);

    const auto currentMs = nowMs();
    refreshElfSymbolComboOptionsIfNeeded(descriptor, state, currentMs);

    drawLuaControlLeftLabel(descriptor, visibleLabel);
    auto labels = elfSymbolComboLabels(state);
    const auto edit = drawEditableCombo(
        inputLabel.c_str(), state.draft, labels, EditableComboOptions{.keepPopupOpenWhileEditing = true});
    if (edit.edited) {
        state.draft = edit.value;
        state.editedAtMs = currentMs;
    }
    if (!edit.selectedFromList) {
        return false;
    }
    return commitElfSymbolComboSelection(descriptor, state, edit.value);
}

void GuiRuntime::seedElfSymbolComboDraft(ElfSymbolComboUiState& state, const scripting::ElfSymbolValue& current) const
{
    if (state.draft.empty() && !current.label.empty()) {
        state.draft = current.label;
    }
}

void GuiRuntime::refreshElfSymbolComboOptionsIfNeeded(const scripting::ControlDescriptor& descriptor,
                                                      ElfSymbolComboUiState& state,
                                                      const std::uint64_t currentMs)
{
    const auto comboConfig = application_.captureConfig().gui.elfSymbolCombo;
    const std::size_t effectiveLimit = descriptor.limitConfigured ? descriptor.limit : comboConfig.limit;
    const int effectiveDebounceMs = descriptor.debounceMsConfigured ? descriptor.debounceMs : comboConfig.debounceMs;
    const auto loadedRevision = application_.elfStaticAddressRevision();
    if (state.editedAtMs == 0) {
        state.editedAtMs = currentMs;
    }

    const bool elfReloaded = state.loadedRevision != loadedRevision;
    const bool queryLimitChanged = state.queriedLimit != effectiveLimit;
    const bool debounceElapsed = currentMs >= state.editedAtMs + static_cast<std::uint64_t>(effectiveDebounceMs);
    if (!elfReloaded && !queryLimitChanged && (state.queriedDraft == state.draft || !debounceElapsed)) {
        return;
    }

    // 核心流程：ELF 成功加载后用空查询预热候选；输入变化后按配置消抖实时刷新候选列表。
    state.options = application_.queryElfStaticAddresses(state.draft, effectiveLimit);
    state.queriedDraft = state.draft;
    state.queriedLimit = effectiveLimit;
    state.loadedRevision = loadedRevision;
}

std::vector<std::string> GuiRuntime::elfSymbolComboLabels(const ElfSymbolComboUiState& state) const
{
    std::vector<std::string> labels;
    labels.reserve(state.options.size());
    for (const auto& option : state.options) {
        labels.push_back(option.label);
    }
    return labels;
}

bool GuiRuntime::commitElfSymbolComboSelection(const scripting::ControlDescriptor& descriptor,
                                               const ElfSymbolComboUiState& state,
                                               const std::string& selectedLabel)
{
    const auto selected = std::find_if(
        state.options.begin(), state.options.end(), [&](const auto& option) { return option.label == selectedLabel; });
    if (selected == state.options.end()) {
        return false;
    }
    updateDynamicControlValueWithFeedback(descriptor, *selected);
    return true;
}

bool GuiRuntime::drawDynamicIntControl(const scripting::ControlSnapshot& control,
                                       const std::string& inputLabel,
                                       std::string_view visibleLabel)
{
    const auto& descriptor = control.descriptor;
    int value = std::get<int>(control.value);
    drawLuaControlLeftLabel(descriptor, visibleLabel);
    if (ImGui::InputInt(inputLabel.c_str(), &value)) {
        updateDynamicControlValueWithFeedback(descriptor, value);
        return true;
    }
    return false;
}

bool GuiRuntime::drawDynamicFloatControl(const scripting::ControlSnapshot& control,
                                         const std::string& inputLabel,
                                         std::string_view visibleLabel)
{
    const auto& descriptor = control.descriptor;
    float value = std::get<float>(control.value);
    drawLuaControlLeftLabel(descriptor, visibleLabel);
    if (ImGui::InputFloat(inputLabel.c_str(), &value)) {
        updateDynamicControlValueWithFeedback(descriptor, value);
        return true;
    }
    return false;
}

bool GuiRuntime::drawValueTableControl(const scripting::ControlSnapshot& control, std::string_view visibleLabel)
{
    const auto* value = std::get_if<scripting::ValueTableValue>(&control.value);
    if (value == nullptr) {
        return false;
    }

    const auto& descriptor = control.descriptor;
    if (!visibleLabel.empty()) {
        ImGui::TextUnformatted(visibleLabel.data(), visibleLabel.data() + visibleLabel.size());
        drawLuaControlCompactTooltip(descriptor, visibleLabel);
    }

    const std::string tableId = "##lua_value_table_" + descriptor.id;
    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(tableId.c_str(), 3, flags)) {
        return false;
    }

    ImGui::TableSetupColumn("label");
    ImGui::TableSetupColumn("value");
    ImGui::TableSetupColumn("unit");
    ImGui::TableHeadersRow();
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(descriptor.valueRows.size()));
    while (clipper.Step()) {
        for (int visibleIndex = clipper.DisplayStart; visibleIndex < clipper.DisplayEnd; ++visibleIndex) {
            const auto index = static_cast<std::size_t>(visibleIndex);
            const auto& row = descriptor.valueRows[index];
            const char* rowValue = "";
            if (index < value->rows.size() && value->rows[index].set) {
                rowValue = value->rows[index].value.c_str();
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(row.label.c_str());
            if (!row.note.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::SetTooltip("%s", row.note.c_str());
            }
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(rowValue);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextUnformatted(row.unit.c_str());
        }
    }
    ImGui::EndTable();
    return false;
}

bool GuiRuntime::drawTxSequenceControl(const scripting::ControlSnapshot& control, std::string_view visibleLabel)
{
    const auto* current = std::get_if<scripting::TxSequenceValue>(&control.value);
    if (current == nullptr) {
        return false;
    }

    const auto& descriptor = control.descriptor;
    auto next = *current;
    bool updated = false;
    if (!visibleLabel.empty()) {
        ImGui::TextUnformatted(visibleLabel.data(), visibleLabel.data() + visibleLabel.size());
        drawLuaControlCompactTooltip(descriptor, visibleLabel);
    }

    ImGui::PushID(descriptor.id.c_str());
    const float toolbarHeight = compactLogToolbarHeight();
    if (beginToolbarGroup("tx_sequence_toolbar", nullptr, toolbarHeight)) {
        if (drawTransferToolbarButton(next.running ? PROTOSCOPE_ICON_PAUSE " 停止" : PROTOSCOPE_ICON_PLAY " 开始",
                                      next.running ? "停止发送序列" : "开始发送序列",
                                      next.running,
                                      76.0F)) {
            next.running = !next.running;
            updated = true;
        }
        ImGui::SameLine();
        if (drawTransferToolbarButton("添加", "添加一行发送帧", false, 54.0F)) {
            next.frames.push_back(makeTxSequenceUiFrame(descriptor, nextTxSequenceFrameId(next)));
            updated = true;
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(PROTOSCOPE_ICON_CLOCK " interval_ms");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(82.0F);
        int intervalMs = next.intervalMs;
        if (ImGui::InputInt("##interval_ms", &intervalMs, 0, 0)) {
            next.intervalMs = std::max(1, intervalMs);
            updated = true;
        }
        ImGui::SameLine();
        if (ImGui::Checkbox("loop", &next.loop)) {
            updated = true;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("启用 %zu / %zu", enabledTxSequenceFrameCount(next), next.frames.size());
    }
    endToolbarGroup();

    const int columnCount = 3 + static_cast<int>(descriptor.txSequenceFields.size());
    const std::string tableId = "##lua_tx_sequence_" + descriptor.id;
    constexpr ImGuiTableFlags flags =
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_SizingFixedFit;
    const float rowHeight = ImGui::GetFrameHeightWithSpacing() + 6.0F;
    const float tableHeight =
        std::min(280.0F, std::max(120.0F, rowHeight * static_cast<float>(std::min<std::size_t>(next.frames.size() + 1U, 8U))));
    if (ImGui::BeginChild("##tx_sequence_table_scroll",
                          ImVec2(0.0F, tableHeight),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        if (ImGui::BeginTable(tableId.c_str(), columnCount, flags)) {
            ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, 48.0F);
            ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, 128.0F);
            for (const auto& field : descriptor.txSequenceFields) {
                ImGui::TableSetupColumn(field.label.c_str(), ImGuiTableColumnFlags_WidthFixed, 124.0F);
            }
            ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 160.0F);
            ImGui::TableHeadersRow();

            for (std::size_t rowIndex = 0; rowIndex < next.frames.size(); ++rowIndex) {
                auto& frame = next.frames[rowIndex];
                ImGui::PushID(static_cast<int>(frame.id));
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                if (ImGui::Checkbox("##enabled", &frame.enabled)) {
                    updated = true;
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::SetNextItemWidth(118.0F);
                char nameBuffer[128]{};
                std::snprintf(nameBuffer, sizeof(nameBuffer), "%s", frame.name.c_str());
                if (ImGui::InputText("##name", nameBuffer, sizeof(nameBuffer))) {
                    frame.name = nameBuffer;
                    updated = true;
                }

                int column = 2;
                for (const auto& field : descriptor.txSequenceFields) {
                    ImGui::TableSetColumnIndex(column++);
                    if (drawTxSequenceFieldEditor(field, frame)) {
                        updated = true;
                    }
                }

                ImGui::TableSetColumnIndex(column);
                const bool canMoveUp = rowIndex > 0;
                const bool canMoveDown = rowIndex + 1 < next.frames.size();
                if (!canMoveUp) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("上", ImVec2(30.0F, 0.0F))) {
                    std::swap(next.frames[rowIndex - 1], next.frames[rowIndex]);
                    updated = true;
                }
                drawIconTooltip("上移一行");
                if (!canMoveUp) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                if (!canMoveDown) {
                    ImGui::BeginDisabled();
                }
                if (ImGui::Button("下", ImVec2(30.0F, 0.0F))) {
                    std::swap(next.frames[rowIndex], next.frames[rowIndex + 1]);
                    updated = true;
                }
                drawIconTooltip("下移一行");
                if (!canMoveDown) {
                    ImGui::EndDisabled();
                }
                ImGui::SameLine();
                if (ImGui::Button("复制", ImVec2(44.0F, 0.0F))) {
                    auto copy = frame;
                    copy.id = nextTxSequenceFrameId(next);
                    next.frames.insert(next.frames.begin() + static_cast<std::ptrdiff_t>(rowIndex + 1), std::move(copy));
                    updated = true;
                }
                drawIconTooltip("复制当前帧");
                ImGui::SameLine();
                if (ImGui::Button(PROTOSCOPE_ICON_TRASH, ImVec2(30.0F, 0.0F))) {
                    next.frames.erase(next.frames.begin() + static_cast<std::ptrdiff_t>(rowIndex));
                    updated = true;
                    ImGui::PopID();
                    break;
                }
                drawIconTooltip("删除当前帧");
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
    ImGui::PopID();

    if (updated) {
        updateDynamicControlValueWithFeedback(descriptor, next);
        return true;
    }
    return false;
}

} // namespace protoscope::ui
