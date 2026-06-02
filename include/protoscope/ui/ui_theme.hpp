#pragma once

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

const UiStyleTokens& defaultUiStyleTokens();
void applyImGuiProfessionalDarkTheme();
void applyImPlotProfessionalDarkTheme();

// beginToolbarGroup() 内部会压入 ImGui 栈；无论返回值是否为 true，后续都必须调用 endToolbarGroup()。
// 传入的 minHeight 为 0 时，ImGui::BeginChild() 会让子窗口占满当前剩余可用高度，而不是按内容自适应。
bool beginToolbarGroup(const char* id, const char* title, float minHeight = 0.0F);
void endToolbarGroup();
bool drawToolbarSectionButton(const char* label, const char* tooltip, bool active = false, const ImVec2& size = ImVec2(0.0F, 0.0F));
void drawHeaderBadge(const char* label, const ImVec4& color, bool filled = false);
bool drawDangerIconButton(const char* label, const char* tooltip);
bool drawGhostIconButton(const char* label, const char* tooltip);

} // namespace protoscope::ui
