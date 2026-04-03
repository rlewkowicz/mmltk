#pragma once

#include "gui/annotation/sidebar_edits.h"
#include "gui/annotation/sidebar.h"

namespace mmltk::gui {

[[nodiscard]] AnnotationSelectedObjectSidebarEditRequest
make_annotation_selected_object_sidebar_edit_request(
    const AnnotationSidebarViewModel& sidebar_model,
    const AnnotationObjectsTabActions& actions,
    bool assist_available);

[[nodiscard]] AnnotationMaskSidebarEditRequest make_annotation_mask_sidebar_edit_request(
    const AnnotationMaskTabActions& actions);

[[nodiscard]] AnnotationSidebarMutationResult apply_annotation_objects_tab_actions(
    const AnnotationObjectsTabApplyContext& context,
    const AnnotationSidebarViewModel& sidebar_model,
    const AnnotationObjectsTabActions& actions);

[[nodiscard]] AnnotationSidebarMutationResult apply_annotation_mask_tab_actions(
    const AnnotationMaskTabApplyContext& context,
    const AnnotationMaskTabActions& actions);

} // namespace mmltk::gui
