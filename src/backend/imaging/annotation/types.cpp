module;
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>
#include "detail/manual_mask_mapping_cuda_abi.h"
module mmltk.backend.imaging.annotation.core;
namespace mmltk::backend::imaging::annotation {
namespace {
std::optional<AnnotationBox> bbox_from_points(const std::vector<AnnotationPoint>& points) {
 if (points.empty()) { return std::nullopt; }
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
AnnotationBox clamp_capture_box(AnnotationBox box, const std::uint32_t capture_width, const std::uint32_t capture_height) {
 const auto clamp_axis = [](int& low, int& high, const int max_value) {
  low = std::clamp(low, 0, max_value);
  high = std::clamp(high, 0, max_value);
  if (high < low) { std::swap(low, high); }
 };
 clamp_axis(box.x1, box.x2, static_cast<int>(capture_width));
 clamp_axis(box.y1, box.y2, static_cast<int>(capture_height));
 return box;
}
bool box_has_area(const AnnotationBox& box) { return box.x2 > box.x1 && box.y2 > box.y1; }
std::pair<int, int> clamped_box_offset(const AnnotationBox& box, const int dx, const int dy, const std::uint32_t capture_width, const std::uint32_t capture_height) {
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
void translate_point(AnnotationPoint* point, const int dx, const int dy, const std::uint32_t capture_width, const std::uint32_t capture_height) {
 if (point == nullptr) { return; }
 point->x = clamp_capture_axis(point->x + static_cast<float>(dx), capture_width);
 point->y = clamp_capture_axis(point->y + static_cast<float>(dy), capture_height);
}
float scale_axis(const float value, const float source_min, const float source_extent, const float target_min, const float target_extent, const std::uint32_t capture_extent) {
 const float normalized = source_extent > 0.0f ? (value - source_min) / source_extent : 0.5f;
 return clamp_capture_axis(target_min + normalized * target_extent, capture_extent);
}
void scale_point_to_box(AnnotationPoint* point, const AnnotationBox& source_box, const AnnotationBox& target_box, const std::uint32_t capture_width, const std::uint32_t capture_height) {
 if (point == nullptr) { return; }
 const auto source_width = static_cast<float>(source_box.x2 - source_box.x1);
 const auto source_height = static_cast<float>(source_box.y2 - source_box.y1);
 const auto target_width = static_cast<float>(target_box.x2 - target_box.x1);
 const auto target_height = static_cast<float>(target_box.y2 - target_box.y1);
 point->x = scale_axis(point->x, static_cast<float>(source_box.x1), source_width, static_cast<float>(target_box.x1), target_width, capture_width);
 point->y = scale_axis(point->y, static_cast<float>(source_box.y1), source_height, static_cast<float>(target_box.y1), target_height, capture_height);
}
std::vector<std::uint8_t> resize_mask_nearest(
 const std::vector<std::uint8_t>& source, const std::uint32_t source_width, const std::uint32_t source_height, const std::uint32_t target_width, const std::uint32_t target_height) {
 if (target_width == 0U || target_height == 0U) { return {}; }
 std::vector<std::uint8_t> resized(static_cast<std::size_t>(target_width) * static_cast<std::size_t>(target_height), 0U);
 if (source_width == 0U || source_height == 0U || source.size() != static_cast<std::size_t>(source_width) * static_cast<std::size_t>(source_height)) {
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
AnnotationPoint cubic_bezier_point(const AnnotationPoint& p0, const AnnotationPoint& p1, const AnnotationPoint& p2, const AnnotationPoint& p3, const float t) {
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
struct SplineSegmentControlPoints {
 AnnotationPoint p0;
 AnnotationPoint p1;
 AnnotationPoint p2;
 AnnotationPoint p3;
};
SplineSegmentControlPoints spline_segment_control_points(const AnnotationSplineShape& spline, const std::size_t segment_index) {
 const AnnotationSplineKnot& start = spline.knots[segment_index];
 const AnnotationSplineKnot& end = spline.knots[(segment_index + 1U) % spline.knots.size()];
 return SplineSegmentControlPoints{
  start.position,
  start.out_handle.enabled ? start.out_handle.position : start.position,
  end.in_handle.enabled ? end.in_handle.position : end.position,
  end.position,
 };
}
}  // namespace
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
  case AnnotationSplineHandleMode::Corner: return "corner";
  case AnnotationSplineHandleMode::Smooth: return "smooth";
  case AnnotationSplineHandleMode::Mirrored: return "mirrored";
 }
 return "corner";
}
AnnotationSplineHandleMode annotation_spline_handle_mode_from_name(const std::string_view value) {
 if (value == "smooth") { return AnnotationSplineHandleMode::Smooth; }
 if (value == "mirrored") { return AnnotationSplineHandleMode::Mirrored; }
 return AnnotationSplineHandleMode::Corner;
}
// AnnotationShapeType is declared in the same order as the AnnotationShapeVariant alternatives, so
// the tag is the held alternative's index. These asserts pin that correspondence, which is what an
// `if constexpr` ladder over the alternatives used to restate by hand.
static_assert(std::variant_size_v<AnnotationShapeVariant> == 5U);
static_assert(std::is_same_v<std::variant_alternative_t<0U, AnnotationShapeVariant>, AnnotationBoxShape>);
static_assert(std::is_same_v<std::variant_alternative_t<1U, AnnotationShapeVariant>, AnnotationMaskShape>);
static_assert(std::is_same_v<std::variant_alternative_t<2U, AnnotationShapeVariant>, AnnotationSplineShape>);
static_assert(std::is_same_v<std::variant_alternative_t<3U, AnnotationShapeVariant>, AnnotationPointShape>);
static_assert(std::is_same_v<std::variant_alternative_t<4U, AnnotationShapeVariant>, AnnotationSkeletonShape>);
static_assert(static_cast<std::size_t>(AnnotationShapeType::Box) == 0U);
static_assert(static_cast<std::size_t>(AnnotationShapeType::Skeleton) == 4U);
AnnotationShapeType annotation_shape_type(const AnnotationShapeVariant& shape) { return static_cast<AnnotationShapeType>(shape.index()); }
const char* annotation_shape_type_name(const AnnotationShapeType shape_type) {
 switch (shape_type) {
  case AnnotationShapeType::Box: return "box";
  case AnnotationShapeType::Mask: return "mask";
  case AnnotationShapeType::Spline: return "spline";
  case AnnotationShapeType::Point: return "point";
  case AnnotationShapeType::Skeleton: return "skeleton";
 }
 return "unknown";
}
const char* annotation_object_shape_label(const AnnotationObject& object) { return annotation_shape_type_name(annotation_shape_type(object.shape)); }
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
     if (knot.in_handle.enabled) { points.push_back(knot.in_handle.position); }
     if (knot.out_handle.enabled) { points.push_back(knot.out_handle.position); }
    }
    return bbox_from_points(points);
   } else {
    std::vector<AnnotationPoint> points;
    points.reserve(shape.nodes.size());
    for (const AnnotationSkeletonNode& node : shape.nodes) {
     if (node.visible) { points.push_back(node.point); }
    }
    return bbox_from_points(points);
   }
  },
  object.shape);
}
std::optional<AnnotationBox> annotation_object_display_box(const AnnotationObject& object) { return annotation_object_bbox(object); }
std::vector<AnnotationPoint> annotation_object_points(const AnnotationObject& object) {
 return std::visit(
  [](const auto& shape) {
   using T = std::decay_t<decltype(shape)>;
   std::vector<AnnotationPoint> points;
   [[maybe_unused]] const auto push_corner = [&points](const int x, const int y) { points.push_back(AnnotationPoint{static_cast<float>(x), static_cast<float>(y)}); };
   if constexpr (std::is_same_v<T, AnnotationBoxShape>) {
    push_corner(shape.box.x1, shape.box.y1);
    push_corner(shape.box.x2, shape.box.y1);
    push_corner(shape.box.x2, shape.box.y2);
    push_corner(shape.box.x1, shape.box.y2);
   } else if constexpr (std::is_same_v<T, AnnotationMaskShape>) {
    push_corner(shape.box.x1, shape.box.y1);
    push_corner(shape.box.x2, shape.box.y2);
   } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
    points.reserve(shape.knots.size());
    for (const AnnotationSplineKnot& knot : shape.knots) { points.push_back(knot.position); }
   } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
    points.push_back(shape.point);
   } else {
    points.reserve(shape.nodes.size());
    for (const AnnotationSkeletonNode& node : shape.nodes) { points.push_back(node.point); }
   }
   return points;
  },
  object.shape);
}
const AnnotationMaskShape* annotation_object_mask_shape(const AnnotationObject& object) { return std::get_if<AnnotationMaskShape>(&object.shape); }
AnnotationMaskShape* annotation_object_mask_shape(AnnotationObject* object) {
 if (object == nullptr) { return nullptr; }
 return std::get_if<AnnotationMaskShape>(&object->shape);
}
bool annotation_object_supports_mask_editing(const AnnotationObject& object) {
 return std::holds_alternative<AnnotationBoxShape>(object.shape) || std::holds_alternative<AnnotationMaskShape>(object.shape);
}
bool translate_annotation_object(AnnotationObject* object, const int dx, const int dy, const std::uint32_t capture_width, const std::uint32_t capture_height, const bool clip_to_bounds) {
 if (object == nullptr || (dx == 0 && dy == 0)) { return false; }
 const std::optional<AnnotationBox> bbox = annotation_object_bbox(*object);
 if (!bbox.has_value()) { return false; }
 const AnnotationBox object_bbox = *bbox;
 const std::pair<int, int> clamped_offset = clip_to_bounds ? std::pair<int, int>{dx, dy} : clamped_box_offset(object_bbox, dx, dy, capture_width, capture_height);
 const int clamped_dx = clamped_offset.first;
 const int clamped_dy = clamped_offset.second;
 if (clamped_dx == 0 && clamped_dy == 0) { return false; }
 if (auto* shape = std::get_if<AnnotationBoxShape>(&object->shape)) {
  shape->box.x1 += clamped_dx;
  shape->box.y1 += clamped_dy;
  shape->box.x2 += clamped_dx;
  shape->box.y2 += clamped_dy;
  if (clip_to_bounds) { shape->box = clamp_capture_box(shape->box, capture_width, capture_height); }
  return true;
 }
 if (auto* shape = std::get_if<AnnotationMaskShape>(&object->shape)) {
  if (!materialize_annotation_mask(shape)) return false;
  if (clip_to_bounds) {
   const int translated_x = static_cast<int>(shape->region.capture_x) + dx;
   const int translated_y = static_cast<int>(shape->region.capture_y) + dy;
   const int translated_right = translated_x + static_cast<int>(shape->region.width);
   const int translated_bottom = translated_y + static_cast<int>(shape->region.height);
   const int visible_x1 = std::clamp(translated_x, 0, static_cast<int>(capture_width));
   const int visible_y1 = std::clamp(translated_y, 0, static_cast<int>(capture_height));
   const int visible_x2 = std::clamp(translated_right, 0, static_cast<int>(capture_width));
   const int visible_y2 = std::clamp(translated_bottom, 0, static_cast<int>(capture_height));
   const std::uint32_t visible_width = static_cast<std::uint32_t>(std::max(0, visible_x2 - visible_x1));
   const std::uint32_t visible_height = static_cast<std::uint32_t>(std::max(0, visible_y2 - visible_y1));
   std::vector<std::uint8_t> clipped_mask(static_cast<std::size_t>(visible_width) * static_cast<std::size_t>(visible_height), 0U);
   const bool valid_mask = shape->mask.size() == static_cast<std::size_t>(shape->region.width) * static_cast<std::size_t>(shape->region.height);
   if (valid_mask && visible_width != 0U && visible_height != 0U) {
    const std::uint32_t source_x = static_cast<std::uint32_t>(visible_x1 - translated_x);
    const std::uint32_t source_y = static_cast<std::uint32_t>(visible_y1 - translated_y);
    for (std::uint32_t row = 0U; row < visible_height; ++row) {
     const std::size_t source_offset = static_cast<std::size_t>(source_y + row) * shape->region.width + source_x;
     const std::size_t target_offset = static_cast<std::size_t>(row) * visible_width;
     std::copy_n(shape->mask.begin() + static_cast<std::ptrdiff_t>(source_offset), visible_width, clipped_mask.begin() + static_cast<std::ptrdiff_t>(target_offset));
    }
   }
   shape->mask = std::move(clipped_mask);
   shape->region = AnnotationMaskRegion{
    static_cast<std::uint32_t>(visible_x1),
    static_cast<std::uint32_t>(visible_y1),
    visible_width,
    visible_height,
   };
   shape->box.x1 += dx;
   shape->box.y1 += dy;
   shape->box.x2 += dx;
   shape->box.y2 += dy;
   shape->box = clamp_capture_box(shape->box, capture_width, capture_height);
   return true;
  }
  const std::pair<int, int> mask_offset = clamped_box_offset(box_from_mask_region(shape->region), dx, dy, capture_width, capture_height);
  const int mask_dx = mask_offset.first;
  const int mask_dy = mask_offset.second;
  if (mask_dx == 0 && mask_dy == 0) { return false; }
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
   if (knot.in_handle.enabled) { translate_point(&knot.in_handle.position, clamped_dx, clamped_dy, capture_width, capture_height); }
   if (knot.out_handle.enabled) { translate_point(&knot.out_handle.position, clamped_dx, clamped_dy, capture_width, capture_height); }
  }
  return true;
 }
 auto* shape = std::get_if<AnnotationSkeletonShape>(&object->shape);
 if (shape == nullptr) { return false; }
 for (AnnotationSkeletonNode& node : shape->nodes) { translate_point(&node.point, clamped_dx, clamped_dy, capture_width, capture_height); }
 return true;
}
bool resize_annotation_object_to_box(AnnotationObject* object, const AnnotationBox& box, const std::uint32_t capture_width, const std::uint32_t capture_height) {
 if (object == nullptr) { return false; }
 const AnnotationBox target_box = clamp_capture_box(box, capture_width, capture_height);
 if (!box_has_area(target_box)) { return false; }
 const std::optional<AnnotationBox> source_box = annotation_object_bbox(*object);
 if (source_box.has_value() && *source_box == target_box) { return false; }
 std::visit(
  [&](auto& shape) {
   using T = std::decay_t<decltype(shape)>;
   if constexpr (std::is_same_v<T, AnnotationBoxShape>) {
    shape.box = target_box;
   } else if constexpr (std::is_same_v<T, AnnotationMaskShape>) {
    if (!materialize_annotation_mask(&shape)) return;
    const AnnotationMaskRegion next_region = mask_region_from_box(target_box);
    shape.mask = resize_mask_nearest(shape.mask, shape.region.width, shape.region.height, next_region.width, next_region.height);
    shape.region = next_region;
    shape.box = target_box;
   } else if constexpr (std::is_same_v<T, AnnotationPointShape>) {
    shape.point = AnnotationPoint{
     clamp_capture_axis(static_cast<float>(target_box.x1 + target_box.x2 - 1) * 0.5f, capture_width),
     clamp_capture_axis(static_cast<float>(target_box.y1 + target_box.y2 - 1) * 0.5f, capture_height),
    };
   } else if constexpr (std::is_same_v<T, AnnotationSplineShape>) {
    if (!source_box.has_value()) { return; }
    for (AnnotationSplineKnot& knot : shape.knots) {
     scale_point_to_box(&knot.position, *source_box, target_box, capture_width, capture_height);
     if (knot.in_handle.enabled) { scale_point_to_box(&knot.in_handle.position, *source_box, target_box, capture_width, capture_height); }
     if (knot.out_handle.enabled) { scale_point_to_box(&knot.out_handle.position, *source_box, target_box, capture_width, capture_height); }
    }
   } else {
    if (!source_box.has_value()) { return; }
    for (AnnotationSkeletonNode& node : shape.nodes) { scale_point_to_box(&node.point, *source_box, target_box, capture_width, capture_height); }
   }
  },
  object->shape);
 if (!source_box.has_value()) {
  return std::holds_alternative<AnnotationBoxShape>(object->shape) || std::holds_alternative<AnnotationMaskShape>(object->shape) || std::holds_alternative<AnnotationPointShape>(object->shape);
 }
 return true;
}
std::optional<AnnotationPoint> annotation_spline_segment_point(const AnnotationSplineShape& spline, const std::size_t segment_index, const float t) {
 if (spline.knots.size() < 2U) { return std::nullopt; }
 const std::size_t segment_count = spline.closed ? spline.knots.size() : spline.knots.size() - 1U;
 if (segment_count == 0U || segment_index >= segment_count) { return std::nullopt; }
 const SplineSegmentControlPoints control = spline_segment_control_points(spline, segment_index);
 return cubic_bezier_point(control.p0, control.p1, control.p2, control.p3, std::clamp(t, 0.0f, 1.0f));
}
std::vector<AnnotationPoint> sample_annotation_spline_points(const AnnotationSplineShape& spline, int samples_per_segment) {
 std::vector<AnnotationPoint> points;
 if (spline.knots.empty()) { return points; }
 if (spline.knots.size() == 1U) {
  points.push_back(spline.knots.front().position);
  return points;
 }
 const int segment_samples = std::max(1, samples_per_segment);
 const std::size_t segment_count = spline.closed ? spline.knots.size() : spline.knots.size() - 1U;
 points.reserve(segment_count * static_cast<std::size_t>(segment_samples + 1));
 for (std::size_t segment_index = 0; segment_index < segment_count; ++segment_index) {
  const SplineSegmentControlPoints control = spline_segment_control_points(spline, segment_index);
  if (segment_index == 0U) { points.push_back(control.p0); }
  for (int sample = 1; sample <= segment_samples; ++sample) {
   const float t = static_cast<float>(sample) / static_cast<float>(segment_samples);
   points.push_back(cubic_bezier_point(control.p0, control.p1, control.p2, control.p3, t));
  }
 }
 return points;
}
bool annotation_deferred_mask_valid(const AnnotationMaskShape& shape) noexcept {
 const std::shared_ptr<const AnnotationDeferredMask>& deferred = shape.deferred;
 if (deferred == nullptr || deferred->source_width == 0U || deferred->source_height == 0U || deferred->source_crop_width == 0U || deferred->source_crop_height == 0U || deferred->output_width == 0U ||
     deferred->output_height == 0U || shape.region.width == 0U || shape.region.height == 0U) {
  return false;
 }
 const auto contained = [](const std::uint64_t origin, const std::uint32_t extent, const std::uint32_t limit) noexcept { return origin + extent <= limit; };
 if (!contained(deferred->source_crop_x, deferred->source_crop_width, deferred->source_width) || !contained(deferred->source_crop_y, deferred->source_crop_height, deferred->source_height) ||
     !contained(static_cast<std::uint64_t>(deferred->view_x) + shape.region.capture_x, shape.region.width, deferred->output_width) ||
     !contained(static_cast<std::uint64_t>(deferred->view_y) + shape.region.capture_y, shape.region.height, deferred->output_height)) {
  return false;
 }
 if (deferred->runs.empty()) { return false; }
 const std::uint64_t source_pixels = static_cast<std::uint64_t>(deferred->source_width) * deferred->source_height;
 std::uint64_t previous_end = 0U;
 for (const AnnotationDeferredMaskRun& run : deferred->runs) {
  const std::uint64_t offset = run.offset;
  const std::uint64_t end = offset + run.length;
  if (run.length == 0U || offset < previous_end || end > source_pixels) { return false; }
  previous_end = end;
 }
 return true;
}
std::optional<std::vector<std::uint8_t>> project_annotation_mask_pixels(const AnnotationMaskShape& shape) {
 const std::size_t pixel_count = static_cast<std::size_t>(shape.region.width) * shape.region.height;
 if (shape.mask.size() == pixel_count) return shape.mask;
 if (!shape.runs.empty()) {
  std::vector<std::uint8_t> pixels(pixel_count, 0U);
  for (const AnnotationDeferredMaskRun& run : shape.runs) {
   if (run.length == 0U || run.offset > pixel_count || run.length > pixel_count - run.offset) return std::nullopt;
   std::fill_n(pixels.begin() + run.offset, run.length, 1U);
  }
  return pixels;
 }
 if (!annotation_deferred_mask_valid(shape)) return std::nullopt;
 const std::shared_ptr<const AnnotationDeferredMask>& deferred = shape.deferred;
 std::vector<std::uint8_t> pixels(pixel_count, 0U);
 std::size_t run_index = 0U;
 for (std::uint32_t y = 0U; y < shape.region.height; ++y) {
  const std::uint32_t output_y = deferred->view_y + shape.region.capture_y + y;
  const std::uint32_t source_y = std::min(deferred->source_height - 1U, deferred->source_crop_y + detail::project_crop_coordinate(output_y, deferred->source_crop_height, deferred->output_height));
  for (std::uint32_t x = 0U; x < shape.region.width; ++x) {
   const std::uint32_t output_x = deferred->view_x + shape.region.capture_x + x;
   const std::uint32_t source_x = std::min(deferred->source_width - 1U, deferred->source_crop_x + detail::project_crop_coordinate(output_x, deferred->source_crop_width, deferred->output_width));
   const std::uint64_t flattened = static_cast<std::uint64_t>(source_y) * deferred->source_width + source_x;
   while (run_index < deferred->runs.size() && static_cast<std::uint64_t>(deferred->runs[run_index].offset) + deferred->runs[run_index].length <= flattened) { ++run_index; }
   if (run_index < deferred->runs.size() && deferred->runs[run_index].offset <= flattened) { pixels[static_cast<std::size_t>(y) * shape.region.width + x] = 1U; }
  }
 }
 return pixels;
}
bool materialize_annotation_mask(AnnotationMaskShape* const shape) {
 if (shape == nullptr) return false;
 const std::size_t pixel_count = static_cast<std::size_t>(shape->region.width) * shape->region.height;
 if (shape->mask.size() == pixel_count) return true;
 std::optional<std::vector<std::uint8_t>> pixels = project_annotation_mask_pixels(*shape);
 if (!pixels.has_value()) return false;
 shape->mask = std::move(*pixels);
 shape->deferred.reset();
 shape->runs.clear();
 return true;
}
}  // namespace mmltk::backend::imaging::annotation
