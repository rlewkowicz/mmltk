#pragma once

#include "gui/preview_rect_drag.h"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace mmltk::gui {

enum class AnnotationToolKind : std::uint8_t {
    Select = 0,
    Direct = 1,
    Box = 2,
    MaskPaint = 3,
    MaskErase = 4,
    MaskFill = 5,
    Spline = 6,
    Point = 7,
    Skeleton = 8,
    Count = 9,
};

constexpr std::size_t annotation_tool_kind_count() noexcept {
    return static_cast<std::size_t>(AnnotationToolKind::Count);
}

const char* annotation_tool_kind_label(AnnotationToolKind tool) noexcept;
bool annotation_tool_kind_uses_brush(AnnotationToolKind tool) noexcept;

enum class AnnotationHandleRole : std::uint8_t {
    None = 0,
    Point = 1,
    SplineKnot = 2,
    SplineInHandle = 3,
    SplineOutHandle = 4,
    SkeletonNode = 5,
};

enum class AnnotationEditTransactionKind : std::uint8_t {
    None = 0,
    SplineConstruction = 1,
    SkeletonConstruction = 2,
};

struct AnnotationHandleId {
    std::size_t object_index = 0;
    std::size_t element_index = 0;
    AnnotationHandleRole role = AnnotationHandleRole::None;
};

inline bool operator==(const AnnotationHandleId& lhs, const AnnotationHandleId& rhs) {
    return lhs.object_index == rhs.object_index && lhs.element_index == rhs.element_index && lhs.role == rhs.role;
}

struct AnnotationHandleDragState {
    bool active = false;
    AnnotationHandleId handle{};
};

struct AnnotationBrushPreview {
    bool visible = false;
    int capture_x = 0;
    int capture_y = 0;
    int radius = 12;
    bool erase = false;
};

struct AnnotationPaintStroke {
    bool active = false;
    int last_capture_x = 0;
    int last_capture_y = 0;
};

struct AnnotationSplineEditState {
    std::optional<std::size_t> object_index;
    std::optional<std::size_t> active_knot_index;
    std::optional<std::size_t> active_segment_index;
    bool close_intent = false;
    bool reopen_requested = false;
};

struct AnnotationSkeletonEditState {
    std::optional<std::size_t> object_index;
    std::optional<std::size_t> active_joint_index;
    std::optional<std::size_t> last_placed_joint_index;
    bool reseed_requested = false;
};

struct AnnotationGroupedEditTransactionState {
    AnnotationEditTransactionKind kind = AnnotationEditTransactionKind::None;
    std::optional<std::size_t> object_index;
    std::optional<std::size_t> restore_selection_index;
};

class AnnotationSession {
   public:
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] std::uint64_t overlay_revision() const noexcept {
        return overlay_revision_;
    }
    [[nodiscard]] AnnotationToolKind active_tool() const noexcept {
        return active_tool_;
    }
    [[nodiscard]] const std::optional<std::size_t>& selected_object_index() const noexcept {
        return selected_object_index_;
    }
    [[nodiscard]] const std::optional<AnnotationHandleId>& hovered_handle() const noexcept {
        return hovered_handle_;
    }
    [[nodiscard]] const std::optional<std::size_t>& hovered_object_index() const noexcept {
        return hovered_object_index_;
    }
    [[nodiscard]] bool pointer_captured() const noexcept {
        return pointer_captured_;
    }
    [[nodiscard]] const AnnotationBrushPreview& brush_preview() const noexcept {
        return brush_preview_;
    }
    [[nodiscard]] const PreviewRectDragSession& create_drag_session() const noexcept {
        return create_drag_session_;
    }
    [[nodiscard]] PreviewRectDragSession& create_drag_session() noexcept {
        return create_drag_session_;
    }
    [[nodiscard]] const PreviewRectDragSession& crop_drag_session() const noexcept {
        return crop_drag_session_;
    }
    [[nodiscard]] PreviewRectDragSession& crop_drag_session() noexcept {
        return crop_drag_session_;
    }
    [[nodiscard]] const PreviewRectDragSession& direct_drag_session() const noexcept {
        return direct_drag_session_;
    }
    [[nodiscard]] PreviewRectDragSession& direct_drag_session() noexcept {
        return direct_drag_session_;
    }
    [[nodiscard]] const std::optional<std::size_t>& direct_drag_index() const noexcept {
        return direct_drag_index_;
    }
    [[nodiscard]] const std::optional<AnnotationHandleDragState>& handle_drag() const noexcept {
        return handle_drag_;
    }
    [[nodiscard]] const AnnotationPaintStroke& paint_stroke() const noexcept {
        return paint_stroke_;
    }
    [[nodiscard]] const AnnotationSplineEditState& spline_edit_state() const noexcept {
        return spline_edit_state_;
    }
    [[nodiscard]] const AnnotationSkeletonEditState& skeleton_edit_state() const noexcept {
        return skeleton_edit_state_;
    }
    [[nodiscard]] const AnnotationGroupedEditTransactionState& grouped_edit_transaction() const noexcept {
        return grouped_edit_transaction_;
    }

    void set_active_tool(AnnotationToolKind tool);
    void select_object(std::optional<std::size_t> index);
    void set_hovered_handle(std::optional<AnnotationHandleId> handle);
    void set_hovered_object_index(std::optional<std::size_t> index);
    void touch_overlay() noexcept;
    void set_pointer_captured(bool captured);
    void set_brush_preview(const AnnotationBrushPreview& preview);
    void set_direct_drag_index(std::optional<std::size_t> index);
    void begin_handle_drag(const AnnotationHandleId& handle);
    void clear_handle_drag();
    void begin_paint_stroke(int capture_x, int capture_y);
    void update_paint_stroke_position(int capture_x, int capture_y);
    void clear_paint_stroke();
    void set_spline_edit_state(const AnnotationSplineEditState& state);
    void clear_spline_edit_state();
    void set_skeleton_edit_state(const AnnotationSkeletonEditState& state);
    void clear_skeleton_edit_state();
    void set_grouped_edit_transaction(const AnnotationGroupedEditTransactionState& state);
    void clear_grouped_edit_transaction();
    void clear_interaction_state();
    void clear_transient_state();

   private:
    [[nodiscard]] bool has_active_interaction_state() const;
    [[nodiscard]] bool has_active_transient_state() const;
    void reset_interaction_members();
    void reset_transient_members();
    void mark_dirty();
    void mark_overlay_dirty();

    std::uint64_t revision_ = 0;
    std::uint64_t overlay_revision_ = 0;
    AnnotationToolKind active_tool_ = AnnotationToolKind::Select;
    std::optional<std::size_t> selected_object_index_;
    std::optional<AnnotationHandleId> hovered_handle_;
    std::optional<std::size_t> hovered_object_index_;
    bool pointer_captured_ = false;
    AnnotationBrushPreview brush_preview_{};
    PreviewRectDragSession create_drag_session_{};
    PreviewRectDragSession crop_drag_session_{};
    PreviewRectDragSession direct_drag_session_{};
    std::optional<std::size_t> direct_drag_index_;
    std::optional<AnnotationHandleDragState> handle_drag_;
    AnnotationPaintStroke paint_stroke_{};
    AnnotationSplineEditState spline_edit_state_{};
    AnnotationSkeletonEditState skeleton_edit_state_{};
    AnnotationGroupedEditTransactionState grouped_edit_transaction_{};
};

}  
