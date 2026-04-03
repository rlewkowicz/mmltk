#pragma once

#include "gui/annotation/tools/tool_manager.h"

namespace mmltk::gui {

struct AnnotationSidebarSelectedSplineViewModel;

} // namespace mmltk::gui

namespace mmltk::gui::tool_detail {

struct AnnotationSplineSidebarEditRequest {
    bool request_insert_active_spline_knot = false;
    bool update_spline_active_segment = false;
    std::optional<std::size_t> spline_active_segment_index;
    std::optional<AnnotationSplineHandleMode> spline_handle_mode;
    std::optional<AnnotationToolActionKind> spline_action;
};

struct AnnotationSplineSidebarMutationResult {
    bool preview_invalidated = false;
    bool reset_canvas_interactions = false;
    std::optional<AnnotationToolKind> next_tool;
};

[[nodiscard]] AnnotationSplineEditState current_or_seed_spline_edit_state(
    const AnnotationSession& session,
    std::size_t object_index,
    const AnnotationSplineShape& shape) noexcept;
[[nodiscard]] AnnotationSidebarSelectedSplineViewModel
make_selected_spline_sidebar_view_model(
    const AnnotationSession& session,
    std::size_t object_index,
    const AnnotationSplineShape& shape);
void seed_created_spline_edit_state(AnnotationSession& session,
                                    std::size_t object_index,
                                    const AnnotationObject& object) noexcept;
void finalize_created_spline_object(AnnotationDocument& document,
                                    AnnotationSession& session,
                                    std::size_t object_index,
                                    std::optional<std::size_t> active_knot_index =
                                        std::nullopt);
void set_active_spline_edit_state(AnnotationSession& session,
                                  std::size_t object_index,
                                  const AnnotationSplineShape& shape,
                                  std::optional<std::size_t> active_knot_index,
                                  std::optional<std::size_t> active_segment_index =
                                      std::nullopt,
                                  bool close_intent = false,
                                  bool reopen_requested = false) noexcept;

[[nodiscard]] AnnotationToolMutation handle_spline_canvas_click(
    const AnnotationToolContext& context,
    const AnnotationToolCanvasClickEvent& event);
[[nodiscard]] AnnotationToolMutation handle_spline_action(
    const AnnotationToolContext& context,
    const AnnotationToolActionEvent& event);

bool insert_selected_spline_knot_at_active_segment(AnnotationDocument& document,
                                                   AnnotationSession& session);
bool set_selected_spline_active_segment(AnnotationDocument& document,
                                        AnnotationSession& session,
                                        std::optional<std::size_t> segment_index);
bool set_selected_spline_handle_mode(AnnotationDocument& document,
                                     const AnnotationSession& session,
                                     AnnotationSplineHandleMode mode);

[[nodiscard]] AnnotationSplineSidebarMutationResult apply_annotation_spline_sidebar_edit(
    AnnotationDocument& document,
    AnnotationSession& session,
    const AnnotationFrame* frame,
    const AnnotationCategories& categories,
    const AnnotationSplineSidebarEditRequest& request);

} // namespace mmltk::gui::tool_detail
