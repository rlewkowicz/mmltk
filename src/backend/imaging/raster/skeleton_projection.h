#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <stdexcept>
#include <vector>
#include "src/backend/data/catalog/keypoint_schema.h"
namespace mmltk::backend::imaging::raster {
// A bounded CPU projection, uploaded once with the caller's other geometry.
// Rendering owns rounding, missing-joint filtering and uncertainty geometry;
// producers retain their original confidence/label visibility and covariance.
struct SkeletonProjection final {
 std::size_t points = 0, edges = 0, uncertainty_points = 0, uncertainty_edges = 0;
 std::uint32_t point_count = 0, edge_count = 0, uncertainty_point_count = 0, uncertainty_edge_count = 0;
 std::array<std::uint8_t, 3> color{};
 float score = 0;
 bool ground_truth = false;
};
struct SkeletonSample final {
 float x = 0, y = 0, confidence = 0;
 std::array<float, 3> covariance{};
};
struct SkeletonTransform final {
 float scale_x = 1, scale_y = 1, offset_x = 0, offset_y = 0;
};
enum class SkeletonRounding { AwayFromZero, ToEven };
// Keep every byte address inside the existing signed-addressable 1 GiB
// prediction envelope without introducing a dependency on a model contract.
inline constexpr std::size_t kSkeletonProjectionByteCapacity = std::size_t{1U} << 30U;
inline constexpr std::size_t kSkeletonProjectionWordCapacity = kSkeletonProjectionByteCapacity / sizeof(std::uint32_t);
template <class SampleAt>
[[nodiscard]] SkeletonProjection append_skeleton_projection(std::vector<std::uint32_t>& words, const mmltk::backend::data::catalog::ClassKeypointSchema& schema, SampleAt sample,
 std::span<const mmltk::backend::data::catalog::KeypointVisibility> visibility = {}, SkeletonTransform transform = {}, SkeletonRounding rounding = SkeletonRounding::AwayFromZero) {
 constexpr std::uint32_t ellipse_segments = 16U;
 if (schema.names.size() > mmltk::backend::data::catalog::kKeypointsPerClassCapacity || schema.skeleton.size() > mmltk::backend::data::catalog::kSkeletonEdgeCapacity ||
     (!visibility.empty() && visibility.size() != schema.names.size()))
  throw std::invalid_argument("skeleton projection schema is invalid");
 for (const auto& edge : schema.skeleton)
  if (edge.first >= schema.names.size() || edge.second >= schema.names.size()) throw std::invalid_argument("skeleton edge exceeds schema");
 const auto required = schema.names.size() * (2U + ellipse_segments * 4U) + schema.skeleton.size() * 2U;
 if (words.size() > kSkeletonProjectionWordCapacity || required > kSkeletonProjectionWordCapacity - words.size()) throw std::length_error("skeleton projection capacity exceeded");
 SkeletonProjection result{.points = words.size(), .point_count = static_cast<std::uint32_t>(schema.names.size())};
 std::array<SkeletonSample, mmltk::backend::data::catalog::kKeypointsPerClassCapacity> points{};
 std::array<bool, mmltk::backend::data::catalog::kKeypointsPerClassCapacity> active{};
 const auto coordinate = [rounding](double value) {
  value = std::clamp(value, -100000000.0, 100000000.0);
  const auto lower = std::floor(value);
  const auto rounded = rounding == SkeletonRounding::ToEven && value - lower == 0.5 ? lower + (static_cast<std::int64_t>(lower) % 2 != 0 ? 1.0 : 0.0) : std::round(value);
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(rounded));
 };
 for (std::size_t joint = 0; joint < schema.names.size(); ++joint) {
  auto point = sample(joint);
  point.x = point.x * transform.scale_x + transform.offset_x;
  point.y = point.y * transform.scale_y + transform.offset_y;
  point.covariance[0] *= transform.scale_x * transform.scale_x;
  point.covariance[1] *= transform.scale_x * transform.scale_y;
  point.covariance[2] *= transform.scale_y * transform.scale_y;
  points[joint] = point;
  active[joint] =
   std::isfinite(point.x) && std::isfinite(point.y) && (visibility.empty() ? point.confidence >= 0.5F : visibility[joint] != mmltk::backend::data::catalog::KeypointVisibility::Unannotated);
  words.push_back(coordinate(active[joint] ? point.x : -100000000));
  words.push_back(coordinate(active[joint] ? point.y : -100000000));
 }
 result.edges = words.size();
 for (const auto& edge : schema.skeleton) {
  if (active[edge.first] && active[edge.second]) {
   words.push_back(edge.first);
   words.push_back(edge.second);
   ++result.edge_count;
  }
 }
 result.uncertainty_points = words.size();
 for (std::size_t joint = 0; joint < schema.names.size(); ++joint) {
  const auto& point = points[joint];
  const auto [xx, xy, yy] = point.covariance;
  if (!active[joint] || !std::isfinite(xx) || !std::isfinite(xy) || !std::isfinite(yy) || xx <= 0 || yy <= 0 || xx * double(yy) <= xy * double(xy)) continue;
  const double a = std::sqrt(xx), b = xy / a, c = std::sqrt(yy - b * b);
  for (std::size_t step = 0; step < ellipse_segments; ++step) {
   const auto angle = 2.0 * std::numbers::pi * static_cast<double>(step) / ellipse_segments;
   words.push_back(coordinate(point.x + a * std::cos(angle)));
   words.push_back(coordinate(point.y + b * std::cos(angle) + c * std::sin(angle)));
  }
  result.uncertainty_point_count += ellipse_segments;
 }
 result.uncertainty_edges = words.size();
 for (std::uint32_t point = 0; point < result.uncertainty_point_count; ++point) {
  words.push_back(point);
  words.push_back(point / ellipse_segments * ellipse_segments + (point + 1U) % ellipse_segments);
  ++result.uncertainty_edge_count;
 }
 return result;
}
}  // namespace mmltk::backend::imaging::raster
