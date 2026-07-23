#pragma once

#include "gui/annotation/sidebar_edits.h"
#include "gui/annotation/sidebar.h"

namespace mmltk::gui {

[[nodiscard]] AnnotationSidebarMutationResult apply_annotation_objects_tab_actions(
    const AnnotationObjectsTabApplyContext& context, const AnnotationSidebarViewModel& sidebar_model,
    const AnnotationObjectsTabActions& actions);

[[nodiscard]] AnnotationSidebarMutationResult apply_annotation_mask_tab_actions(
    const AnnotationMaskTabApplyContext& context, const AnnotationMaskTabActions& actions);

}  
