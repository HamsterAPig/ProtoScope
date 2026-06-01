#include "protoscope/ui/editable_combo.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <cstdio>

namespace protoscope::ui {

namespace {

constexpr std::size_t kEditableComboBufferSize = 512;

void clearPendingOpenState(ImGuiStorage* storage, ImGuiID pendingOpenKey, ImGuiID pendingOpenTimeKey) {
    storage->SetBool(pendingOpenKey, false);
    storage->SetInt(pendingOpenTimeKey, 0);
}

bool beginEditableComboPopup(const char* label, const char* previewValue, bool& enterEditMode) {
    ImGuiContext& context = *GImGui;
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = window->GetID(label);
    const ImGuiID popupId = ImHashStr("##ComboPopup", 0, id);
    const ImGuiID pendingOpenKey = window->GetID("pending-open");
    const ImGuiID pendingOpenTimeKey = window->GetID("pending-open-time-ms");

    ImGuiNextWindowDataFlags backupNextWindowDataFlags = context.NextWindowData.HasFlags;
    context.NextWindowData.ClearFlags();
    if (window->SkipItems) {
        clearPendingOpenState(storage, pendingOpenKey, pendingOpenTimeKey);
        return false;
    }

    const ImGuiStyle& style = context.Style;
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
        clearPendingOpenState(storage, pendingOpenKey, pendingOpenTimeKey);
        return false;
    }

    bool hovered = false;
    bool held = false;
    const ImGuiButtonFlags buttonFlags = ImGuiButtonFlags_PressedOnClickRelease | ImGuiButtonFlags_PressedOnDoubleClick;
    const bool pressed = ImGui::ButtonBehavior(bb, id, &hovered, &held, buttonFlags);
    bool popupOpen = ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None);
    const bool pendingOpen = storage->GetBool(pendingOpenKey, false);
    const int pendingOpenTimeMs = storage->GetInt(pendingOpenTimeKey, 0);
    const int nowMs = static_cast<int>(ImGui::GetTime() * 1000.0);
    const int doubleClickTimeMs = static_cast<int>(ImGui::GetIO().MouseDoubleClickTime * 1000.0f);
    const bool doubleClicked = pressed && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left, id);

    // 同一区域同时支持“单击展开”和“双击编辑”时，先挂起单击，等双击窗口结束后再决定是否展开。
    enterEditMode = doubleClicked;
    if (popupOpen) {
        clearPendingOpenState(storage, pendingOpenKey, pendingOpenTimeKey);
    } else if (enterEditMode) {
        clearPendingOpenState(storage, pendingOpenKey, pendingOpenTimeKey);
    } else if (pendingOpen && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered) {
        clearPendingOpenState(storage, pendingOpenKey, pendingOpenTimeKey);
    } else if (pressed) {
        storage->SetBool(pendingOpenKey, true);
        storage->SetInt(pendingOpenTimeKey, nowMs);
    } else if (pendingOpen && nowMs - pendingOpenTimeMs >= doubleClickTimeMs) {
        ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
        popupOpen = true;
        clearPendingOpenState(storage, pendingOpenKey, pendingOpenTimeKey);
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
    return drawEditableCombo(label, draft, options, EditableComboOptions{}, validator);
}

EditableComboResult drawEditableCombo(const char* label,
                                      std::string& draft,
                                      const std::vector<std::string>& options,
                                      const EditableComboOptions& comboOptions,
                                      const std::function<bool(const std::string&)>& validator) {
    EditableComboResult result;
    result.value = draft;

    ImGui::PushID(label);
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID editingKey = ImGui::GetID("editing");
    const ImGuiID focusKey = ImGui::GetID("focus-on-edit");
    const ImGuiID suppressFirstDeactivateKey = ImGui::GetID("suppress-first-deactivate");
    const ImGuiID pendingOpenKey = ImGui::GetID("pending-open");
    const ImGuiID pendingOpenTimeKey = ImGui::GetID("pending-open-time-ms");
    const bool editing = storage->GetBool(editingKey, false);

    if (editing) {
        clearPendingOpenState(storage, pendingOpenKey, pendingOpenTimeKey);
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
        const bool inputActive = ImGui::IsItemActive();

        const ImGuiID id = ImGui::GetCurrentWindow()->GetID(label);
        const ImGuiID popupId = ImHashStr("##ComboPopup", 0, id);
        const ImRect inputBb(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());

        if (ImGui::IsItemDeactivated()) {
            // 双击第二次抬起会落在输入框刚创建的第一帧，这一帧只吞掉失焦事件，不立刻退出编辑态。
            if (storage->GetBool(suppressFirstDeactivateKey, false)) {
                storage->SetBool(suppressFirstDeactivateKey, false);
            } else {
                storage->SetBool(editingKey, false);
            }
        }

        if (comboOptions.keepPopupOpenWhileEditing && storage->GetBool(editingKey, false)) {
            // 核心流程：编辑态实时筛选时持续保持候选弹层打开，后端刷新 options 后下一帧直接呈现。
            // 仅在未打开时触发 OpenPopup，避免无意义地重复打断当前焦点状态。
            if (!ImGui::IsPopupOpen(popupId, ImGuiPopupFlags_None)) {
                ImGui::OpenPopupEx(popupId, ImGuiPopupFlags_None);
            }
            if (ImGui::BeginComboPopup(popupId, inputBb, ImGuiComboFlags_None)) {
                for (const std::string& option : options) {
                    const bool selected = draft == option;
                    if (ImGui::Selectable(option.c_str(), selected)) {
                        draft = option;
                        result.selectedFromList = true;
                        result.edited = true;
                        result.value = draft;
                        storage->SetBool(editingKey, false);
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::EndCombo();
            }
            if (storage->GetBool(editingKey, false)
                && !inputActive
                && !ImGui::IsAnyItemActive()
                && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                // 核心流程：弹层保持展开时，若输入框意外失去激活且用户当前未在操作其他项，则下一帧自动回焦到输入框。
                storage->SetBool(focusKey, true);
            }
        }
    } else {
        bool enterEditMode = false;
        const char* preview = draft.empty() ? "" : draft.c_str();
        const bool popupOpened = beginEditableComboPopup(label, preview, enterEditMode);
        if (enterEditMode) {
            clearPendingOpenState(storage, pendingOpenKey, pendingOpenTimeKey);
            storage->SetBool(editingKey, true);
            storage->SetBool(focusKey, true);
            storage->SetBool(suppressFirstDeactivateKey, true);
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
        if (!enterEditMode) {
            storage->SetBool(suppressFirstDeactivateKey, false);
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
