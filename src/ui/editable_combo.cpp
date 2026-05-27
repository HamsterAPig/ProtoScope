#include "protoscope/ui/editable_combo.hpp"

#include <imgui.h>

#include <cstdio>

namespace protoscope::ui {

namespace {

constexpr std::size_t kEditableComboBufferSize = 512;

bool shouldEnterEditMode() {
    return ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
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
        const char* preview = draft.empty() ? "" : draft.c_str();
        const bool comboOpened = ImGui::BeginCombo(label, preview);
        const bool enterEditMode = shouldEnterEditMode();
        if (enterEditMode) {
            storage->SetBool(editingKey, true);
            storage->SetBool(focusKey, true);
            if (comboOpened) {
                ImGui::CloseCurrentPopup();
            }
        }

        if (comboOpened) {
            for (const auto& option : options) {
                const bool selected = option == draft;
                if (ImGui::Selectable(option.c_str(), selected)) {
                    draft = option;
                    result.edited = true;
                    result.selectedFromList = true;
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
