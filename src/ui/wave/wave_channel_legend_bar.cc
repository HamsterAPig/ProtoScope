#include "wave_render_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace protoscope::ui {

namespace {

    struct ChannelLegendCardStyle {
        float spacing{0.0F};
        float innerPaddingX{10.0F};
        float innerPaddingY{8.0F};
        float textSpacingY{3.0F};
        float colorBandWidth{4.0F};
        float rounding{6.0F};
    };

    ChannelLegendCardStyle makeChannelLegendCardStyle()
    {
        ChannelLegendCardStyle cardStyle;
        cardStyle.spacing = ImGui::GetStyle().ItemSpacing.x;
        return cardStyle;
    }

    std::string trimMetricText(std::string text)
    {
        while (!text.empty() && text.back() == ' ') {
            text.pop_back();
        }
        return text;
    }

    const char* divisionReadoutLabel(plot::WaveGridDivisionReadoutMode mode)
    {
        switch (mode) {
            case plot::WaveGridDivisionReadoutMode::DisplayValue:
                return "显示";
            case plot::WaveGridDivisionReadoutMode::ActualValue:
                return "实际";
            case plot::WaveGridDivisionReadoutMode::RawValue:
                return "Raw";
        }
        return "显示";
    }

    std::string formatChannelDivisionSummary(const plot::WaveViewState& view, const plot::ChannelSpec& spec)
    {
        const double displayPerDiv = plot::waveDisplayValuePerDivision(view.viewMinValue, view.viewMaxValue);
        const auto perDiv =
            plot::waveChannelValuePerDivision(displayPerDiv, spec, view.displayFormula, view.gridDivisionReadoutMode);
        const std::string label = divisionReadoutLabel(view.gridDivisionReadoutMode);
        if (!perDiv.has_value()) {
            return label + " n/a/格";
        }
        const char* unit = view.gridDivisionReadoutMode == plot::WaveGridDivisionReadoutMode::ActualValue
                               ? (spec.unit.empty() ? nullptr : spec.unit.c_str())
                               : nullptr;
        return label + " " + trimMetricText(formatMetricText(*perDiv, unit)) + "/格";
    }

    std::string formatLegendGridSummary(const plot::WaveDockState& wave)
    {
        const auto& view = wave.view;
        const double xPerDiv =
            std::abs(view.viewMaxTime - view.viewMinTime) / static_cast<double>(plot::kWaveGridMajorXDivisions);
        const double yPerDiv = plot::waveDisplayValuePerDivision(view.viewMinValue, view.viewMaxValue);
        const char* timeUnit = wave.cachedDisplayData.timeUnit.empty() ? nullptr : wave.cachedDisplayData.timeUnit.c_str();
        return "网格 10x8 · X " + trimMetricText(formatMetricText(xPerDiv, timeUnit)) + "/格 · Y " +
               trimMetricText(formatMetricText(yPerDiv, nullptr)) + " 显示值/格";
    }

    void drawActiveChannelHeaderSummary(const plot::WaveDockState& wave, float reservedRightWidth)
    {
        const auto& view = wave.view;
        const auto spec = wave.buffer.channelSpec(view.measurementChannelIndex);
        if (!spec.has_value()) {
            return;
        }

        ImGui::SameLine();
        const auto& style = ImGui::GetStyle();
        const float contentRight = ImGui::GetContentRegionMax().x - reservedRightWidth - style.ItemSpacing.x;
        const float summaryStartX = ImGui::GetCursorPosX();
        const float swatchSize = ImGui::GetTextLineHeight() * 0.72F;
        if (contentRight - summaryStartX <= swatchSize + style.ItemSpacing.x) {
            return;
        }

        const ImVec2 cursorScreenPos = ImGui::GetCursorScreenPos();
        const float swatchOffsetY = (ImGui::GetTextLineHeight() - swatchSize) * 0.5F;
        const ImVec2 swatchMin(cursorScreenPos.x, cursorScreenPos.y + swatchOffsetY);
        const ImVec2 swatchMax(swatchMin.x + swatchSize, swatchMin.y + swatchSize);
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(
            swatchMin, swatchMax, ImGui::ColorConvertFloat4ToU32(channelColor(*spec, view.measurementChannelIndex)), 2.0F);
        ImGui::Dummy(ImVec2(swatchSize, ImGui::GetTextLineHeight()));
        const bool swatchHovered = ImGui::IsItemHovered();

        const std::string shortText = "CH" + std::to_string(view.measurementChannelIndex + 1);
        const std::string fullText = "当前 " + shortText + " · " + spec->label;
        const float textStartX = summaryStartX + swatchSize + style.ItemInnerSpacing.x;
        const float textWidth = contentRight - textStartX;
        const std::string& visibleText =
            ImGui::CalcTextSize(fullText.c_str()).x <= textWidth ? fullText : shortText;
        if (ImGui::CalcTextSize(visibleText.c_str()).x > textWidth) {
            if (swatchHovered) {
                ImGui::SetTooltip("当前通道：%s", fullText.c_str());
            }
            return;
        }

        ImGui::SameLine(0.0F, style.ItemInnerSpacing.x);
        ImGui::TextDisabled("%s", visibleText.c_str());
        if (swatchHovered || ImGui::IsItemHovered()) {
            ImGui::SetTooltip("当前通道：%s", fullText.c_str());
        }
    }

    void drawChannelLegendHeader(plot::WaveDockState& wave)
    {
        const auto& view = wave.view;
        ImGui::AlignTextToFramePadding();
        const std::string headerText =
            std::string("图例 / 吸附范围：") + snapScopeName(view.cursorSnapScope) + " · " + formatLegendGridSummary(wave);
        ImGui::Text("%s", headerText.c_str());
        const float buttonWidth =
            ImGui::CalcTextSize(wave.legendCollapsed ? "v" : "^").x + ImGui::GetStyle().FramePadding.x * 2.0F;
        drawActiveChannelHeaderSummary(wave, buttonWidth);
        ImGui::SameLine();
        ImGui::SetCursorPosX((std::max)(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - buttonWidth));
        if (ImGui::SmallButton(wave.legendCollapsed ? "v##wave_channel_legend_collapse"
                                                    : "^##wave_channel_legend_collapse")) {
            wave.legendCollapsed = !wave.legendCollapsed;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", wave.legendCollapsed ? "展开图例" : "折叠图例");
        }
        ImGui::Separator();
    }

    bool updateChannelLegendCardSelection(plot::WaveDockState& wave,
                                          plot::WaveViewState& view,
                                          std::size_t channelIndex,
                                          const plot::ChannelSpec& spec,
                                          bool scrollActiveIntoView)
    {
        bool active = channelIndex == view.measurementChannelIndex;
        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
        if (clicked) {
            view.measurementChannelIndex = channelIndex;
            active = true;
        }
        if (active && (scrollActiveIntoView || clicked)) {
            ImGui::SetScrollHereX(0.5F);
        }
        // 核心流程：双击当前 CH 卡片按配置恢复默认值，不依赖激活通道。
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            const plot::ChannelSpec defaultSpec = channelDefaultSpec(wave, channelIndex, spec);
            if (plot::resetChannelConfigToDefault(wave, channelIndex, defaultSpec, view.channelDoubleClickAction)) {
                invalidateWaveDisplayCaches(wave);
            }
        }
        return active;
    }

    void drawChannelLegendCardBody(const ImVec2& cardMin,
                                   const ImVec2& cardSize,
                                   const ChannelLegendCardStyle& cardStyle,
                                   const plot::ChannelSpec& spec,
                                   const plot::WaveViewState& view,
                                   std::size_t channelIndex,
                                   bool active)
    {
        const ImVec2 cardMax(cardMin.x + cardSize.x, cardMin.y + cardSize.y);
        const ImU32 fillColor =
            ImGui::GetColorU32(active ? ImVec4(0.18F, 0.28F, 0.20F, 0.95F) : ImVec4(0.11F, 0.12F, 0.14F, 0.95F));
        const ImU32 borderColor =
            ImGui::GetColorU32(active ? ImVec4(0.50F, 0.82F, 0.56F, 1.0F) : ImVec4(1.0F, 1.0F, 1.0F, 0.14F));
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(cardMin, cardMax, fillColor, cardStyle.rounding);
        drawList->AddRect(cardMin, cardMax, borderColor, cardStyle.rounding, 0, active ? 2.0F : 1.0F);
        drawList->AddRectFilled(cardMin,
                                ImVec2(cardMin.x + cardStyle.colorBandWidth, cardMax.y),
                                ImGui::ColorConvertFloat4ToU32(channelColor(spec, channelIndex)),
                                cardStyle.rounding,
                                ImDrawFlags_RoundCornersLeft);

        const ImVec2 textMin(cardMin.x + cardStyle.colorBandWidth + cardStyle.innerPaddingX,
                             cardMin.y + cardStyle.innerPaddingY);
        const ImVec2 textMax(cardMax.x - cardStyle.innerPaddingX, cardMax.y - cardStyle.innerPaddingY);
        const float lineHeight = ImGui::GetTextLineHeight();
        const ImVec2 titleMax(textMax.x, textMin.y + lineHeight);
        const ImVec2 summaryMin(textMin.x, titleMax.y + cardStyle.textSpacingY);
        const ImVec2 summaryMax(textMax.x, summaryMin.y + lineHeight);
        drawChannelCardText(textMin, titleMax, spec.label, ImGui::GetColorU32(ImGuiCol_Text));
        std::string summary = formatChannelDivisionSummary(view, spec);
        if (bitDisplayEnabled(spec.bitDisplay)) {
            summary = "Bits " + std::to_string(spec.bitDisplay.firstBit) + ".." +
                      std::to_string(spec.bitDisplay.firstBit + spec.bitDisplay.bitCount - 1U) + "  Y " +
                      formatMetricText(spec.bitDisplay.yOffset, nullptr);
        }
        drawChannelCardText(summaryMin, summaryMax, summary, ImGui::GetColorU32(ImGuiCol_TextDisabled));
    }

    void drawChannelLegendCard(plot::WaveDockState& wave,
                               plot::WaveViewState& view,
                               const ChannelLegendMetrics& metrics,
                               const ChannelLegendCardStyle& cardStyle,
                               std::size_t channelIndex,
                               const plot::ChannelSpec& spec,
                               bool scrollActiveIntoView)
    {
        ImGui::PushID(static_cast<int>(channelIndex));
        const ImVec2 cardMin = ImGui::GetCursorScreenPos();
        const ImVec2 cardSize(metrics.cardWidth, metrics.cardHeight);
        ImGui::InvisibleButton("##channel_card", cardSize);

        const bool active = updateChannelLegendCardSelection(wave, view, channelIndex, spec, scrollActiveIntoView);
        const bool hovered = ImGui::IsItemHovered();
        drawChannelLegendCardBody(cardMin, cardSize, cardStyle, spec, view, channelIndex, active);
        if (hovered) {
            drawChannelCardTooltip(spec, active);
        }
        if (ImGui::BeginPopupContextItem("##channel_popup")) {
            drawChannelLegendPopup(wave, channelIndex, spec, active);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    void handleChannelLegendStripMouseWheel(const ChannelLegendMetrics& metrics)
    {
        // 交互约定：鼠标停在卡片条上滚轮优先转成横向滚动，减少必须拖滚动条的成本。
        if (ImGui::IsWindowHovered() && ImGui::GetScrollMaxX() > 0.0F && std::abs(ImGui::GetIO().MouseWheel) > 0.0F) {
            const float targetScroll =
                (std::clamp)(ImGui::GetScrollX() - ImGui::GetIO().MouseWheel * metrics.cardWidth * 0.85F,
                             0.0F,
                             ImGui::GetScrollMaxX());
            ImGui::SetScrollX(targetScroll);
        }
    }

    void drawChannelLegendStrip(plot::WaveDockState& wave,
                                const plot::WaveSnapshot& snapshot,
                                const ChannelLegendMetrics& metrics,
                                bool scrollActiveIntoView)
    {
        if (wave.legendCollapsed) {
            return;
        }
        if (ImGui::BeginChild("##wave_channel_legend_strip",
                              ImVec2(0.0F, metrics.stripHeight),
                              ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            const ChannelLegendCardStyle cardStyle = makeChannelLegendCardStyle();
            auto& view = wave.view;

            // 核心流程：顶部通道区固定为单行卡片，通道再多也只走横向滚动，不再向下堆高。
            for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
                const auto spec = wave.buffer.channelSpec(channelIndex);
                if (!spec.has_value()) {
                    continue;
                }
                if (channelIndex > 0) {
                    ImGui::SameLine(0.0F, cardStyle.spacing);
                }

                drawChannelLegendCard(wave, view, metrics, cardStyle, channelIndex, *spec, scrollActiveIntoView);
            }

            handleChannelLegendStripMouseWheel(metrics);
        }
        ImGui::EndChild();
    }

} // namespace

void drawChannelLegendBar(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot)
{
    if (snapshot.channels.empty()) {
        return;
    }
    auto& view = wave.view;
    clampActiveChannel(view, snapshot.channels.size());
    const ChannelLegendMetrics metrics = measureChannelLegendMetrics(ImGui::GetContentRegionAvail().x, view);
    const bool scrollActiveIntoView = wave.lastLegendMeasurementChannelIndex != view.measurementChannelIndex;

    drawChannelLegendHeader(wave);
    drawChannelLegendStrip(wave, snapshot, metrics, scrollActiveIntoView);
    wave.lastLegendMeasurementChannelIndex = view.measurementChannelIndex;
}

namespace {

    void setChannelHidden(plot::WaveDockState& wave, const std::string& label, bool hidden)
    {
        auto& hiddenLabels = wave.hiddenChannelLabels;
        const auto existing = std::find(hiddenLabels.begin(), hiddenLabels.end(), label);
        if (hidden && existing == hiddenLabels.end()) {
            hiddenLabels.push_back(label);
            wave.legendVisibilityRestorePending = true;
        } else if (!hidden && existing != hiddenLabels.end()) {
            hiddenLabels.erase(existing);
            wave.legendVisibilityRestorePending = true;
        }
    }

    void drawLegendColorSwatch(const ImVec4& color, float size)
    {
        const ImVec2 min = ImGui::GetCursorScreenPos();
        const ImVec2 max(min.x + size, min.y + size);
        ImGui::Dummy(ImVec2(size, size));
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, ImGui::ColorConvertFloat4ToU32(color), 3.0F);
        drawList->AddRect(min, max, ImGui::GetColorU32(ImVec4(1.0F, 1.0F, 1.0F, 0.22F)), 3.0F);
    }

    void drawCompactLegendRows(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot)
    {
        const float swatchSize = 11.0F;
        const std::size_t maxCompactRows = (std::min<std::size_t>)(snapshot.channels.size(), 6U);
        for (std::size_t channelIndex = 0; channelIndex < maxCompactRows; ++channelIndex) {
            const auto spec = wave.buffer.channelSpec(channelIndex);
            if (!spec.has_value()) {
                continue;
            }
            const bool hidden = channelHiddenByLegendState(wave, spec->label);
            ImGui::PushID(static_cast<int>(channelIndex));
            drawLegendColorSwatch(withAlpha(channelColor(*spec, channelIndex), hidden ? 0.35F : 1.0F), swatchSize);
            ImGui::SameLine();
            ImGui::TextDisabled("CH%zu", channelIndex + 1U);
            ImGui::SameLine();
            ImGui::Text("%s%s", spec->label.c_str(), hidden ? " (隐藏)" : "");
            if (ImGui::IsItemClicked()) {
                setChannelHidden(wave, spec->label, !hidden);
            }
            ImGui::PopID();
        }
        if (snapshot.channels.size() > maxCompactRows) {
            ImGui::TextDisabled("+ %zu 个通道", snapshot.channels.size() - maxCompactRows);
        }
    }

    void drawExpandedLegendChannelRow(plot::WaveDockState& wave, std::size_t channelIndex, const plot::ChannelSpec& spec)
    {
        ImGui::PushID(static_cast<int>(channelIndex));
        const plot::ChannelSpec defaultSpec = channelDefaultSpec(wave, channelIndex, spec);
        auto updated = spec;
        bool visible = !channelHiddenByLegendState(wave, spec.label);
        if (ImGui::Checkbox("##visible", &visible)) {
            setChannelHidden(wave, spec.label, !visible);
        }
        ImGui::SameLine();

        std::array<float, 4> color = spec.color.value_or(std::array<float, 4>{
            fallbackChannelColor(channelIndex).x,
            fallbackChannelColor(channelIndex).y,
            fallbackChannelColor(channelIndex).z,
            fallbackChannelColor(channelIndex).w,
        });
        ImGui::SetNextItemWidth(92.0F);
        if (ImGui::ColorEdit4("##color", color.data(), ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_AlphaBar)) {
            updated.color = color;
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        ImGui::SameLine();

        ImGui::Text("CH%zu", channelIndex + 1U);
        ImGui::SameLine();
        char labelBuffer[128]{};
        std::snprintf(labelBuffer, sizeof(labelBuffer), "%s", updated.label.c_str());
        ImGui::SetNextItemWidth(110.0F);
        if (ImGui::InputText("##label", labelBuffer, sizeof(labelBuffer))) {
            updated.label = labelBuffer;
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(74.0F);
        if (ImGui::InputDouble("##ratio", &updated.ratio, 0.0, 0.0, "%.4g")) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(74.0F);
        if (ImGui::InputDouble("##scale", &updated.scale, 0.0, 0.0, "%.4g")) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(74.0F);
        if (ImGui::InputDouble("##offset", &updated.offset, 0.0, 0.0, "%.4g")) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", spec.unit.empty() ? "-" : spec.unit.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("恢复")) {
            if (plot::resetOneChannelViewSettings(wave, channelIndex)) {
                invalidateWaveDisplayCaches(wave);
            }
        }
        ImGui::PopID();
    }

    void drawExpandedLegendRows(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot)
    {
        ImGui::TextDisabled("显示  颜色       通道  名称          Ratio    Scale    Offset   单位");
        ImGui::Separator();
        const float rowHeight = ImGui::GetFrameHeightWithSpacing();
        const float maxRowsHeight = rowHeight * 8.0F + ImGui::GetStyle().ScrollbarSize;
        if (ImGui::BeginChild("##wave_legend_overlay_rows",
                              ImVec2(0.0F, (std::min)(maxRowsHeight, rowHeight * snapshot.channels.size() + 6.0F)),
                              false,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
                const auto spec = wave.buffer.channelSpec(channelIndex);
                if (!spec.has_value()) {
                    continue;
                }
                drawExpandedLegendChannelRow(wave, channelIndex, *spec);
            }
        }
        ImGui::EndChild();
    }

} // namespace

void drawChannelLegendOverlay(plot::WaveDockState& wave,
                              const plot::WaveSnapshot& snapshot,
                              const ImVec2& plotPos,
                              const ImVec2& plotSize)
{
    if (snapshot.channels.empty() || !wave.view.showChannelLegend) {
        return;
    }

    if (plotSize.x <= 80.0F || plotSize.y <= 60.0F) {
        return;
    }

    const bool expanded = wave.legendOverlay.expanded;
    const bool floating = expanded || wave.legendOverlay.hoverFloating;
    const ImVec2 windowSize(expanded ? (std::min)(560.0F, plotSize.x - 16.0F) : (floating ? 360.0F : 220.0F),
                            expanded ? 310.0F : (floating ? 160.0F : 118.0F));
    const float clampedX = (std::clamp)(wave.legendOverlay.offsetX, 0.0F, (std::max)(0.0F, plotSize.x - windowSize.x));
    const float clampedY = (std::clamp)(wave.legendOverlay.offsetY, 0.0F, (std::max)(0.0F, plotSize.y - windowSize.y));

    ImGui::SetNextWindowPos(ImVec2(plotPos.x + clampedX, plotPos.y + clampedY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, expanded ? ImVec4(0.051F, 0.075F, 0.106F, 0.96F)
                                                      : ImVec4(0.051F, 0.075F, 0.106F, 0.78F));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30F, 0.42F, 0.54F, 0.55F));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##wave_channel_overlay_legend", nullptr, flags)) {
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        if (hovered) {
            wave.legendOverlay.hoverFloating = true;
            wave.legendOverlay.hoverCloseRemainingSec = wave.legendOverlay.hoverCloseDelaySec;
        } else if (!expanded && wave.legendOverlay.hoverFloating) {
            wave.legendOverlay.hoverCloseRemainingSec -= ImGui::GetIO().DeltaTime;
            if (wave.legendOverlay.hoverCloseRemainingSec <= 0.0F) {
                wave.legendOverlay.hoverFloating = false;
            }
        }

        if (floating) {
            ImGui::TextUnformatted("通道图例");
            ImGui::SameLine();
            if (ImGui::SmallButton("全部恢复")) {
                if (plot::resetAllChannelViewSettings(wave)) {
                    invalidateWaveDisplayCaches(wave);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(expanded ? "收起" : "展开")) {
                wave.legendOverlay.expanded = !wave.legendOverlay.expanded;
            }
            ImGui::Separator();
        }

        if (expanded) {
            drawExpandedLegendRows(wave, snapshot);
        } else {
            drawCompactLegendRows(wave, snapshot);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
}

void drawChannelControls(plot::WaveDockState& wave, const plot::WaveSnapshot& snapshot)
{
    static_cast<void>(wave);
    static_cast<void>(snapshot);
}

float measureChannelLegendHeight(const plot::WaveSnapshot& snapshot, const plot::WaveDockState& wave)
{
    if (snapshot.channels.empty()) {
        return 0.0F;
    }
    const auto metrics = measureChannelLegendMetrics(ImGui::GetContentRegionAvail().x, wave.view);
    if (wave.legendCollapsed) {
        return metrics.totalHeight - metrics.stripHeight;
    }
    return metrics.totalHeight;
}

} // namespace protoscope::ui
