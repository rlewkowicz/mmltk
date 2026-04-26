#include "gui/annotation/controller.h"
#include "gui/annotation/document/edit.h"
#include "gui/annotation/editor_history.h"
#include "gui/annotation/tools/default_tools.h"
#include "gui/annotation/tools/tool_helpers.h"

#include <algorithm>

namespace mmltk::gui {

namespace {

std::size_t clamp_category_index(const AnnotationCategories& categories, const std::size_t category_index) {
    if (categories.items.empty()) {
        return 0U;
    }
    return std::min(category_index, categories.items.size() - 1U);
}

std::size_t selected_category_index(const AnnotationDocument& document, const AnnotationSession& session) {
    const auto& selected_index = session.selected_object_index();
    if (!selected_index.has_value()) {
        return 0U;
    }
    const AnnotationObject* object = document.object(*selected_index);
    return object != nullptr ? object->category_index : 0U;
}

}  // namespace

AnnotationController::AnnotationController() {
    register_default_annotation_tools(tools_);
}

bool AnnotationController::set_active_tool(const AnnotationToolKind kind, AnnotationDocument& document,
                                           AnnotationSession& session) {
    return tools_.set_active_tool(kind, document, session);
}

void AnnotationController::reset_active_drawing(AnnotationSession& session) {
    if (AnnotationTool* tool = tools_.find_tool(session.active_tool()); tool != nullptr) {
        tool->reset_active_drawing(session);
    } else {
        session.clear_transient_state();
    }
}

bool AnnotationController::handle_canvas_click(AnnotationDocument& document, AnnotationSession& session,
                                               const AnnotationFrame& frame, const AnnotationCategories& categories,
                                               const int capture_x, const int capture_y, const bool double_click) {
    AnnotationTool* tool = tools_.find_tool(session.active_tool());
    if (tool == nullptr) {
        return false;
    }
    const AnnotationToolMutation mutation =
        tool->handle_canvas_click(make_tool_context(document, session, frame, categories),
                                  AnnotationToolCanvasClickEvent{
                                      capture_x,
                                      capture_y,
                                      double_click,
                                      clamp_category_index(categories, selected_category_index(document, session)),
                                  });
    return apply_tool_mutation(mutation, document, session);
}

bool AnnotationController::handle_box_commit(AnnotationDocument& document, AnnotationSession& session,
                                             const AnnotationFrame& frame, const AnnotationCategories& categories,
                                             const RectDragKind drag_kind, const AnnotationBox& capture_box,
                                             const std::size_t default_category_index) {
    AnnotationTool* tool = tools_.find_tool(session.active_tool());
    if (tool == nullptr) {
        return false;
    }
    const AnnotationToolMutation mutation =
        tool->handle_box_commit(make_tool_context(document, session, frame, categories),
                                AnnotationToolBoxCommitEvent{
                                    drag_kind,
                                    capture_box,
                                    clamp_category_index(categories, default_category_index),
                                });
    return apply_tool_mutation(mutation, document, session);
}

bool AnnotationController::handle_brush_sample(AnnotationDocument& document, AnnotationSession& session,
                                               const AnnotationFrame& frame, const AnnotationCategories& categories,
                                               const int capture_x, const int capture_y, const int radius) {
    AnnotationTool* tool = tools_.find_tool(session.active_tool());
    if (tool == nullptr) {
        return false;
    }
    return tool->handle_brush_sample(make_tool_context(document, session, frame, categories), AnnotationToolBrushEvent{
                                                                                                  capture_x,
                                                                                                  capture_y,
                                                                                                  radius,
                                                                                              });
}

bool AnnotationController::handle_handle_drag(AnnotationDocument& document, AnnotationSession& session,
                                              const AnnotationFrame& frame, const AnnotationCategories& categories,
                                              const AnnotationHandleId& handle, const int capture_x,
                                              const int capture_y) {
    AnnotationTool* tool = tools_.find_tool(session.active_tool());
    if (tool == nullptr) {
        return false;
    }
    return tool->handle_handle_drag(make_tool_context(document, session, frame, categories),
                                    AnnotationToolHandleDragEvent{
                                        handle,
                                        capture_x,
                                        capture_y,
                                    });
}

bool AnnotationController::handle_fill(AnnotationDocument& document, AnnotationSession& session,
                                       const AnnotationFrame& frame, const AnnotationCategories& categories,
                                       const int capture_x, const int capture_y) {
    AnnotationTool* tool = tools_.find_tool(session.active_tool());
    if (tool == nullptr) {
        return false;
    }
    return tool->handle_fill(make_tool_context(document, session, frame, categories), AnnotationToolFillEvent{
                                                                                          capture_x,
                                                                                          capture_y,
                                                                                      });
}

bool AnnotationController::handle_tool_action(AnnotationDocument& document, AnnotationSession& session,
                                              const AnnotationFrame& frame, const AnnotationCategories& categories,
                                              const AnnotationToolActionKind action) {
    return handle_tool_action_for_kind(session.active_tool(), document, session, frame, categories, action);
}

bool AnnotationController::handle_tool_action_for_kind(const AnnotationToolKind kind, AnnotationDocument& document,
                                                       AnnotationSession& session, const AnnotationFrame& frame,
                                                       const AnnotationCategories& categories,
                                                       const AnnotationToolActionKind action) {
    AnnotationTool* tool = tools_.find_tool(kind);
    if (tool == nullptr) {
        return false;
    }
    const AnnotationToolMutation mutation =
        tool->handle_action(make_tool_context(document, session, frame, categories), AnnotationToolActionEvent{action});
    return apply_tool_mutation(mutation, document, session);
}

std::optional<std::size_t> AnnotationController::create_object(AnnotationToolKind kind, AnnotationDocument& document,
                                                               AnnotationSession& session, const AnnotationFrame& frame,
                                                               const AnnotationCategories& categories,
                                                               const std::size_t category_index) {
    AnnotationTool* tool = tools_.find_tool(kind);
    if (tool == nullptr) {
        return std::nullopt;
    }

    (void)tool_detail::cancel_active_grouped_tool_edit(document, session);
    const std::size_t normalized_category_index = clamp_category_index(categories, category_index);
    const AnnotationPoint initial_point = annotation_frame_capture_center(frame);
    const AnnotationCategory* skeleton_category =
        normalized_category_index < categories.items.size() ? &categories.items[normalized_category_index] : nullptr;
    std::optional<AnnotationObject> object = tool->make_object(AnnotationToolCreateObjectRequest{
        document.size(),
        normalized_category_index,
        initial_point,
        skeleton_category,
    });
    const AnnotationEditTransactionKind grouped_edit_kind = tool_detail::grouped_edit_kind_for_tool(kind);
    const bool grouped_edit = grouped_edit_kind != AnnotationEditTransactionKind::None;
    if (grouped_edit && !tool_detail::begin_grouped_tool_edit(document, session, kind, std::nullopt,
                                                              normalize_selected_object_index(document, session))) {
        return std::nullopt;
    }
    if (!object.has_value() || !document.apply(AnnotationInsertObjectCommand{std::move(*object), std::nullopt})) {
        if (grouped_edit) {
            (void)tool_detail::cancel_active_grouped_tool_edit(document, session);
        }
        return std::nullopt;
    }

    const std::size_t inserted_index = document.size() - 1U;
    session.select_object(inserted_index);
    if (grouped_edit) {
        tool_detail::bind_grouped_tool_edit_object(session, inserted_index);
    }
    tool->on_object_created(document, session, inserted_index);
    return inserted_index;
}

AnnotationToolContext AnnotationController::make_tool_context(AnnotationDocument& document, AnnotationSession& session,
                                                              const AnnotationFrame& frame,
                                                              const AnnotationCategories& categories) {
    return AnnotationToolContext{
        document,
        session,
        frame,
        categories,
        annotation_frame_capture_width(frame),
        annotation_frame_capture_height(frame),
    };
}

bool AnnotationController::apply_tool_mutation(const AnnotationToolMutation& mutation, AnnotationDocument& document,
                                               AnnotationSession& session) {
    if (mutation.selection_changed) {
        select_object(session, document, mutation.selected_object_index);
    }
    if (mutation.clear_transient_state) {
        session.clear_transient_state();
    }
    return mutation.changed;
}

}  // namespace mmltk::gui
