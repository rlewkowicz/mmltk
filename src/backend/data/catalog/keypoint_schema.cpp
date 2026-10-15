#include "src/backend/data/catalog/keypoint_schema.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
namespace mmltk::backend::data::catalog {
namespace {
SkeletonEdge ordered_edge(SkeletonEdge edge) noexcept {
 if (edge.second < edge.first) std::swap(edge.first, edge.second);
 return edge;
}
}
JointIndex checked_joint_index(const std::size_t index, const std::size_t count) {
 if (count > kKeypointsPerClassCapacity || index >= count) throw std::invalid_argument("keypoint index exceeds admitted joint count");
 return static_cast<JointIndex>(index);
}
std::uint32_t checked_keypoint_payload_count(const std::size_t instances, const std::size_t joints_per_instance) {
 constexpr auto limit = std::numeric_limits<std::uint32_t>::max() / sizeof(KeypointAnnotation);
 if (joints_per_instance > kKeypointsPerClassCapacity || (joints_per_instance != 0U && instances > limit / joints_per_instance))
  throw std::invalid_argument("keypoint payload exceeds checked byte capacity");
 return static_cast<std::uint32_t>(instances * joints_per_instance);
}
void validate_keypoint_schema(const ClassKeypointSchema& schema) {
 const auto count = schema.names.size();
 if (count > kKeypointsPerClassCapacity || schema.horizontal_flip.size() != count || schema.oks_sigmas.size() != count || schema.skeleton.size() > kSkeletonEdgeCapacity)
  throw std::invalid_argument("keypoint schema counts exceed capacity or disagree");
 std::unordered_set<std::string_view> names;
 names.reserve(count);
 for (std::size_t index = 0; index < count; ++index) {
  const auto& name = schema.names[index];
  if (name.empty() || name.size() > kKeypointNameCapacity || name.find('\0') != std::string::npos || !names.insert(name).second)
   throw std::invalid_argument("keypoint names must be unique, nonempty, bounded and NUL-free");
  const auto flip = schema.horizontal_flip[index];
  if (flip >= count || schema.horizontal_flip[flip] != index) throw std::invalid_argument("keypoint horizontal flip must be an involutive permutation");
  if (!std::isfinite(schema.oks_sigmas[index]) || schema.oks_sigmas[index] <= 0.0) throw std::invalid_argument("keypoint OKS sigmas must be finite and positive");
 }
 std::set<SkeletonEdge> edges;
 for (const auto edge : schema.skeleton) {
  if (edge.first >= count || edge.second >= count || edge.first == edge.second || !edges.insert(ordered_edge(edge)).second)
   throw std::invalid_argument("keypoint skeleton contains an invalid or duplicate edge");
 }
}
void validate_keypoint_annotations(const std::span<const KeypointAnnotation> joints, const std::size_t expected_count) {
 if (expected_count > kKeypointsPerClassCapacity || joints.size() != expected_count) throw std::invalid_argument("instance joint count disagrees with admitted schema");
 for (const auto joint : joints)
  if (!std::isfinite(joint.x) || !std::isfinite(joint.y) || !mmltk::frameworks::reflection::enum_contains(joint.visibility))
   throw std::invalid_argument("instance has nonfinite joint coordinates or invalid visibility");
}
ClassKeypointSchema permute_keypoint_schema(const ClassKeypointSchema& source, const std::span<const JointIndex> permutation) {
 validate_keypoint_schema(source);
 const auto count = source.names.size();
 if (permutation.size() != count) throw std::invalid_argument("keypoint permutation count disagrees with schema");
 std::array<bool, kKeypointsPerClassCapacity> seen{};
 for (const auto target : permutation) {
  if (target >= count || seen[target]) throw std::invalid_argument("keypoint permutation is not bijective");
  seen[target] = true;
 }
 ClassKeypointSchema result;
 result.names.resize(count);
 result.horizontal_flip.resize(count);
 result.oks_sigmas.resize(count);
 result.skeleton.reserve(source.skeleton.size());
 for (std::size_t index = 0; index < count; ++index) {
  const auto target = permutation[index];
  result.names[target] = source.names[index];
  result.horizontal_flip[target] = permutation[source.horizontal_flip[index]];
  result.oks_sigmas[target] = source.oks_sigmas[index];
 }
 for (const auto edge : source.skeleton) result.skeleton.push_back(ordered_edge({permutation[edge.first], permutation[edge.second]}));
 std::ranges::sort(result.skeleton);
 return result;
}
std::vector<JointIndex> keypoint_permutation(const ClassKeypointSchema& source, const ClassKeypointSchema& destination) {
 validate_keypoint_schema(source);
 validate_keypoint_schema(destination);
 if (source.names.size() != destination.names.size()) throw std::invalid_argument("keypoint schemas have different joint sets");
 std::unordered_map<std::string_view, JointIndex> lookup;
 for (std::size_t index = 0; index < destination.names.size(); ++index) lookup.emplace(destination.names[index], checked_joint_index(index, destination.names.size()));
 std::vector<JointIndex> permutation;
 permutation.reserve(source.names.size());
 for (const auto& name : source.names) {
  const auto found = lookup.find(name);
  if (found == lookup.end()) throw std::invalid_argument("keypoint schemas have different joint sets");
  permutation.push_back(found->second);
 }
 const auto mapped = permute_keypoint_schema(source, permutation);
 auto edges = destination.skeleton;
 for (auto& edge : edges) edge = ordered_edge(edge);
 std::ranges::sort(edges);
 if (mapped.horizontal_flip != destination.horizontal_flip || mapped.oks_sigmas != destination.oks_sigmas || mapped.skeleton != edges)
  throw std::invalid_argument("keypoint schemas disagree on joint meaning");
 return permutation;
}
ClassKeypointSchema coco_person_keypoint_schema() {
 // COCO person category order; edges use zero-based canonical joint references.
 return {
  .names = {"nose", "left_eye", "right_eye", "left_ear", "right_ear", "left_shoulder", "right_shoulder", "left_elbow", "right_elbow", "left_wrist", "right_wrist", "left_hip", "right_hip", "left_knee", "right_knee", "left_ankle", "right_ankle"},
  .skeleton = {{15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12}, {5, 6}, {5, 7}, {6, 8}, {7, 9}, {8, 10}, {1, 2}, {0, 1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4, 6}},
  .horizontal_flip = {0, 2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15},
  .oks_sigmas = {.026, .025, .025, .035, .035, .079, .079, .072, .072, .062, .062, .107, .107, .087, .087, .089, .089},
 };
}
}  // namespace mmltk::backend::data::catalog
