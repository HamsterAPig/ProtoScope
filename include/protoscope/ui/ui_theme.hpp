#pragma once

#include "protoscope/config/config.hpp"

#include <imgui.h>

namespace protoscope::ui {

struct UiStyleTokens {
    ImVec4 appBackground;
    ImVec4 panelBackground;
    ImVec4 panelBackgroundAlt;
    ImVec4 panelBorder;
    ImVec4 accent;
    ImVec4 accentMuted;
    ImVec4 success;
    ImVec4 warning;
    ImVec4 danger;
    ImVec4 textStrong;
    ImVec4 textMuted;
    ImVec4 genericPlotBackground;
    float windowRounding{10.0F};
    float frameRounding{8.0F};
    float grabRounding{8.0F};
    float tabRounding{8.0F};
    float itemSpacingX{10.0F};
    float itemSpacingY{8.0F};
    float windowPaddingX{12.0F};
    float windowPaddingY{10.0F};
    float framePaddingX{10.0F};
    float framePaddingY{7.0F};
};

struct WaveStyleTokens {
    ImVec4 plotBackground;
    ImVec4 gridMajor;
    ImVec4 gridMinorTick;
    ImVec4 gridCenter;
    ImVec4 statusOverlayBackground;
    ImVec4 statusOverlayBorder;
    ImVec4 statusOverlayText;
    ImVec4 channelSeparator;
    ImVec4 channelLabel;
    ImVec4 splitChannelLabel;
    ImVec4 bitLabel;
    ImVec4 legendOverlayBackground;
    ImVec4 legendOverlayBorder;
    ImVec4 legendOverlayTextPrimary;
    ImVec4 legendOverlayTextSecondary;
    ImVec4 legendOverlayRowHover;
    ImVec4 legendOverlayRowActive;
    ImVec4 legendOverlayRowActiveBorder;
    ImVec4 measurementOverlayBackground;
    ImVec4 measurementOverlayBorder;
    ImVec4 measurementOverlayAccent;
    ImVec4 measurementOverlayTitle;
    ImVec4 measurementChipBackground;
    ImVec4 measurementChipBorder;
    ImVec4 measurementChipLabel;
    ImVec4 measurementChipValue;
    float gridMajorWidth{1.0F};
    float gridMinorTickWidth{1.0F};
    float gridCenterWidth{1.4F};
    float gridMinorTickHalfLength{2.0F};
};

struct UiThemeDefinition {
    config::GuiTheme theme{config::GuiTheme::ProfessionalDark};
    UiStyleTokens ui{};
    WaveStyleTokens wave{};
};

const UiThemeDefinition& uiThemeDefinition(config::GuiTheme theme);
const UiStyleTokens& activeUiStyleTokens();
const WaveStyleTokens& activeWaveStyleTokens();
void applyUiTheme(config::GuiTheme theme);

// 兼容既有调用；返回当前活动主题令牌。
const UiStyleTokens& defaultUiStyleTokens();
void applyImGuiProfessionalDarkTheme();
void applyImPlotProfessionalDarkTheme();

// beginToolbarGroup() 内部会压入 ImGui 栈；无论返回值是否为 true，后续都必须调用 endToolbarGroup()。
// 传入的 minHeight 为 0 时，ImGui::BeginChild() 会让子窗口占满当前剩余可用高度，而不是按内容自适应。
// title 为空时仅绘制容器背景和边框，不绘制内部标题与分割线，并使用更紧凑的子窗口纵向留白。
bool beginToolbarGroup(const char* id, const char* title, float minHeight = 0.0F);
void endToolbarGroup();
bool drawToolbarSectionButton(const char* label,
                              const char* tooltip,
                              bool active = false,
                              const ImVec2& size = ImVec2(0.0F, 0.0F));
void drawHeaderBadge(const char* label, const ImVec4& color, bool filled = false);
bool drawDangerIconButton(const char* label, const char* tooltip);
bool drawGhostIconButton(const char* label, const char* tooltip);

} // namespace protoscope::ui
