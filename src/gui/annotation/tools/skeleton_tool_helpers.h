#pragma once

#include "gui/annotation/tools/tool_manager.h"

namespace mmltk::gui {

struct AnnotationSidebarSelectedSkeletonViewModel;

}

namespace mmltk::gui::tool_detail {

struct AnnotationSkeletonSidebarEditRequest {
    bool update_skeleton_active_joint = false;
    std::optional<std::size_t> skeleton_active_joint_index;
    std::optional<AnnotationToolActionKind> skeleton_action;
};

struct AnnotationSkeletonSidebarMutationResult {
    bool preview_invalidated = false;
    bool reset_canvas_interactions = false;
    std::optional<AnnotationToolKind> next_tool;
};

[[nodiscard]] AnnotationSkeletonEditState current_or_seed_skeleton_edit_state(
    const AnnotationSession& session, std::size_t object_index, const AnnotationSkeletonShape& shape) noexcept;
[[nodiscard]] AnnotationSidebarSelectedSkeletonViewModel make_selected_skeleton_sidebar_view_model(
    const AnnotationSession& session, std::size_t object_index, const AnnotationSkeletonShape& shape);
void seed_created_skeleton_edit_state(AnnotationSession& session, std::size_t object_index,
                                      const AnnotationObject& object) noexcept;
void finalize_created_skeleton_object(AnnotationDocument& document, AnnotationSession& session,
                                      std::size_t object_index);
void set_active_skeleton_edit_state(AnnotationSession& session, std::size_t object_index,
                                    const AnnotationSkeletonShape& shape, std::optional<std::size_t> active_joint_index,
                                    std::optional<std::size_t> last_placed_joint_index,
                                    bool reseed_requested = false) noexcept;
[[nodiscard]] bool update_selected_skeleton_object_metadata(AnnotationDocument& document, AnnotationSession& session,
                                                            const AnnotationCategories& categories, bool enabled,
                                                            std::size_t category_index);

[[nodiscard]] AnnotationToolMutation handle_skeleton_canvas_click(const AnnotationToolContext& context,
                                                                  const AnnotationToolCanvasClickEvent& event);
[[nodiscard]] AnnotationToolMutation handle_skeleton_action(const AnnotationToolContext& context,
                                                            const AnnotationToolActionEvent& event);

bool set_selected_skeleton_active_joint(AnnotationDocument& document, AnnotationSession& session,
                                        std::optional<std::size_t> joint_index);
[[nodiscard]] AnnotationSkeletonSidebarMutationResult apply_annotation_skeleton_sidebar_edit(
    AnnotationDocument& document, AnnotationSession& session, const AnnotationFrame* frame,
    const AnnotationCategories& categories, const AnnotationSkeletonSidebarEditRequest& request);

}  // namespace mmltk::gui::tool_detail
