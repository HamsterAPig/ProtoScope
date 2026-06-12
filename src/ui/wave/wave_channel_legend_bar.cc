#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
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
        ImGui::Text("图例 / 吸附范围：%s", snapScopeName(view.cursorSnapScope));
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
            plot::resetChannelConfigToDefault(wave, channelIndex, defaultSpec, view.channelDoubleClickAction);
        }
        return active;
    }

    void drawChannelLegendCardBody(const ImVec2& cardMin,
                                   const ImVec2& cardSize,
                                   const ChannelLegendCardStyle& cardStyle,
                                   const plot::ChannelSpec& spec,
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
        std::string summary = "R " + formatMetricText(spec.ratio, nullptr) + "  S " +
                              formatMetricText(spec.scale, nullptr) + "  O " + formatMetricText(spec.offset, nullptr);
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
        drawChannelLegendCardBody(cardMin, cardSize, cardStyle, spec, channelIndex, active);
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
