#include "gui/annotation/save_tab.h"

#include "gui/annotation/annotation_ui.h"
#include "gui/ui_controls.h"

#include <imgui.h>

namespace mmltk::gui {

namespace {

[[nodiscard]] bool hold_save_button_is_active(const AnnotationSaveTabPlan& plan) noexcept {
    return annotation_hold_save_ready(plan);
}

} // namespace

AnnotationSaveTabUiActions draw_annotation_save_tab_ui(
    AnnotationSaveTabUiState state,
    const AnnotationSaveTabPlan& plan) {
    AnnotationSaveTabUiActions actions;
    actions.hold_save_active = state.hold_save_active;
    actions.hold_save_blocked = state.hold_save_blocked;

    draw_section_heading("Output");
    if (state.output_root != nullptr) {
        draw_full_width_input("Output Root", *state.output_root);
    }
    if (state.split != nullptr) {
        draw_full_width_input("Split", *state.split);
    }
    annotation_ui::draw_compact_wrapped_text(state.compact_font,
                                             "Loaded classes: %zu",
                                             state.loaded_class_count);

    draw_section_heading("Actions");
    if (annotation_ui::draw_primary_button("Save 1", annotation_save_now_ready(plan))) {
        actions.request_save_now = true;
    } else {
        annotation_ui::draw_compact_disabled_hint(
            state.compact_font,
            "Save 1 becomes available after a frame is loaded, objects resolve successfully, and no save is already running.");
    }

    if (!state.live_video) {
        actions.hold_save_active = false;
        actions.hold_save_blocked = false;
        return actions;
    }

    const bool hold_save_enabled = hold_save_button_is_active(plan);
    (void)annotation_ui::draw_button("Hold Save", hold_save_enabled);
    const bool hold_button_active = hold_save_enabled && ImGui::IsItemActive();
    if (!hold_button_active) {
        actions.hold_save_blocked = false;
    }
    actions.hold_save_active = hold_button_active && !actions.hold_save_blocked;

    if (actions.hold_save_blocked) {
        annotation_ui::draw_compact_wrapped_text(state.compact_font,
                                                 annotation_ui::TextTone::Warning,
                                                 "Release Hold Save to re-arm.");
    } else if (actions.hold_save_active) {
        annotation_ui::draw_compact_wrapped_text(state.compact_font,
                                                 "Saving newest frames while held.");
    } else if (!hold_save_enabled) {
        annotation_ui::draw_compact_disabled_hint(
            state.compact_font,
            "Hold Save activates only while live annotate is running with a writable frame.");
    }

    return actions;
}

} // namespace mmltk::gui
