#pragma once

#include "gui/annotation/document/document.h"
#include "gui/annotation/render/renderer.h"
#include "gui/source_selection.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mmltk::gui {

struct AnnotationCategories;

struct AnnotationWorkspaceSelectionModel {
    std::optional<std::size_t> selected_object_index;
    std::optional<AnnotationBox> selected_capture_box;
    std::optional<AnnotationBox> selected_frame_box;
    bool selected_box_fully_visible = false;
};

struct AnnotationWorkspaceViewModel {
    AnnotationWorkspaceSelectionModel selection;
    std::shared_ptr<const AnnotationProjectedScene> projected_scene;
    std::optional<AnnotationBox> crop_frame_box;
};

struct AnnotationSidebarClassViewModel {
    int id = 0;
    std::string name;
};

struct AnnotationSidebarObjectViewModel {
    std::size_t index = 0;
    std::string label;
    bool selected = false;
};

struct AnnotationSidebarSelectedSplineViewModel {
    std::size_t knot_count = 0;
    std::size_t segment_count = 0;
    bool closed = false;
    std::optional<std::size_t> active_knot_index;
    std::optional<std::size_t> active_segment_index;
    std::optional<AnnotationSplineHandleMode> active_handle_mode;
    bool close_intent = false;
    bool reopen_requested = false;
    bool can_close = false;
    bool can_reopen = false;
    bool can_insert_active_segment_knot = false;
    bool can_delete_active_knot = false;
    bool can_edit_active_handle_mode = false;
    bool can_cancel_edit = false;
};

struct AnnotationSidebarSkeletonJointViewModel {
    std::size_t index = 0;
    std::string label;
    bool visible = false;
    bool active = false;
};

struct AnnotationSidebarSelectedSkeletonViewModel {
    std::size_t visible_joint_count = 0;
    std::size_t total_joint_count = 0;
    std::optional<std::size_t> active_joint_index;
    std::optional<std::string> active_joint_key;
    std::optional<std::size_t> next_joint_index;
    std::optional<std::string> next_joint_key;
    bool active_joint_visible = false;
    bool reseed_requested = false;
    bool can_skip_joint = false;
    bool can_hide_active_joint = false;
    bool can_reactivate_active_joint = false;
    bool can_reseed_active_joint = false;
    bool can_cancel_edit = false;
    std::vector<AnnotationSidebarSkeletonJointViewModel> joints;
};

struct AnnotationSidebarSelectedObjectViewModel {
    bool enabled = true;
    std::size_t category_index = 0;
    std::string shape_label;
    std::optional<AnnotationBox> display_box;
    bool supports_mask_editing = false;
    AnnotationColorRange sup{};
    AnnotationColorRange nosup{};
    std::optional<AnnotationPoint> point;
    std::optional<AnnotationSidebarSelectedSplineViewModel> spline;
    std::optional<AnnotationSidebarSelectedSkeletonViewModel> skeleton;
};

struct AnnotationSidebarViewModel {
    std::size_t preferred_new_object_category_index = 0;
    bool has_classes = false;
    bool can_undo = false;
    bool can_redo = false;
    bool can_create_objects = false;
    bool can_delete_selected = false;
    bool can_redraw_selected_box = false;
    bool can_edit_selected_mask = false;
    bool has_selected_object = false;
    std::vector<AnnotationSidebarClassViewModel> classes;
    std::vector<AnnotationSidebarObjectViewModel> objects;
    std::optional<std::size_t> selected_object_index;
    std::optional<AnnotationSidebarSelectedObjectViewModel> selected_object;
};

class AnnotationWorkspaceModelBuilder {
public:
    static AnnotationWorkspaceViewModel build(const AnnotationFrame& frame,
                                              const AnnotationDocument& document,
                                              const AnnotationSession& session,
                                              const SourceSelectionState& source,
                                              bool include_crop_box);
    static AnnotationWorkspaceViewModel build(const AnnotationFrame& frame,
                                              const AnnotationDocument& document,
                                              const SourceSelectionState& source,
                                              bool include_crop_box,
                                              std::shared_ptr<const AnnotationProjectedScene> projected_scene);
};

class AnnotationSidebarModelBuilder {
public:
    static AnnotationSidebarViewModel build(const AnnotationDocument& document,
                                            const AnnotationCategories& categories,
                                            const AnnotationSession& session,
                                            bool has_annotation_frame = true);
};

} // namespace mmltk::gui
