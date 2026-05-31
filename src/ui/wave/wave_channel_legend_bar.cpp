// 本文件由 wave_dock_renderer.cpp 按原顺序包含，承接对应 Wave 业务组件实现。

#if !defined(PROTOSCOPE_WAVE_RENDERER_COMPONENT_INCLUDE)
#error "This wave component implementation is included by wave_dock_renderer.cpp"
#endif


void drawChannelLegendBar(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot) {
    if (snapshot.channels.empty()) {
        return;
    }
    auto& view = wave.view;
    clampActiveChannel(view, snapshot.channels.size());
    const ChannelLegendMetrics metrics = measureChannelLegendMetrics(ImGui::GetContentRegionAvail().x, view);
    const bool scrollActiveIntoView = wave.lastLegendMeasurementChannelIndex != view.measurementChannelIndex;

    ImGui::Text("图例 / 吸附范围：%s", snapScopeName(view.cursorSnapScope));
    ImGui::Separator();

    if (ImGui::BeginChild("##wave_channel_legend_strip",
                          ImVec2(0.0F, metrics.stripHeight),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        const auto& style = ImGui::GetStyle();
        const float spacing = style.ItemSpacing.x;
        const float innerPaddingX = 10.0F;
        const float innerPaddingY = 8.0F;
        const float textSpacingY = 3.0F;
        const float colorBandWidth = 4.0F;
        const float rounding = 6.0F;

        // 核心流程：顶部通道区固定为单行卡片，通道再多也只走横向滚动，不再向下堆高。
        for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
            const auto spec = wave.buffer.channelSpec(channelIndex);
            if (!spec.has_value()) {
                continue;
            }
            if (channelIndex > 0) {
                ImGui::SameLine(0.0F, spacing);
            }

            ImGui::PushID(static_cast<int>(channelIndex));
            const ImVec2 cardMin = ImGui::GetCursorScreenPos();
            const ImVec2 cardSize(metrics.cardWidth, metrics.cardHeight);
            ImGui::InvisibleButton("##channel_card", cardSize);

            bool active = channelIndex == view.measurementChannelIndex;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                view.measurementChannelIndex = channelIndex;
                active = true;
            }
            if (active && (scrollActiveIntoView || ImGui::IsItemClicked(ImGuiMouseButton_Left))) {
                ImGui::SetScrollHereX(0.5F);
            }

            const bool hovered = ImGui::IsItemHovered();
            const ImVec2 cardMax(cardMin.x + cardSize.x, cardMin.y + cardSize.y);
            const ImU32 fillColor = ImGui::GetColorU32(active ? ImVec4(0.18F, 0.28F, 0.20F, 0.95F)
                                                              : ImVec4(0.11F, 0.12F, 0.14F, 0.95F));
            const ImU32 borderColor = ImGui::GetColorU32(active ? ImVec4(0.50F, 0.82F, 0.56F, 1.0F)
                                                                : ImVec4(1.0F, 1.0F, 1.0F, 0.14F));
            auto* drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(cardMin, cardMax, fillColor, rounding);
            drawList->AddRect(cardMin, cardMax, borderColor, rounding, 0, active ? 2.0F : 1.0F);
            drawList->AddRectFilled(cardMin,
                                    ImVec2(cardMin.x + colorBandWidth, cardMax.y),
                                    ImGui::ColorConvertFloat4ToU32(channelColor(*spec, channelIndex)),
                                    rounding,
                                    ImDrawFlags_RoundCornersLeft);

            const ImVec2 textMin(cardMin.x + colorBandWidth + innerPaddingX, cardMin.y + innerPaddingY);
            const ImVec2 textMax(cardMax.x - innerPaddingX, cardMax.y - innerPaddingY);
            const float lineHeight = ImGui::GetTextLineHeight();
            const ImVec2 titleMax(textMax.x, textMin.y + lineHeight);
            const ImVec2 summaryMin(textMin.x, titleMax.y + textSpacingY);
            const ImVec2 summaryMax(textMax.x, summaryMin.y + lineHeight);
            drawChannelCardText(textMin, titleMax, spec->label, ImGui::GetColorU32(ImGuiCol_Text));
            const std::string summary = "R " + formatMetricText(spec->ratio, nullptr)
                + "  S " + formatMetricText(spec->scale, nullptr)
                + "  O " + formatMetricText(spec->offset, nullptr);
            drawChannelCardText(summaryMin, summaryMax, summary, ImGui::GetColorU32(ImGuiCol_TextDisabled));

            if (hovered) {
                drawChannelCardTooltip(*spec, active);
            }
            if (ImGui::BeginPopupContextItem("##channel_popup")) {
                drawChannelLegendPopup(wave, channelIndex, *spec, active);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        // 交互约定：鼠标停在卡片条上滚轮优先转成横向滚动，减少必须拖滚动条的成本。
        if (ImGui::IsWindowHovered() && ImGui::GetScrollMaxX() > 0.0F && std::abs(ImGui::GetIO().MouseWheel) > 0.0F) {
            const float targetScroll = (std::clamp)(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * metrics.cardWidth * 0.85F,
                                                    0.0F,
                                                    ImGui::GetScrollMaxX());
            ImGui::SetScrollX(targetScroll);
        }
    }
    ImGui::EndChild();
    wave.lastLegendMeasurementChannelIndex = view.measurementChannelIndex;
}

void drawChannelControls(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot) {
    static_cast<void>(wave);
    static_cast<void>(snapshot);
}

float measureChannelLegendHeight(const plot::WaveSnapshot& snapshot, const plot::WaveViewState& view) {
    if (snapshot.channels.empty()) {
        return 0.0F;
    }
    return measureChannelLegendMetrics(ImGui::GetContentRegionAvail().x, view).totalHeight;
}
