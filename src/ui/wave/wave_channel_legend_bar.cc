#include "wave_render_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

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
        const char* timeUnit =
            wave.cachedDisplayData.timeUnit.empty() ? nullptr : wave.cachedDisplayData.timeUnit.c_str();
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
        drawList->AddRectFilled(swatchMin,
                                swatchMax,
                                ImGui::ColorConvertFloat4ToU32(channelColor(*spec, view.measurementChannelIndex)),
                                2.0F);
        ImGui::Dummy(ImVec2(swatchSize, ImGui::GetTextLineHeight()));
        const bool swatchHovered = ImGui::IsItemHovered();

        const std::string shortText = "CH" + std::to_string(view.measurementChannelIndex + 1);
        const std::string fullText = "激活 " + shortText + " · " + spec->label;
        const float textStartX = summaryStartX + swatchSize + style.ItemInnerSpacing.x;
        const float textWidth = contentRight - textStartX;
        const std::string& visibleText = ImGui::CalcTextSize(fullText.c_str()).x <= textWidth ? fullText : shortText;
        if (ImGui::CalcTextSize(visibleText.c_str()).x > textWidth) {
            if (swatchHovered) {
                ImGui::SetTooltip("激活通道：%s", fullText.c_str());
            }
            return;
        }

        ImGui::SameLine(0.0F, style.ItemInnerSpacing.x);
        ImGui::TextDisabled("%s", visibleText.c_str());
        if (swatchHovered || ImGui::IsItemHovered()) {
            ImGui::SetTooltip("激活通道：%s", fullText.c_str());
        }
    }

    void drawChannelLegendHeader(plot::WaveDockState& wave)
    {
        const auto& view = wave.view;
        ImGui::AlignTextToFramePadding();
        const std::string headerText = std::string("图例 / 吸附范围：") + snapScopeName(view.cursorSnapScope) + " · " +
                                       formatLegendGridSummary(wave);
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
        const ImVec4 channelTint = channelColor(spec, channelIndex);
        const ImU32 fillColor =
            ImGui::GetColorU32(active ? ImVec4(0.16F, 0.34F, 0.22F, 0.98F) : ImVec4(0.11F, 0.12F, 0.14F, 0.95F));
        const ImU32 borderColor =
            ImGui::GetColorU32(active ? ImVec4(0.72F, 0.96F, 0.62F, 1.0F) : ImVec4(1.0F, 1.0F, 1.0F, 0.14F));
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(cardMin, cardMax, fillColor, cardStyle.rounding);
        if (active) {
            drawList->AddRectFilled(cardMin,
                                    cardMax,
                                    ImGui::GetColorU32(ImVec4(channelTint.x, channelTint.y, channelTint.z, 0.10F)),
                                    cardStyle.rounding);
        }
        drawList->AddRect(cardMin, cardMax, borderColor, cardStyle.rounding, 0, active ? 2.4F : 1.0F);
        if (active) {
            drawList->AddRect(ImVec2(cardMin.x + 3.0F, cardMin.y + 3.0F),
                              ImVec2(cardMax.x - 3.0F, cardMax.y - 3.0F),
                              ImGui::GetColorU32(ImVec4(channelTint.x, channelTint.y, channelTint.z, 0.65F)),
                              cardStyle.rounding - 2.0F,
                              0,
                              1.2F);
        }
        drawList->AddRectFilled(cardMin,
                                ImVec2(cardMin.x + cardStyle.colorBandWidth, cardMax.y),
                                ImGui::ColorConvertFloat4ToU32(channelTint),
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

    constexpr float kLegendOverlayFontScale = 0.88F;
    constexpr std::size_t kCompactLegendMaxRows = 8U;
    constexpr std::size_t kExpandedLegendMaxVisibleRows = 10U;

    std::vector<std::size_t> collectLegendAnalogChannels(const plot::WaveDockState& wave,
                                                         const plot::WaveSnapshot& snapshot,
                                                         bool includeHidden)
    {
        std::vector<std::size_t> channels;
        channels.reserve(snapshot.channels.size());
        for (std::size_t channelIndex = 0; channelIndex < snapshot.channels.size(); ++channelIndex) {
            const auto spec = wave.buffer.channelSpec(channelIndex);
            if (!spec.has_value() || bitDisplayEnabled(spec->bitDisplay)) {
                continue;
            }
            if (!includeHidden && channelHiddenByLegendState(wave, spec->label)) {
                continue;
            }
            channels.push_back(channelIndex);
        }
        return channels;
    }

    std::string formatActualPerDivText(const plot::WaveViewState& view, const plot::ChannelSpec& spec)
    {
        const double displayPerDiv = plot::waveDisplayValuePerDivision(view.viewMinValue, view.viewMaxValue);
        const auto perDiv = plot::waveChannelValuePerDivision(
            displayPerDiv, spec, view.displayFormula, plot::WaveGridDivisionReadoutMode::ActualValue);
        if (!perDiv.has_value()) {
            return "n/a/div";
        }
        return trimMetricText(formatMetricText(*perDiv, nullptr)) + "/div";
    }

    void drawClippedText(const ImVec2& min,
                         const ImVec2& max,
                         const std::string& text,
                         ImU32 color,
                         bool tooltipWhenClipped = true)
    {
        auto* drawList = ImGui::GetWindowDrawList();
        const float textY = min.y + (max.y - min.y - ImGui::GetTextLineHeight()) * 0.5F;
        drawList->PushClipRect(min, max, true);
        drawList->AddText(ImVec2(min.x, textY), color, text.c_str());
        drawList->PopClipRect();
        if (tooltipWhenClipped && ImGui::IsMouseHoveringRect(min, max) &&
            ImGui::CalcTextSize(text.c_str()).x > max.x - min.x) {
            ImGui::SetTooltip("%s", text.c_str());
        }
    }

    ImVec2 windowPositionForSize(const plot::WaveLegendOverlayState& overlay,
                                 const ImVec2& plotPos,
                                 const ImVec2& plotSize,
                                 const ImVec2& windowSize)
    {
        const float clampedX = (std::clamp)(overlay.offsetX, 0.0F, (std::max)(0.0F, plotSize.x - windowSize.x));
        const float clampedY = (std::clamp)(overlay.offsetY, 0.0F, (std::max)(0.0F, plotSize.y - windowSize.y));
        return ImVec2(plotPos.x + clampedX, plotPos.y + clampedY);
    }

    ImVec2 compactLegendWindowSize(plot::WaveDockState& wave,
                                   const std::vector<std::size_t>& channels,
                                   const ImVec2& plotSize)
    {
        float maxNameWidth = 0.0F;
        float maxDivisionWidth = ImGui::CalcTextSize("n/a/div").x;
        const std::size_t rowCount = (std::min<std::size_t>)(channels.size(), kCompactLegendMaxRows);
        for (std::size_t channelIndex = 0; channelIndex < rowCount; ++channelIndex) {
            const auto spec = wave.buffer.channelSpec(channels[channelIndex]);
            if (!spec.has_value()) {
                continue;
            }
            maxNameWidth = (std::max)(maxNameWidth, ImGui::CalcTextSize(spec->label.c_str()).x);
            maxDivisionWidth = (std::max)(
                maxDivisionWidth, ImGui::CalcTextSize(formatActualPerDivText(wave.view, *spec).c_str()).x);
        }

        const auto& style = ImGui::GetStyle();
        const float swatchAndChannelWidth = 10.0F + style.ItemSpacing.x + ImGui::CalcTextSize("CH99").x;
        const float rightWidth = (std::min)(96.0F, maxDivisionWidth + 10.0F);
        const float maxWindowWidth = (std::max)(64.0F, plotSize.x - 16.0F);
        const float nameRoom = (std::max)(56.0F, maxWindowWidth - rightWidth - swatchAndChannelWidth - 34.0F);
        const float nameWidth = (std::clamp)(maxNameWidth, 56.0F, nameRoom);
        const float minWindowWidth = (std::min)(220.0F, maxWindowWidth);
        const float width =
            (std::clamp)(swatchAndChannelWidth + nameWidth + rightWidth + 30.0F, minWindowWidth, maxWindowWidth);
        const float rowHeight = ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0F;
        const float effectiveRowCount = static_cast<float>((std::max<std::size_t>)(rowCount, 1U));
        const float rowsHeight = effectiveRowCount * rowHeight;
        const float moreHeight = channels.size() > rowCount ? ImGui::GetTextLineHeightWithSpacing() : 0.0F;
        const float height = (std::min)(plotSize.y - 16.0F, rowsHeight + moreHeight + style.WindowPadding.y * 2.0F);
        return ImVec2(width, (std::max)(44.0F, height));
    }

    void drawCompactLegendRows(plot::WaveDockState& wave, const std::vector<std::size_t>& channels, float windowWidth)
    {
        auto& view = wave.view;
        const auto& style = ImGui::GetStyle();
        const float rowHeight = ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0F;
        const float swatchSize = 9.0F;
        const float channelWidth = ImGui::CalcTextSize("CH99").x + 4.0F;
        const float divisionWidth = 96.0F;
        const float contentWidth = windowWidth - style.WindowPadding.x * 2.0F;
        const float nameWidth = (std::max)(0.0F,
                                           contentWidth - swatchSize - channelWidth - divisionWidth -
                                               style.ItemSpacing.x * 4.0F);
        const std::size_t maxCompactRows = (std::min<std::size_t>)(channels.size(), kCompactLegendMaxRows);

        if (maxCompactRows == 0U) {
            ImGui::TextDisabled("无可见 CH");
            return;
        }

        for (std::size_t rowIndex = 0; rowIndex < maxCompactRows; ++rowIndex) {
            const std::size_t channelIndex = channels[rowIndex];
            const auto spec = wave.buffer.channelSpec(channelIndex);
            if (!spec.has_value()) {
                continue;
            }

            const bool active = channelIndex == view.measurementChannelIndex;
            const ImVec2 rowMin = ImGui::GetCursorScreenPos();
            const ImVec2 rowSize(contentWidth, rowHeight);
            ImGui::PushID(static_cast<int>(channelIndex));
            ImGui::InvisibleButton("##compact_legend_row", rowSize);
            const bool hovered = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                view.measurementChannelIndex = channelIndex;
            }
            if (ImGui::BeginPopupContextItem("##compact_legend_context")) {
                if (ImGui::MenuItem("隐藏通道")) {
                    setChannelHidden(wave, spec->label, true);
                }
                ImGui::EndPopup();
            }

            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 rowMax(rowMin.x + rowSize.x, rowMin.y + rowSize.y);
            const ImVec4 tint = channelColor(*spec, channelIndex);
            if (active || hovered) {
                const ImVec4 fill = active ? ImVec4(tint.x, tint.y, tint.z, 0.24F)
                                           : ImVec4(1.0F, 1.0F, 1.0F, 0.06F);
                drawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(fill), 4.0F);
            }
            if (active) {
                drawList->AddRect(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(tint), 4.0F, 0, 1.5F);
            }

            float x = rowMin.x + 4.0F;
            const float centerY = rowMin.y + (rowHeight - swatchSize) * 0.5F;
            drawList->AddRectFilled(ImVec2(x, centerY),
                                    ImVec2(x + swatchSize, centerY + swatchSize),
                                    ImGui::ColorConvertFloat4ToU32(tint),
                                    3.0F);
            x += swatchSize + style.ItemSpacing.x;

            const std::string channelText = "CH" + std::to_string(channelIndex + 1U);
            drawClippedText(ImVec2(x, rowMin.y),
                            ImVec2(x + channelWidth, rowMax.y),
                            channelText,
                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                            false);
            x += channelWidth + style.ItemSpacing.x;

            drawClippedText(
                ImVec2(x, rowMin.y), ImVec2(x + nameWidth, rowMax.y), spec->label, ImGui::GetColorU32(ImGuiCol_Text));
            x += nameWidth + style.ItemSpacing.x;

            drawClippedText(ImVec2(x, rowMin.y),
                            ImVec2(x + divisionWidth, rowMax.y),
                            formatActualPerDivText(view, *spec),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled));

            ImGui::PopID();
        }
        if (channels.size() > maxCompactRows) {
            ImGui::TextDisabled("+ %zu 个 CH", channels.size() - maxCompactRows);
        }
    }

    void activateLegendRowFromBlankArea(plot::WaveDockState& wave,
                                        const ImVec2& rowMin,
                                        const ImVec2& rowMax,
                                        std::size_t channelIndex)
    {
        if (ImGui::IsMouseHoveringRect(rowMin, rowMax) && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsAnyItemHovered() && !ImGui::IsAnyItemActive()) {
            wave.view.measurementChannelIndex = channelIndex;
        }
    }

    void drawExpandedLegendChannelRow(plot::WaveDockState& wave, std::size_t channelIndex, const plot::ChannelSpec& spec)
    {
        ImGui::PushID(static_cast<int>(channelIndex));
        const plot::ChannelSpec defaultSpec = channelDefaultSpec(wave, channelIndex, spec);
        auto updated = spec;
        bool visible = !channelHiddenByLegendState(wave, spec.label);
        const bool active = channelIndex == wave.view.measurementChannelIndex;
        const ImVec2 rowMin = ImGui::GetCursorScreenPos();
        if (active) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(ImVec4(0.20F, 0.38F, 0.22F, 0.42F)));
        }

        ImGui::TableNextColumn();
        if (ImGui::Checkbox("##visible", &visible)) {
            setChannelHidden(wave, spec.label, !visible);
        }

        ImGui::TableNextColumn();
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

        ImGui::TableNextColumn();
        ImGui::Text("CH%zu", channelIndex + 1U);

        ImGui::TableNextColumn();
        char labelBuffer[128]{};
        std::snprintf(labelBuffer, sizeof(labelBuffer), "%s", updated.label.c_str());
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputText("##label", labelBuffer, sizeof(labelBuffer))) {
            updated.label = labelBuffer;
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(updated.label.c_str()).x > ImGui::GetItemRectSize().x) {
            ImGui::SetTooltip("%s", updated.label.c_str());
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputDouble("##ratio", &updated.ratio, 0.0, 0.0, "%.4g")) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputDouble("##scale", &updated.scale, 0.0, 0.0, "%.4g")) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0F);
        if (ImGui::InputDouble("##offset", &updated.offset, 0.0, 0.0, "%.4g")) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }

        ImGui::TableNextColumn();
        if (active) {
            ImGui::TextDisabled("激活");
            ImGui::SameLine();
        }
        if (ImGui::SmallButton(visible ? "隐藏" : "显示")) {
            setChannelHidden(wave, spec.label, visible);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("恢复")) {
            if (plot::resetOneChannelViewSettings(wave, channelIndex)) {
                invalidateWaveDisplayCaches(wave);
            }
        }
        const ImVec2 rowMax(ImGui::GetWindowPos().x + ImGui::GetWindowWidth(), ImGui::GetCursorScreenPos().y);
        activateLegendRowFromBlankArea(wave, rowMin, rowMax, channelIndex);
        ImGui::PopID();
    }

    void drawExpandedLegendRows(plot::WaveDockState& wave, const std::vector<std::size_t>& channels)
    {
        if (channels.empty()) {
            ImGui::TextDisabled("没有普通 CH 可显示");
            return;
        }

        if (ImGui::BeginChild("##wave_legend_overlay_rows", ImVec2(0.0F, 0.0F), false, ImGuiWindowFlags_None)) {
            const ImGuiTableFlags tableFlags =
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##wave_legend_overlay_table", 8, tableFlags)) {
                ImGui::TableSetupColumn("显示", ImGuiTableColumnFlags_WidthFixed, 38.0F);
                ImGui::TableSetupColumn("颜色", ImGuiTableColumnFlags_WidthFixed, 90.0F);
                ImGui::TableSetupColumn("通道", ImGuiTableColumnFlags_WidthFixed, 42.0F);
                ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 120.0F);
                ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_WidthFixed, 68.0F);
                ImGui::TableSetupColumn("Scale", ImGuiTableColumnFlags_WidthFixed, 68.0F);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 78.0F);
                ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 118.0F);
                ImGui::TableHeadersRow();
                for (const std::size_t channelIndex : channels) {
                    const auto spec = wave.buffer.channelSpec(channelIndex);
                    if (!spec.has_value()) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    drawExpandedLegendChannelRow(wave, channelIndex, *spec);
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();
    }

    ImVec2 expandedLegendWindowSize(const ImVec2& plotSize, std::size_t channelCount)
    {
        const float maxWidth = (std::max)(64.0F, plotSize.x - 16.0F);
        const float minWidth = (std::min)(600.0F, maxWidth);
        const float width = (std::clamp)(plotSize.x * 0.70F, minWidth, maxWidth);
        const float maxHeight = (std::max)(48.0F, plotSize.y - 16.0F);
        const float visibleRows = static_cast<float>((std::min<std::size_t>)(channelCount, kExpandedLegendMaxVisibleRows));
        const float rowHeight = ImGui::GetFrameHeightWithSpacing();
        const float tableHeight =
            rowHeight * (visibleRows + 1.0F) + (channelCount > kExpandedLegendMaxVisibleRows ? ImGui::GetStyle().ScrollbarSize : 0.0F);
        const float chromeHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() +
                                   ImGui::GetStyle().WindowPadding.y * 2.0F + 8.0F;
        const float wantedHeight = chromeHeight + (channelCount == 0U ? rowHeight : tableHeight);
        const float minHeight = (std::min)(110.0F, maxHeight);
        const float height = (std::clamp)(wantedHeight, minHeight, maxHeight);
        return ImVec2(width, height);
    }

} // namespace

void drawChannelLegendOverlay(plot::WaveDockState& wave,
                              const plot::WaveSnapshot& snapshot,
                              const ImVec2& plotPos,
                              const ImVec2& plotSize)
{
    if (wave.legendOverlay.openMode == plot::WaveLegendOverlayOpenMode::Disabled) {
        wave.legendOverlay.expanded = false;
        wave.legendOverlay.hoverFloating = false;
        wave.legendOverlay.hoverInteractionLocked = false;
        wave.legendOverlay.hoverCloseRemainingSec = 0.0F;
        return;
    }

    // 固定展开只属于双击模式；悬浮模式每帧都回到瞬态展开语义。
    if (wave.legendOverlay.openMode == plot::WaveLegendOverlayOpenMode::Hover) {
        wave.legendOverlay.expanded = false;
    } else {
        wave.legendOverlay.hoverFloating = false;
        wave.legendOverlay.hoverInteractionLocked = false;
        wave.legendOverlay.hoverCloseRemainingSec = 0.0F;
    }

    if (snapshot.channels.empty() || !wave.view.showChannelLegend) {
        return;
    }

    if (plotSize.x <= 80.0F || plotSize.y <= 60.0F) {
        return;
    }

    const std::vector<std::size_t> compactChannels = collectLegendAnalogChannels(wave, snapshot, false);
    const std::vector<std::size_t> expandedChannels = collectLegendAnalogChannels(wave, snapshot, true);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0F, 6.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4.0F, 2.0F));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(5.0F, 3.0F));

    const bool effectiveExpanded = wave.legendOverlay.openMode == plot::WaveLegendOverlayOpenMode::DoubleClick
                                       ? wave.legendOverlay.expanded
                                       : wave.legendOverlay.hoverFloating;
    const ImVec2 compactSize = compactLegendWindowSize(wave, compactChannels, plotSize);
    const ImVec2 expandedSize = expandedLegendWindowSize(plotSize, expandedChannels.size());
    const ImVec2 compactPos = windowPositionForSize(wave.legendOverlay, plotPos, plotSize, compactSize);
    const ImVec2 expandedPos = windowPositionForSize(wave.legendOverlay, plotPos, plotSize, expandedSize);
    const ImVec2 windowSize = effectiveExpanded ? expandedSize : compactSize;
    const ImVec2 windowPos = effectiveExpanded ? expandedPos : compactPos;

    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,
                          effectiveExpanded ? ImVec4(0.051F, 0.075F, 0.106F, 0.96F)
                                            : ImVec4(0.051F, 0.075F, 0.106F, 0.78F));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30F, 0.42F, 0.54F, 0.55F));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##wave_channel_overlay_legend", nullptr, flags)) {
        ImGui::SetWindowFontScale(kLegendOverlayFontScale);
        const bool hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem |
                                                    ImGuiHoveredFlags_ChildWindows);
        const ImVec2 actualWindowMin = ImGui::GetWindowPos();
        const ImVec2 actualWindowMax(actualWindowMin.x + ImGui::GetWindowWidth(),
                                     actualWindowMin.y + ImGui::GetWindowHeight());
        const bool mouseInActualWindow = ImGui::IsMouseHoveringRect(actualWindowMin, actualWindowMax, false);
        bool collapseRequested = false;

        if (effectiveExpanded) {
            ImGui::TextUnformatted("通道图例");
            ImGui::SameLine();
            if (ImGui::SmallButton("全部恢复")) {
                if (plot::resetAllChannelViewSettings(wave)) {
                    invalidateWaveDisplayCaches(wave);
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("收起")) {
                wave.legendOverlay.expanded = false;
                wave.legendOverlay.hoverFloating = false;
                wave.legendOverlay.hoverInteractionLocked = false;
                wave.legendOverlay.hoverCloseRemainingSec = 0.0F;
                collapseRequested = true;
            }
            ImGui::Separator();
        }

        if (effectiveExpanded) {
            drawExpandedLegendRows(wave, expandedChannels);
        } else {
            drawCompactLegendRows(wave, compactChannels, windowSize.x);
        }

        if (wave.legendOverlay.openMode == plot::WaveLegendOverlayOpenMode::DoubleClick && !effectiveExpanded &&
            hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            wave.legendOverlay.expanded = true;
        }

        // 实际窗口矩形只用于保持已展开交互，不作为新的悬浮触发源。
        const bool previousInteractionLocked = wave.legendOverlay.hoverInteractionLocked;
        const bool mouseHeldInLegend = (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                                        ImGui::IsMouseDown(ImGuiMouseButton_Right)) &&
                                       (hovered || previousInteractionLocked ||
                                        (wave.legendOverlay.hoverFloating && mouseInActualWindow));
        const bool activeLegendInteraction =
            ImGui::IsAnyItemActive() &&
            (hovered || previousInteractionLocked || (wave.legendOverlay.hoverFloating && mouseInActualWindow));
        wave.legendOverlay.hoverInteractionLocked = mouseHeldInLegend || activeLegendInteraction;

        if (wave.legendOverlay.openMode == plot::WaveLegendOverlayOpenMode::Hover) {
            if (collapseRequested) {
                wave.legendOverlay.hoverFloating = false;
                wave.legendOverlay.hoverInteractionLocked = false;
                wave.legendOverlay.hoverCloseRemainingSec = 0.0F;
            } else if (hovered || wave.legendOverlay.hoverInteractionLocked) {
                wave.legendOverlay.hoverFloating = true;
                wave.legendOverlay.hoverCloseRemainingSec = wave.legendOverlay.hoverCloseDelaySec;
            } else if (!wave.legendOverlay.expanded && wave.legendOverlay.hoverFloating) {
                wave.legendOverlay.hoverCloseRemainingSec -= ImGui::GetIO().DeltaTime;
                if (wave.legendOverlay.hoverCloseRemainingSec <= 0.0F) {
                    wave.legendOverlay.hoverFloating = false;
                }
            }
        } else {
            wave.legendOverlay.hoverFloating = false;
            wave.legendOverlay.hoverInteractionLocked = false;
            wave.legendOverlay.hoverCloseRemainingSec = 0.0F;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
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
