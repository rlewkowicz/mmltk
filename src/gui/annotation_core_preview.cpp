#include "annotation_core.h"
#include "gui/annotation/render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace mmltk::gui {

namespace {

constexpr float kHueRange = 360.0f;
constexpr float kMaskBlend = 0.38f;

struct EffectiveObjectSeed {
    AnnotationShapeType shape_type = AnnotationShapeType::Box;
    AnnotationBox box{};
    std::vector<std::uint8_t> mask;
    std::vector<AnnotationPoint> points_xy;
};

bool hue_in_window(const float hue, const float minimum, const float maximum) {
    const float wrapped_hue = annotation_wrap_hue(hue);
    const float wrapped_min = annotation_wrap_hue(minimum);
    const float wrapped_max = annotation_wrap_hue(maximum);
    if (wrapped_min <= wrapped_max) {
        return wrapped_hue >= wrapped_min && wrapped_hue <= wrapped_max;
    }
    return wrapped_hue >= wrapped_min || wrapped_hue <= wrapped_max;
}

bool pixel_matches_range(const AnnotationColorRange& range, const AnnotationHsv& hsv) {
    if (!annotation_range_active(range)) {
        return false;
    }

    const float hue_minus = range.tolerance.hue_minus_pct * (kHueRange / 100.0f);
    const float hue_plus = range.tolerance.hue_plus_pct * (kHueRange / 100.0f);
    const float sat_min = annotation_clamp_unit(range.center.saturation - range.tolerance.saturation_minus_pct / 100.0f);
    const float sat_max = annotation_clamp_unit(range.center.saturation + range.tolerance.saturation_plus_pct / 100.0f);
    const float value_min = annotation_clamp_unit(range.center.value - range.tolerance.value_minus_pct / 100.0f);
    const float value_max = annotation_clamp_unit(range.center.value + range.tolerance.value_plus_pct / 100.0f);

    return hue_in_window(hsv.hue_degrees, range.center.hue_degrees - hue_minus, range.center.hue_degrees + hue_plus) &&
           hsv.saturation >= sat_min && hsv.saturation <= sat_max &&
           hsv.value >= value_min && hsv.value <= value_max;
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

void draw_rect_bgr(std::vector<std::uint8_t>& pixels,
                   const std::uint32_t width,
                   const std::uint32_t height,
                   const AnnotationBox& box,
                   const std::array<std::uint8_t, 3>& color,
                   const int thickness) {
    if (width == 0 || height == 0 || thickness <= 0) {
        return;
    }
    const AnnotationBox clamped = normalize_annotation_box(box, width, height);
    if (clamped.x2 <= clamped.x1 || clamped.y2 <= clamped.y1) {
        return;
    }

    const auto paint = [&](const int x, const int y) {
        if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
            return;
        }
        const std::size_t offset =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3U;
        pixels[offset + 0] = color[2];
        pixels[offset + 1] = color[1];
        pixels[offset + 2] = color[0];
    };

    for (int t = 0; t < thickness; ++t) {
        const int x1 = clamped.x1 - t;
        const int x2 = clamped.x2 - 1 + t;
        const int y1 = clamped.y1 - t;
        const int y2 = clamped.y2 - 1 + t;
        for (int x = x1; x <= x2; ++x) {
            paint(x, y1);
            paint(x, y2);
        }
        for (int y = y1; y <= y2; ++y) {
            paint(x1, y);
            paint(x2, y);
        }
    }
}

std::vector<std::uint8_t> seed_mask_from_box(const AnnotationBox& box,
                                             const std::uint32_t width,
                                             const std::uint32_t height) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    const AnnotationBox clamped = normalize_annotation_box(box, width, height);
    for (int y = clamped.y1; y < clamped.y2; ++y) {
        const std::size_t row_offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = clamped.x1; x < clamped.x2; ++x) {
            mask[row_offset + static_cast<std::size_t>(x)] = 1U;
        }
    }
    return mask;
}

void paint_mask_pixel(std::vector<std::uint8_t>& mask,
                      const std::uint32_t width,
                      const std::uint32_t height,
                      const int x,
                      const int y) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width) || y >= static_cast<int>(height)) {
        return;
    }
    mask[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)] = 1U;
}

void rasterize_line(std::vector<std::uint8_t>& mask,
                    const std::uint32_t width,
                    const std::uint32_t height,
                    AnnotationPoint start,
                    AnnotationPoint end) {
    int x0 = static_cast<int>(std::lround(start.x));
    int y0 = static_cast<int>(std::lround(start.y));
    const int x1 = static_cast<int>(std::lround(end.x));
    const int y1 = static_cast<int>(std::lround(end.y));
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        paint_mask_pixel(mask, width, height, x0, y0);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice_error = error * 2;
        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

AnnotationBox mask_region_box(const AnnotationMaskRegion& region) {
    return AnnotationBox{
        static_cast<int>(region.capture_x),
        static_cast<int>(region.capture_y),
        static_cast<int>(region.capture_x + region.width),
        static_cast<int>(region.capture_y + region.height),
    };
}

bool mask_shape_valid(const AnnotationMaskShape& shape) {
    return shape.region.width > 0 &&
           shape.region.height > 0 &&
           shape.mask.size() ==
               static_cast<std::size_t>(shape.region.width) *
                   static_cast<std::size_t>(shape.region.height);
}

std::vector<std::uint8_t> project_mask_region_to_frame(const AnnotationFrame& frame,
                                                       const AnnotationMaskRegion& region,
                                                       const std::vector<std::uint8_t>& mask) {
    std::vector<std::uint8_t> projected(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height), 0U);
    if (projected.empty() || mask.empty() || region.width == 0 || region.height == 0) {
        return projected;
    }

    const AnnotationBox region_box = mask_region_box(region);
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    const AnnotationBox overlap = annotation_intersect_boxes(region_box, view_box);
    if (!annotation_box_has_area(overlap)) {
        return projected;
    }

    for (int capture_y = overlap.y1; capture_y < overlap.y2; ++capture_y) {
        const std::size_t src_row =
            static_cast<std::size_t>(capture_y - region_box.y1) * static_cast<std::size_t>(region.width);
        const std::size_t dst_row =
            static_cast<std::size_t>(capture_y - view_box.y1) * static_cast<std::size_t>(frame.width);
        for (int capture_x = overlap.x1; capture_x < overlap.x2; ++capture_x) {
            const std::size_t src_index = src_row + static_cast<std::size_t>(capture_x - region_box.x1);
            const std::size_t dst_index = dst_row + static_cast<std::size_t>(capture_x - view_box.x1);
            projected[dst_index] = mask[src_index];
        }
    }
    return projected;
}

std::optional<AnnotationPoint> capture_point_to_frame(const AnnotationFrame& frame, const AnnotationPoint& point) {
    const AnnotationBox view_box = annotation_frame_view_box(frame);
    if (point.x < static_cast<float>(view_box.x1) ||
        point.y < static_cast<float>(view_box.y1) ||
        point.x >= static_cast<float>(view_box.x2) ||
        point.y >= static_cast<float>(view_box.y2)) {
        return std::nullopt;
    }
    return AnnotationPoint{
        point.x - static_cast<float>(view_box.x1),
        point.y - static_cast<float>(view_box.y1),
    };
}

std::vector<AnnotationPoint> capture_points_to_frame(const AnnotationFrame& frame,
                                                     const std::vector<AnnotationPoint>& capture_points) {
    std::vector<AnnotationPoint> frame_points;
    frame_points.reserve(capture_points.size());
    for (const AnnotationPoint& capture_point : capture_points) {
        if (const std::optional<AnnotationPoint> frame_point = capture_point_to_frame(frame, capture_point);
            frame_point.has_value()) {
            frame_points.push_back(*frame_point);
        }
    }
    return frame_points;
}

std::vector<std::uint8_t> seed_mask_from_paths(const std::vector<AnnotationPoint>& points,
                                               const std::uint32_t width,
                                               const std::uint32_t height,
                                               const bool closed) {
    std::vector<std::uint8_t> mask(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0U);
    if (points.empty()) {
        return mask;
    }

    paint_mask_pixel(mask, width, height,
                     static_cast<int>(std::lround(points.front().x)),
                     static_cast<int>(std::lround(points.front().y)));
    for (std::size_t index = 1; index < points.size(); ++index) {
        rasterize_line(mask, width, height, points[index - 1U], points[index]);
    }
    if (closed && points.size() > 1U) {
        rasterize_line(mask, width, height, points.back(), points.front());
    }
    return mask;
}

EffectiveObjectSeed effective_object_seed(const AnnotationFrame& frame,
                                          const AnnotationObject& object,
                                          const bool live_mode,
                                          const AnnotationVisibleObject* projected_object) {
    EffectiveObjectSeed seed;
    seed.shape_type = annotation_shape_type(object.shape);

    std::visit(
        [&](const auto& shape) {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, AnnotationBoxShape>) {
                seed.box = annotation_box_to_frame(frame, shape.box);
                seed.mask = seed_mask_from_box(seed.box, frame.width, frame.height);
            } else if constexpr (std::is_same_v<T, AnnotationMaskShape>) {
                seed.box = annotation_box_to_frame(frame, shape.box);
                bool frame_match = true;
                if (live_mode) {
                    if (shape.seed_live_frame_id.has_value() && frame.live_frame_id.has_value()) {
                        frame_match = *shape.seed_live_frame_id == *frame.live_frame_id;
                    } else {
                        frame_match = shape.seed_frame_id == 0U || shape.seed_frame_id == frame.frame_id;
                    }
                }
                if (mask_shape_valid(shape) && frame_match) {
                    seed.mask = project_mask_region_to_frame(frame, shape.region, shape.mask);
                    if (const std::optional<AnnotationBox> mask_box =
                            annotation_bbox_from_mask(seed.mask, frame.width, frame.height);
                        mask_box.has_value()) {
                        seed.box = *mask_box;
                    }
                } else {
                    seed.mask = seed_mask_from_box(seed.box, frame.width, frame.height);
                }
            } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                if (projected_object != nullptr &&
                    !projected_object->geometry.frame_points.empty()) {
                    seed.points_xy.push_back(projected_object->geometry.frame_points.front());
                } else if (const std::optional<AnnotationPoint> frame_point =
                               capture_point_to_frame(frame, shape.point);
                           frame_point.has_value()) {
                    seed.points_xy.push_back(*frame_point);
                }
                if (!seed.points_xy.empty()) {
                    const AnnotationPoint& frame_point = seed.points_xy.front();
                    seed.box = AnnotationBox{
                        static_cast<int>(std::floor(frame_point.x)),
                        static_cast<int>(std::floor(frame_point.y)),
                        static_cast<int>(std::floor(frame_point.x)) + 1,
                        static_cast<int>(std::floor(frame_point.y)) + 1,
                    };
                    seed.mask = seed_mask_from_paths(seed.points_xy, frame.width, frame.height, false);
                }
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                if (projected_object != nullptr &&
                    !projected_object->geometry.frame_points.empty()) {
                    seed.points_xy = projected_object->geometry.frame_points;
                } else {
                    const std::vector<AnnotationPoint> capture_points =
                        sample_annotation_spline_points(shape);
                    seed.points_xy = capture_points_to_frame(frame, capture_points);
                }
                if (!seed.points_xy.empty()) {
                    seed.mask =
                        seed_mask_from_paths(seed.points_xy, frame.width, frame.height, shape.closed);
                    if (const std::optional<AnnotationBox> bbox =
                            annotation_bbox_from_mask(seed.mask, frame.width, frame.height);
                        bbox.has_value()) {
                        seed.box = *bbox;
                    }
                }
            } else {
                seed.mask.assign(static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height), 0U);
                if (projected_object != nullptr) {
                    seed.points_xy = projected_object->geometry.frame_points;
                    for (const AnnotationSkeletonEdge& edge : projected_object->geometry.edges) {
                        if (edge.source_index >= seed.points_xy.size() ||
                            edge.target_index >= seed.points_xy.size()) {
                            continue;
                        }
                        rasterize_line(seed.mask,
                                       frame.width,
                                       frame.height,
                                       seed.points_xy[edge.source_index],
                                       seed.points_xy[edge.target_index]);
                    }
                } else {
                    std::vector<AnnotationPoint> node_points_capture;
                    node_points_capture.reserve(shape.nodes.size());
                    for (const AnnotationSkeletonNode& node : shape.nodes) {
                        if (node.visible) {
                            node_points_capture.push_back(node.point);
                        }
                    }
                    seed.points_xy = capture_points_to_frame(frame, node_points_capture);
                    for (const AnnotationSkeletonEdge& edge : shape.edges) {
                        if (edge.source_index >= shape.nodes.size() ||
                            edge.target_index >= shape.nodes.size()) {
                            continue;
                        }
                        const AnnotationSkeletonNode& source = shape.nodes[edge.source_index];
                        const AnnotationSkeletonNode& target = shape.nodes[edge.target_index];
                        if (!source.visible || !target.visible) {
                            continue;
                        }
                        const std::optional<AnnotationPoint> source_frame =
                            capture_point_to_frame(frame, source.point);
                        const std::optional<AnnotationPoint> target_frame =
                            capture_point_to_frame(frame, target.point);
                        if (!source_frame.has_value() || !target_frame.has_value()) {
                            continue;
                        }
                        rasterize_line(seed.mask,
                                       frame.width,
                                       frame.height,
                                       *source_frame,
                                       *target_frame);
                    }
                }
                for (const AnnotationPoint& point : seed.points_xy) {
                    paint_mask_pixel(seed.mask,
                                     frame.width,
                                     frame.height,
                                     static_cast<int>(std::lround(point.x)),
                                     static_cast<int>(std::lround(point.y)));
                }
                if (const std::optional<AnnotationBox> bbox =
                        annotation_bbox_from_mask(seed.mask, frame.width, frame.height);
                    bbox.has_value()) {
                    seed.box = *bbox;
                }
            }
        },
        object.shape);

    return seed;
}

AnnotationResolvedObject resolve_object(const AnnotationFrame& frame,
                                        const AnnotationCategories& categories,
                                        const AnnotationObject& object,
                                        const std::size_t object_index,
                                        const bool live_mode,
                                        const AnnotationVisibleObject* projected_object) {
    if (object.category_index >= categories.items.size()) {
        throw std::runtime_error("annotation object category index is out of range");
    }

    EffectiveObjectSeed seed = effective_object_seed(frame, object, live_mode, projected_object);
    AnnotationResolvedObject resolved;
    resolved.object_index = object_index;
    resolved.category_index = object.category_index;
    resolved.class_name = categories.items[object.category_index].name;
    resolved.shape_type = seed.shape_type;
    resolved.points_xy = seed.points_xy;

    const bool has_dense_mask =
        seed.mask.size() == static_cast<std::size_t>(frame.width) * static_cast<std::size_t>(frame.height);
    if (has_dense_mask) {
        resolved.mask.assign(seed.mask.size(), 0U);
        const AnnotationBox box = normalize_annotation_box(seed.box, frame.width, frame.height);
        for (int y = box.y1; y < box.y2; ++y) {
            const std::size_t row_offset = static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width);
            for (int x = box.x1; x < box.x2; ++x) {
                const std::size_t pixel_index = row_offset + static_cast<std::size_t>(x);
                if (seed.mask[pixel_index] == 0U) {
                    continue;
                }
                const std::size_t byte_offset = pixel_index * 3U;
                const AnnotationHsv hsv = annotation_bgr_to_hsv(frame.pixels_bgr[byte_offset + 0],
                                                                 frame.pixels_bgr[byte_offset + 1],
                                                                 frame.pixels_bgr[byte_offset + 2]);
                const bool sup_match = pixel_matches_range(object.sup, hsv);
                const bool nosup_match = pixel_matches_range(object.nosup, hsv);
                if (!sup_match || nosup_match) {
                    resolved.mask[pixel_index] = 1U;
                }
            }
        }
        if (const std::optional<AnnotationBox> bbox = annotation_bbox_from_mask(resolved.mask, frame.width, frame.height);
            bbox.has_value()) {
            resolved.bbox = *bbox;
            resolved.mask_rle = encode_annotation_mask_rle(resolved.mask);
        }
    }

    if (!annotation_box_has_area(resolved.bbox)) {
        if (const std::optional<AnnotationBox> object_bbox = annotation_object_bbox(object); object_bbox.has_value()) {
            resolved.bbox = annotation_box_to_frame(frame, *object_bbox);
        }
    }

    if (annotation_box_has_area(resolved.bbox) && has_dense_mask && !resolved.mask_rle.empty()) {
        resolved.crop_width = static_cast<std::uint32_t>(resolved.bbox.x2 - resolved.bbox.x1);
        resolved.crop_height = static_cast<std::uint32_t>(resolved.bbox.y2 - resolved.bbox.y1);
        resolved.crop_rgba.assign(static_cast<std::size_t>(resolved.crop_width) *
                                      static_cast<std::size_t>(resolved.crop_height) * 4U,
                                  0U);
        for (int y = resolved.bbox.y1; y < resolved.bbox.y2; ++y) {
            for (int x = resolved.bbox.x1; x < resolved.bbox.x2; ++x) {
                const std::size_t source_index =
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(frame.width) + static_cast<std::size_t>(x);
                const std::size_t crop_index =
                    (static_cast<std::size_t>(y - resolved.bbox.y1) * static_cast<std::size_t>(resolved.crop_width) +
                     static_cast<std::size_t>(x - resolved.bbox.x1)) *
                    4U;
                const std::size_t source_byte = source_index * 3U;
                resolved.crop_rgba[crop_index + 0] = frame.pixels_bgr[source_byte + 2];
                resolved.crop_rgba[crop_index + 1] = frame.pixels_bgr[source_byte + 1];
                resolved.crop_rgba[crop_index + 2] = frame.pixels_bgr[source_byte + 0];
                resolved.crop_rgba[crop_index + 3] = resolved.mask[source_index] == 0U ? 0U : 255U;
            }
        }
    }

    return resolved;
}

std::vector<AnnotationResolvedObject> resolve_annotation_objects_with_lookup(
    const AnnotationFrame& frame,
    const AnnotationCategories& categories,
    const std::vector<AnnotationObject>& objects,
    const bool live_mode,
    const AnnotationProjectedSceneLookup& projected_scene_lookup) {
    std::vector<AnnotationResolvedObject> resolved_objects;
    resolved_objects.reserve(objects.size());

    for (std::size_t index = 0; index < objects.size(); ++index) {
        const AnnotationObject& object = objects[index];
        if (!object.enabled) {
            continue;
        }
        AnnotationResolvedObject resolved;
        try {
            const AnnotationVisibleObject* projected_object =
                projected_scene_lookup.visible_object(index);
            resolved = resolve_object(frame,
                                      categories,
                                      object,
                                      index,
                                      live_mode,
                                      projected_object);
        } catch (const std::exception&) {
            continue;
        }
        if (!annotation_box_has_area(resolved.bbox) && resolved.points_xy.empty()) {
            continue;
        }
        resolved_objects.push_back(std::move(resolved));
    }

    return resolved_objects;
}

const AnnotationVisibleObject* lookup_projected_scene_visible_object(
    const AnnotationProjectedScene* projected_scene,
    const std::size_t object_count,
    const std::size_t object_index,
    std::size_t* cursor) noexcept {
    if (projected_scene == nullptr || object_index >= object_count) {
        return nullptr;
    }

    const std::size_t visible_object_count = projected_scene->visible_objects.size();
    if (visible_object_count == 0U) {
        return nullptr;
    }

    const std::size_t start_index = cursor == nullptr ? 0U : std::min(*cursor, visible_object_count);
    for (std::size_t position = start_index; position < visible_object_count; ++position) {
        const AnnotationVisibleObject& visible_object = projected_scene->visible_objects[position];
        if (visible_object.index < object_index) {
            if (cursor != nullptr) {
                *cursor = position + 1U;
            }
            continue;
        }
        if (visible_object.index > object_index) {
            if (cursor != nullptr) {
                *cursor = position;
            }
            break;
        }
        if (cursor != nullptr) {
            *cursor = position + 1U;
        }
        return &visible_object;
    }

    if (cursor != nullptr) {
        *cursor = visible_object_count;
    }
    return nullptr;
}

} // namespace

AnnotationProjectedSceneLookup make_annotation_projected_scene_lookup(
    const AnnotationProjectedScene* projected_scene,
    const std::size_t object_count) {
    AnnotationProjectedSceneLookup lookup;
    lookup.projected_scene = projected_scene;
    lookup.object_count = object_count;
    return lookup;
}

const AnnotationVisibleObject* AnnotationProjectedSceneLookup::visible_object(
    const std::size_t index) const noexcept {
    return lookup_projected_scene_visible_object(projected_scene,
                                                 object_count,
                                                 index,
                                                 &visible_object_cursor);
}

const AnnotationVisibleGeometry* AnnotationProjectedSceneLookup::visible_geometry(
    const std::size_t index) const noexcept {
    const AnnotationVisibleObject* visible_object =
        lookup_projected_scene_visible_object(projected_scene,
                                              object_count,
                                              index,
                                              &visible_geometry_cursor);
    return visible_object != nullptr ? &visible_object->geometry : nullptr;
}

std::vector<AnnotationResolvedObject> resolve_annotation_objects(const AnnotationFrame& frame,
                                                                const AnnotationCategories& categories,
                                                                const std::vector<AnnotationObject>& objects,
                                                                const bool live_mode,
                                                                const AnnotationProjectedScene* projected_scene) {
    return resolve_annotation_objects_with_lookup(frame,
                                                  categories,
                                                  objects,
                                                  live_mode,
                                                  make_annotation_projected_scene_lookup(projected_scene,
                                                                                         objects.size()));
}

AnnotationPreviewResult build_annotation_preview(const AnnotationFrame& frame,
                                                 const AnnotationCategories& categories,
                                                 const std::vector<AnnotationObject>& objects,
                                                 const bool live_mode,
                                                 const AnnotationProjectedScene* projected_scene) {
    AnnotationPreviewResult preview;
    preview.preview_bgr = frame.pixels_bgr;
    preview.resolved_objects =
        resolve_annotation_objects(frame,
                                   categories,
                                   objects,
                                   live_mode,
                                   projected_scene);

    for (const AnnotationResolvedObject& resolved : preview.resolved_objects) {
        const std::array<std::uint8_t, 3> color = category_color(resolved.category_index);
        if (resolved.mask.size() * 3U <= preview.preview_bgr.size()) {
            for (std::size_t pixel_index = 0; pixel_index < resolved.mask.size(); ++pixel_index) {
                if (resolved.mask[pixel_index] == 0U) {
                    continue;
                }
                const std::size_t byte_offset = pixel_index * 3U;
                const auto b = static_cast<float>(preview.preview_bgr[byte_offset + 0]);
                const auto g = static_cast<float>(preview.preview_bgr[byte_offset + 1]);
                const auto r = static_cast<float>(preview.preview_bgr[byte_offset + 2]);
                const auto color_b = static_cast<float>(color[2]);
                const auto color_g = static_cast<float>(color[1]);
                const auto color_r = static_cast<float>(color[0]);
                preview.preview_bgr[byte_offset + 0] =
                    static_cast<std::uint8_t>(std::clamp((1.0f - kMaskBlend) * b + kMaskBlend * color_b, 0.0f, 255.0f));
                preview.preview_bgr[byte_offset + 1] =
                    static_cast<std::uint8_t>(std::clamp((1.0f - kMaskBlend) * g + kMaskBlend * color_g, 0.0f, 255.0f));
                preview.preview_bgr[byte_offset + 2] =
                    static_cast<std::uint8_t>(std::clamp((1.0f - kMaskBlend) * r + kMaskBlend * color_r, 0.0f, 255.0f));
            }
        }
        if (annotation_box_has_area(resolved.bbox)) {
            draw_rect_bgr(preview.preview_bgr, frame.width, frame.height, resolved.bbox, color, 2);
        }
    }

    return preview;
}

} // namespace mmltk::gui
