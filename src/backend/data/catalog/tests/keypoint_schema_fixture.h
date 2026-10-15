#pragma once
#include <cstddef>
#include <string>
#include <utility>
#include "src/backend/data/catalog/keypoint_schema.h"
namespace mmltk::backend::data::catalog::test_support {
// Full admitted extents for independent transport and runtime boundary cases.
[[nodiscard]] inline ClassKeypointSchema maximum_keypoint_schema() {
 ClassKeypointSchema schema;
 schema.names.reserve(kKeypointsPerClassCapacity);
 schema.horizontal_flip.reserve(kKeypointsPerClassCapacity);
 schema.oks_sigmas.assign(kKeypointsPerClassCapacity, .1);
 for (std::size_t joint = 0; joint < kKeypointsPerClassCapacity; ++joint) {
  auto name = std::to_string(joint);
  name.append(kKeypointNameCapacity - name.size(), 'j');
  schema.names.push_back(std::move(name));
  schema.horizontal_flip.push_back(static_cast<JointIndex>(joint));
 }
 schema.skeleton.reserve(kSkeletonEdgeCapacity);
 for (std::size_t edge = 0; edge < kSkeletonEdgeCapacity; ++edge)
  schema.skeleton.push_back(
   {static_cast<JointIndex>(edge % kKeypointsPerClassCapacity), static_cast<JointIndex>((edge % kKeypointsPerClassCapacity + 1 + edge / kKeypointsPerClassCapacity) % kKeypointsPerClassCapacity)});
 return schema;
}
}  // namespace mmltk::backend::data::catalog::test_support
