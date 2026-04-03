#pragma once

#include "gui/annotation/document/edit.h"
#include "gui/annotation/tools/tool_manager.h"
#include "gui/annotation/workspace/model.h"

#include <functional>
#include <optional>
#include <string>

struct ImFont;

namespace mmltk::gui {

struct AnnotationCategories;

struct AnnotationObjectsTabState {
    const AnnotationSidebarViewModel* model = nullptr;
    const AnnotationCategories* categories = nullptr;
    bool has_annotation_frame = false;
    bool assist_available = false;
    bool assist_running = false;
    std::string* pending_class_name = nullptr;
    ImFont* compact_font = nullptr;
};

struct AnnotationObjectsTabActions {
    bool request_add_class = false;
    std::optional<AnnotationToolKind> create_object_tool;
    bool request_assist = false;
    bool request_undo = false;
    bool request_redo = false;
    bool request_delete_selected = false;
    std::optional<std::size_t> selected_object_index;
    bool update_selected_metadata = false;
    bool selected_enabled = true;
    std::size_t selected_category_index = 0;
    bool request_redraw_box = false;
    bool request_insert_active_spline_knot = false;
    bool update_spline_active_segment = false;
    std::optional<std::size_t> spline_active_segment_index;
    std::optional<AnnotationSplineHandleMode> spline_handle_mode;
    std::optional<AnnotationToolActionKind> spline_action;
    bool update_skeleton_active_joint = false;
    std::optional<std::size_t> skeleton_active_joint_index;
    std::optional<AnnotationToolActionKind> skeleton_action;
};

using AnnotationRangeControlsDrawer =
    std::function<void(const char*, AnnotationColorRange&, AnnotationColorRange&)>;

struct AnnotationMaskTabState {
    const AnnotationSidebarViewModel* model = nullptr;
    bool has_annotation_frame = false;
    int cleanup_radius = 1;
    ImFont* compact_font = nullptr;
};

struct AnnotationMaskTabActions {
    int cleanup_radius = 1;
    bool request_redraw_box = false;
    std::optional<AnnotationMaskCleanupOp> cleanup_op;
    bool update_color_ranges = false;
    AnnotationColorRange sup{};
    AnnotationColorRange nosup{};
};

[[nodiscard]] AnnotationObjectsTabActions draw_annotation_objects_tab(
    AnnotationObjectsTabState state);

[[nodiscard]] AnnotationMaskTabActions draw_annotation_mask_tab(
    AnnotationMaskTabState state,
    const AnnotationRangeControlsDrawer& draw_range_controls);

} // namespace mmltk::gui
