#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "src/controller/contracts/annotation.h"
#include "src/controller/subsystems/annotation/detail/annotation_mask.h"

namespace mmltk::controller {

struct AnnotationDragPreview final {
    contracts::AnnotationPointerTarget target;
    contracts::AnnotationPoint origin;
    contracts::AnnotationPoint point;
    bool operator==(const AnnotationDragPreview&) const = default;
};
void materialize_annotation_drag(contracts::AnnotationObject&, const AnnotationDragPreview&, const contracts::AnnotationSceneContent&,
                                 subsystems::annotation::MaskScratch&);
[[nodiscard]] contracts::AnnotationSplineKnot annotation_drag_knot(contracts::AnnotationSplineKnot, const AnnotationDragPreview&,
                                                                   const contracts::AnnotationSceneContent&);
// One retained description has exclusive input custody while being filled and
// exclusive renderer custody from submission through GPU settlement.
struct AnnotationRenderState final {
    std::shared_ptr<const contracts::AnnotationSceneContent> scene;
    std::shared_ptr<const std::vector<contracts::AnnotationObjectIdentity>> identities;
    contracts::AnnotationEditorFacts editor{};
    std::uint64_t scene_revision = 0U;
    mutable contracts::AnnotationObject preview{};
    std::optional<AnnotationDragPreview> drag;
    std::optional<subsystems::annotation::MaskRows> brush_rows;
    mutable bool preview_materialized = false;
    mutable subsystems::annotation::MaskScratch preview_scratch;
    std::optional<std::size_t> preview_object;
    std::uint64_t preview_identity = 0U;
    std::uint64_t generation = 0U;
    std::uint64_t document_epoch = 0U;

    [[nodiscard]] std::size_t ObjectCount() const noexcept {
        return scene->objects.size() + (preview_object == scene->objects.size() ? 1U : 0U);
    }
    [[nodiscard]] const contracts::AnnotationObject& ObjectAt(const std::size_t index) const {
        if (preview_object != index) return scene->objects.at(index);
        if (!preview_materialized) {
            if (drag) {
                preview = scene->objects.at(index);
                materialize_annotation_drag(preview, *drag, *scene, preview_scratch);
            }
            if (brush_rows) brush_rows->Materialize(preview);
            preview_materialized = true;
        }
        return preview;
    }
    [[nodiscard]] bool TransformsMask(std::size_t index) const noexcept {
        return drag && preview_object == index && scene->objects[index].shape == contracts::AnnotationShape::Mask;
    }
    [[nodiscard]] const contracts::AnnotationObject& DrawingObjectAt(std::size_t index) const {
        return drag && preview_object == index ? scene->objects[index] : ObjectAt(index);
    }
    [[nodiscard]] contracts::AnnotationBox TargetBox(std::size_t index) const {
        contracts::AnnotationObject bounds;
        bounds.box = scene->objects[index].box;
        materialize_annotation_drag(bounds, *drag, *scene, preview_scratch);
        return bounds.box;
    }
    [[nodiscard]] contracts::AnnotationPoint DrawingPoint(std::size_t index) const {
        return drag && preview_object == index && drag->target.role == contracts::AnnotationHandleRole::Point
                   ? drag->point
                   : DrawingObjectAt(index).point;
    }
    [[nodiscard]] contracts::AnnotationSplineKnot DrawingKnot(std::size_t index, std::size_t knot) const {
        auto result = DrawingObjectAt(index).spline_knots[knot];
        if (drag && preview_object == index && drag->target.element == knot) result = annotation_drag_knot(result, *drag, *scene);
        return result;
    }
    [[nodiscard]] contracts::AnnotationPoint DrawingNode(std::size_t index, std::size_t node) const {
        return drag && preview_object == index && drag->target.role == contracts::AnnotationHandleRole::SkeletonNode &&
                       drag->target.element == node
                   ? drag->point
                   : DrawingObjectAt(index).skeleton_nodes[node].point;
    }
};

}  // namespace mmltk::controller
