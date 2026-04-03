#pragma once

#include "ui_style.h"

#include <cfloat>
#include <string>

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

namespace mmltk::gui {

inline void draw_section_heading(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.87f, 0.89f, 0.92f, 1.00f));
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::Separator();
}

template <typename DrawFn>
void draw_with_optional_font(ImFont* font, DrawFn&& draw_fn) {
    if (font != nullptr) {
        ImGui::PushFont(font);
    }
    draw_fn();
    if (font != nullptr) {
        ImGui::PopFont();
    }
}

template <typename DrawFn>
void draw_labeled_widget(const char* label,
                         float width,
                         DrawFn&& draw_fn,
                         const char* tooltip = nullptr) {
    ImGui::PushID(label);
    ImGui::PushID(static_cast<int>(ImGui::GetCursorScreenPos().y));
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding,
                        ImVec2(4.0f * current_ui_settings().ui_scale,
                               3.0f * current_ui_settings().ui_scale));
    if (ImGui::BeginTable("##property_row",
                          2,
                          ImGuiTableFlags_SizingFixedFit |
                              ImGuiTableFlags_NoSavedSettings |
                              ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, ui_label_width());
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s:", label);
        bool row_hovered = ImGui::IsItemHovered();
        ImGui::TableSetColumnIndex(1);
        if (width > 0.0f) {
            ImGui::SetNextItemWidth(ui_scaled(width));
        } else if (width == -FLT_MIN) {
            ImGui::SetNextItemWidth(-FLT_MIN);
        }
        draw_fn();
        row_hovered = row_hovered || ImGui::IsItemHovered();
        if ((tooltip != nullptr) && (tooltip[0] != '\0') && row_hovered) {
            ImGui::SetTooltip("%s", tooltip);
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
    ImGui::PopID();
}

inline void draw_full_width_input(const char* label, std::string& value, const char* tooltip = nullptr) {
    draw_labeled_widget(label, -FLT_MIN, [&value]() {
        ImGui::InputText("##value", &value);
    }, tooltip);
}

inline bool draw_labeled_int_input(const char* label,
                                   int& value,
                                   float width,
                                   int step = 1,
                                   int step_fast = 100,
                                   const char* tooltip = nullptr) {
    bool changed = false;
    draw_labeled_widget(label, width, [&value, step, step_fast, &changed]() {
        changed = ImGui::InputInt("##value", &value, step, step_fast);
    }, tooltip);
    return changed;
}

inline void draw_labeled_float_input(const char* label,
                                     float& value,
                                     float width,
                                     float step,
                                     float step_fast,
                                     const char* format,
                                     const char* tooltip = nullptr) {
    draw_labeled_widget(label, width, [&value, step, step_fast, format]() {
        ImGui::InputFloat("##value", &value, step, step_fast, format);
    }, tooltip);
}

inline bool draw_labeled_double_input(const char* label,
                                      double& value,
                                      float width,
                                      const char* format,
                                      const char* tooltip = nullptr) {
    bool changed = false;
    draw_labeled_widget(label, width, [&value, format, &changed]() {
        changed = ImGui::InputDouble("##value", &value, 0.0, 0.0, format);
    }, tooltip);
    return changed;
}

inline void draw_labeled_percent_slider(const char* label,
                                        float& value,
                                        float width,
                                        const char* tooltip = nullptr) {
    draw_labeled_widget(label, width, [&value]() {
        ImGui::SliderFloat("##value", &value, 0.0f, 100.0f, "%.1f%%");
    }, tooltip);
}

inline bool draw_labeled_checkbox(const char* label, bool& value, const char* tooltip = nullptr) {
    bool changed = false;
    draw_labeled_widget(label, 0.0f, [&value, &changed]() {
        changed = ImGui::Checkbox("##value", &value);
    }, tooltip);
    return changed;
}

template <typename DrawItemsFn>
void draw_labeled_combo(const char* label,
                        const char* preview,
                        float width,
                        DrawItemsFn&& draw_items_fn,
                        const char* tooltip = nullptr) {
    draw_labeled_widget(label, width, [&draw_items_fn, preview]() {
        if (ImGui::BeginCombo("##value", preview)) {
            draw_items_fn();
            ImGui::EndCombo();
        }
    }, tooltip);
}

} // namespace mmltk::gui
