#include "gui/annotation/sidebar_actions.h"

namespace mmltk::gui {

AnnotationSidebarMutationResult apply_annotation_objects_tab_actions(const AnnotationObjectsTabApplyContext& context,
                                                                     const AnnotationSidebarViewModel& sidebar_model,
                                                                     const AnnotationObjectsTabActions& actions) {
    AnnotationObjectsTabApplyContext apply_context = context;
    apply_context.preferred_new_object_category_index = sidebar_model.preferred_new_object_category_index;
    return apply_annotation_selected_object_sidebar_payload(apply_context, actions);
}

AnnotationSidebarMutationResult apply_annotation_mask_tab_actions(const AnnotationMaskTabApplyContext& context,
                                                                  const AnnotationMaskTabActions& actions) {
    return apply_annotation_mask_sidebar_edit(context, actions);
}

}  
