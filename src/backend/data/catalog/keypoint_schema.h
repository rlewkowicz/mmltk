#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "src/frameworks/reflection/reflected_declarations.h"
namespace mmltk::backend::data::catalog {
using JointIndex = std::uint16_t;
inline constexpr std::size_t kKeypointsPerClassCapacity = 256U;
inline constexpr std::size_t kSkeletonEdgeCapacity = 512U;
inline constexpr std::size_t kKeypointNameCapacity = mmltk::frameworks::reflection::kMaximumNameBytes;
enum class KeypointVisibility : std::uint8_t { Unannotated = 0U, Occluded = 1U, Visible = 2U };
MMLTK_REFLECT_ENUM(KeypointVisibility)
struct SkeletonEdge final {
 JointIndex first = 0U;
 JointIndex second = 0U;
 auto operator<=>(const SkeletonEdge&) const = default;
};
MMLTK_REFLECT_FIELDS(SkeletonEdge)
struct ClassKeypointSchema final {
 MMLTK_MAX_ITEMS(kKeypointsPerClassCapacity) MMLTK_MAX_BYTES(kKeypointNameCapacity) std::vector<std::string> names;
 MMLTK_MAX_ITEMS(kSkeletonEdgeCapacity) std::vector<SkeletonEdge> skeleton;
 MMLTK_MAX_ITEMS(kKeypointsPerClassCapacity) std::vector<JointIndex> horizontal_flip;
 MMLTK_MAX_ITEMS(kKeypointsPerClassCapacity) std::vector<double> oks_sigmas;
 auto operator<=>(const ClassKeypointSchema&) const = default;
};
MMLTK_REFLECT_FIELDS(ClassKeypointSchema)
struct KeypointAnnotation final {
 MMLTK_FINITE float x = 0.0F;
 MMLTK_FINITE float y = 0.0F;
 KeypointVisibility visibility = KeypointVisibility::Unannotated;
 bool operator==(const KeypointAnnotation&) const = default;
};
MMLTK_REFLECT_FIELDS(KeypointAnnotation)
// Admission validates the complete candidate before the catalog takes ownership.
void validate_keypoint_schema(const ClassKeypointSchema& schema);
void validate_keypoint_annotations(std::span<const KeypointAnnotation> joints, std::size_t expected_count);
[[nodiscard]] JointIndex checked_joint_index(std::size_t index, std::size_t count);
[[nodiscard]] std::uint32_t checked_keypoint_payload_count(std::size_t instances, std::size_t joints_per_instance);
// Source index -> destination index; verifies sigma, flip and skeleton meaning.
[[nodiscard]] std::vector<JointIndex> keypoint_permutation(const ClassKeypointSchema& source, const ClassKeypointSchema& destination);
[[nodiscard]] ClassKeypointSchema permute_keypoint_schema(const ClassKeypointSchema& source, std::span<const JointIndex> source_to_destination);
[[nodiscard]] ClassKeypointSchema coco_person_keypoint_schema();
}  // namespace mmltk::backend::data::catalog
