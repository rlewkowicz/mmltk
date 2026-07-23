#pragma once

#include "gui/annotation/controller.h"
#include "gui/annotation/editor.h"
#include "gui/annotation/sidebar_action_payloads.h"
#include "gui/annotation/tools/skeleton_tool_helpers.h"
#include "gui/annotation/tools/spline_tool_helpers.h"
#include "gui/annotation_core.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>

namespace mmltk::gui {

struct AnnotationSidebarMutationResult {
    bool preview_invalidated = false;
    bool reset_canvas_interactions = false;
    std::optional<AnnotationToolKind> next_tool;
    bool request_assist = false;
};

struct AnnotationObjectsTabApplyContext {
    AnnotationController& controller;
    AnnotationDocument& document;
    AnnotationSession& session;
    AnnotationCategories& categories;
    const AnnotationFrame* frame = nullptr;
    std::string* pending_class_name = nullptr;
    bool assist_available = false;
    std::size_t preferred_new_object_category_index = 0;
};

struct AnnotationMaskTabApplyContext {
    AnnotationDocument& document;
    AnnotationSession& session;
    const AnnotationFrame* frame = nullptr;
    int* cleanup_radius = nullptr;
};

[[nodiscard]] inline std::size_t selected_object_sidebar_core_change_count(
    const AnnotationSelectedObjectSidebarActionPayload& request) noexcept {
    std::size_t action_count = 0U;
    action_count += request.update_selected_metadata ? 1U : 0U;
    action_count += request.request_redraw_box ? 1U : 0U;
    return action_count;
}

[[nodiscard]] inline std::size_t mask_sidebar_semantic_change_count(
    const AnnotationMaskSidebarActionPayload& request) noexcept {
    std::size_t action_count = 0U;
    action_count += request.request_redraw_box ? 1U : 0U;
    action_count += request.cleanup_op.has_value() ? 1U : 0U;
    action_count += request.update_color_ranges ? 1U : 0U;
    return action_count;
}

inline void apply_sidebar_redraw_box_request(AnnotationDocument& document, AnnotationSession& session,
                                             const AnnotationFrame* frame, AnnotationSidebarMutationResult& result) {
    if (frame == nullptr) {
        return;
    }
    if (AnnotationEditor::reset_selected_object_box(document, session, *frame)) {
        result.preview_invalidated = true;
        result.reset_canvas_interactions = true;
        result.next_tool = AnnotationToolKind::Box;
    }
}

inline AnnotationSidebarMutationResult apply_annotation_selected_object_sidebar_payload(
    const AnnotationObjectsTabApplyContext& context, const AnnotationSelectedObjectSidebarActionPayload& request) {
    AnnotationSidebarMutationResult result;

    if (request.selected_object_index.has_value()) {
        select_object(context.session, context.document, request.selected_object_index);
    }

    if (request.request_add_class && context.pending_class_name != nullptr &&
        AnnotationEditor::add_category(context.categories, *context.pending_class_name)) {
        context.pending_class_name->clear();
    }

    if (request.create_object_tool.has_value() && context.frame != nullptr) {
        AnnotationEditor::ensure_default_category(context.categories);
        if (context.controller
                .create_object(*request.create_object_tool, context.document, context.session, *context.frame,
                               context.categories, context.preferred_new_object_category_index)
                .has_value()) {
            result.preview_invalidated = true;
            result.reset_canvas_interactions = *request.create_object_tool == AnnotationToolKind::Box;
            result.next_tool = request.create_object_tool;
        }
    }

    if (request.request_assist && context.assist_available) {
        result.request_assist = true;
    }

    if (request.request_undo && AnnotationEditor::undo(context.document, context.session)) {
        result.preview_invalidated = true;
        result.reset_canvas_interactions = true;
    }

    if (request.request_redo && AnnotationEditor::redo(context.document, context.session)) {
        result.preview_invalidated = true;
        result.reset_canvas_interactions = true;
    }

    if (request.request_delete_selected &&
        AnnotationEditor::remove_selected_object(context.document, context.session)) {
        result.preview_invalidated = true;
    }

    {
        const bool grouped_selected_object_edits =
            !context.document.transaction_active() &&
            should_group_transaction_for_change_count(selected_object_sidebar_core_change_count(request));
        AnnotationEditorTransactionScope transaction(context.document, grouped_selected_object_edits);

        if (request.update_selected_metadata && tool_detail::update_selected_skeleton_object_metadata(
                                                    context.document, context.session, context.categories,
                                                    request.selected_enabled, request.selected_category_index)) {
            result.preview_invalidated = true;
        }

        if (request.request_redraw_box) {
            apply_sidebar_redraw_box_request(context.document, context.session, context.frame, result);
        }

        transaction.finish();
    }

    const tool_detail::AnnotationSplineSidebarEditRequest spline_request{
        request.request_insert_active_spline_knot,
        request.update_spline_active_segment,
        request.spline_active_segment_index,
        request.spline_handle_mode,
        request.spline_action,
    };
    const tool_detail::AnnotationSplineSidebarMutationResult spline_result =
        tool_detail::apply_annotation_spline_sidebar_edit(context.document, context.session, context.frame,
                                                          context.categories, spline_request);
    result.preview_invalidated = result.preview_invalidated || spline_result.preview_invalidated;
    result.reset_canvas_interactions = result.reset_canvas_interactions || spline_result.reset_canvas_interactions;
    if (spline_result.next_tool.has_value()) {
        result.next_tool = spline_result.next_tool;
    }

    const tool_detail::AnnotationSkeletonSidebarEditRequest skeleton_request{
        request.update_skeleton_active_joint,
        request.skeleton_active_joint_index,
        request.skeleton_action,
    };
    const tool_detail::AnnotationSkeletonSidebarMutationResult skeleton_result =
        tool_detail::apply_annotation_skeleton_sidebar_edit(context.document, context.session, context.frame,
                                                            context.categories, skeleton_request);
    result.preview_invalidated = result.preview_invalidated || skeleton_result.preview_invalidated;
    result.reset_canvas_interactions = result.reset_canvas_interactions || skeleton_result.reset_canvas_interactions;
    if (skeleton_result.next_tool.has_value()) {
        result.next_tool = skeleton_result.next_tool;
    }

    return result;
}

inline AnnotationSidebarMutationResult apply_annotation_mask_sidebar_edit(
    const AnnotationMaskTabApplyContext& context, const AnnotationMaskSidebarActionPayload& request) {
    AnnotationSidebarMutationResult result;
    if (context.cleanup_radius != nullptr) {
        *context.cleanup_radius = std::clamp(request.cleanup_radius, 1, 32);
    }

    const bool grouped_mask_edits =
        !context.document.transaction_active() &&
        should_group_transaction_for_change_count(mask_sidebar_semantic_change_count(request));
    AnnotationEditorTransactionScope transaction(context.document, grouped_mask_edits);

    if (request.request_redraw_box) {
        apply_sidebar_redraw_box_request(context.document, context.session, context.frame, result);
    }

    if (request.cleanup_op.has_value() && context.frame != nullptr && context.cleanup_radius != nullptr &&
        AnnotationEditor::cleanup_selected_mask(context.document, context.session, *context.frame, *request.cleanup_op,
                                                *context.cleanup_radius)) {
        result.preview_invalidated = true;
    }

    if (request.update_color_ranges && AnnotationEditor::update_selected_object_color_ranges(
                                           context.document, context.session, request.sup, request.nosup)) {
        result.preview_invalidated = true;
    }

    transaction.finish();
    return result;
}

}  
