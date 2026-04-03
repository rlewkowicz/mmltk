#include "gui/annotation/sidebar.h"

#include "gui/annotation/annotation_ui.h"
#include "gui/annotation_core.h"
#include "gui/ui_controls.h"

#include <imgui.h>

#include <algorithm>

namespace mmltk::gui {

namespace {

bool annotation_color_ranges_equal(const AnnotationColorRange& lhs,
                                   const AnnotationColorRange& rhs) {
    return lhs.center.hue_degrees == rhs.center.hue_degrees &&
           lhs.center.saturation == rhs.center.saturation &&
           lhs.center.value == rhs.center.value &&
           lhs.tolerance.hue_minus_pct == rhs.tolerance.hue_minus_pct &&
           lhs.tolerance.hue_plus_pct == rhs.tolerance.hue_plus_pct &&
           lhs.tolerance.saturation_minus_pct == rhs.tolerance.saturation_minus_pct &&
           lhs.tolerance.saturation_plus_pct == rhs.tolerance.saturation_plus_pct &&
           lhs.tolerance.value_minus_pct == rhs.tolerance.value_minus_pct &&
           lhs.tolerance.value_plus_pct == rhs.tolerance.value_plus_pct &&
           lhs.sampling == rhs.sampling;
}

} // namespace

AnnotationObjectsTabActions draw_annotation_objects_tab(AnnotationObjectsTabState state) {
    AnnotationObjectsTabActions actions;
    const AnnotationSidebarViewModel empty_model{};
    const AnnotationSidebarViewModel& model =
        state.model != nullptr ? *state.model : empty_model;

    draw_section_heading("Classes");
    ImGui::Text("Loaded Classes: %zu", model.classes.size());
    if (model.has_classes) {
        ImGui::BeginChild("annotate_classes_sidebar", ImVec2(0.0f, 96.0f), true);
        for (const AnnotationSidebarClassViewModel& category : model.classes) {
            ImGui::Text("%d  %s", category.id, category.name.c_str());
        }
        ImGui::EndChild();
    }

    if (state.pending_class_name != nullptr) {
        draw_full_width_input("Add Class", *state.pending_class_name);
    }
    if (annotation_ui::draw_primary_button("Add Class")) {
        actions.request_add_class = true;
    }

    draw_section_heading("Objects");
    if (const auto pressed =
            annotation_ui::draw_inline_button_row(
                std::array{
                    annotation_ui::button_token("New Box Object", model.can_create_objects),
                    annotation_ui::button_token("New Point Object", model.can_create_objects),
                    annotation_ui::button_token("New Spline Object", model.can_create_objects),
                    annotation_ui::button_token("New Skeleton Object", model.can_create_objects),
                })) {
        switch (*pressed) {
        case 0U:
            actions.create_object_tool = AnnotationToolKind::Box;
            break;
        case 1U:
            actions.create_object_tool = AnnotationToolKind::Point;
            break;
        case 2U:
            actions.create_object_tool = AnnotationToolKind::Spline;
            break;
        case 3U:
            actions.create_object_tool = AnnotationToolKind::Skeleton;
            break;
        default:
            break;
        }
    }

    if (annotation_ui::draw_primary_button(
            state.assist_running ? "Assisting..." : "Assist Current Frame",
            state.assist_available && !state.assist_running &&
                state.has_annotation_frame)) {
        actions.request_assist = true;
    }
    if (!state.assist_available) {
        annotation_ui::draw_compact_disabled_hint(
            state.compact_font,
            "Manual mode is active. Full-box seeds are used until a model-backed assist mode is selected.");
    } else if (!state.has_annotation_frame) {
        annotation_ui::draw_compact_disabled_hint(
            state.compact_font,
            "Load a frame before requesting assist.");
    }

    if (const auto pressed =
            annotation_ui::draw_inline_button_row(
                std::array{
                    annotation_ui::button_token("Undo", model.can_undo),
                    annotation_ui::button_token("Redo", model.can_redo),
                    annotation_ui::button_token("Delete Selected", model.can_delete_selected),
                })) {
        switch (*pressed) {
        case 0U:
            actions.request_undo = true;
            break;
        case 1U:
            actions.request_redo = true;
            break;
        case 2U:
            actions.request_delete_selected = true;
            break;
        default:
            break;
        }
    }

    ImGui::BeginChild("annotate_instances_sidebar", ImVec2(0.0f, 180.0f), true);
    for (const AnnotationSidebarObjectViewModel& object_row : model.objects) {
        if (ImGui::Selectable(object_row.label.c_str(), object_row.selected)) {
            actions.selected_object_index = object_row.index;
        }
    }
    ImGui::EndChild();

    if (!model.selected_object_index.has_value() || !model.selected_object.has_value()) {
        return actions;
    }

    const AnnotationSidebarSelectedObjectViewModel& selected_object =
        *model.selected_object;
    bool enabled = selected_object.enabled;
    std::size_t category_index = selected_object.category_index;
    draw_section_heading("Selected Object");
    draw_labeled_checkbox("Enabled", enabled);
    if (state.categories != nullptr && !state.categories->items.empty()) {
        const std::size_t preview_index =
            std::min(category_index, state.categories->items.size() - 1U);
        const char* preview_label =
            state.categories->items[preview_index].name.c_str();
        draw_labeled_combo("Class", preview_label, 220.0f, [&]() {
            for (std::size_t index = 0; index < state.categories->items.size(); ++index) {
                const bool selected = index == category_index;
                if (ImGui::Selectable(state.categories->items[index].name.c_str(), selected)) {
                    category_index = index;
                }
                if (selected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
        });
    }
    ImGui::Text("Shape: %s", selected_object.shape_label.c_str());
    if (annotation_ui::draw_button("Redraw Box", model.can_redraw_selected_box)) {
        actions.request_redraw_box = true;
    }
    annotation_ui::draw_compact_box_summary(state.compact_font,
                                            selected_object.display_box);

    if (selected_object.point.has_value()) {
        ImGui::TextWrapped("Point: %.1f, %.1f",
                           selected_object.point->x,
                           selected_object.point->y);
    } else if (selected_object.spline.has_value()) {
        const AnnotationSidebarSelectedSplineViewModel& spline =
            *selected_object.spline;
        ImGui::TextWrapped("Knots: %zu  %s",
                           spline.knot_count,
                           spline.closed ? "closed" : "open");
        if (spline.active_knot_index.has_value()) {
            ImGui::TextWrapped("Active Knot: %zu",
                               *spline.active_knot_index + 1U);
        } else {
            ImGui::TextWrapped("Active Knot: none");
        }
        if (spline.segment_count > 0U) {
            std::optional<std::size_t> active_segment_index =
                spline.active_segment_index;
            std::string active_segment_label =
                active_segment_index.has_value()
                    ? "#" + std::to_string(*active_segment_index + 1U)
                    : std::string("None");
            draw_labeled_combo("Segment",
                               active_segment_label.c_str(),
                               220.0f,
                               [&]() {
                                   const bool none_selected =
                                       !active_segment_index.has_value();
                                   if (ImGui::Selectable("None", none_selected)) {
                                       active_segment_index.reset();
                                   }
                                   if (none_selected) {
                                       ImGui::SetItemDefaultFocus();
                                   }
                                   for (std::size_t segment_index = 0;
                                        segment_index < spline.segment_count;
                                        ++segment_index) {
                                       const std::string label =
                                           "Segment " +
                                           std::to_string(segment_index + 1U);
                                       const bool selected =
                                           active_segment_index.has_value() &&
                                           *active_segment_index == segment_index;
                                       if (ImGui::Selectable(label.c_str(), selected)) {
                                           active_segment_index = segment_index;
                                       }
                                       if (selected) {
                                           ImGui::SetItemDefaultFocus();
                                       }
                                   }
                               });
            if (active_segment_index != spline.active_segment_index) {
                actions.update_spline_active_segment = true;
                actions.spline_active_segment_index = active_segment_index;
            }
        } else {
            ImGui::TextWrapped("Active Segment: none");
        }
        if (annotation_ui::draw_button("Insert Midpoint Knot",
                                       state.has_annotation_frame &&
                                           spline.can_insert_active_segment_knot)) {
            actions.request_insert_active_spline_knot = true;
        }
        if (spline.active_handle_mode.has_value()) {
            AnnotationSplineHandleMode handle_mode =
                *spline.active_handle_mode;
            {
                [[maybe_unused]] const annotation_ui::DisabledScope disabled_scope(
                    !spline.can_edit_active_handle_mode);
                draw_labeled_combo("Handle Mode",
                                   annotation_spline_handle_mode_name(handle_mode),
                                   220.0f,
                                   [&]() {
                                       for (const AnnotationSplineHandleMode option : {
                                                AnnotationSplineHandleMode::Corner,
                                                AnnotationSplineHandleMode::Smooth,
                                                AnnotationSplineHandleMode::Mirrored,
                                            }) {
                                           const bool selected = option == handle_mode;
                                           if (ImGui::Selectable(
                                                   annotation_spline_handle_mode_name(option),
                                                   selected)) {
                                               handle_mode = option;
                                           }
                                           if (selected) {
                                               ImGui::SetItemDefaultFocus();
                                           }
                                       }
                                   });
            }
            if (handle_mode != *spline.active_handle_mode) {
                actions.spline_handle_mode = handle_mode;
            }
        }
        if (const auto pressed =
                annotation_ui::draw_inline_button_row(
                    std::array{
                        annotation_ui::button_token("Close Spline",
                                                     state.has_annotation_frame &&
                                                         spline.can_close),
                        annotation_ui::button_token("Reopen Spline",
                                                     state.has_annotation_frame &&
                                                         spline.can_reopen),
                        annotation_ui::button_token("Delete Active Knot",
                                                     state.has_annotation_frame &&
                                                         spline.can_delete_active_knot),
                    })) {
            switch (*pressed) {
            case 0U:
                actions.spline_action = AnnotationToolActionKind::Confirm;
                break;
            case 1U:
                actions.spline_action = AnnotationToolActionKind::ReopenSpline;
                break;
            case 2U:
                actions.spline_action = AnnotationToolActionKind::DeleteActiveElement;
                break;
            default:
                break;
            }
        }
        if (spline.close_intent) {
            annotation_ui::draw_compact_wrapped_text(
                state.compact_font,
                "Spline close is armed for the next confirm/click.");
        }
    } else if (selected_object.skeleton.has_value()) {
        const AnnotationSidebarSelectedSkeletonViewModel& skeleton =
            *selected_object.skeleton;
        ImGui::TextWrapped("Joints: %zu / %zu visible",
                           skeleton.visible_joint_count,
                           skeleton.total_joint_count);
        if (skeleton.active_joint_index.has_value()) {
            ImGui::TextWrapped("Active Joint: %zu%s%s",
                               *skeleton.active_joint_index + 1U,
                               skeleton.active_joint_key.has_value() ? "  " : "",
                               skeleton.active_joint_key.has_value()
                                   ? skeleton.active_joint_key->c_str()
                                   : "");
        } else {
            ImGui::TextWrapped("Active Joint: none");
        }
        if (skeleton.next_joint_index.has_value()) {
            ImGui::TextWrapped("Next Hidden Joint: %zu%s%s",
                               *skeleton.next_joint_index + 1U,
                               skeleton.next_joint_key.has_value() ? "  " : "",
                               skeleton.next_joint_key.has_value()
                                   ? skeleton.next_joint_key->c_str()
                                   : "");
        }
        std::optional<std::size_t> active_joint_index =
            skeleton.active_joint_index;
        std::string active_joint_label = "None";
        for (const AnnotationSidebarSkeletonJointViewModel& joint :
             skeleton.joints) {
            if (joint.active) {
                active_joint_label = joint.label;
                break;
            }
        }
        if (!skeleton.joints.empty()) {
            draw_labeled_combo("Joint",
                               active_joint_label.c_str(),
                               260.0f,
                               [&]() {
                                   const bool none_selected =
                                       !active_joint_index.has_value();
                                   if (ImGui::Selectable("None", none_selected)) {
                                       active_joint_index.reset();
                                   }
                                   if (none_selected) {
                                       ImGui::SetItemDefaultFocus();
                                   }
                                   for (const AnnotationSidebarSkeletonJointViewModel& joint :
                                        skeleton.joints) {
                                       const bool selected =
                                           active_joint_index.has_value() &&
                                           *active_joint_index == joint.index;
                                       if (ImGui::Selectable(joint.label.c_str(), selected)) {
                                           active_joint_index = joint.index;
                                       }
                                       if (selected) {
                                           ImGui::SetItemDefaultFocus();
                                       }
                                   }
                               });
            if (active_joint_index != skeleton.active_joint_index) {
                actions.update_skeleton_active_joint = true;
                actions.skeleton_active_joint_index = active_joint_index;
            }
        }
        if (const auto pressed =
                annotation_ui::draw_inline_button_row(
                    std::array{
                        annotation_ui::button_token("Skip Joint",
                                                     state.has_annotation_frame &&
                                                         skeleton.can_skip_joint),
                        annotation_ui::button_token("Hide Joint",
                                                     state.has_annotation_frame &&
                                                         skeleton.can_hide_active_joint),
                        annotation_ui::button_token("Show Joint",
                                                     state.has_annotation_frame &&
                                                         skeleton.can_reactivate_active_joint),
                        annotation_ui::button_token("Reseed Joint",
                                                     state.has_annotation_frame &&
                                                         skeleton.can_reseed_active_joint),
                    })) {
            switch (*pressed) {
            case 0U:
                actions.skeleton_action = AnnotationToolActionKind::SkipJoint;
                break;
            case 1U:
                actions.skeleton_action = AnnotationToolActionKind::HideJoint;
                break;
            case 2U:
                actions.skeleton_action = AnnotationToolActionKind::ReactivateJoint;
                break;
            case 3U:
                actions.skeleton_action = AnnotationToolActionKind::ReseedJoint;
                break;
            default:
                break;
            }
        }
        if (skeleton.reseed_requested) {
            annotation_ui::draw_compact_wrapped_text(
                state.compact_font,
                "The active joint is cleared and ready to be placed again.");
        }
    }

    if (enabled != selected_object.enabled ||
        category_index != selected_object.category_index) {
        actions.update_selected_metadata = true;
        actions.selected_enabled = enabled;
        actions.selected_category_index = category_index;
    }

    return actions;
}

AnnotationMaskTabActions draw_annotation_mask_tab(
    AnnotationMaskTabState state,
    const AnnotationRangeControlsDrawer& draw_range_controls) {
    AnnotationMaskTabActions actions;
    actions.cleanup_radius = std::clamp(state.cleanup_radius, 1, 32);

    const AnnotationSidebarViewModel empty_model{};
    const AnnotationSidebarViewModel& model =
        state.model != nullptr ? *state.model : empty_model;

    if (!model.selected_object.has_value()) {
        annotation_ui::draw_compact_wrapped_text(
            state.compact_font,
            "Select an object to edit its mask and suppress / no-suppress HSV controls.");
        return actions;
    }

    if (!model.can_edit_selected_mask) {
        annotation_ui::draw_compact_wrapped_text(
            state.compact_font,
            "Mask controls apply only to box and mask objects. Use the workspace tools for point, spline, and skeleton editing.");
        return actions;
    }

    if (!state.has_annotation_frame) {
        annotation_ui::draw_compact_wrapped_text(state.compact_font,
                                                 "Mask editing requires an active annotation frame.");
        return actions;
    }

    const AnnotationSidebarSelectedObjectViewModel& selected_object =
        *model.selected_object;
    AnnotationColorRange updated_sup = selected_object.sup;
    AnnotationColorRange updated_nosup = selected_object.nosup;
    const AnnotationColorRange original_sup = updated_sup;
    const AnnotationColorRange original_nosup = updated_nosup;

    ImGui::Text("Shape: %s", selected_object.shape_label.c_str());
    annotation_ui::draw_compact_box_summary(state.compact_font,
                                            selected_object.display_box);

    if (annotation_ui::draw_button("Redraw Box", model.can_redraw_selected_box)) {
        actions.request_redraw_box = true;
    }

    draw_section_heading("Mask Tools");
    ImGui::TextWrapped(
        "Use canvas modes for direct drag, paint, erase, and fill. Cleanup runs on the selected ROI mask.");
    ImGui::Text("Cleanup Radius: %d", actions.cleanup_radius);
    draw_labeled_int_input("Radius", actions.cleanup_radius, 70.0f);
    actions.cleanup_radius = std::clamp(actions.cleanup_radius, 1, 32);
    if (const auto pressed =
            annotation_ui::draw_inline_button_row(
                std::array{
                    annotation_ui::button_token(
                        annotation_mask_cleanup_label(AnnotationMaskCleanupOp::LargestComponent)),
                    annotation_ui::button_token(
                        annotation_mask_cleanup_label(AnnotationMaskCleanupOp::FillHoles)),
                    annotation_ui::button_token(
                        annotation_mask_cleanup_label(AnnotationMaskCleanupOp::Dilate)),
                    annotation_ui::button_token(
                        annotation_mask_cleanup_label(AnnotationMaskCleanupOp::Erode)),
                    annotation_ui::button_token(
                        annotation_mask_cleanup_label(AnnotationMaskCleanupOp::Open)),
                    annotation_ui::button_token(
                        annotation_mask_cleanup_label(AnnotationMaskCleanupOp::Close)),
                })) {
        switch (*pressed) {
        case 0U:
            actions.cleanup_op = AnnotationMaskCleanupOp::LargestComponent;
            break;
        case 1U:
            actions.cleanup_op = AnnotationMaskCleanupOp::FillHoles;
            break;
        case 2U:
            actions.cleanup_op = AnnotationMaskCleanupOp::Dilate;
            break;
        case 3U:
            actions.cleanup_op = AnnotationMaskCleanupOp::Erode;
            break;
        case 4U:
            actions.cleanup_op = AnnotationMaskCleanupOp::Open;
            break;
        case 5U:
            actions.cleanup_op = AnnotationMaskCleanupOp::Close;
            break;
        default:
            break;
        }
    }

    if (draw_range_controls) {
        draw_range_controls("Suppress", updated_sup, updated_nosup);
        draw_range_controls("No Suppress", updated_nosup, updated_sup);
    }
    if (!annotation_color_ranges_equal(original_sup, updated_sup) ||
        !annotation_color_ranges_equal(original_nosup, updated_nosup)) {
        actions.update_color_ranges = true;
        actions.sup = updated_sup;
        actions.nosup = updated_nosup;
    }

    return actions;
}

} // namespace mmltk::gui
