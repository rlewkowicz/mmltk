#include "gui/annotation/workspace/model.h"

#include "gui/annotation/editor_history.h"
#include "gui/annotation/projected_scene_cache.h"
#include "gui/annotation/tools/skeleton_tool_helpers.h"
#include "gui/annotation/tools/spline_tool_helpers.h"
#include "gui/annotation_core.h"
#include "gui/canvas_layers.h"

#include <algorithm>
#include <type_traits>
#include <utility>

namespace mmltk::gui {

AnnotationWorkspaceViewModel AnnotationWorkspaceModelBuilder::build(const AnnotationFrame& frame,
                                                                    const AnnotationDocument& document,
                                                                    const AnnotationSession& session,
                                                                    const SourceSelectionState& source,
                                                                    const bool include_crop_box) {
    const std::optional<std::size_t> selected_object_index = normalize_selected_object_index(document, session);
    return build(frame, document, source, include_crop_box,
                 refresh_annotation_projected_scene(frame, document, {}, selected_object_index));
}

AnnotationWorkspaceViewModel AnnotationWorkspaceModelBuilder::build(
    const AnnotationFrame& frame, const AnnotationDocument& document, const SourceSelectionState& source,
    const bool include_crop_box, std::shared_ptr<const AnnotationProjectedScene> projected_scene) {
    AnnotationWorkspaceViewModel model;
    if (projected_scene == nullptr) {
        projected_scene = refresh_annotation_projected_scene(frame, document, {}, std::nullopt);
    }
    const AnnotationProjectedScene& scene = *projected_scene;
    model.projected_scene = std::move(projected_scene);
    model.selection.selected_object_index = scene.selected_object_index;

    if (model.selection.selected_object_index.has_value()) {
        const auto visible_it = std::ranges::find_if(scene.visible_objects, [&](const AnnotationVisibleObject& object) {
            return object.index == *model.selection.selected_object_index;
        });
        if (visible_it != scene.visible_objects.end()) {
            model.selection.selected_capture_box = visible_it->capture_box;
            model.selection.selected_frame_box = visible_it->frame_box;
            model.selection.selected_box_fully_visible = visible_it->fully_visible;
        } else if (const AnnotationObject* object = document.object(*model.selection.selected_object_index);
                   object != nullptr) {
            const std::optional<AnnotationBox> selected_box = annotation_object_display_box(*object);
            if (selected_box.has_value()) {
                const AnnotationBox normalized_box = normalize_annotation_box(
                    *selected_box, annotation_frame_capture_width(frame), annotation_frame_capture_height(frame));
                if (annotation_box_has_area(normalized_box)) {
                    model.selection.selected_capture_box = normalized_box;
                    const AnnotationProjectedBox projected =
                        AnnotationRenderer::project_capture_box(frame, normalized_box);
                    model.selection.selected_frame_box = projected.frame_box;
                    model.selection.selected_box_fully_visible = projected.fully_visible;
                }
            }
        }
    }

    if (include_crop_box) {
        model.crop_frame_box =
            AnnotationRenderer::project_capture_box(frame, resolved_video_crop_box(source)).frame_box;
    }

    return model;
}

AnnotationSidebarViewModel AnnotationSidebarModelBuilder::build(const AnnotationDocument& document,
                                                                const AnnotationCategories& categories,
                                                                const AnnotationSession& session,
                                                                const bool has_annotation_frame) {
    AnnotationSidebarViewModel model;
    model.classes.reserve(categories.items.size());
    for (const AnnotationCategory& category : categories.items) {
        model.classes.push_back(AnnotationSidebarClassViewModel{
            category.id,
            category.name,
        });
    }
    model.has_classes = !model.classes.empty();
    model.can_undo = document.can_undo();
    model.can_redo = document.can_redo();

    model.selected_object_index = normalize_selected_object_index(document, session);
    if (model.selected_object_index.has_value()) {
        if (const AnnotationObject* selected_object = document.object(*model.selected_object_index);
            selected_object != nullptr) {
            model.preferred_new_object_category_index = selected_object->category_index;
        }
    }
    if (!categories.items.empty()) {
        model.preferred_new_object_category_index =
            std::min(model.preferred_new_object_category_index, categories.items.size() - 1U);
    } else {
        model.preferred_new_object_category_index = 0U;
    }

    model.objects.reserve(document.size());
    for (std::size_t index = 0; index < document.size(); ++index) {
        const AnnotationObject* object = document.object(index);
        if (object == nullptr) {
            continue;
        }
        const std::string class_name = object->category_index < categories.items.size()
                                           ? categories.items[object->category_index].name
                                           : std::string("unassigned");
        model.objects.push_back(AnnotationSidebarObjectViewModel{
            index,
            "#" + std::to_string(index + 1U) + " " + class_name + " [" + annotation_object_shape_label(*object) + "]",
            model.selected_object_index.has_value() && *model.selected_object_index == index,
        });
    }

    if (model.selected_object_index.has_value()) {
        if (const AnnotationObject* selected_object = document.object(*model.selected_object_index);
            selected_object != nullptr) {
            AnnotationSidebarSelectedObjectViewModel selected_model;
            selected_model.enabled = selected_object->enabled;
            selected_model.category_index = selected_object->category_index;
            selected_model.shape_label = annotation_object_shape_label(*selected_object);
            selected_model.display_box = annotation_object_display_box(*selected_object);
            selected_model.supports_mask_editing = annotation_object_supports_mask_editing(*selected_object);
            selected_model.sup = selected_object->sup;
            selected_model.nosup = selected_object->nosup;
            std::visit(
                [&](const auto& shape) {
                    using T = std::decay_t<decltype(shape)>;
                    if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                        selected_model.point = shape.point;
                    } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                        selected_model.spline = tool_detail::make_selected_spline_sidebar_view_model(
                            session, *model.selected_object_index, shape);
                    } else if constexpr (std::is_same_v<T, AnnotationSkeletonShape>) {
                        selected_model.skeleton = tool_detail::make_selected_skeleton_sidebar_view_model(
                            session, *model.selected_object_index, shape);
                    }
                },
                selected_object->shape);
            model.selected_object = std::move(selected_model);
        }
    }

    const bool box_creation_armed =
        session.active_tool() == AnnotationToolKind::Box || session.create_drag_session().active;
    model.has_selected_object = model.selected_object.has_value();
    model.can_create_objects = has_annotation_frame && !box_creation_armed;
    model.can_delete_selected = model.selected_object_index.has_value();
    model.can_edit_selected_mask = model.selected_object.has_value() && model.selected_object->supports_mask_editing;
    model.can_redraw_selected_box = has_annotation_frame && !box_creation_armed && model.can_edit_selected_mask;

    return model;
}

}  
