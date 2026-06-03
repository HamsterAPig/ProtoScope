#include "wave_render_service.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace protoscope::ui {

ChannelLegendMetrics measureChannelLegendMetrics(float availableWidth, const plot::WaveViewState& view) {
    const auto& style = ImGui::GetStyle();
    const float titleHeight = ImGui::GetTextLineHeightWithSpacing();
    const float lineHeight = ImGui::GetTextLineHeight();
    const float cardWidth = static_cast<float>(
        plot::resolveChannelCardWidth(view.channelCardWidthMode, view.channelCardFixedWidth, view.channelCardAdaptiveRatio, availableWidth));
    const float cardHeight = 18.0F + lineHeight * 2.0F;
    const float stripHeight = cardHeight + style.FramePadding.y * 2.0F + style.ScrollbarSize + 6.0F;
    const float separatorHeight = style.ItemSpacing.y + 1.0F + style.ItemSpacing.y;
    return {
        .cardWidth = cardWidth,
        .cardHeight = cardHeight,
        .stripHeight = stripHeight,
        .totalHeight = titleHeight + separatorHeight + stripHeight,
    };
}

void drawChannelCardText(const ImVec2& min, const ImVec2& max, const std::string& text, ImU32 color) {
    auto* drawList = ImGui::GetWindowDrawList();
    drawList->PushClipRect(min, max, true);
    drawList->AddText(min, color, text.c_str());
    drawList->PopClipRect();
}

void drawChannelCardTooltip(const plot::ChannelSpec& spec, bool active) {
    ImGui::BeginTooltip();
    ImGui::TextUnformatted(spec.label.c_str());
    ImGui::Text("单位：%s", spec.unit.empty() ? "-" : spec.unit.c_str());
    ImGui::Text("Ratio：%.6g", spec.ratio);
    ImGui::Text("Scale：%.6g", spec.scale);
    ImGui::Text("Offset：%.6g", spec.offset);
    ImGui::TextUnformatted(active ? "状态：激活" : "状态：未激活");
    ImGui::EndTooltip();
}

void drawChannelLegendPopup(plot::WaveDockState& wave,
                            std::size_t channelIndex,
                            const plot::ChannelSpec& spec,
                            bool active) {
    const plot::ChannelSpec defaultSpec = channelDefaultSpec(wave, channelIndex, spec);
    auto updated = spec;
    ImGui::Text("%s", spec.label.c_str());
    if (!spec.unit.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("[%s]", spec.unit.c_str());
    }

    char labelBuffer[128]{};
    std::snprintf(labelBuffer, sizeof(labelBuffer), "%s", updated.label.c_str());
    if (ImGui::InputText("标签", labelBuffer, sizeof(labelBuffer))) {
        updated.label = labelBuffer;
        applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
    }

    ImGui::TextDisabled(active ? "当前激活通道" : "非激活通道");
    ImGui::Separator();
    if (ImGui::InputDouble("比率", &updated.ratio, 0.01, 0.1, "%.6g")) {
        applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
    }
    if (ImGui::InputDouble("缩放", &updated.scale, 0.1, 1.0, "%.6g")) {
        applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
    }
    if (ImGui::InputDouble("偏移", &updated.offset, 0.1, 1.0, "%.6g")) {
        applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
    }
    if (ImGui::Button(active ? "激活中" : "设为激活")) {
        wave.view.measurementChannelIndex = channelIndex;
    }
    if (ImGui::Button("恢复默认标签")) {
        updated.label = defaultSpec.label;
        applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
    }
    if (ImGui::Button("恢复默认变换")) {
        updated.ratio = defaultSpec.ratio;
        updated.scale = defaultSpec.scale;
        updated.offset = defaultSpec.offset;
        applyChannelTransformOverride(wave, channelIndex, updated, defaultSpec);
    }
    if (ImGui::Button("恢复全部默认")) {
        applyChannelTransformOverride(wave, channelIndex, defaultSpec, defaultSpec);
    }
}

double offsetParameterDeltaFromDisplayDelta(const plot::ChannelSpec& spec,
                                            plot::WaveDisplayFormula formula,
                                            double displayDelta) {
    if (formula == plot::WaveDisplayFormula::ScaleThenOffset) {
        return displayDelta;
    }
    if (std::abs(spec.scale) <= 1e-12) {
        return 0.0;
    }
    return displayDelta / spec.scale;
}

} // namespace protoscope::ui
