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
using JointLookup = std::unordered_map<std::string_view, JointIndex>;
JointLookup joint_lookup(const ClassKeypointSchema& schema) {
 JointLookup lookup;
 lookup.reserve(schema.names.size());
 for (std::size_t index = 0; index < schema.names.size(); ++index) lookup.emplace(schema.names[index], checked_joint_index(index, schema.names.size()));
 return lookup;
}
std::set<std::pair<std::string_view, std::string_view>> shared_edges(const ClassKeypointSchema& schema, const JointLookup& other) {
 std::set<std::pair<std::string_view, std::string_view>> edges;
 for (const auto edge : schema.skeleton) {
  std::string_view first = schema.names[edge.first], second = schema.names[edge.second];
  if (!other.contains(first) || !other.contains(second)) continue;
  if (second < first) std::swap(first, second);
  edges.emplace(first, second);
 }
 return edges;
}
std::vector<std::pair<JointIndex, JointIndex>> joint_correspondence(const ClassKeypointSchema& source, const ClassKeypointSchema& destination, bool exact) {
 validate_keypoint_schema(source);
 validate_keypoint_schema(destination);
 if (exact && source.names.size() != destination.names.size()) throw std::invalid_argument("keypoint schemas have different joint sets");
 const auto source_lookup = joint_lookup(source), destination_lookup = joint_lookup(destination);
 std::vector<std::pair<JointIndex, JointIndex>> result;
 result.reserve(std::min(source.names.size(), destination.names.size()));
 for (std::size_t target = 0; target < destination.names.size(); ++target) {
  const auto found = source_lookup.find(destination.names[target]);
  if (found == source_lookup.end()) {
   if (exact) throw std::invalid_argument("keypoint schemas have different joint sets");
   continue;
  }
  const auto from = found->second;
  if (source.oks_sigmas[from] != destination.oks_sigmas[target] || source.names[source.horizontal_flip[from]] != destination.names[destination.horizontal_flip[target]])
   throw std::invalid_argument("keypoint schemas disagree on joint meaning: " + destination.names[target]);
  result.emplace_back(from, checked_joint_index(target, destination.names.size()));
 }
 if (shared_edges(source, destination_lookup) != shared_edges(destination, source_lookup)) throw std::invalid_argument("keypoint schemas disagree on skeleton meaning for shared joints");
 return result;
}
}  // namespace
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
 const auto pairs = joint_correspondence(source, destination, true);
 std::vector<JointIndex> permutation(source.names.size());
 for (const auto& [from, to] : pairs) permutation[from] = to;
 return permutation;
}
std::vector<std::pair<JointIndex, JointIndex>> shared_keypoint_correspondence(const ClassKeypointSchema& source, const ClassKeypointSchema& destination) {
 return joint_correspondence(source, destination, false);
}
ClassKeypointSchema coco_person_keypoint_schema() {
 // COCO person category order; edges use zero-based canonical joint references.
 return {
  .names =
   {"nose", "left_eye", "right_eye", "left_ear", "right_ear", "left_shoulder", "right_shoulder", "left_elbow", "right_elbow", "left_wrist", "right_wrist", "left_hip", "right_hip", "left_knee",
    "right_knee", "left_ankle", "right_ankle"},
  .skeleton = {{15, 13}, {13, 11}, {16, 14}, {14, 12}, {11, 12}, {5, 11}, {6, 12}, {5, 6}, {5, 7}, {6, 8}, {7, 9}, {8, 10}, {1, 2}, {0, 1}, {0, 2}, {1, 3}, {2, 4}, {3, 5}, {4, 6}},
  .horizontal_flip = {0, 2, 1, 4, 3, 6, 5, 8, 7, 10, 9, 12, 11, 14, 13, 16, 15},
  .oks_sigmas = {.026, .025, .025, .035, .035, .079, .079, .072, .072, .062, .062, .107, .107, .087, .087, .089, .089},
 };
}
}  // namespace mmltk::backend::data::catalog
