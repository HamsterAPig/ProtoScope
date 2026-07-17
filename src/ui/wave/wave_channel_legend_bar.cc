#include "protoscope/ui/ui_theme.hpp"

#include "wave_render_service.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include <imgui_internal.h>

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

    float configuredLegendNameMaxWidth(const plot::WaveViewState& view)
    {
        return std::isfinite(view.legendChannelNameMaxWidth) && view.legendChannelNameMaxWidth > 0.0
                   ? static_cast<float>(view.legendChannelNameMaxWidth)
                   : 0.0F;
    }

    float advanceLegendOverlayProgress(plot::WaveDockState& wave, bool expanded)
    {
        const float target = expanded ? 1.0F : 0.0F;
        if (!(wave.view.interactionAnimationEnabled && wave.view.effectiveInteractionAnimationEnabled)) {
            wave.legendOverlayProgress = target;
            return target;
        }
        const float step = (std::max)(0.0F, ImGui::GetIO().DeltaTime) * 12.0F;
        if (wave.legendOverlayProgress < target) {
            wave.legendOverlayProgress = (std::min)(target, wave.legendOverlayProgress + step);
        } else {
            wave.legendOverlayProgress = (std::max)(target, wave.legendOverlayProgress - step);
        }
        return wave.legendOverlayProgress;
    }

    ImVec2 lerpVec2(const ImVec2& from, const ImVec2& to, float progress)
    {
        return ImVec2(from.x + (to.x - from.x) * progress, from.y + (to.y - from.y) * progress);
    }

    float limitLegendNameWidth(float width, const plot::WaveViewState& view)
    {
        const float configuredMaxWidth = configuredLegendNameMaxWidth(view);
        return configuredMaxWidth > 0.0F ? (std::min)(width, configuredMaxWidth) : width;
    }

    struct LegendRowBlankHitTest {
        ImRect rowRect{};
        bool hasRowRect{false};
        std::vector<ImRect> occupiedRects;
    };

    void extendRowRect(LegendRowBlankHitTest& hitTest, const ImRect& rect)
    {
        if (!hitTest.hasRowRect) {
            hitTest.rowRect = rect;
            hitTest.hasRowRect = true;
            return;
        }
        hitTest.rowRect.Add(rect);
    }

    void recordCurrentTableCell(LegendRowBlankHitTest& hitTest)
    {
        if (const ImGuiTable* table = ImGui::GetCurrentTable()) {
            extendRowRect(hitTest, ImGui::TableGetCellBgRect(table, ImGui::TableGetColumnIndex()));
        }
    }

    void recordLastItem(LegendRowBlankHitTest& hitTest)
    {
        hitTest.occupiedRects.emplace_back(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    }

    bool pointInOccupiedLegendItem(const LegendRowBlankHitTest& hitTest, const ImVec2& point)
    {
        for (const ImRect& rect : hitTest.occupiedRects) {
            if (rect.Contains(point)) {
                return true;
            }
        }
        return false;
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
        if (bitDisplayEnabled(spec.bitDisplay)) {
            return "Bits " + std::to_string(spec.bitDisplay.firstBit) + ".." +
                   std::to_string(spec.bitDisplay.firstBit + spec.bitDisplay.bitCount - 1U) + "  Y offset " +
                   formatMetricText(spec.bitDisplay.yOffset, nullptr);
        }

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
        const ImVec2 titleMax(textMin.x + limitLegendNameWidth(textMax.x - textMin.x, view), textMin.y + lineHeight);
        const ImVec2 summaryMin(textMin.x, titleMax.y + cardStyle.textSpacingY);
        const ImVec2 summaryMax(textMax.x, summaryMin.y + lineHeight);
        drawChannelCardText(textMin, titleMax, spec.label, ImGui::GetColorU32(ImGuiCol_Text));
        const std::string summary = formatChannelDivisionSummary(view, spec);
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
            drawChannelCardTooltip(wave.view, spec, active);
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

    void setChannelHidden(plot::WaveDockState& wave, std::size_t channelIndex, bool hidden)
    {
        auto& hiddenIndices = wave.hiddenChannelIndices;
        const auto existing = std::find(hiddenIndices.begin(), hiddenIndices.end(), channelIndex);
        if (hidden && existing == hiddenIndices.end()) {
            hiddenIndices.push_back(channelIndex);
            wave.hiddenChannelLabels.clear();
            wave.legendVisibilityRestorePending = true;
        } else if (!hidden && existing != hiddenIndices.end()) {
            hiddenIndices.erase(existing);
            wave.hiddenChannelLabels.clear();
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
            if (!spec.has_value()) {
                continue;
            }
            if (!includeHidden && channelHiddenByLegendState(wave, channelIndex)) {
                continue;
            }
            channels.push_back(channelIndex);
        }
        return channels;
    }

    std::string formatActualPerDivText(const plot::WaveViewState& view, const plot::ChannelSpec& spec)
    {
        if (bitDisplayEnabled(spec.bitDisplay)) {
            return "Bits " + std::to_string(spec.bitDisplay.firstBit) + ".." +
                   std::to_string(spec.bitDisplay.firstBit + spec.bitDisplay.bitCount - 1U) + "  Y offset " +
                   formatMetricText(spec.bitDisplay.yOffset, nullptr);
        }

        const double displayPerDiv = plot::waveDisplayValuePerDivision(view.viewMinValue, view.viewMaxValue);
        const auto perDiv = plot::waveChannelValuePerDivision(
            displayPerDiv, spec, view.displayFormula, plot::WaveGridDivisionReadoutMode::ActualValue);
        if (!perDiv.has_value()) {
            return "n/a/div";
        }
        return trimMetricText(formatMetricText(*perDiv, nullptr)) + "/div";
    }

    void drawClippedText(
        const ImVec2& min, const ImVec2& max, const std::string& text, ImU32 color, bool tooltipWhenClipped = true)
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

    bool windowBelongsToLegendOverlay(const ImGuiWindow* window, const ImGuiWindow* legendWindow)
    {
        if (window == nullptr || legendWindow == nullptr) {
            return false;
        }

        const ImGuiWindow* legendRoot =
            legendWindow->RootWindowPopupTree != nullptr ? legendWindow->RootWindowPopupTree : legendWindow;
        return window == legendWindow || window == legendRoot || window->RootWindow == legendRoot ||
               window->RootWindowPopupTree == legendRoot;
    }

    bool legendOverlayPopupOpen(const ImGuiWindow* legendWindow)
    {
        if (legendWindow == nullptr || GImGui == nullptr) {
            return false;
        }

        const ImGuiContext& context = *GImGui;
        for (const ImGuiPopupData& popup : context.OpenPopupStack) {
            // 核心边界：只保护图例 overlay/子窗口打开的 popup，避免其他区域的全局弹窗影响图例收起。
            if (windowBelongsToLegendOverlay(popup.Window, legendWindow) ||
                windowBelongsToLegendOverlay(popup.RestoreNavWindow, legendWindow)) {
                return true;
            }
        }
        return false;
    }

    ImVec2 compactLegendWindowSize(plot::WaveDockState& wave,
                                   const std::vector<std::size_t>& channels,
                                   const ImVec2& plotSize)
    {
        float maxNameWidth = 0.0F;
        float maxDivisionWidth = ImGui::CalcTextSize("n/a/div").x;
        const std::size_t rowCount = (std::min<std::size_t>) (channels.size(), kCompactLegendMaxRows);
        for (std::size_t channelIndex = 0; channelIndex < rowCount; ++channelIndex) {
            const auto spec = wave.buffer.channelSpec(channels[channelIndex]);
            if (!spec.has_value()) {
                continue;
            }
            maxNameWidth =
                (std::max)(maxNameWidth, limitLegendNameWidth(ImGui::CalcTextSize(spec->label.c_str()).x, wave.view));
            maxDivisionWidth =
                (std::max)(maxDivisionWidth, ImGui::CalcTextSize(formatActualPerDivText(wave.view, *spec).c_str()).x);
        }

        const auto& style = ImGui::GetStyle();
        const float swatchAndChannelWidth = 10.0F + style.ItemSpacing.x + ImGui::CalcTextSize("CH99").x;
        const float rightWidth = (std::min)(96.0F, maxDivisionWidth + 10.0F);
        const float maxWindowWidth = (std::max)(64.0F, plotSize.x - 16.0F);
        const float nameRoom = (std::max)(56.0F, maxWindowWidth - rightWidth - swatchAndChannelWidth - 34.0F);
        const float unclampedNameWidth = (std::clamp)(maxNameWidth, 56.0F, nameRoom);
        const float nameWidth = limitLegendNameWidth(unclampedNameWidth, wave.view);
        const float minWindowWidth = (std::min)(220.0F, maxWindowWidth);
        const float width =
            (std::clamp)(swatchAndChannelWidth + nameWidth + rightWidth + 30.0F, minWindowWidth, maxWindowWidth);
        const float rowHeight = ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0F;
        const float effectiveRowCount = static_cast<float>((std::max<std::size_t>) (rowCount, 1U));
        const float rowsHeight = effectiveRowCount * rowHeight;
        const float moreHeight = channels.size() > rowCount ? ImGui::GetTextLineHeightWithSpacing() : 0.0F;
        const float height = (std::min)(plotSize.y - 16.0F, rowsHeight + moreHeight + style.WindowPadding.y * 2.0F);
        return ImVec2(width, (std::max)(44.0F, height));
    }

    void drawCompactLegendEmptyState(plot::WaveDockState& wave, bool hasAnalogChannels)
    {
        ImGui::TextDisabled(hasAnalogChannels ? "所有 CH 已隐藏" : "无 CH");
        if (hasAnalogChannels) {
            ImGui::SameLine();
            if (ImGui::SmallButton("全部恢复") && plot::resetAllChannelViewSettings(wave)) {
                invalidateWaveDisplayCaches(wave);
            }
            ImGui::SetItemTooltip("恢复所有通道的隐藏状态和显示设置；不清空波形数据。");
        }
    }

    void drawCompactLegendRows(plot::WaveDockState& wave,
                               const std::vector<std::size_t>& channels,
                               bool hasAnalogChannels,
                               float windowWidth)
    {
        auto& view = wave.view;
        const auto& style = ImGui::GetStyle();
        const float rowHeight = ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0F;
        const float swatchSize = 9.0F;
        const float channelWidth = ImGui::CalcTextSize("CH99").x + 4.0F;
        const float divisionWidth = 96.0F;
        const float contentWidth = windowWidth - style.WindowPadding.x * 2.0F;
        const float nameWidth = limitLegendNameWidth(
            (std::max)(0.0F, contentWidth - swatchSize - channelWidth - divisionWidth - style.ItemSpacing.x * 4.0F),
            view);
        const std::size_t maxCompactRows = (std::min<std::size_t>) (channels.size(), kCompactLegendMaxRows);

        if (maxCompactRows == 0U) {
            drawCompactLegendEmptyState(wave, hasAnalogChannels);
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
                    setChannelHidden(wave, channelIndex, true);
                }
                ImGui::SetItemTooltip("隐藏当前通道，波形数据仍保留。");
                ImGui::EndPopup();
            }

            auto* drawList = ImGui::GetWindowDrawList();
            const ImVec2 rowMax(rowMin.x + rowSize.x, rowMin.y + rowSize.y);
            const ImVec4 tint = channelColor(*spec, channelIndex);
            const auto& waveTokens = activeWaveStyleTokens();
            if (active || hovered) {
                const ImVec4 fill =
                    active ? (waveTokens.legendOverlayRowActive.w > 0.0F
                                  ? waveTokens.legendOverlayRowActive
                                  : ImVec4(tint.x, tint.y, tint.z, 0.24F))
                           : (waveTokens.legendOverlayRowHover.w > 0.0F
                                  ? waveTokens.legendOverlayRowHover
                                  : ImVec4(1.0F, 1.0F, 1.0F, 0.06F));
                drawList->AddRectFilled(rowMin, rowMax, ImGui::GetColorU32(fill), 4.0F);
            }
            if (active) {
                const ImVec4 border = waveTokens.legendOverlayRowActiveBorder.w > 0.0F
                                          ? waveTokens.legendOverlayRowActiveBorder
                                          : tint;
                drawList->AddRect(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(border), 4.0F, 0, 1.5F);
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

    void setNextLegendNameInputWidth(const plot::WaveViewState& view)
    {
        const float configuredMaxWidth = configuredLegendNameMaxWidth(view);
        if (configuredMaxWidth <= 0.0F) {
            ImGui::SetNextItemWidth(-1.0F);
            return;
        }
        ImGui::SetNextItemWidth((std::min)(configuredMaxWidth, ImGui::GetContentRegionAvail().x));
    }

    void activateLegendRowFromBlankArea(plot::WaveDockState& wave,
                                        const LegendRowBlankHitTest& hitTest,
                                        std::size_t channelIndex)
    {
        const ImVec2 mousePos = ImGui::GetIO().MousePos;
        if (hitTest.hasRowRect && ImGui::IsMouseHoveringRect(hitTest.rowRect.Min, hitTest.rowRect.Max) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !pointInOccupiedLegendItem(hitTest, mousePos)) {
            ImGui::ClearActiveID();
            wave.view.measurementChannelIndex = channelIndex;
        }
    }

    void drawExpandedLegendChannelRow(plot::WaveDockState& wave,
                                      std::size_t channelIndex,
                                      const plot::ChannelSpec& spec)
    {
        ImGui::PushID(static_cast<int>(channelIndex));
        const plot::ChannelSpec defaultSpec = channelDefaultSpec(wave, channelIndex, spec);
        auto updated = spec;
        bool visible = !channelHiddenByLegendState(wave, channelIndex);
        const bool active = channelIndex == wave.view.measurementChannelIndex;
        const bool bitChannel = bitDisplayEnabled(spec.bitDisplay);
        LegendRowBlankHitTest blankHitTest;

        ImGui::TableNextColumn();
        recordCurrentTableCell(blankHitTest);
        if (ImGui::Checkbox("##visible", &visible)) {
            setChannelHidden(wave, channelIndex, !visible);
        }
        ImGui::SetItemTooltip(visible ? "当前通道已显示；取消勾选后隐藏该通道。" : "当前通道已隐藏；勾选后恢复显示。");
        recordLastItem(blankHitTest);

        ImGui::TableNextColumn();
        recordCurrentTableCell(blankHitTest);
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
        recordLastItem(blankHitTest);

        ImGui::TableNextColumn();
        recordCurrentTableCell(blankHitTest);
        if (activeWaveStyleTokens().legendOverlayRowActiveBorder.w > 0.0F) {
            ImGui::TextDisabled("CH%zu", channelIndex + 1U);
        } else {
            ImGui::Text("CH%zu", channelIndex + 1U);
        }
        if (bitChannel) {
            ImGui::SameLine();
            ImGui::TextDisabled(
                "bit %zu..%zu", spec.bitDisplay.firstBit, spec.bitDisplay.firstBit + spec.bitDisplay.bitCount - 1U);
        }
        recordLastItem(blankHitTest);

        ImGui::TableNextColumn();
        recordCurrentTableCell(blankHitTest);
        char labelBuffer[128]{};
        std::snprintf(labelBuffer, sizeof(labelBuffer), "%s", updated.label.c_str());
        setNextLegendNameInputWidth(wave.view);
        if (ImGui::InputText("##label", labelBuffer, sizeof(labelBuffer))) {
            updated.label = labelBuffer;
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        recordLastItem(blankHitTest);
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(updated.label.c_str()).x > ImGui::GetItemRectSize().x) {
            ImGui::SetTooltip("%s", updated.label.c_str());
        }

        ImGui::TableNextColumn();
        recordCurrentTableCell(blankHitTest);
        ImGui::SetNextItemWidth(-1.0F);
        if (bitChannel) {
            ImGui::BeginDisabled();
        }
        if (ImGui::InputDouble("##ratio", &updated.ratio, 0.0, 0.0, "%.4g") && !bitChannel) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        if (bitChannel) {
            ImGui::EndDisabled();
        }
        recordLastItem(blankHitTest);

        ImGui::TableNextColumn();
        recordCurrentTableCell(blankHitTest);
        ImGui::SetNextItemWidth(-1.0F);
        if (bitChannel) {
            ImGui::BeginDisabled();
        }
        if (drawChannelActualValuePerDivisionEditor("##scale", wave.view, updated, "%.4g") && !bitChannel) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        if (bitChannel) {
            ImGui::EndDisabled();
        }
        recordLastItem(blankHitTest);

        ImGui::TableNextColumn();
        recordCurrentTableCell(blankHitTest);
        ImGui::SetNextItemWidth(-1.0F);
        double& offsetValue = bitChannel ? updated.bitDisplay.yOffset : updated.offset;
        if (ImGui::InputDouble("##offset", &offsetValue, 0.0, 0.0, "%.4g")) {
            applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
        }
        recordLastItem(blankHitTest);

        ImGui::TableNextColumn();
        recordCurrentTableCell(blankHitTest);
        if (active) {
            ImGui::TextDisabled("激活");
            recordLastItem(blankHitTest);
            ImGui::SameLine();
        }
        const std::string visibilityButtonLabel = waveChannelItemLabel(visible ? "隐藏" : "显示", channelIndex);
        if (ImGui::SmallButton(visibilityButtonLabel.c_str())) {
            setChannelHidden(wave, channelIndex, visible);
        }
        ImGui::SetItemTooltip(visible ? "隐藏该通道；波形数据仍保留。" : "恢复显示该通道。");
        recordLastItem(blankHitTest);
        ImGui::SameLine();
        const std::string resetButtonLabel = waveChannelItemLabel("恢复", channelIndex);
        if (ImGui::SmallButton(resetButtonLabel.c_str())) {
            if (plot::resetOneChannelViewSettings(wave, channelIndex)) {
                invalidateWaveDisplayCaches(wave);
            }
        }
        ImGui::SetItemTooltip("恢复该通道的颜色、名称、比例和偏移；不清空波形数据。");
        recordLastItem(blankHitTest);
        const auto& waveTokens = activeWaveStyleTokens();
        const bool hovered = blankHitTest.hasRowRect &&
                             ImGui::IsMouseHoveringRect(blankHitTest.rowRect.Min, blankHitTest.rowRect.Max);
        if (active) {
            const ImVec4 fill = waveTokens.legendOverlayRowActive.w > 0.0F
                                    ? waveTokens.legendOverlayRowActive
                                    : ImVec4(0.20F, 0.38F, 0.22F, 0.42F);
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ImGui::GetColorU32(fill));
            if (waveTokens.legendOverlayRowActiveBorder.w > 0.0F && blankHitTest.hasRowRect) {
                ImGui::GetWindowDrawList()->AddRect(blankHitTest.rowRect.Min,
                                                    blankHitTest.rowRect.Max,
                                                    ImGui::GetColorU32(waveTokens.legendOverlayRowActiveBorder),
                                                    3.0F,
                                                    0,
                                                    1.5F);
            }
        } else if (hovered && waveTokens.legendOverlayRowHover.w > 0.0F) {
            ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                   ImGui::GetColorU32(waveTokens.legendOverlayRowHover));
        }
        activateLegendRowFromBlankArea(wave, blankHitTest, channelIndex);
        ImGui::PopID();
    }

    void drawExpandedLegendRows(plot::WaveDockState& wave, const std::vector<std::size_t>& channels)
    {
        if (channels.empty()) {
            ImGui::TextDisabled("没有 CH 可显示");
            return;
        }

        if (ImGui::BeginChild("##wave_legend_overlay_rows", ImVec2(0.0F, 0.0F), false, ImGuiWindowFlags_None)) {
            const ImGuiTableFlags tableFlags =
                ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##wave_legend_overlay_table", 8, tableFlags)) {
                ImGui::TableSetupColumn("显示##wave_legend_visible_column", ImGuiTableColumnFlags_WidthFixed, 38.0F);
                ImGui::TableSetupColumn("颜色", ImGuiTableColumnFlags_WidthFixed, 90.0F);
                ImGui::TableSetupColumn("通道", ImGuiTableColumnFlags_WidthFixed, 42.0F);
                const float nameMaxWidth = configuredLegendNameMaxWidth(wave.view);
                if (nameMaxWidth > 0.0F) {
                    ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthFixed, nameMaxWidth);
                } else {
                    ImGui::TableSetupColumn("名称", ImGuiTableColumnFlags_WidthStretch, 120.0F);
                }
                ImGui::TableSetupColumn("Ratio", ImGuiTableColumnFlags_WidthFixed, 68.0F);
                ImGui::TableSetupColumn("实际值/格", ImGuiTableColumnFlags_WidthFixed, 68.0F);
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
        const float visibleRows =
            static_cast<float>((std::min<std::size_t>) (channelCount, kExpandedLegendMaxVisibleRows));
        const float rowHeight = ImGui::GetFrameHeightWithSpacing();
        const float tableHeight =
            rowHeight * (visibleRows + 1.0F) +
            (channelCount > kExpandedLegendMaxVisibleRows ? ImGui::GetStyle().ScrollbarSize : 0.0F);
        const float chromeHeight = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() +
                                   ImGui::GetStyle().WindowPadding.y * 2.0F + 8.0F;
        const float wantedHeight = chromeHeight + (channelCount == 0U ? rowHeight : tableHeight);
        const float minHeight = (std::min)(110.0F, maxHeight);
        const float height = (std::clamp)(wantedHeight, minHeight, maxHeight);
        return ImVec2(width, height);
    }

    void resetLegendOverlayTransient(plot::WaveLegendOverlayState& overlay)
    {
        overlay.hoverFloating = false;
        overlay.hoverInteractionLocked = false;
        overlay.hoverCloseRemainingSec = 0.0F;
    }

    void collapseLegendOverlay(plot::WaveLegendOverlayState& overlay)
    {
        overlay.expanded = false;
        resetLegendOverlayTransient(overlay);
    }

    bool normalizeLegendOverlayModeState(plot::WaveDockState& wave)
    {
        if (wave.legendOverlay.openMode == plot::WaveLegendOverlayOpenMode::Disabled) {
            collapseLegendOverlay(wave.legendOverlay);
            return false;
        }

        // 固定展开只属于双击模式；悬浮模式每帧都回到瞬态展开语义。
        if (wave.legendOverlay.openMode == plot::WaveLegendOverlayOpenMode::Hover) {
            wave.legendOverlay.expanded = false;
        } else if (!wave.legendOverlay.doubleClickAutoCollapse) {
            resetLegendOverlayTransient(wave.legendOverlay);
        }
        return true;
    }

    bool drawExpandedLegendHeader(plot::WaveDockState& wave)
    {
        bool collapseRequested = false;
        ImGui::TextUnformatted("通道图例");
        ImGui::SameLine();
        if (ImGui::SmallButton("全部恢复")) {
            if (plot::resetAllChannelViewSettings(wave)) {
                invalidateWaveDisplayCaches(wave);
            }
        }
        ImGui::SetItemTooltip("恢复所有通道的隐藏状态和显示设置；不清空波形数据。");
        ImGui::SameLine();
        if (ImGui::SmallButton("收起")) {
            collapseLegendOverlay(wave.legendOverlay);
            collapseRequested = true;
        }
        ImGui::SetItemTooltip("收起图内通道图例，保留当前展开方式设置。");
        ImGui::Separator();
        return collapseRequested;
    }

    void updateLegendOverlayInteractionState(
        plot::WaveDockState& wave, bool legendPopupOpen, bool hovered, bool mouseInActualWindow, bool collapseRequested)
    {
        auto& overlay = wave.legendOverlay;
        // 实际窗口矩形只用于保持已展开交互，不作为新的悬浮触发源。
        const bool previousInteractionLocked = overlay.hoverInteractionLocked;
        const bool mouseHeldInLegend =
            (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) &&
            (hovered || legendPopupOpen || previousInteractionLocked || (overlay.hoverFloating && mouseInActualWindow));
        const bool activeLegendInteraction =
            ImGui::IsAnyItemActive() &&
            (hovered || legendPopupOpen || previousInteractionLocked || (overlay.hoverFloating && mouseInActualWindow));
        overlay.hoverInteractionLocked = legendPopupOpen || mouseHeldInLegend || activeLegendInteraction;

        if (overlay.openMode == plot::WaveLegendOverlayOpenMode::Hover) {
            if (collapseRequested) {
                resetLegendOverlayTransient(overlay);
            } else if (hovered || overlay.hoverInteractionLocked) {
                overlay.hoverFloating = true;
                overlay.hoverCloseRemainingSec = overlay.hoverCloseDelaySec;
            } else if (!overlay.expanded && overlay.hoverFloating) {
                overlay.hoverCloseRemainingSec -= ImGui::GetIO().DeltaTime;
                if (overlay.hoverCloseRemainingSec <= 0.0F) {
                    overlay.hoverFloating = false;
                }
            }
        } else if (overlay.doubleClickAutoCollapse) {
            overlay.hoverFloating = false;
            if (collapseRequested || !overlay.expanded) {
                overlay.hoverInteractionLocked = false;
                overlay.hoverCloseRemainingSec = 0.0F;
            } else if (hovered || mouseInActualWindow || overlay.hoverInteractionLocked) {
                // 核心流程：双击展开后只在离开图例且没有拖动/输入交互时开始倒计时收起。
                overlay.hoverCloseRemainingSec = overlay.hoverCloseDelaySec;
            } else {
                overlay.hoverCloseRemainingSec -= ImGui::GetIO().DeltaTime;
                if (overlay.hoverCloseRemainingSec <= 0.0F) {
                    collapseLegendOverlay(overlay);
                }
            }
        } else {
            resetLegendOverlayTransient(overlay);
        }
    }

} // namespace

void drawChannelLegendOverlay(plot::WaveDockState& wave,
                              const plot::WaveSnapshot& snapshot,
                              const ImVec2& plotPos,
                              const ImVec2& plotSize,
                              ImGuiViewport* hostViewport,
                              WaveLegendOverlayLayerPolicy layerPolicy)
{
    if (!normalizeLegendOverlayModeState(wave)) {
        return;
    }

    if (snapshot.channels.empty() || !wave.view.showChannelLegend) {
        return;
    }

    if (plotSize.x <= 80.0F || plotSize.y <= 60.0F) {
        return;
    }

    const std::vector<std::size_t> compactChannels = collectLegendAnalogChannels(wave, snapshot, false);
    const std::vector<std::size_t> expandedChannels = collectLegendAnalogChannels(wave, snapshot, true);
    const bool hasAnalogChannels = !expandedChannels.empty();
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
    const float overlayProgress = advanceLegendOverlayProgress(wave, effectiveExpanded);
    const ImVec2 windowSize = lerpVec2(compactSize, expandedSize, overlayProgress);
    const ImVec2 windowPos = lerpVec2(compactPos, expandedPos, overlayProgress);

    if (hostViewport != nullptr) {
        ImGui::SetNextWindowViewport(hostViewport->ID);
    }
    ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(windowSize, ImGuiCond_Always);
    const float animatedBgAlpha = 0.78F + (0.96F - 0.78F) * overlayProgress;
    const auto& waveTokens = activeWaveStyleTokens();
    const float bgAlpha =
        waveTokens.legendOverlayBackground.w < 1.0F ? waveTokens.legendOverlayBackground.w : animatedBgAlpha;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0F, 0.0F, 0.0F, 0.0F));
    ImGui::PushStyleColor(ImGuiCol_Text, waveTokens.legendOverlayTextPrimary);
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, waveTokens.legendOverlayTextSecondary);
    ImGui::PushStyleColor(
        ImGuiCol_ChildBg,
        waveTokens.legendOverlayRowActiveBorder.w > 0.0F ? ImVec4(0.0F, 0.0F, 0.0F, 0.0F)
                                                        : ImGui::GetStyleColorVec4(ImGuiCol_ChildBg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    if (ImGui::Begin("##wave_channel_overlay_legend", nullptr, flags)) {
        ImGuiWindow* legendWindow = ImGui::GetCurrentWindow();
        const ImVec2 legendMin = ImGui::GetWindowPos();
        const ImVec2 legendMax(legendMin.x + ImGui::GetWindowWidth(), legendMin.y + ImGui::GetWindowHeight());
        auto* drawList = ImGui::GetWindowDrawList();
        // 图例背景独立于通用 WindowBg 绘制，避免主题切换后继承灰白窗口底色。
        drawList->AddRectFilled(
            legendMin,
            legendMax,
            ImGui::GetColorU32(ImVec4(waveTokens.legendOverlayBackground.x,
                                      waveTokens.legendOverlayBackground.y,
                                      waveTokens.legendOverlayBackground.z,
                                      bgAlpha)),
            ImGui::GetStyle().WindowRounding);
        bool legendPopupOpen = legendOverlayPopupOpen(legendWindow);
        if (layerPolicy == WaveLegendOverlayLayerPolicy::ForceDisplayFront && !legendPopupOpen) {
            ImGui::BringWindowToDisplayFront(legendWindow);
        }
        ImGui::SetWindowFontScale(kLegendOverlayFontScale);
        const bool hovered =
            ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem | ImGuiHoveredFlags_ChildWindows);
        const ImVec2 actualWindowMin = ImGui::GetWindowPos();
        const ImVec2 actualWindowMax(actualWindowMin.x + ImGui::GetWindowWidth(),
                                     actualWindowMin.y + ImGui::GetWindowHeight());
        const bool mouseInActualWindow = ImGui::IsMouseHoveringRect(actualWindowMin, actualWindowMax, false);
        const bool collapseRequested = effectiveExpanded && drawExpandedLegendHeader(wave);

        if (effectiveExpanded) {
            drawExpandedLegendRows(wave, expandedChannels);
        } else {
            drawCompactLegendRows(wave, compactChannels, hasAnalogChannels, windowSize.x);
        }
        legendPopupOpen = legendPopupOpen || legendOverlayPopupOpen(legendWindow);

        if (wave.legendOverlay.openMode == plot::WaveLegendOverlayOpenMode::DoubleClick && !effectiveExpanded &&
            hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            wave.legendOverlay.expanded = true;
            wave.legendOverlay.hoverCloseRemainingSec = wave.legendOverlay.hoverCloseDelaySec;
        }

        updateLegendOverlayInteractionState(wave, legendPopupOpen, hovered, mouseInActualWindow, collapseRequested);
        drawList->AddRect(legendMin,
                          legendMax,
                          ImGui::GetColorU32(waveTokens.legendOverlayBorder),
                          ImGui::GetStyle().WindowRounding,
                          0,
                          1.0F);
    }
    ImGui::End();
    ImGui::PopStyleColor(5);
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
