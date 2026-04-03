#include "gui/annotation/session.h"

#include <utility>

namespace mmltk::gui {

namespace {

bool spline_edit_states_equal(const AnnotationSplineEditState& lhs,
                              const AnnotationSplineEditState& rhs) {
    return lhs.object_index == rhs.object_index &&
           lhs.active_knot_index == rhs.active_knot_index &&
           lhs.active_segment_index == rhs.active_segment_index &&
           lhs.close_intent == rhs.close_intent &&
           lhs.reopen_requested == rhs.reopen_requested;
}

bool skeleton_edit_states_equal(const AnnotationSkeletonEditState& lhs,
                                const AnnotationSkeletonEditState& rhs) {
    return lhs.object_index == rhs.object_index &&
           lhs.active_joint_index == rhs.active_joint_index &&
           lhs.last_placed_joint_index == rhs.last_placed_joint_index &&
           lhs.reseed_requested == rhs.reseed_requested;
}

bool grouped_edit_transactions_equal(const AnnotationGroupedEditTransactionState& lhs,
                                     const AnnotationGroupedEditTransactionState& rhs) {
    return lhs.kind == rhs.kind &&
           lhs.object_index == rhs.object_index &&
           lhs.restore_selection_index == rhs.restore_selection_index;
}

} // namespace

const char* annotation_tool_kind_label(const AnnotationToolKind tool) noexcept {
    switch (tool) {
    case AnnotationToolKind::Select:
        return "Select";
    case AnnotationToolKind::Direct:
        return "Direct";
    case AnnotationToolKind::Box:
        return "Box";
    case AnnotationToolKind::MaskPaint:
        return "Paint";
    case AnnotationToolKind::MaskErase:
        return "Erase";
    case AnnotationToolKind::MaskFill:
        return "Fill";
    case AnnotationToolKind::Spline:
        return "Spline";
    case AnnotationToolKind::Point:
        return "Point";
    case AnnotationToolKind::Skeleton:
        return "Skeleton";
    case AnnotationToolKind::Count:
        break;
    }
    return "Unknown";
}

bool annotation_tool_kind_uses_brush(const AnnotationToolKind tool) noexcept {
    return tool == AnnotationToolKind::MaskPaint ||
           tool == AnnotationToolKind::MaskErase;
}

void AnnotationSession::set_active_tool(const AnnotationToolKind tool) {
    if (active_tool_ == tool) {
        return;
    }
    clear_spline_edit_state();
    clear_skeleton_edit_state();
    active_tool_ = tool;
    mark_dirty();
}

void AnnotationSession::select_object(const std::optional<std::size_t> index) {
    if (selected_object_index_ == index) {
        return;
    }
    selected_object_index_ = index;
    clear_spline_edit_state();
    clear_skeleton_edit_state();
    mark_dirty();
    mark_overlay_dirty();
}

void AnnotationSession::set_hovered_handle(std::optional<AnnotationHandleId> handle) {
    if (hovered_handle_ == handle) {
        return;
    }
    hovered_handle_ = handle;
    mark_dirty();
}

void AnnotationSession::set_pointer_captured(const bool captured) {
    if (pointer_captured_ == captured) {
        return;
    }
    pointer_captured_ = captured;
    mark_dirty();
}

void AnnotationSession::set_brush_preview(const AnnotationBrushPreview& preview) {
    if (brush_preview_.visible == preview.visible &&
        brush_preview_.capture_x == preview.capture_x &&
        brush_preview_.capture_y == preview.capture_y &&
        brush_preview_.radius == preview.radius &&
        brush_preview_.erase == preview.erase) {
        return;
    }
    brush_preview_ = preview;
    mark_dirty();
}

void AnnotationSession::set_direct_drag_index(const std::optional<std::size_t> index) {
    if (direct_drag_index_ == index) {
        return;
    }
    direct_drag_index_ = index;
    mark_dirty();
}

void AnnotationSession::begin_handle_drag(const AnnotationHandleId& handle) {
    if (handle_drag_.has_value() &&
        handle_drag_->active &&
        handle_drag_->handle == handle) {
        return;
    }
    handle_drag_ = AnnotationHandleDragState{
        true,
        handle,
    };
    mark_dirty();
}

void AnnotationSession::clear_handle_drag() {
    if (!handle_drag_.has_value() || !handle_drag_->active) {
        return;
    }
    handle_drag_.reset();
    mark_dirty();
}

void AnnotationSession::begin_paint_stroke(const int capture_x, const int capture_y) {
    if (paint_stroke_.active &&
        paint_stroke_.last_capture_x == capture_x &&
        paint_stroke_.last_capture_y == capture_y) {
        return;
    }
    paint_stroke_.active = true;
    paint_stroke_.last_capture_x = capture_x;
    paint_stroke_.last_capture_y = capture_y;
    mark_dirty();
}

void AnnotationSession::update_paint_stroke_position(const int capture_x, const int capture_y) {
    if (paint_stroke_.active &&
        paint_stroke_.last_capture_x == capture_x &&
        paint_stroke_.last_capture_y == capture_y) {
        return;
    }
    paint_stroke_.active = true;
    paint_stroke_.last_capture_x = capture_x;
    paint_stroke_.last_capture_y = capture_y;
    mark_dirty();
}

void AnnotationSession::clear_paint_stroke() {
    if (!paint_stroke_.active) {
        return;
    }
    paint_stroke_ = {};
    mark_dirty();
}

void AnnotationSession::set_spline_edit_state(const AnnotationSplineEditState& state) {
    if (spline_edit_states_equal(spline_edit_state_, state)) {
        return;
    }
    spline_edit_state_ = state;
    mark_dirty();
}

void AnnotationSession::clear_spline_edit_state() {
    if (spline_edit_states_equal(spline_edit_state_, AnnotationSplineEditState{})) {
        return;
    }
    spline_edit_state_ = {};
    mark_dirty();
}

void AnnotationSession::set_skeleton_edit_state(const AnnotationSkeletonEditState& state) {
    if (skeleton_edit_states_equal(skeleton_edit_state_, state)) {
        return;
    }
    skeleton_edit_state_ = state;
    mark_dirty();
}

void AnnotationSession::clear_skeleton_edit_state() {
    if (skeleton_edit_states_equal(skeleton_edit_state_, AnnotationSkeletonEditState{})) {
        return;
    }
    skeleton_edit_state_ = {};
    mark_dirty();
}

void AnnotationSession::set_grouped_edit_transaction(
    const AnnotationGroupedEditTransactionState& state) {
    if (grouped_edit_transactions_equal(grouped_edit_transaction_, state)) {
        return;
    }
    grouped_edit_transaction_ = state;
    mark_dirty();
}

void AnnotationSession::clear_grouped_edit_transaction() {
    if (grouped_edit_transactions_equal(grouped_edit_transaction_,
                                        AnnotationGroupedEditTransactionState{})) {
        return;
    }
    grouped_edit_transaction_ = {};
    mark_dirty();
}

void AnnotationSession::clear_interaction_state() {
    const bool had_direct_drag_index = direct_drag_index_.has_value();
    const bool had_handle_drag = handle_drag_.has_value() && handle_drag_->active;
    const bool had_paint_stroke = paint_stroke_.active;
    const bool had_create_drag = create_drag_session_.active;
    const bool had_crop_drag = crop_drag_session_.active;
    const bool had_direct_drag = direct_drag_session_.active;
    if (!had_direct_drag_index &&
        !had_handle_drag &&
        !had_paint_stroke &&
        !had_create_drag &&
        !had_crop_drag &&
        !had_direct_drag) {
        return;
    }

    create_drag_session_ = {};
    crop_drag_session_ = {};
    direct_drag_session_ = {};
    direct_drag_index_.reset();
    handle_drag_.reset();
    paint_stroke_ = {};
    spline_edit_state_ = {};
    skeleton_edit_state_ = {};
    mark_dirty();
}

void AnnotationSession::clear_transient_state() {
    const bool had_hovered_handle = hovered_handle_.has_value();
    const bool had_pointer_capture = pointer_captured_;
    const bool had_brush_preview = brush_preview_.visible;
    const bool had_direct_drag_index = direct_drag_index_.has_value();
    const bool had_handle_drag = handle_drag_.has_value() && handle_drag_->active;
    const bool had_paint_stroke = paint_stroke_.active;
    const bool had_create_drag = create_drag_session_.active;
    const bool had_crop_drag = crop_drag_session_.active;
    const bool had_direct_drag = direct_drag_session_.active;
    const bool had_spline_edit_state =
        !spline_edit_states_equal(spline_edit_state_, AnnotationSplineEditState{});
    const bool had_skeleton_edit_state =
        !skeleton_edit_states_equal(skeleton_edit_state_, AnnotationSkeletonEditState{});
    if (!had_hovered_handle &&
        !had_pointer_capture &&
        !had_brush_preview &&
        !had_direct_drag_index &&
        !had_handle_drag &&
        !had_paint_stroke &&
        !had_create_drag &&
        !had_crop_drag &&
        !had_direct_drag &&
        !had_spline_edit_state &&
        !had_skeleton_edit_state) {
        return;
    }

    hovered_handle_.reset();
    pointer_captured_ = false;
    brush_preview_ = {};
    create_drag_session_ = {};
    crop_drag_session_ = {};
    direct_drag_session_ = {};
    direct_drag_index_.reset();
    handle_drag_.reset();
    paint_stroke_ = {};
    spline_edit_state_ = {};
    skeleton_edit_state_ = {};
    mark_dirty();
}

void AnnotationSession::mark_dirty() {
    ++revision_;
}

void AnnotationSession::mark_overlay_dirty() {
    ++overlay_revision_;
}

} // namespace mmltk::gui
