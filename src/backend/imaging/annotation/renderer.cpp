module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
module mmltk.backend.imaging.annotation.renderer;
namespace mmltk::backend::imaging::annotation {
namespace {
[[nodiscard]] AnnotationPoint clamp_capture_point_to_bounds(const AnnotationFrame& frame, const AnnotationPoint& point) {
 const float max_x = annotation_frame_capture_width(frame) == 0U ? 0.0F : static_cast<float>(annotation_frame_capture_width(frame) - 1U);
 const float max_y = annotation_frame_capture_height(frame) == 0U ? 0.0F : static_cast<float>(annotation_frame_capture_height(frame) - 1U);
 return {
  std::clamp(point.x, 0.0F, max_x),
  std::clamp(point.y, 0.0F, max_y),
 };
}
[[nodiscard]] std::optional<AnnotationPoint> default_spline_handle_capture_point(const AnnotationFrame& frame, const AnnotationSplineShape& spline,
                                                                                 const std::size_t knot_index, const AnnotationHandleRole role) {
 if ((role != AnnotationHandleRole::SplineInHandle && role != AnnotationHandleRole::SplineOutHandle) || knot_index >= spline.knots.size() ||
     spline.knots.size() < 2U) {
  return std::nullopt;
 }
 const AnnotationSplineKnot& knot = spline.knots[knot_index];
 const auto mirrored_from_handle = [&](const AnnotationSplineHandle& opposite) -> std::optional<AnnotationPoint> {
  if (!opposite.enabled) return std::nullopt;
  const float vx = opposite.position.x - knot.position.x;
  const float vy = opposite.position.y - knot.position.y;
  if (std::abs(vx) <= 0.001F && std::abs(vy) <= 0.001F) { return std::nullopt; }
  return clamp_capture_point_to_bounds(frame, {knot.position.x - vx, knot.position.y - vy});
 };
 if (role == AnnotationHandleRole::SplineInHandle) {
  if (const auto mirrored = mirrored_from_handle(knot.out_handle); mirrored.has_value()) { return mirrored; }
 } else if (const auto mirrored = mirrored_from_handle(knot.in_handle); mirrored.has_value()) {
  return mirrored;
 }
 const std::optional<std::size_t> previous = knot_index > 0U ? std::optional<std::size_t>{knot_index - 1U}
                                             : spline.closed ? std::optional<std::size_t>{spline.knots.size() - 1U}
                                                             : std::nullopt;
 const std::optional<std::size_t> next = knot_index + 1U < spline.knots.size() ? std::optional<std::size_t>{knot_index + 1U}
                                         : spline.closed                       ? std::optional<std::size_t>{0U}
                                                                               : std::nullopt;
 if ((role == AnnotationHandleRole::SplineInHandle && !previous.has_value()) || (role == AnnotationHandleRole::SplineOutHandle && !next.has_value())) {
  return std::nullopt;
 }
 float tangent_x = 0.0F;
 float tangent_y = 0.0F;
 float handle_length = 0.0F;
 if (previous.has_value() && next.has_value()) {
  const AnnotationPoint& before = spline.knots[*previous].position;
  const AnnotationPoint& after = spline.knots[*next].position;
  tangent_x = after.x - before.x;
  tangent_y = after.y - before.y;
  handle_length =
   std::min(std::hypot(knot.position.x - before.x, knot.position.y - before.y), std::hypot(after.x - knot.position.x, after.y - knot.position.y)) / 3.0F;
 } else if (next.has_value()) {
  const AnnotationPoint& after = spline.knots[*next].position;
  tangent_x = after.x - knot.position.x;
  tangent_y = after.y - knot.position.y;
  handle_length = std::hypot(tangent_x, tangent_y) / 3.0F;
 } else {
  const AnnotationPoint& before = spline.knots[*previous].position;
  tangent_x = knot.position.x - before.x;
  tangent_y = knot.position.y - before.y;
  handle_length = std::hypot(tangent_x, tangent_y) / 3.0F;
 }
 const float tangent_length = std::hypot(tangent_x, tangent_y);
 if (tangent_length <= 0.001F || handle_length <= 0.001F) { return std::nullopt; }
 const float direction = role == AnnotationHandleRole::SplineInHandle ? -1.0F : 1.0F;
 return clamp_capture_point_to_bounds(
  frame, {knot.position.x + tangent_x / tangent_length * handle_length * direction, knot.position.y + tangent_y / tangent_length * handle_length * direction});
}
void push_handle(AnnotationProjectedScene& scene, const std::size_t selected_object_index, const std::size_t index, const AnnotationHandleRole role,
                 const std::size_t category_index, const AnnotationPoint& capture_point, const AnnotationPoint& frame_point,
                 const AnnotationPoint& tether_frame_point = {}, const bool has_tether = false, const bool materialized = true) {
 scene.editable_handles.push_back({
  {selected_object_index, index, role},
  category_index,
  capture_point,
  frame_point,
  tether_frame_point,
  has_tether,
  materialized,
 });
}
struct VisibleSkeletonTopology {
 std::vector<std::size_t> node_indices;
 std::vector<AnnotationSkeletonEdge> edges;
};
[[nodiscard]] VisibleSkeletonTopology visible_skeleton_topology(const AnnotationSkeletonShape& shape) {
 VisibleSkeletonTopology topology;
 std::vector<std::optional<std::size_t>> node_remap(shape.nodes.size());
 topology.node_indices.reserve(shape.nodes.size());
 for (std::size_t index = 0; index < shape.nodes.size(); ++index) {
  if (!shape.nodes[index].visible) continue;
  node_remap[index] = topology.node_indices.size();
  topology.node_indices.push_back(index);
 }
 topology.edges.reserve(shape.edges.size());
 for (const AnnotationSkeletonEdge& edge : shape.edges) {
  if (edge.source_index >= node_remap.size() || edge.target_index >= node_remap.size()) { continue; }
  const auto& source = node_remap[edge.source_index];
  const auto& target = node_remap[edge.target_index];
  if (source.has_value() && target.has_value()) { topology.edges.push_back({*source, *target}); }
 }
 return topology;
}
[[nodiscard]] AnnotationVisibleGeometry build_visible_geometry(const AnnotationFrame& frame, const AnnotationObject& object) {
 AnnotationVisibleGeometry geometry;
 std::visit(
  [&](const auto& shape) {
   using Shape = std::decay_t<decltype(shape)>;
   if constexpr (std::is_same_v<Shape, AnnotationPointShape>) {
    geometry.frame_points.push_back(annotation_capture_point_to_frame_unclipped(frame, shape.point));
   } else if constexpr (std::is_same_v<Shape, AnnotationSplineShape>) {
    geometry.frame_points = sample_annotation_spline_points(shape);
    for (AnnotationPoint& point : geometry.frame_points) { point = annotation_capture_point_to_frame_unclipped(frame, point); }
   } else if constexpr (std::is_same_v<Shape, AnnotationSkeletonShape>) {
    VisibleSkeletonTopology topology = visible_skeleton_topology(shape);
    geometry.frame_points.reserve(topology.node_indices.size());
    for (const std::size_t node_index : topology.node_indices) {
     const AnnotationPoint& point = shape.nodes[node_index].point;
     geometry.frame_points.push_back(annotation_capture_point_to_frame_unclipped(frame, point));
    }
    geometry.edges = std::move(topology.edges);
   }
  },
  object.shape);
 return geometry;
}
void project_selected_handles(const AnnotationFrame& frame, const AnnotationSceneView& document, const std::optional<std::size_t> selected_object_index,
                              AnnotationProjectedScene& scene) {
 scene.editable_handles.clear();
 if (!selected_object_index.has_value()) return;
 const AnnotationObject* object = document.object(*selected_object_index);
 if (object == nullptr) {
  scene.selected_object_index.reset();
  return;
 }
 const std::size_t selected_index = *selected_object_index;
 std::visit(
  [&](const auto& shape) {
   using Shape = std::decay_t<decltype(shape)>;
   if constexpr (std::is_same_v<Shape, AnnotationPointShape>) {
    push_handle(scene, selected_index, 0U, AnnotationHandleRole::Point, object->category_index, shape.point,
                annotation_capture_point_to_frame_unclipped(frame, shape.point));
   } else if constexpr (std::is_same_v<Shape, AnnotationSplineShape>) {
    scene.editable_handles.reserve(shape.knots.size() * 5U);
    for (std::size_t index = 0; index < shape.knots.size(); ++index) {
     const AnnotationSplineKnot& knot = shape.knots[index];
     push_handle(scene, selected_index, index, AnnotationHandleRole::SplineKnot, object->category_index, knot.position,
                 annotation_capture_point_to_frame_unclipped(frame, knot.position));
     const auto push_spline_handle = [&](const AnnotationSplineHandle& handle, const AnnotationHandleRole role) {
      std::optional<AnnotationPoint> point;
      if (handle.enabled) {
       point = handle.position;
      } else {
       point = default_spline_handle_capture_point(frame, shape, index, role);
      }
      if (!point.has_value()) return;
      push_handle(scene, selected_index, index, role, object->category_index, *point, annotation_capture_point_to_frame_unclipped(frame, *point),
                  annotation_capture_point_to_frame_unclipped(frame, knot.position), true, handle.enabled);
     };
     push_spline_handle(knot.in_handle, AnnotationHandleRole::SplineInHandle);
     push_spline_handle(knot.out_handle, AnnotationHandleRole::SplineOutHandle);
    }
   } else if constexpr (std::is_same_v<Shape, AnnotationSkeletonShape>) {
    scene.editable_handles.reserve(shape.nodes.size());
    for (std::size_t index = 0; index < shape.nodes.size(); ++index) {
     const AnnotationSkeletonNode& node = shape.nodes[index];
     if (!node.visible) continue;
     push_handle(scene, selected_index, index, AnnotationHandleRole::SkeletonNode, object->category_index, node.point,
                 annotation_capture_point_to_frame_unclipped(frame, node.point));
    }
   }
  },
  object->shape);
}
}  // namespace
AnnotationProjectedScene build_annotation_projected_scene(const AnnotationFrame& frame, const AnnotationSceneView& document,
                                                          std::optional<std::size_t> selected_object_index) {
 if (selected_object_index.has_value() && *selected_object_index >= document.size()) { selected_object_index.reset(); }
 AnnotationProjectedScene scene{
  .document_generation = document.generation,
  .selected_object_index = selected_object_index,
  .visible_objects = {},
  .editable_handles = {},
 };
 const std::uint32_t capture_width = annotation_frame_capture_width(frame);
 const std::uint32_t capture_height = annotation_frame_capture_height(frame);
 scene.visible_objects.reserve(document.size());
 for (std::size_t index = 0; index < document.size(); ++index) {
  const AnnotationObject* object = document.object(index);
  if (object == nullptr || !object->enabled) continue;
  const std::optional<AnnotationBox> object_box = annotation_object_display_box(*object);
  if (!object_box.has_value()) continue;
  const AnnotationBox capture_box = normalize_annotation_box(*object_box, capture_width, capture_height);
  if (!annotation_box_has_area(capture_box)) continue;
  const AnnotationBox frame_box = annotation_box_to_frame(frame, capture_box);
  if (!annotation_box_has_area(frame_box)) continue;
  scene.visible_objects.push_back({
   index,
   capture_box,
   frame_box,
   annotation_box_from_frame(frame, frame_box) == capture_box,
   build_visible_geometry(frame, *object),
  });
 }
 project_selected_handles(frame, document, selected_object_index, scene);
 return scene;
}
AnnotationProjectedScene refresh_annotation_projected_scene_selection(const AnnotationFrame& frame, const AnnotationSceneView& document,
                                                                      AnnotationProjectedScene scene, std::optional<std::size_t> selected_object_index) {
 if (selected_object_index.has_value() && *selected_object_index >= document.size()) { selected_object_index.reset(); }
 scene.selected_object_index = selected_object_index;
 project_selected_handles(frame, document, selected_object_index, scene);
 return scene;
}
AnnotationProjectedBox project_annotation_capture_box(const AnnotationFrame& frame, const AnnotationBox& capture_box) {
 const AnnotationBox normalized = normalize_annotation_box(capture_box, annotation_frame_capture_width(frame), annotation_frame_capture_height(frame));
 const AnnotationBox projected = annotation_box_to_frame(frame, normalized);
 if (!annotation_box_has_area(projected)) return {};
 return {
  projected,
  annotation_box_from_frame(frame, projected) == normalized,
 };
}
}  // namespace mmltk::backend::imaging::annotation
