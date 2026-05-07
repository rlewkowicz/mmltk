#include "gui/annotation/session.h"

#include <array>
#include <utility>

namespace mmltk::gui {

namespace {

bool spline_edit_states_equal(const AnnotationSplineEditState& lhs, const AnnotationSplineEditState& rhs) {
    return lhs.object_index == rhs.object_index && lhs.active_knot_index == rhs.active_knot_index &&
           lhs.active_segment_index == rhs.active_segment_index && lhs.close_intent == rhs.close_intent &&
           lhs.reopen_requested == rhs.reopen_requested;
}

bool skeleton_edit_states_equal(const AnnotationSkeletonEditState& lhs, const AnnotationSkeletonEditState& rhs) {
    return lhs.object_index == rhs.object_index && lhs.active_joint_index == rhs.active_joint_index &&
           lhs.last_placed_joint_index == rhs.last_placed_joint_index && lhs.reseed_requested == rhs.reseed_requested;
}

bool grouped_edit_transactions_equal(const AnnotationGroupedEditTransactionState& lhs,
                                     const AnnotationGroupedEditTransactionState& rhs) {
    return lhs.kind == rhs.kind && lhs.object_index == rhs.object_index &&
           lhs.restore_selection_index == rhs.restore_selection_index;
}

constexpr std::array<const char*, annotation_tool_kind_count()> kAnnotationToolKindLabels{
    "Select", "Direct", "Box", "Paint", "Erase", "Fill", "Spline", "Point", "Skeleton",
};

constexpr std::array<bool, annotation_tool_kind_count()> kAnnotationToolKindUsesBrush{
    false, false, false, true, true, false, false, false, false,
};

}  // namespace

const char* annotation_tool_kind_label(const AnnotationToolKind tool) noexcept {
    const auto slot = static_cast<std::size_t>(tool);
    if (slot < kAnnotationToolKindLabels.size()) {
        return kAnnotationToolKindLabels[slot];
    }
    return "Unknown";
}

bool annotation_tool_kind_uses_brush(const AnnotationToolKind tool) noexcept {
    const auto slot = static_cast<std::size_t>(tool);
    return slot < kAnnotationToolKindUsesBrush.size() && kAnnotationToolKindUsesBrush[slot];
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
    if (brush_preview_.visible == preview.visible && brush_preview_.capture_x == preview.capture_x &&
        brush_preview_.capture_y == preview.capture_y && brush_preview_.radius == preview.radius &&
        brush_preview_.erase == preview.erase) {
        return;
    }
    brush_preview_ = preview;
    mark_dirty();
    mark_overlay_dirty();
}

void AnnotationSession::set_direct_drag_index(const std::optional<std::size_t> index) {
    if (direct_drag_index_ == index) {
        return;
    }
    direct_drag_index_ = index;
    mark_dirty();
}

void AnnotationSession::begin_handle_drag(const AnnotationHandleId& handle) {
    if (handle_drag_.has_value() && handle_drag_->active && handle_drag_->handle == handle) {
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
    if (paint_stroke_.active && paint_stroke_.last_capture_x == capture_x &&
        paint_stroke_.last_capture_y == capture_y) {
        return;
    }
    paint_stroke_.active = true;
    paint_stroke_.last_capture_x = capture_x;
    paint_stroke_.last_capture_y = capture_y;
    mark_dirty();
}

void AnnotationSession::update_paint_stroke_position(const int capture_x, const int capture_y) {
    if (paint_stroke_.active && paint_stroke_.last_capture_x == capture_x &&
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

void AnnotationSession::set_grouped_edit_transaction(const AnnotationGroupedEditTransactionState& state) {
    if (grouped_edit_transactions_equal(grouped_edit_transaction_, state)) {
        return;
    }
    grouped_edit_transaction_ = state;
    mark_dirty();
}

void AnnotationSession::clear_grouped_edit_transaction() {
    if (grouped_edit_transactions_equal(grouped_edit_transaction_, AnnotationGroupedEditTransactionState{})) {
        return;
    }
    grouped_edit_transaction_ = {};
    mark_dirty();
}

void AnnotationSession::clear_interaction_state() {
    if (!has_active_interaction_state()) {
        return;
    }
    reset_interaction_members();
    mark_dirty();
    mark_overlay_dirty();
}

void AnnotationSession::clear_transient_state() {
    if (!has_active_transient_state()) {
        return;
    }
    reset_transient_members();
    mark_dirty();
    mark_overlay_dirty();
}

bool AnnotationSession::has_active_interaction_state() const {
    return direct_drag_index_.has_value() || (handle_drag_.has_value() && handle_drag_->active) ||
           paint_stroke_.active || create_drag_session_.active || crop_drag_session_.active ||
           direct_drag_session_.active;
}

bool AnnotationSession::has_active_transient_state() const {
    return hovered_handle_.has_value() || pointer_captured_ || brush_preview_.visible ||
           has_active_interaction_state() ||
           !spline_edit_states_equal(spline_edit_state_, AnnotationSplineEditState{}) ||
           !skeleton_edit_states_equal(skeleton_edit_state_, AnnotationSkeletonEditState{});
}

void AnnotationSession::reset_interaction_members() {
    create_drag_session_ = {};
    crop_drag_session_ = {};
    direct_drag_session_ = {};
    direct_drag_index_.reset();
    handle_drag_.reset();
    paint_stroke_ = {};
    spline_edit_state_ = {};
    skeleton_edit_state_ = {};
}

void AnnotationSession::reset_transient_members() {
    hovered_handle_.reset();
    pointer_captured_ = false;
    brush_preview_ = {};
    reset_interaction_members();
}

void AnnotationSession::mark_dirty() {
    ++revision_;
}

void AnnotationSession::mark_overlay_dirty() {
    ++overlay_revision_;
}

}  // namespace mmltk::gui
