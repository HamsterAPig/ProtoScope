#pragma once

#include <imgui.h>

namespace protoscope::ui {

inline void drawDockHelpMarker(const char* text)
{
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::TextUnformatted(text);
        ImGui::EndTooltip();
    }
}

} // namespace protoscope::ui
