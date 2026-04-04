#include "gui/annotation/render/renderer.h"

#include "gui/annotation_core.h"
#include "gui/annotation/document/types.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>
#include <utility>

namespace mmltk::gui {

namespace {

constexpr int kInteractionCropStrokeWidth = 2;
constexpr std::uint8_t kSelectedR = 255U;
constexpr std::uint8_t kSelectedG = 220U;
constexpr std::uint8_t kSelectedB = 96U;
constexpr std::uint8_t kHoveredR = 124U;
constexpr std::uint8_t kHoveredG = 198U;
constexpr std::uint8_t kHoveredB = 255U;
constexpr std::uint8_t kActiveHandleR = 255U;
constexpr std::uint8_t kActiveHandleG = 248U;
constexpr std::uint8_t kActiveHandleB = 236U;
constexpr std::uint8_t kHoveredHandleR = 255U;
constexpr std::uint8_t kHoveredHandleG = 220U;
constexpr std::uint8_t kHoveredHandleB = 96U;
constexpr std::uint8_t kLatentHandleR = 196U;
constexpr std::uint8_t kLatentHandleG = 204U;
constexpr std::uint8_t kLatentHandleB = 216U;
constexpr float kMinObjectHitRadiusPx = 8.0f;
constexpr float kMaxObjectHitRadiusPx = 20.0f;

struct AnnotationScreenPoint {
    float x = 0.0f;
    float y = 0.0f;
};

} // namespace

AnnotationPoint clamp_capture_point_to_bounds(const AnnotationFrame& frame,
                                              const AnnotationPoint& point) {
    const float max_x =
        annotation_frame_capture_width(frame) == 0U
            ? 0.0f
            : static_cast<float>(annotation_frame_capture_width(frame) - 1U);
    const float max_y =
        annotation_frame_capture_height(frame) == 0U
            ? 0.0f
            : static_cast<float>(annotation_frame_capture_height(frame) - 1U);
    return AnnotationPoint{
        std::clamp(point.x, 0.0f, max_x),
        std::clamp(point.y, 0.0f, max_y),
    };
}

std::optional<AnnotationPoint> default_spline_handle_capture_point(const AnnotationFrame& frame,
                                                                   const AnnotationSplineShape& spline,
                                                                   std::size_t knot_index,
                                                                   AnnotationHandleRole role) {
    if ((role != AnnotationHandleRole::SplineInHandle &&
         role != AnnotationHandleRole::SplineOutHandle) ||
        knot_index >= spline.knots.size() ||
        spline.knots.size() < 2U) {
        return std::nullopt;
    }

    const AnnotationSplineKnot& knot = spline.knots[knot_index];
    const auto mirrored_from_handle = [&](const AnnotationSplineHandle& opposite)
        -> std::optional<AnnotationPoint> {
        if (!opposite.enabled) {
            return std::nullopt;
        }
        const float vx = opposite.position.x - knot.position.x;
        const float vy = opposite.position.y - knot.position.y;
        if (std::abs(vx) <= 0.001f && std::abs(vy) <= 0.001f) {
            return std::nullopt;
        }
        return clamp_capture_point_to_bounds(
            frame,
            AnnotationPoint{
                knot.position.x - vx,
                knot.position.y - vy,
            });
    };

    if (role == AnnotationHandleRole::SplineInHandle) {
        if (const std::optional<AnnotationPoint> mirrored = mirrored_from_handle(knot.out_handle);
            mirrored.has_value()) {
            return mirrored;
        }
    } else {
        if (const std::optional<AnnotationPoint> mirrored = mirrored_from_handle(knot.in_handle);
            mirrored.has_value()) {
            return mirrored;
        }
    }

    const std::optional<std::size_t> prev_index =
        knot_index > 0U ? std::optional<std::size_t>{knot_index - 1U}
        : spline.closed ? std::optional<std::size_t>{spline.knots.size() - 1U}
                        : std::nullopt;
    const std::optional<std::size_t> next_index =
        knot_index + 1U < spline.knots.size() ? std::optional<std::size_t>{knot_index + 1U}
        : spline.closed ? std::optional<std::size_t>{0U}
                        : std::nullopt;

    if (role == AnnotationHandleRole::SplineInHandle && !prev_index.has_value()) {
        return std::nullopt;
    }
    if (role == AnnotationHandleRole::SplineOutHandle && !next_index.has_value()) {
        return std::nullopt;
    }

    float tangent_x = 0.0f;
    float tangent_y = 0.0f;
    float handle_length = 0.0f;
    if (prev_index.has_value() && next_index.has_value()) {
        const AnnotationPoint& prev = spline.knots[*prev_index].position;
        const AnnotationPoint& next = spline.knots[*next_index].position;
        tangent_x = next.x - prev.x;
        tangent_y = next.y - prev.y;
        handle_length = std::min(std::hypot(knot.position.x - prev.x, knot.position.y - prev.y),
                                 std::hypot(next.x - knot.position.x, next.y - knot.position.y)) /
                        3.0f;
    } else if (next_index.has_value()) {
        const AnnotationPoint& next = spline.knots[*next_index].position;
        tangent_x = next.x - knot.position.x;
        tangent_y = next.y - knot.position.y;
        handle_length = std::hypot(tangent_x, tangent_y) / 3.0f;
    } else if (prev_index.has_value()) {
        const AnnotationPoint& prev = spline.knots[*prev_index].position;
        tangent_x = knot.position.x - prev.x;
        tangent_y = knot.position.y - prev.y;
        handle_length = std::hypot(tangent_x, tangent_y) / 3.0f;
    }

    const float tangent_length = std::hypot(tangent_x, tangent_y);
    if (tangent_length <= 0.001f || handle_length <= 0.001f) {
        return std::nullopt;
    }

    const float direction = role == AnnotationHandleRole::SplineInHandle ? -1.0f : 1.0f;
    const float normalized_x = tangent_x / tangent_length;
    const float normalized_y = tangent_y / tangent_length;
    return clamp_capture_point_to_bounds(
        frame,
        AnnotationPoint{
            knot.position.x + normalized_x * handle_length * direction,
            knot.position.y + normalized_y * handle_length * direction,
        });
}

namespace {

bool boxes_equal(const AnnotationBox& lhs, const AnnotationBox& rhs) {
    return lhs.x1 == rhs.x1 &&
           lhs.y1 == rhs.y1 &&
           lhs.x2 == rhs.x2 &&
           lhs.y2 == rhs.y2;
}

void push_handle(AnnotationProjectedScene* scene,
                 const std::optional<std::size_t>& selected_object_index,
                 std::size_t index,
                 AnnotationHandleRole role,
                 std::size_t category_index,
                 const AnnotationPoint& capture_point,
                 const AnnotationPoint& frame_point,
                 const AnnotationPoint& tether_frame_point = {},
                 bool has_tether = false,
                 bool materialized = true) {
    scene->editable_handles.push_back(AnnotationEditableHandle{
        AnnotationHandleId{*selected_object_index, index, role},
        category_index,
        capture_point,
        frame_point,
        tether_frame_point,
        has_tether,
        materialized,
    });
}

bool box_contains_point(const AnnotationBox& box, const int x, const int y) {
    return annotation_box_has_area(box) &&
           x >= box.x1 && x < box.x2 &&
           y >= box.y1 && y < box.y2;
}

std::array<std::uint8_t, 3> category_color(const std::size_t index) {
    static constexpr std::array<std::array<std::uint8_t, 3>, 8> palette{{
        {{240, 196, 68}},
        {{88, 188, 255}},
        {{255, 128, 88}},
        {{96, 214, 146}},
        {{214, 112, 255}},
        {{255, 96, 152}},
        {{180, 214, 92}},
        {{255, 168, 64}},
    }};
    return palette[index % palette.size()];
}

std::array<std::uint8_t, 3> interaction_color(const bool selected,
                                              const bool hovered,
                                              const std::size_t category_index) {
    if (selected) {
        return {kSelectedR, kSelectedG, kSelectedB};
    }
    if (hovered) {
        return {kHoveredR, kHoveredG, kHoveredB};
    }
    return category_color(category_index);
}

void append_overlay_box(PreviewInteractionOverlaySnapshot& snapshot,
                        const AnnotationBox& box,
                        const std::uint8_t r,
                        const std::uint8_t g,
                        const std::uint8_t b,
                        const int thickness,
                        const bool draw_handles = false,
                        const int handle_radius = 4) {
    if (!annotation_box_has_area(box)) {
        return;
    }
    snapshot.boxes.push_back(PreviewInteractionOverlayBox{
        box,
        r,
        g,
        b,
        thickness,
        draw_handles,
        std::max(1, handle_radius),
    });
}

AnnotationPoint capture_point_to_frame_unclipped(const AnnotationFrame& frame,
                                                 const AnnotationPoint& point) {
    return AnnotationPoint{
        point.x - static_cast<float>(frame.view_x),
        point.y - static_cast<float>(frame.view_y),
    };
}

PreviewInteractionOverlayPoint to_preview_overlay_point(const AnnotationPoint& point) {
    return PreviewInteractionOverlayPoint{
        static_cast<int>(std::lround(point.x)),
        static_cast<int>(std::lround(point.y)),
    };
}

PreviewInteractionOverlayPoint capture_point_to_preview_overlay_point(const AnnotationFrame& frame,
                                                                     const AnnotationPoint& point) {
    return to_preview_overlay_point(capture_point_to_frame_unclipped(frame, point));
}

mmltk::live::ManualOverlayPoint to_overlay_point(const AnnotationPoint& point) {
    return mmltk::live::ManualOverlayPoint{
        static_cast<int>(std::lround(point.x)),
        static_cast<int>(std::lround(point.y)),
    };
}

std::vector<mmltk::live::ManualOverlayPoint> to_overlay_points(
    const std::vector<AnnotationPoint>& points) {
    std::vector<mmltk::live::ManualOverlayPoint> overlay_points;
    overlay_points.reserve(points.size());
    for (const AnnotationPoint& point : points) {
        overlay_points.push_back(to_overlay_point(point));
    }
    return overlay_points;
}

std::vector<AnnotationPoint> capture_points_to_frame_unclipped(
    const AnnotationFrame& frame,
    const std::vector<AnnotationPoint>& points) {
    std::vector<AnnotationPoint> frame_points;
    frame_points.reserve(points.size());
    for (const AnnotationPoint& point : points) {
        frame_points.push_back(capture_point_to_frame_unclipped(frame, point));
    }
    return frame_points;
}

std::vector<PreviewInteractionOverlayPoint> frame_points_to_preview_overlay_points(
    const std::vector<AnnotationPoint>& points) {
    std::vector<PreviewInteractionOverlayPoint> overlay_points;
    overlay_points.reserve(points.size());
    for (const AnnotationPoint& point : points) {
        overlay_points.push_back(to_preview_overlay_point(point));
    }
    return overlay_points;
}

std::vector<PreviewInteractionOverlayEdge> to_preview_overlay_edges(
    const std::vector<AnnotationSkeletonEdge>& edges) {
    std::vector<PreviewInteractionOverlayEdge> overlay_edges;
    overlay_edges.reserve(edges.size());
    for (const AnnotationSkeletonEdge& edge : edges) {
        overlay_edges.push_back(PreviewInteractionOverlayEdge{
            static_cast<std::uint32_t>(edge.source_index),
            static_cast<std::uint32_t>(edge.target_index),
        });
    }
    return overlay_edges;
}

std::vector<mmltk::live::ManualOverlayEdge> to_overlay_edges(
    const std::vector<AnnotationSkeletonEdge>& edges) {
    std::vector<mmltk::live::ManualOverlayEdge> overlay_edges;
    overlay_edges.reserve(edges.size());
    for (const AnnotationSkeletonEdge& edge : edges) {
        overlay_edges.push_back(mmltk::live::ManualOverlayEdge{
            static_cast<std::uint32_t>(edge.source_index),
            static_cast<std::uint32_t>(edge.target_index),
        });
    }
    return overlay_edges;
}

AnnotationVisibleGeometry build_visible_geometry(const AnnotationFrame& frame,
                                                 const AnnotationObject& object) {
    AnnotationVisibleGeometry geometry;
    std::visit(
        [&](const auto& shape) {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                geometry.capture_points.push_back(shape.point);
                geometry.frame_points.push_back(capture_point_to_frame_unclipped(frame, shape.point));
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                geometry.closed = shape.closed;
                geometry.capture_points = sample_annotation_spline_points(shape);
                geometry.frame_points =
                    capture_points_to_frame_unclipped(frame, geometry.capture_points);
                geometry.control_preview_points.reserve(shape.knots.size());
                geometry.handle_preview_points.reserve(shape.knots.size() * 2U);
                geometry.latent_handle_preview_points.reserve(shape.knots.size() * 2U);
                geometry.handle_preview_segments.reserve(shape.knots.size() * 2U);
                geometry.latent_handle_preview_segments.reserve(shape.knots.size() * 2U);
                for (std::size_t knot_index = 0; knot_index < shape.knots.size(); ++knot_index) {
                    const AnnotationSplineKnot& knot = shape.knots[knot_index];
                    const PreviewInteractionOverlayPoint knot_point =
                        capture_point_to_preview_overlay_point(frame, knot.position);
                    geometry.control_preview_points.push_back(knot_point);

                    const auto append_materialized_handle =
                        [&](const AnnotationSplineHandle& handle) {
                            const PreviewInteractionOverlayPoint handle_point =
                                capture_point_to_preview_overlay_point(frame, handle.position);
                            geometry.handle_preview_points.push_back(handle_point);
                            geometry.handle_preview_segments.emplace_back(knot_point, handle_point);
                        };
                    const auto append_latent_handle =
                        [&](const AnnotationHandleRole role) {
                            const std::optional<AnnotationPoint> latent_handle =
                                default_spline_handle_capture_point(frame,
                                                                    shape,
                                                                    knot_index,
                                                                    role);
                            if (!latent_handle.has_value()) {
                                return;
                            }
                            const PreviewInteractionOverlayPoint handle_point =
                                capture_point_to_preview_overlay_point(frame, *latent_handle);
                            geometry.latent_handle_preview_points.push_back(handle_point);
                            geometry.latent_handle_preview_segments.emplace_back(knot_point, handle_point);
                        };

                    if (knot.in_handle.enabled) {
                        append_materialized_handle(knot.in_handle);
                    } else {
                        append_latent_handle(AnnotationHandleRole::SplineInHandle);
                    }
                    if (knot.out_handle.enabled) {
                        append_materialized_handle(knot.out_handle);
                    } else {
                        append_latent_handle(AnnotationHandleRole::SplineOutHandle);
                    }
                }
            } else if constexpr (std::is_same_v<T, AnnotationSkeletonShape>) {
                std::vector<std::optional<std::size_t>> node_remap(shape.nodes.size());
                geometry.capture_points.reserve(shape.nodes.size());
                geometry.frame_points.reserve(shape.nodes.size());
                for (std::size_t index = 0; index < shape.nodes.size(); ++index) {
                    const AnnotationSkeletonNode& node = shape.nodes[index];
                    if (!node.visible) {
                        continue;
                    }
                    node_remap[index] = geometry.capture_points.size();
                    geometry.capture_points.push_back(node.point);
                    geometry.frame_points.push_back(capture_point_to_frame_unclipped(frame, node.point));
                }
                geometry.edges.reserve(shape.edges.size());
                for (const AnnotationSkeletonEdge& edge : shape.edges) {
                    if (edge.source_index >= node_remap.size() ||
                        edge.target_index >= node_remap.size()) {
                        continue;
                    }
                    const std::optional<std::size_t>& source_index = node_remap[edge.source_index];
                    const std::optional<std::size_t>& target_index = node_remap[edge.target_index];
                    if (!source_index.has_value() || !target_index.has_value()) {
                        continue;
                    }
                    geometry.edges.push_back(AnnotationSkeletonEdge{
                        *source_index,
                        *target_index,
                    });
                }
            }
        },
        object.shape);
    if (!geometry.frame_points.empty()) {
        geometry.preview_points =
            frame_points_to_preview_overlay_points(geometry.frame_points);
    }
    if (!geometry.capture_points.empty()) {
        geometry.manual_points = to_overlay_points(geometry.capture_points);
    }
    if (!geometry.edges.empty()) {
        geometry.preview_edges = to_preview_overlay_edges(geometry.edges);
        geometry.manual_edges = to_overlay_edges(geometry.edges);
    }
    return geometry;
}

void append_selected_object_editable_handles(const AnnotationFrame& frame,
                                            const AnnotationDocument& document,
                                            const std::optional<std::size_t> selected_object_index,
                                            AnnotationProjectedScene* scene) {
    if (scene == nullptr || !selected_object_index.has_value()) {
        if (scene != nullptr) {
            scene->editable_handles.clear();
        }
        return;
    }
    scene->editable_handles.clear();
    const AnnotationObject* object = document.object(*selected_object_index);
    if (object == nullptr) {
        scene->selected_object_index.reset();
        return;
    }
    std::visit(
        [&](const auto& shape) {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                push_handle(scene, selected_object_index, 0U, AnnotationHandleRole::Point, object->category_index,
                            shape.point, capture_point_to_frame_unclipped(frame, shape.point));
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                scene->editable_handles.reserve(shape.knots.size() * 5U);
                for (std::size_t index = 0; index < shape.knots.size(); ++index) {
                    const AnnotationSplineKnot& knot = shape.knots[index];
                    push_handle(scene, selected_object_index, index, AnnotationHandleRole::SplineKnot,
                                object->category_index, knot.position, capture_point_to_frame_unclipped(frame, knot.position));
                    if (knot.in_handle.enabled) {
                        push_handle(scene, selected_object_index, index, AnnotationHandleRole::SplineInHandle,
                                    object->category_index, knot.in_handle.position,
                                    capture_point_to_frame_unclipped(frame, knot.in_handle.position),
                                    capture_point_to_frame_unclipped(frame, knot.position), true);
                    } else if (const std::optional<AnnotationPoint> latent_handle =
                                   default_spline_handle_capture_point(frame,
                                                                       shape,
                                                                       index,
                                                                       AnnotationHandleRole::SplineInHandle);
                               latent_handle.has_value()) {
                        push_handle(scene, selected_object_index, index, AnnotationHandleRole::SplineInHandle,
                                    object->category_index, *latent_handle,
                                    capture_point_to_frame_unclipped(frame, *latent_handle),
                                    capture_point_to_frame_unclipped(frame, knot.position), true, false);
                    }
                    if (knot.out_handle.enabled) {
                        push_handle(scene, selected_object_index, index, AnnotationHandleRole::SplineOutHandle,
                                    object->category_index, knot.out_handle.position,
                                    capture_point_to_frame_unclipped(frame, knot.out_handle.position),
                                    capture_point_to_frame_unclipped(frame, knot.position), true);
                    } else if (const std::optional<AnnotationPoint> latent_handle =
                                   default_spline_handle_capture_point(frame,
                                                                       shape,
                                                                       index,
                                                                       AnnotationHandleRole::SplineOutHandle);
                               latent_handle.has_value()) {
                        push_handle(scene, selected_object_index, index, AnnotationHandleRole::SplineOutHandle,
                                    object->category_index, *latent_handle,
                                    capture_point_to_frame_unclipped(frame, *latent_handle),
                                    capture_point_to_frame_unclipped(frame, knot.position), true, false);
                    }
                }
            } else if constexpr (std::is_same_v<T, AnnotationSkeletonShape>) {
                scene->editable_handles.reserve(shape.nodes.size());
                for (std::size_t index = 0; index < shape.nodes.size(); ++index) {
                    const AnnotationSkeletonNode& node = shape.nodes[index];
                    if (!node.visible) {
                        continue;
                    }
                    push_handle(scene, selected_object_index, index, AnnotationHandleRole::SkeletonNode,
                                object->category_index, node.point, capture_point_to_frame_unclipped(frame, node.point));
                }
            }
        },
        object->shape);
}

AnnotationProjectedScene rebuild_projected_scene_selection(
    const AnnotationFrame& frame,
    const AnnotationDocument& document,
    AnnotationProjectedScene scene,
    std::optional<std::size_t> selected_object_index) {
    if (selected_object_index.has_value() && *selected_object_index >= document.size()) {
        selected_object_index.reset();
    }
    scene.selected_object_index = selected_object_index;
    scene.editable_handles.clear();
    append_selected_object_editable_handles(frame, document, selected_object_index, &scene);
    return scene;
}

AnnotationScreenPoint frame_point_to_screen_point(const CanvasViewport& viewport,
                                                  const AnnotationPoint& point) {
    const float scale_x =
        viewport.image_width == 0U ? 1.0f : viewport.screen_width / static_cast<float>(viewport.image_width);
    const float scale_y =
        viewport.image_height == 0U ? 1.0f : viewport.screen_height / static_cast<float>(viewport.image_height);
    return AnnotationScreenPoint{
        viewport.screen_x + point.x * scale_x,
        viewport.screen_y + point.y * scale_y,
    };
}

float object_hit_radius_px(const CanvasViewport& viewport) {
    const float scale_x =
        viewport.image_width == 0U ? 1.0f : viewport.screen_width / static_cast<float>(viewport.image_width);
    const float scale_y =
        viewport.image_height == 0U ? 1.0f : viewport.screen_height / static_cast<float>(viewport.image_height);
    const float scale = std::max(0.0f, std::min(scale_x, scale_y));
    return std::clamp(scale * 4.0f, kMinObjectHitRadiusPx, kMaxObjectHitRadiusPx);
}

float squared_distance(const AnnotationScreenPoint& lhs,
                       const AnnotationScreenPoint& rhs) {
    const float dx = lhs.x - rhs.x;
    const float dy = lhs.y - rhs.y;
    return dx * dx + dy * dy;
}

float squared_distance_to_segment(const AnnotationScreenPoint& point,
                                  const AnnotationScreenPoint& start,
                                  const AnnotationScreenPoint& end) {
    const float segment_dx = end.x - start.x;
    const float segment_dy = end.y - start.y;
    const float segment_length_sq = segment_dx * segment_dx + segment_dy * segment_dy;
    if (segment_length_sq <= 0.0001f) {
        return squared_distance(point, start);
    }

    const float t = std::clamp(((point.x - start.x) * segment_dx + (point.y - start.y) * segment_dy) /
                                   segment_length_sq,
                               0.0f,
                               1.0f);
    const AnnotationScreenPoint projection{
        start.x + segment_dx * t,
        start.y + segment_dy * t,
    };
    return squared_distance(point, projection);
}

bool point_cloud_hit_test(const std::vector<AnnotationPoint>& frame_points,
                          const CanvasViewport& viewport,
                          const CanvasPointerState& pointer,
                          const float hit_radius_px) {
    if (frame_points.empty()) {
        return false;
    }
    const AnnotationScreenPoint pointer_point{pointer.screen_x, pointer.screen_y};
    const float hit_radius_sq = hit_radius_px * hit_radius_px;
    return std::ranges::any_of(frame_points,
                       [&](const AnnotationPoint& frame_point) {
                           return squared_distance(pointer_point,
                                                   frame_point_to_screen_point(viewport, frame_point)) <= hit_radius_sq;
                       });
}

bool polyline_hit_test(const std::vector<AnnotationPoint>& frame_points,
                       const bool closed,
                       const CanvasViewport& viewport,
                       const CanvasPointerState& pointer,
                       const float hit_radius_px) {
    if (frame_points.empty()) {
        return false;
    }
    if (frame_points.size() == 1U) {
        return point_cloud_hit_test(frame_points, viewport, pointer, hit_radius_px);
    }

    const AnnotationScreenPoint pointer_point{pointer.screen_x, pointer.screen_y};
    const float hit_radius_sq = hit_radius_px * hit_radius_px;
    const std::size_t segment_count =
        closed ? frame_points.size() : frame_points.size() - 1U;
    for (std::size_t index = 0; index < segment_count; ++index) {
        const AnnotationScreenPoint segment_start =
            frame_point_to_screen_point(viewport, frame_points[index]);
        const AnnotationScreenPoint segment_end =
            frame_point_to_screen_point(viewport, frame_points[(index + 1U) % frame_points.size()]);
        if (squared_distance_to_segment(pointer_point, segment_start, segment_end) <= hit_radius_sq) {
            return true;
        }
    }
    return false;
}

bool skeleton_hit_test(const AnnotationVisibleGeometry& geometry,
                       const CanvasViewport& viewport,
                       const CanvasPointerState& pointer,
                       const float hit_radius_px) {
    if (point_cloud_hit_test(geometry.frame_points, viewport, pointer, hit_radius_px)) {
        return true;
    }
    if (geometry.edges.empty() || geometry.frame_points.empty()) {
        return false;
    }

    const AnnotationScreenPoint pointer_point{pointer.screen_x, pointer.screen_y};
    const float hit_radius_sq = hit_radius_px * hit_radius_px;
    for (const AnnotationSkeletonEdge& edge : geometry.edges) {
        if (edge.source_index >= geometry.frame_points.size() ||
            edge.target_index >= geometry.frame_points.size()) {
            continue;
        }
        const AnnotationScreenPoint segment_start =
            frame_point_to_screen_point(viewport, geometry.frame_points[edge.source_index]);
        const AnnotationScreenPoint segment_end =
            frame_point_to_screen_point(viewport, geometry.frame_points[edge.target_index]);
        if (squared_distance_to_segment(pointer_point, segment_start, segment_end) <= hit_radius_sq) {
            return true;
        }
    }
    return false;
}

bool is_resize_drag_kind(const RectDragKind drag_kind) {
    return drag_kind == RectDragKind::ResizeTopLeft ||
           drag_kind == RectDragKind::ResizeTopRight ||
           drag_kind == RectDragKind::ResizeBottomLeft ||
           drag_kind == RectDragKind::ResizeBottomRight;
}

bool geometry_hit_test(const AnnotationVisibleObject& object,
                       const CanvasViewport& viewport,
                       const CanvasPointerState& pointer,
                       const int image_x,
                       const int image_y) {
    const float hit_radius_px = object_hit_radius_px(viewport);
    switch (object.shape_type) {
    case AnnotationShapeType::Box:
    case AnnotationShapeType::Mask:
        return box_contains_point(object.frame_box, image_x, image_y);
    case AnnotationShapeType::Point:
        return point_cloud_hit_test(object.geometry.frame_points,
                                    viewport,
                                    pointer,
                                    hit_radius_px);
    case AnnotationShapeType::Spline:
        return polyline_hit_test(object.geometry.frame_points,
                                 object.geometry.closed,
                                 viewport,
                                 pointer,
                                 hit_radius_px);
    case AnnotationShapeType::Skeleton:
        return skeleton_hit_test(object.geometry,
                                 viewport,
                                 pointer,
                                 hit_radius_px);
    }
    return false;
}

const AnnotationEditableHandle* find_editable_handle(
    const std::vector<AnnotationEditableHandle>& handles,
    const AnnotationHandleId& id) {
    const auto it = std::ranges::find_if(handles,
                                 [&](const AnnotationEditableHandle& handle) {
                                     return handle.id == id;
                                 });
    return it != handles.end() ? &(*it) : nullptr;
}

void append_annotation_shape_overlay_primitives(AnnotationInteractionOverlayRequest* request,
                                                const AnnotationVisibleObject& object,
                                                const AnnotationVisibleGeometry& geometry,
                                                const bool selected,
                                                const bool hovered) {
    if (request == nullptr) {
        return;
    }
    const std::array<std::uint8_t, 3> color =
        interaction_color(selected, hovered, object.category_index);

    switch (object.shape_type) {
    case AnnotationShapeType::Box:
    case AnnotationShapeType::Mask:
        return;
    case AnnotationShapeType::Spline:
        if (geometry.preview_points.size() >= 2U) {
            request->polylines.push_back(PreviewInteractionOverlayPolyline{
                geometry.preview_points,
                geometry.closed,
                color[0],
                color[1],
                color[2],
                selected ? 3 : 2,
            });
        }
        if (!geometry.control_preview_points.empty()) {
            request->marker_sets.push_back(PreviewInteractionOverlayMarkerSet{
                geometry.control_preview_points,
                selected ? 4 : 3,
                color[0],
                color[1],
                color[2],
                static_cast<std::uint8_t>(240U),
            });
        }
        if (!selected) {
            return;
        }
        for (const auto& segment : geometry.handle_preview_segments) {
            request->polylines.push_back(PreviewInteractionOverlayPolyline{
                {
                    segment.first,
                    segment.second,
                },
                false,
                color[0],
                color[1],
                color[2],
                1,
            });
        }
        for (const auto& segment : geometry.latent_handle_preview_segments) {
            request->polylines.push_back(PreviewInteractionOverlayPolyline{
                {
                    segment.first,
                    segment.second,
                },
                false,
                kLatentHandleR,
                kLatentHandleG,
                kLatentHandleB,
                1,
            });
        }
        if (!geometry.handle_preview_points.empty()) {
            request->marker_sets.push_back(PreviewInteractionOverlayMarkerSet{
                geometry.handle_preview_points,
                3,
                color[0],
                color[1],
                color[2],
                static_cast<std::uint8_t>(220U),
            });
        }
        if (!geometry.latent_handle_preview_points.empty()) {
            request->marker_sets.push_back(PreviewInteractionOverlayMarkerSet{
                geometry.latent_handle_preview_points,
                3,
                kLatentHandleR,
                kLatentHandleG,
                kLatentHandleB,
                static_cast<std::uint8_t>(176U),
            });
        }
        return;
    case AnnotationShapeType::Point:
        if (geometry.preview_points.empty()) {
            return;
        }
        request->marker_sets.push_back(PreviewInteractionOverlayMarkerSet{
            {geometry.preview_points.front()},
            selected ? 5 : 4,
            color[0],
            color[1],
            color[2],
            static_cast<std::uint8_t>(255U),
        });
        return;
    case AnnotationShapeType::Skeleton:
        if (!geometry.preview_points.empty() && !geometry.preview_edges.empty()) {
            request->skeletons.push_back(PreviewInteractionOverlaySkeleton{
                geometry.preview_points,
                geometry.preview_edges,
                color[0],
                color[1],
                color[2],
                selected ? 3 : 2,
            });
        }
        if (!geometry.preview_points.empty()) {
            request->marker_sets.push_back(PreviewInteractionOverlayMarkerSet{
                geometry.preview_points,
                selected ? 4 : 3,
                color[0],
                color[1],
                color[2],
                static_cast<std::uint8_t>(240U),
            });
        }
        return;
    }
}

void append_handle_highlight(AnnotationInteractionOverlayRequest* request,
                             const std::vector<AnnotationEditableHandle>& handles,
                             const std::optional<AnnotationHandleId>& handle,
                             const int radius,
                             const std::uint8_t r,
                             const std::uint8_t g,
                             const std::uint8_t b) {
    if (request == nullptr || !handle.has_value()) {
        return;
    }
    const AnnotationEditableHandle* editable_handle =
        find_editable_handle(handles, *handle);
    if (editable_handle == nullptr) {
        return;
    }
    request->marker_sets.push_back(PreviewInteractionOverlayMarkerSet{
        {to_preview_overlay_point(editable_handle->frame_point)},
        radius,
        r,
        g,
        b,
        static_cast<std::uint8_t>(255U),
    });
}

mmltk::live::ManualOverlayBox to_overlay_box(const AnnotationBox& box) {
    return mmltk::live::ManualOverlayBox{
        box.x1,
        box.y1,
        box.x2,
        box.y2,
    };
}

mmltk::live::ManualOverlayInstance to_overlay_instance(const AnnotationObject& object,
                                                       const AnnotationVisibleGeometry* geometry) {
    mmltk::live::ManualOverlayInstance overlay_object;
    overlay_object.instance_id = object.object_id;
    overlay_object.enabled = object.enabled;
    overlay_object.category_index = object.category_index;

    if (const std::optional<AnnotationBox> bbox = annotation_object_bbox(object); bbox.has_value()) {
        overlay_object.box = to_overlay_box(*bbox);
    }

    if (const AnnotationMaskShape* mask_shape = annotation_object_mask_shape(object); mask_shape != nullptr &&
        mask_shape->region.width > 0 &&
        mask_shape->region.height > 0 &&
        mask_shape->mask.size() ==
            static_cast<std::size_t>(mask_shape->region.width) * static_cast<std::size_t>(mask_shape->region.height)) {
        overlay_object.mask_region = mmltk::live::ManualOverlayMaskRegion{
            mask_shape->region.capture_x,
            mask_shape->region.capture_y,
            mask_shape->region.width,
            mask_shape->region.height,
        };
        overlay_object.mask = mask_shape->mask;
    }

    std::visit(
        [&](const auto& shape) {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                if (geometry != nullptr && !geometry->manual_points.empty()) {
                    overlay_object.polyline_points = geometry->manual_points;
                } else {
                    overlay_object.polyline_points =
                        to_overlay_points(sample_annotation_spline_points(shape));
                }
                overlay_object.polyline_closed = shape.closed;
                overlay_object.points.reserve(shape.knots.size());
                for (const AnnotationSplineKnot& knot : shape.knots) {
                    overlay_object.points.push_back(to_overlay_point(knot.position));
                }
            } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                overlay_object.points.push_back(
                    geometry != nullptr && !geometry->manual_points.empty()
                        ? geometry->manual_points.front()
                        : to_overlay_point(shape.point));
            } else if constexpr (std::is_same_v<T, AnnotationSkeletonShape>) {
                if (geometry != nullptr && !geometry->manual_points.empty()) {
                    overlay_object.points = geometry->manual_points;
                    overlay_object.skeleton_edges = geometry->manual_edges;
                    return;
                }
                std::vector<std::optional<std::size_t>> node_remap(shape.nodes.size());
                overlay_object.points.reserve(shape.nodes.size());
                for (std::size_t index = 0; index < shape.nodes.size(); ++index) {
                    if (!shape.nodes[index].visible) {
                        continue;
                    }
                    node_remap[index] = overlay_object.points.size();
                    overlay_object.points.push_back(to_overlay_point(shape.nodes[index].point));
                }
                overlay_object.skeleton_edges.reserve(shape.edges.size());
                for (const AnnotationSkeletonEdge& edge : shape.edges) {
                    if (edge.source_index >= node_remap.size() ||
                        edge.target_index >= node_remap.size()) {
                        continue;
                    }
                    const std::optional<std::size_t>& source_index =
                        node_remap[edge.source_index];
                    const std::optional<std::size_t>& target_index =
                        node_remap[edge.target_index];
                    if (!source_index.has_value() || !target_index.has_value()) {
                        continue;
                    }
                    overlay_object.skeleton_edges.push_back(
                        mmltk::live::ManualOverlayEdge{
                            static_cast<std::uint32_t>(*source_index),
                            static_cast<std::uint32_t>(*target_index),
                        });
                }
            }
        },
        object.shape);

    return overlay_object;
}

} // namespace

AnnotationProjectedScene AnnotationRenderer::build_projected_scene(
    const AnnotationFrame& frame,
    const AnnotationDocument& document,
    std::optional<std::size_t> selected_object_index) {
    if (selected_object_index.has_value() &&
        *selected_object_index >= document.size()) {
        selected_object_index.reset();
    }

    AnnotationProjectedScene scene;
    scene.document_generation = document.generation();
    scene.selected_object_index = selected_object_index;
    const std::uint32_t capture_width = annotation_frame_capture_width(frame);
    const std::uint32_t capture_height = annotation_frame_capture_height(frame);

    scene.visible_objects.reserve(document.size());
    for (std::size_t index = 0; index < document.size(); ++index) {
        const AnnotationObject* object = document.object(index);
        if (object == nullptr || !object->enabled) {
            continue;
        }

        const std::optional<AnnotationBox> object_box = annotation_object_display_box(*object);
        if (!object_box.has_value()) {
            continue;
        }

        const AnnotationBox normalized_box =
            normalize_annotation_box(*object_box, capture_width, capture_height);
        if (!annotation_box_has_area(normalized_box)) {
            continue;
        }

        const AnnotationBox frame_box = annotation_box_to_frame(frame, normalized_box);
        if (!annotation_box_has_area(frame_box)) {
            continue;
        }

        scene.visible_objects.push_back(AnnotationVisibleObject{
            index,
            object->category_index,
            annotation_shape_type(object->shape),
            normalized_box,
            frame_box,
            boxes_equal(annotation_box_from_frame(frame, frame_box), normalized_box),
            build_visible_geometry(frame, *object),
        });
    }

    append_selected_object_editable_handles(frame,
                                            document,
                                            scene.selected_object_index,
                                            &scene);
    return scene;
}

std::vector<AnnotationEditableHandle> AnnotationRenderer::build_editable_handles(
    const AnnotationFrame& frame,
    const AnnotationDocument& document,
    std::optional<std::size_t> selected_object_index) {
    return build_projected_scene(frame, document, selected_object_index).editable_handles;
}

AnnotationProjectedScene AnnotationRenderer::refresh_projected_scene_selection(
    const AnnotationFrame& frame,
    const AnnotationDocument& document,
    AnnotationProjectedScene scene,
    const std::optional<std::size_t> selected_object_index) {
    return rebuild_projected_scene_selection(frame,
                                             document,
                                             std::move(scene),
                                             selected_object_index);
}

AnnotationProjectedBox AnnotationRenderer::project_capture_box(const AnnotationFrame& frame,
                                                               const AnnotationBox& capture_box) {
    const AnnotationBox normalized_box =
        normalize_annotation_box(capture_box,
                                 annotation_frame_capture_width(frame),
                                 annotation_frame_capture_height(frame));
    const AnnotationBox frame_box = annotation_box_to_frame(frame, normalized_box);
    if (!annotation_box_has_area(frame_box)) {
        return {};
    }

    return AnnotationProjectedBox{
        frame_box,
        boxes_equal(annotation_box_from_frame(frame, frame_box), normalized_box),
    };
}

std::optional<AnnotationVisibleObjectHit> AnnotationRenderer::hit_test_visible_objects(
    const std::vector<AnnotationVisibleObject>& objects,
    const CanvasViewport& viewport,
    const CanvasPointerState& pointer,
    const int image_x,
    const int image_y,
    const bool direct_drag_mode) {
    for (std::size_t index = objects.size(); index-- > 0;) {
        const auto& object = objects[index];
        const RectDragKind hover_kind =
            rectangle_hover_kind(pointer, viewport, object.frame_box);
        const bool geometry_hit =
            geometry_hit_test(object, viewport, pointer, image_x, image_y);
        if (direct_drag_mode) {
            const bool box_like =
                object.shape_type == AnnotationShapeType::Box ||
                object.shape_type == AnnotationShapeType::Mask;
            if (box_like && hover_kind != RectDragKind::None) {
                return AnnotationVisibleObjectHit{
                    object,
                    hover_kind,
                };
            }
            if (!box_like && is_resize_drag_kind(hover_kind)) {
                return AnnotationVisibleObjectHit{
                    object,
                    hover_kind,
                };
            }
            if (geometry_hit) {
                return AnnotationVisibleObjectHit{
                    object,
                    RectDragKind::Move,
                };
            }
        } else if (geometry_hit) {
            return AnnotationVisibleObjectHit{
                object,
                RectDragKind::None,
            };
        }
    }
    return std::nullopt;
}

AnnotationInteractionOverlayRequest AnnotationRenderer::build_interaction_overlay_request(
    const AnnotationFrame& frame,
    const int cuda_device_index,
    const AnnotationProjectedScene& scene,
    const std::optional<std::size_t> hovered_object_index,
    const std::optional<std::size_t> replaced_object_index,
    std::optional<AnnotationBox> crop_box,
    const int crop_handle_radius,
    std::optional<AnnotationBox> drag_box,
    std::optional<AnnotationBox> create_box,
    const std::optional<AnnotationHandleId> hovered_handle,
    const std::optional<AnnotationHandleId> active_handle) {
    AnnotationInteractionOverlayRequest request;
    request.width = frame.width;
    request.height = frame.height;
    request.cuda_device_index = cuda_device_index;
    request.objects.reserve(scene.visible_objects.size());
    request.polylines.reserve(scene.visible_objects.size());
    request.marker_sets.reserve(scene.visible_objects.size());
    request.skeletons.reserve(scene.visible_objects.size());

    if (crop_box.has_value() && annotation_box_has_area(*crop_box)) {
        request.crop_box = *crop_box;
        request.crop_handle_radius = std::max(1, crop_handle_radius);
    }

    for (const AnnotationVisibleObject& object : scene.visible_objects) {
        if (replaced_object_index.has_value() && *replaced_object_index == object.index) {
            continue;
        }
        const bool selected =
            scene.selected_object_index.has_value() && *scene.selected_object_index == object.index;
        const bool hovered =
            hovered_object_index.has_value() && *hovered_object_index == object.index;
        const bool show_box =
            object.shape_type == AnnotationShapeType::Box ||
            object.shape_type == AnnotationShapeType::Mask ||
            selected ||
            hovered;
        if (show_box) {
            request.objects.push_back(AnnotationInteractionOverlayObject{
                object.index,
                object.category_index,
                object.shape_type,
                object.frame_box,
                selected,
                hovered,
            });
        }
        append_annotation_shape_overlay_primitives(&request,
                                                   object,
                                                   object.geometry,
                                                   selected,
                                                   hovered);
    }

    if (drag_box.has_value()) {
        request.drag_box = *drag_box;
    }
    if (create_box.has_value()) {
        request.create_box = *create_box;
    }

    if (active_handle.has_value()) {
        append_handle_highlight(&request,
                                scene.editable_handles,
                                active_handle,
                                6,
                                kActiveHandleR,
                                kActiveHandleG,
                                kActiveHandleB);
    }
    if (hovered_handle.has_value() &&
        (!active_handle.has_value() || !(*hovered_handle == *active_handle))) {
        append_handle_highlight(&request,
                                scene.editable_handles,
                                hovered_handle,
                                5,
                                kHoveredHandleR,
                                kHoveredHandleG,
                                kHoveredHandleB);
    }

    return request;
}

PreviewInteractionOverlaySnapshot AnnotationRenderer::build_interaction_overlay_snapshot(
    const AnnotationInteractionOverlayRequest& request) {
    PreviewInteractionOverlaySnapshot snapshot;
    snapshot.width = request.width;
    snapshot.height = request.height;
    snapshot.cuda_device_index = request.cuda_device_index;
    snapshot.polylines = request.polylines;
    snapshot.marker_sets = request.marker_sets;
    snapshot.skeletons = request.skeletons;
    snapshot.boxes.reserve(request.objects.size() +
                           (request.crop_box.has_value() ? 1U : 0U) +
                           (request.drag_box.has_value() ? 1U : 0U) +
                           (request.create_box.has_value() ? 1U : 0U));

    if (request.crop_box.has_value()) {
        append_overlay_box(snapshot,
                           *request.crop_box,
                           255U,
                           255U,
                           255U,
                           kInteractionCropStrokeWidth,
                           true,
                           request.crop_handle_radius);
    }

    for (const AnnotationInteractionOverlayObject& object : request.objects) {
        const std::array<std::uint8_t, 3> color =
            interaction_color(object.selected, object.hovered, object.category_index);
        if (object.selected) {
            append_overlay_box(snapshot,
                               object.frame_box,
                               color[0],
                               color[1],
                               color[2],
                               3,
                               true);
        } else if (object.hovered) {
            append_overlay_box(snapshot,
                               object.frame_box,
                               color[0],
                               color[1],
                               color[2],
                               2);
        } else {
            append_overlay_box(snapshot,
                               object.frame_box,
                               color[0],
                               color[1],
                               color[2],
                               1);
        }
    }

    if (request.drag_box.has_value()) {
        append_overlay_box(snapshot, *request.drag_box, 255U, 220U, 96U, 3, true);
    }
    if (request.create_box.has_value()) {
        append_overlay_box(snapshot, *request.create_box, 255U, 220U, 96U, 3);
    }

    return snapshot;
}

mmltk::live::ManualOverlayDocumentSnapshot AnnotationRenderer::build_manual_overlay_snapshot(
    const AnnotationFrame& frame,
    const AnnotationDocument& document,
    const AnnotationSession& session) {
    const AnnotationProjectedScene scene =
        build_projected_scene(frame, document, session.selected_object_index());
    return build_manual_overlay_snapshot(frame, document, session, scene);
}

mmltk::live::ManualOverlayDocumentSnapshot AnnotationRenderer::build_manual_overlay_snapshot(
    const AnnotationFrame& frame,
    const AnnotationDocument& document,
    const AnnotationSession& session,
    const AnnotationProjectedScene& scene) {
    mmltk::live::ManualOverlayDocumentSnapshot snapshot;
    snapshot.capture_width = annotation_frame_capture_width(frame);
    snapshot.capture_height = annotation_frame_capture_height(frame);
    snapshot.selected_instance = scene.selected_object_index;
    snapshot.instances.reserve(document.size());
    const AnnotationProjectedSceneLookup projected_scene_lookup =
        make_annotation_projected_scene_lookup(&scene, document.size());
    for (std::size_t index = 0; index < document.size(); ++index) {
        const AnnotationObject* object = document.object(index);
        if (object == nullptr) {
            continue;
        }
        const AnnotationVisibleGeometry* geometry = projected_scene_lookup.visible_geometry(index);
        snapshot.instances.push_back(to_overlay_instance(*object, geometry));
    }
    snapshot.document_generation = document.generation();
    snapshot.session_revision = session.overlay_revision();
    return snapshot;
}

} // namespace mmltk::gui
