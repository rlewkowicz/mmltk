#include "gui/annotation/document/types.h"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

namespace mmltk::gui {

namespace {

std::optional<AnnotationBox> bbox_from_points(const std::vector<AnnotationPoint>& points) {
    if (points.empty()) {
        return std::nullopt;
    }

    float min_x = points.front().x;
    float min_y = points.front().y;
    float max_x = points.front().x;
    float max_y = points.front().y;
    for (const AnnotationPoint& point : points) {
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }

    return AnnotationBox{
        static_cast<int>(min_x),
        static_cast<int>(min_y),
        static_cast<int>(max_x + 1.0f),
        static_cast<int>(max_y + 1.0f),
    };
}

AnnotationBox clamp_capture_box(AnnotationBox box, const std::uint32_t capture_width,
                                const std::uint32_t capture_height) {
    const auto clamp_axis = [](int& low, int& high, const int max_value) {
        low = std::clamp(low, 0, max_value);
        high = std::clamp(high, 0, max_value);
        if (high < low) {
            std::swap(low, high);
        }
    };
    clamp_axis(box.x1, box.x2, static_cast<int>(capture_width));
    clamp_axis(box.y1, box.y2, static_cast<int>(capture_height));
    return box;
}

bool box_has_area(const AnnotationBox& box) {
    return box.x2 > box.x1 && box.y2 > box.y1;
}

bool boxes_equal(const AnnotationBox& lhs, const AnnotationBox& rhs) {
    return lhs.x1 == rhs.x1 && lhs.y1 == rhs.y1 && lhs.x2 == rhs.x2 && lhs.y2 == rhs.y2;
}

std::pair<int, int> clamped_box_offset(const AnnotationBox& box, const int dx, const int dy,
                                       const std::uint32_t capture_width, const std::uint32_t capture_height) {
    const int box_width = std::max(0, box.x2 - box.x1);
    const int box_height = std::max(0, box.y2 - box.y1);
    const int max_x1 = std::max(0, static_cast<int>(capture_width) - box_width);
    const int max_y1 = std::max(0, static_cast<int>(capture_height) - box_height);
    const int next_x1 = std::clamp(box.x1 + dx, 0, max_x1);
    const int next_y1 = std::clamp(box.y1 + dy, 0, max_y1);
    return {
        next_x1 - box.x1,
        next_y1 - box.y1,
    };
}

float clamp_capture_axis(const float value, const std::uint32_t extent) {
    const float max_value = extent == 0U ? 0.0f : static_cast<float>(extent - 1U);
    return std::clamp(value, 0.0f, max_value);
}

void translate_point(AnnotationPoint* point, const int dx, const int dy, const std::uint32_t capture_width,
                     const std::uint32_t capture_height) {
    if (point == nullptr) {
        return;
    }
    point->x = clamp_capture_axis(point->x + static_cast<float>(dx), capture_width);
    point->y = clamp_capture_axis(point->y + static_cast<float>(dy), capture_height);
}

float scale_axis(const float value, const float source_min, const float source_extent, const float target_min,
                 const float target_extent, const std::uint32_t capture_extent) {
    const float normalized = source_extent > 0.0f ? (value - source_min) / source_extent : 0.5f;
    return clamp_capture_axis(target_min + normalized * target_extent, capture_extent);
}

void scale_point_to_box(AnnotationPoint* point, const AnnotationBox& source_box, const AnnotationBox& target_box,
                        const std::uint32_t capture_width, const std::uint32_t capture_height) {
    if (point == nullptr) {
        return;
    }
    const auto source_width = static_cast<float>(source_box.x2 - source_box.x1);
    const auto source_height = static_cast<float>(source_box.y2 - source_box.y1);
    const auto target_width = static_cast<float>(target_box.x2 - target_box.x1);
    const auto target_height = static_cast<float>(target_box.y2 - target_box.y1);
    point->x = scale_axis(point->x, static_cast<float>(source_box.x1), source_width, static_cast<float>(target_box.x1),
                          target_width, capture_width);
    point->y = scale_axis(point->y, static_cast<float>(source_box.y1), source_height, static_cast<float>(target_box.y1),
                          target_height, capture_height);
}

std::vector<std::uint8_t> resize_mask_nearest(const std::vector<std::uint8_t>& source, const std::uint32_t source_width,
                                              const std::uint32_t source_height, const std::uint32_t target_width,
                                              const std::uint32_t target_height) {
    if (target_width == 0U || target_height == 0U) {
        return {};
    }
    std::vector<std::uint8_t> resized(static_cast<std::size_t>(target_width) * static_cast<std::size_t>(target_height),
                                      0U);
    if (source_width == 0U || source_height == 0U ||
        source.size() != static_cast<std::size_t>(source_width) * static_cast<std::size_t>(source_height)) {
        std::ranges::fill(resized, 1U);
        return resized;
    }

    for (std::uint32_t y = 0; y < target_height; ++y) {
        const std::uint32_t source_y = std::min(source_height - 1U, (y * source_height) / target_height);
        const std::size_t target_row = static_cast<std::size_t>(y) * static_cast<std::size_t>(target_width);
        const std::size_t source_row = static_cast<std::size_t>(source_y) * static_cast<std::size_t>(source_width);
        for (std::uint32_t x = 0; x < target_width; ++x) {
            const std::uint32_t source_x = std::min(source_width - 1U, (x * source_width) / target_width);
            resized[target_row + static_cast<std::size_t>(x)] = source[source_row + static_cast<std::size_t>(source_x)];
        }
    }
    return resized;
}

AnnotationPoint cubic_bezier_point(const AnnotationPoint& p0, const AnnotationPoint& p1, const AnnotationPoint& p2,
                                   const AnnotationPoint& p3, const float t) {
    const float one_minus_t = 1.0f - t;
    const float a = one_minus_t * one_minus_t * one_minus_t;
    const float b = 3.0f * one_minus_t * one_minus_t * t;
    const float c = 3.0f * one_minus_t * t * t;
    const float d = t * t * t;
    return AnnotationPoint{
        a * p0.x + b * p1.x + c * p2.x + d * p3.x,
        a * p0.y + b * p1.y + c * p2.y + d * p3.y,
    };
}

}  

AnnotationMaskRegion mask_region_from_box(const AnnotationBox& box) {
    return AnnotationMaskRegion{
        static_cast<std::uint32_t>(std::max(0, box.x1)),
        static_cast<std::uint32_t>(std::max(0, box.y1)),
        static_cast<std::uint32_t>(std::max(0, box.x2 - box.x1)),
        static_cast<std::uint32_t>(std::max(0, box.y2 - box.y1)),
    };
}

AnnotationBox box_from_mask_region(const AnnotationMaskRegion& region) {
    return AnnotationBox{
        static_cast<int>(region.capture_x),
        static_cast<int>(region.capture_y),
        static_cast<int>(region.capture_x + region.width),
        static_cast<int>(region.capture_y + region.height),
    };
}

const char* annotation_spline_handle_mode_name(const AnnotationSplineHandleMode mode) noexcept {
    switch (mode) {
        case AnnotationSplineHandleMode::Corner:
            return "corner";
        case AnnotationSplineHandleMode::Smooth:
            return "smooth";
        case AnnotationSplineHandleMode::Mirrored:
            return "mirrored";
    }
    return "corner";
}

AnnotationSplineHandleMode annotation_spline_handle_mode_from_name(const std::string_view value) {
    if (value == "smooth") {
        return AnnotationSplineHandleMode::Smooth;
    }
    if (value == "mirrored") {
        return AnnotationSplineHandleMode::Mirrored;
    }
    return AnnotationSplineHandleMode::Corner;
}

AnnotationShapeType annotation_shape_type(const AnnotationShapeVariant& shape) {
    return std::visit(
        []<typename T>(const T&) {
            if constexpr (std::is_same_v<T, AnnotationBoxShape>) {
                return AnnotationShapeType::Box;
            } else if constexpr (std::is_same_v<T, AnnotationMaskShape>) {
                return AnnotationShapeType::Mask;
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                return AnnotationShapeType::Spline;
            } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                return AnnotationShapeType::Point;
            } else {
                return AnnotationShapeType::Skeleton;
            }
        },
        shape);
}

const char* annotation_shape_type_name(const AnnotationShapeType shape_type) {
    switch (shape_type) {
        case AnnotationShapeType::Box:
            return "box";
        case AnnotationShapeType::Mask:
            return "mask";
        case AnnotationShapeType::Spline:
            return "spline";
        case AnnotationShapeType::Point:
            return "point";
        case AnnotationShapeType::Skeleton:
            return "skeleton";
    }
    return "unknown";
}

const char* annotation_object_shape_label(const AnnotationObject& object) {
    return annotation_shape_type_name(annotation_shape_type(object.shape));
}

std::optional<AnnotationBox> annotation_object_bbox(const AnnotationObject& object) {
    return std::visit(
        [](const auto& shape) -> std::optional<AnnotationBox> {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, AnnotationBoxShape> || std::is_same_v<T, AnnotationMaskShape>) {
                return shape.box;
            } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                return AnnotationBox{
                    static_cast<int>(shape.point.x),
                    static_cast<int>(shape.point.y),
                    static_cast<int>(shape.point.x) + 1,
                    static_cast<int>(shape.point.y) + 1,
                };
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                std::vector<AnnotationPoint> points;
                points.reserve(shape.knots.size());
                for (const AnnotationSplineKnot& knot : shape.knots) {
                    points.push_back(knot.position);
                    if (knot.in_handle.enabled) {
                        points.push_back(knot.in_handle.position);
                    }
                    if (knot.out_handle.enabled) {
                        points.push_back(knot.out_handle.position);
                    }
                }
                return bbox_from_points(points);
            } else {
                std::vector<AnnotationPoint> points;
                points.reserve(shape.nodes.size());
                for (const AnnotationSkeletonNode& node : shape.nodes) {
                    if (node.visible) {
                        points.push_back(node.point);
                    }
                }
                return bbox_from_points(points);
            }
        },
        object.shape);
}

std::optional<AnnotationBox> annotation_object_display_box(const AnnotationObject& object) {
    return annotation_object_bbox(object);
}

std::vector<AnnotationPoint> annotation_object_points(const AnnotationObject& object) {
    return std::visit(
        [](const auto& shape) {
            using T = std::decay_t<decltype(shape)>;
            std::vector<AnnotationPoint> points;
            if constexpr (std::is_same_v<T, AnnotationBoxShape>) {
                points.push_back(AnnotationPoint{static_cast<float>(shape.box.x1), static_cast<float>(shape.box.y1)});
                points.push_back(AnnotationPoint{static_cast<float>(shape.box.x2), static_cast<float>(shape.box.y1)});
                points.push_back(AnnotationPoint{static_cast<float>(shape.box.x2), static_cast<float>(shape.box.y2)});
                points.push_back(AnnotationPoint{static_cast<float>(shape.box.x1), static_cast<float>(shape.box.y2)});
            } else if constexpr (std::is_same_v<T, AnnotationMaskShape>) {
                points.push_back(AnnotationPoint{static_cast<float>(shape.box.x1), static_cast<float>(shape.box.y1)});
                points.push_back(AnnotationPoint{static_cast<float>(shape.box.x2), static_cast<float>(shape.box.y2)});
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                points.reserve(shape.knots.size());
                for (const AnnotationSplineKnot& knot : shape.knots) {
                    points.push_back(knot.position);
                }
            } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                points.push_back(shape.point);
            } else {
                points.reserve(shape.nodes.size());
                for (const AnnotationSkeletonNode& node : shape.nodes) {
                    points.push_back(node.point);
                }
            }
            return points;
        },
        object.shape);
}

const AnnotationMaskShape* annotation_object_mask_shape(const AnnotationObject& object) {
    return std::get_if<AnnotationMaskShape>(&object.shape);
}

AnnotationMaskShape* annotation_object_mask_shape(AnnotationObject* object) {
    if (object == nullptr) {
        return nullptr;
    }
    return std::get_if<AnnotationMaskShape>(&object->shape);
}

bool annotation_object_supports_mask_editing(const AnnotationObject& object) {
    return std::holds_alternative<AnnotationBoxShape>(object.shape) ||
           std::holds_alternative<AnnotationMaskShape>(object.shape);
}

bool translate_annotation_object(AnnotationObject* object, const int dx, const int dy,
                                 const std::uint32_t capture_width, const std::uint32_t capture_height) {
    if (object == nullptr || (dx == 0 && dy == 0)) {
        return false;
    }

    const std::optional<AnnotationBox> bbox = annotation_object_bbox(*object);
    if (!bbox.has_value()) {
        return false;
    }
    const AnnotationBox object_bbox = *bbox;
    const std::pair<int, int> clamped_offset = clamped_box_offset(object_bbox, dx, dy, capture_width, capture_height);
    const int clamped_dx = clamped_offset.first;
    const int clamped_dy = clamped_offset.second;
    if (clamped_dx == 0 && clamped_dy == 0) {
        return false;
    }

    if (auto* shape = std::get_if<AnnotationBoxShape>(&object->shape)) {
        shape->box.x1 += clamped_dx;
        shape->box.y1 += clamped_dy;
        shape->box.x2 += clamped_dx;
        shape->box.y2 += clamped_dy;
        return true;
    }

    if (auto* shape = std::get_if<AnnotationMaskShape>(&object->shape)) {
        const std::pair<int, int> mask_offset =
            clamped_box_offset(box_from_mask_region(shape->region), dx, dy, capture_width, capture_height);
        const int mask_dx = mask_offset.first;
        const int mask_dy = mask_offset.second;
        if (mask_dx == 0 && mask_dy == 0) {
            return false;
        }
        shape->box.x1 += mask_dx;
        shape->box.y1 += mask_dy;
        shape->box.x2 += mask_dx;
        shape->box.y2 += mask_dy;
        shape->region.capture_x = static_cast<std::uint32_t>(static_cast<int>(shape->region.capture_x) + mask_dx);
        shape->region.capture_y = static_cast<std::uint32_t>(static_cast<int>(shape->region.capture_y) + mask_dy);
        return true;
    }

    if (auto* shape = std::get_if<AnnotationPointShape>(&object->shape)) {
        translate_point(&shape->point, clamped_dx, clamped_dy, capture_width, capture_height);
        return true;
    }

    if (auto* shape = std::get_if<AnnotationSplineShape>(&object->shape)) {
        for (AnnotationSplineKnot& knot : shape->knots) {
            translate_point(&knot.position, clamped_dx, clamped_dy, capture_width, capture_height);
            if (knot.in_handle.enabled) {
                translate_point(&knot.in_handle.position, clamped_dx, clamped_dy, capture_width, capture_height);
            }
            if (knot.out_handle.enabled) {
                translate_point(&knot.out_handle.position, clamped_dx, clamped_dy, capture_width, capture_height);
            }
        }
        return true;
    }

    auto* shape = std::get_if<AnnotationSkeletonShape>(&object->shape);
    if (shape == nullptr) {
        return false;
    }
    for (AnnotationSkeletonNode& node : shape->nodes) {
        translate_point(&node.point, clamped_dx, clamped_dy, capture_width, capture_height);
    }
    return true;
}

bool resize_annotation_object_to_box(AnnotationObject* object, const AnnotationBox& box,
                                     const std::uint32_t capture_width, const std::uint32_t capture_height) {
    if (object == nullptr) {
        return false;
    }

    const AnnotationBox target_box = clamp_capture_box(box, capture_width, capture_height);
    if (!box_has_area(target_box)) {
        return false;
    }

    const std::optional<AnnotationBox> source_box = annotation_object_bbox(*object);
    if (source_box.has_value() && boxes_equal(*source_box, target_box)) {
        return false;
    }
    std::visit(
        [&](auto& shape) {
            using T = std::decay_t<decltype(shape)>;
            if constexpr (std::is_same_v<T, AnnotationBoxShape>) {
                shape.box = target_box;
            } else if constexpr (std::is_same_v<T, AnnotationMaskShape>) {
                const AnnotationMaskRegion next_region = mask_region_from_box(target_box);
                shape.mask = resize_mask_nearest(shape.mask, shape.region.width, shape.region.height, next_region.width,
                                                 next_region.height);
                shape.region = next_region;
                shape.box = target_box;
            } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
                shape.point = AnnotationPoint{
                    clamp_capture_axis(static_cast<float>(target_box.x1 + target_box.x2 - 1) * 0.5f, capture_width),
                    clamp_capture_axis(static_cast<float>(target_box.y1 + target_box.y2 - 1) * 0.5f, capture_height),
                };
            } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
                if (!source_box.has_value()) {
                    return;
                }
                for (AnnotationSplineKnot& knot : shape.knots) {
                    scale_point_to_box(&knot.position, *source_box, target_box, capture_width, capture_height);
                    if (knot.in_handle.enabled) {
                        scale_point_to_box(&knot.in_handle.position, *source_box, target_box, capture_width,
                                           capture_height);
                    }
                    if (knot.out_handle.enabled) {
                        scale_point_to_box(&knot.out_handle.position, *source_box, target_box, capture_width,
                                           capture_height);
                    }
                }
            } else {
                if (!source_box.has_value()) {
                    return;
                }
                for (AnnotationSkeletonNode& node : shape.nodes) {
                    scale_point_to_box(&node.point, *source_box, target_box, capture_width, capture_height);
                }
            }
        },
        object->shape);

    if (!source_box.has_value()) {
        return std::holds_alternative<AnnotationBoxShape>(object->shape) ||
               std::holds_alternative<AnnotationMaskShape>(object->shape) ||
               std::holds_alternative<AnnotationPointShape>(object->shape);
    }
    return true;
}

std::optional<AnnotationPoint> annotation_spline_segment_point(const AnnotationSplineShape& spline,
                                                               const std::size_t segment_index, const float t) {
    if (spline.knots.size() < 2U) {
        return std::nullopt;
    }

    const std::size_t segment_count = spline.closed ? spline.knots.size() : spline.knots.size() - 1U;
    if (segment_count == 0U || segment_index >= segment_count) {
        return std::nullopt;
    }

    const AnnotationSplineKnot& start = spline.knots[segment_index];
    const AnnotationSplineKnot& end = spline.knots[(segment_index + 1U) % spline.knots.size()];
    const AnnotationPoint p0 = start.position;
    const AnnotationPoint p1 = start.out_handle.enabled ? start.out_handle.position : start.position;
    const AnnotationPoint p2 = end.in_handle.enabled ? end.in_handle.position : end.position;
    const AnnotationPoint p3 = end.position;
    return cubic_bezier_point(p0, p1, p2, p3, std::clamp(t, 0.0f, 1.0f));
}

std::vector<AnnotationPoint> sample_annotation_spline_points(const AnnotationSplineShape& spline,
                                                             int samples_per_segment) {
    std::vector<AnnotationPoint> points;
    if (spline.knots.empty()) {
        return points;
    }
    if (spline.knots.size() == 1U) {
        points.push_back(spline.knots.front().position);
        return points;
    }

    const int segment_samples = std::max(1, samples_per_segment);
    const std::size_t segment_count = spline.closed ? spline.knots.size() : spline.knots.size() - 1U;
    points.reserve(segment_count * static_cast<std::size_t>(segment_samples + 1));
    for (std::size_t segment_index = 0; segment_index < segment_count; ++segment_index) {
        const AnnotationSplineKnot& start = spline.knots[segment_index];
        const AnnotationSplineKnot& end = spline.knots[(segment_index + 1U) % spline.knots.size()];
        const AnnotationPoint p0 = start.position;
        const AnnotationPoint p1 = start.out_handle.enabled ? start.out_handle.position : start.position;
        const AnnotationPoint p2 = end.in_handle.enabled ? end.in_handle.position : end.position;
        const AnnotationPoint p3 = end.position;

        if (segment_index == 0U) {
            points.push_back(p0);
        }
        for (int sample = 1; sample <= segment_samples; ++sample) {
            const float t = static_cast<float>(sample) / static_cast<float>(segment_samples);
            points.push_back(cubic_bezier_point(p0, p1, p2, p3, t));
        }
    }
    return points;
}

}  
