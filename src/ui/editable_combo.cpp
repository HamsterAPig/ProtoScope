#include "protoscope/ui/editable_combo.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>

namespace protoscope::ui {

namespace {

constexpr std::size_t kEditableComboBufferSize = 512;

bool beginEditableComboPopup(const char* label, const char* previewValue, bool& enterEditMode) {
    ImGuiContext& context = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();

    ImGuiNextWindowDataFlags backupNextWindowDataFlags = context.NextWindowData.HasFlags;
    context.NextWindowData.ClearFlags();
    if (window->SkipItems) {
        return false;
    }

    const ImGuiStyle& style = context.Style;
    const ImGuiID id = window->GetID(label);
    const ImGuiID popupId = ImHashStr("##ComboPopup", 0, id);
    const float arrowSize = ImGui::GetFrameHeight();
    const char* labelEnd = ImGui::FindRenderedTextEnd(label);
    const ImVec2 labelSize = ImGui::CalcTextSize(label, labelEnd, false);
    const float width = ImGui::CalcItemWidth();
    const ImRect bb(
        window->DC.CursorPos,
        ImVec2(window->DC.CursorPos.x + width, window->DC.CursorPos.y + labelSize.y + style.FramePadding.y * 2.0f));
    const ImRect totalBb(
        bb.Min,
        ImVec2(bb.Max.x + (labelSize.x > 0.0f ? style.ItemInnerSpacing.x + labelSize.x : 0.0f), bb.Max.y));
    ImGui::ItemSize(totalBb, style.FramePadding.y);
    if (!ImGui::ItemAdd(totalBb, id, &bb)) {
        return false;
    }

    bool hovered = false;
    bool held = false;
    const ImGuiButtonFlags buttonFlags = ImGuiButtonFlags_PressedOnClickRelease | ImGuiButtonFlags_PressedOnDoubleClick;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, buttonFlags);
    bool popupOpen = ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None);

    // 先在 popup 打开前截获双击；双击进入编辑态，单击仍保持原有下拉行为。
    enterEditMode = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
    if (enterEditMode) {
        if (popupOpen) {
            ImGui::ClosePopupToLevel(context.BeginPopupStack.Size, true);
            popupOpen = false;
        }
    } else if (pressed && !popupOpen) {
        ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
        popupOpen = true;
    }

    const ImU32 frameColor = ImGui::GetColorU32(hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
    const float valueX2 = (ImMax)(bb.Min.x, bb.Max.x - arrowSize);
    ImGui::RenderNavCursor(bb, id);
    window->DrawList->AddRectFilled(
        bb.Min,
        ImVec2(valueX2, bb.Max.y),
        frameColor,
        style.FrameRounding,
        ImDrawFlags_RoundCornersLeft);

    const ImU32 buttonColor = ImGui::GetColorU32((popupOpen || hovered) ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    const ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
    window->DrawList->AddRectFilled(
        ImVec2(valueX2, bb.Min.y),
        bb.Max,
        buttonColor,
        style.FrameRounding,
        (width <= arrowSize) ? ImDrawFlags_RoundCornersAll : ImDrawFlags_RoundCornersRight);
    if (valueX2 + arrowSize - style.FramePadding.x <= bb.Max.x) {
        ImGui::RenderArrow(
            window->DrawList,
            ImVec2(valueX2 + style.FramePadding.y, bb.Min.y + style.FramePadding.y),
            textColor,
            ImGuiDir_Down,
            1.0f);
    }
    ImGui::RenderFrameBorder(bb.Min, bb.Max, style.FrameRounding);

    if (previewValue != nullptr) {
        if (context.LogEnabled) {
            ImGui::LogSetNextTextDecoration("{", "}");
        }
        ImGui::RenderTextClipped(
            ImVec2(bb.Min.x + style.FramePadding.x, bb.Min.y + style.FramePadding.y),
            ImVec2(valueX2, bb.Max.y),
            previewValue,
            nullptr,
            nullptr);
    }
    if (labelSize.x > 0.0f) {
        ImGui::RenderText(ImVec2(bb.Max.x + style.ItemInnerSpacing.x, bb.Min.y + style.FramePadding.y), label, labelEnd, false);
    }

    if (!popupOpen) {
        return false;
    }

    context.NextWindowData.HasFlags = backupNextWindowDataFlags;
    return ImGui::BeginComboPopup(popupId, bb, ImGuiComboFlags_None);
}

} // namespace

EditableComboResult drawEditableCombo(const char* label,
                                      std::string& draft,
                                      const std::vector<std::string>& options,
                                      const std::function<bool(const std::string&)>& validator) {
    EditableComboResult result;
    result.value = draft;

    ImGui::PushID(label);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID editingKey = ImGui::GetID("editing");
    const ImGuiID focusKey = ImGui::GetID("focus-on-edit");
    const bool editing = storage->GetBool(editingKey, false);

    if (editing) {
        char buffer[kEditableComboBufferSize]{};
        std::snprintf(buffer, sizeof(buffer), "%s", draft.c_str());

        if (storage->GetBool(focusKey, false)) {
            ImGui::SetKeyboardFocusHere();
            storage->SetBool(focusKey, false);
        }

        // 编辑态复用同一个 label，让输入框占用原 Combo 的位置与宽度。
        if (ImGui::InputText(label, buffer, sizeof(buffer))) {
            draft = buffer;
            result.edited = true;
            result.value = draft;
        }

        if (ImGui::IsItemDeactivated()) {
            storage->SetBool(editingKey, false);
        }
    } else {
        bool enterEditMode = false;
        const char* preview = draft.empty() ? "" : draft.c_str();
        const bool popupOpened = beginEditableComboPopup(label, preview, enterEditMode);
        if (enterEditMode) {
            storage->SetBool(editingKey, true);
            storage->SetBool(focusKey, true);
        } else if (popupOpened) {
            for (const std::string& option : options) {
                const bool selected = draft == option;
                if (ImGui::Selectable(option.c_str(), selected)) {
                    draft = option;
                    result.selectedFromList = true;
                    result.edited = true;
                    result.value = draft;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::PopID();

    if (validator) {
        result.valid = validator(draft);
    }
    result.value = draft;
    return result;
}

} // namespace protoscope::ui
