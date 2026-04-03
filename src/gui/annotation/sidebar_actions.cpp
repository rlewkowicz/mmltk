#include "gui/annotation/sidebar_actions.h"

namespace mmltk::gui {

AnnotationSelectedObjectSidebarEditRequest
make_annotation_selected_object_sidebar_edit_request(
    const AnnotationSidebarViewModel& sidebar_model,
    const AnnotationObjectsTabActions& actions,
    const bool assist_available) {
    return AnnotationSelectedObjectSidebarEditRequest{
        actions.selected_object_index,
        sidebar_model.preferred_new_object_category_index,
        actions.request_add_class,
        actions.create_object_tool,
        actions.request_assist && assist_available,
        actions.request_undo,
        actions.request_redo,
        actions.request_delete_selected,
        actions.update_selected_metadata,
        actions.selected_enabled,
        actions.selected_category_index,
        actions.request_redraw_box,
        actions.request_insert_active_spline_knot,
        actions.update_spline_active_segment,
        actions.spline_active_segment_index,
        actions.spline_handle_mode,
        actions.spline_action,
        actions.update_skeleton_active_joint,
        actions.skeleton_active_joint_index,
        actions.skeleton_action,
    };
}

AnnotationMaskSidebarEditRequest make_annotation_mask_sidebar_edit_request(
    const AnnotationMaskTabActions& actions) {
    return AnnotationMaskSidebarEditRequest{
        actions.cleanup_radius,
        actions.request_redraw_box,
        actions.cleanup_op,
        actions.update_color_ranges,
        actions.sup,
        actions.nosup,
    };
}

AnnotationSidebarMutationResult apply_annotation_objects_tab_actions(
    const AnnotationObjectsTabApplyContext& context,
    const AnnotationSidebarViewModel& sidebar_model,
    const AnnotationObjectsTabActions& actions) {
    return apply_annotation_selected_object_sidebar_edit(
        context,
        make_annotation_selected_object_sidebar_edit_request(sidebar_model,
                                                             actions,
                                                             context.assist_available));
}

AnnotationSidebarMutationResult apply_annotation_mask_tab_actions(
    const AnnotationMaskTabApplyContext& context,
    const AnnotationMaskTabActions& actions) {
    return apply_annotation_mask_sidebar_edit(
        context,
        make_annotation_mask_sidebar_edit_request(actions));
}

} // namespace mmltk::gui
