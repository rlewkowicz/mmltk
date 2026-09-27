#pragma once
#include "src/frameworks/gpu/cuda/device_execution.h"
#include "src/frameworks/gpu/image/image_buffer.h"
#include "src/common/system/execution_policy.h"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <span>
#include <cstddef>
#include <utility>
namespace mmltk::frameworks::gpu::test_support {
struct IsolatedTestDevice final {
 DeviceExecution execution;
 DeviceContext context;
 explicit IsolatedTestDevice(DeviceExecution selected = resolve_device_execution(0, mmltk::common::system::NumaTopology::Capture()))
     : execution(std::move(selected)), context(execution.device, cuda_image_copy_backend(), DeviceContextMode::Isolated, execution.placement.numa_node, execution) {}
};
// The caller has settled the source and bound its allocating context. Retain
// the borrowed view until this synchronous copy has completed.
inline void read_plane(ImagePlaneView plane, std::span<std::byte> pixels) {
 const auto row_bytes = plane.descriptor.row_bytes();
 REQUIRE(row_bytes > 0U);
 REQUIRE(plane.descriptor.height <= pixels.size() / row_bytes);
 CUDA_MEMCPY2D copy{};
 copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
 copy.srcDevice = plane.data;
 copy.srcPitch = plane.descriptor.pitch_bytes;
 copy.dstMemoryType = CU_MEMORYTYPE_HOST;
 copy.dstHost = pixels.data();
 copy.dstPitch = row_bytes;
 copy.WidthInBytes = row_bytes;
 copy.Height = plane.descriptor.height;
 REQUIRE(cuMemcpy2D(&copy) == CUDA_SUCCESS);
}
// Hardware may legitimately have no reported locality on a multi-node host.
// Verify rejection before deliberately choosing an eligible node for the test.
inline DeviceExecution selected_test_device(int ordinal, const mmltk::common::system::NumaTopology& topology) {
 try {
  return resolve_device_execution(ordinal, topology);
 } catch (const std::invalid_argument& error) {
  REQUIRE(std::string_view(error.what()).find("unknown") != std::string_view::npos);
  REQUIRE(topology.nodes.size() > 1);
 }
 for (int node : topology.permitted_nodes) {
  if (std::ranges::none_of(topology.cpus, [&](const auto& cpu) { return cpu.node == node && std::ranges::find(topology.permitted_cpus, cpu.cpu) != topology.permitted_cpus.end(); })) continue;
  const auto selected = resolve_device_execution(ordinal, topology, node);
  CHECK(selected.placement.numa_node == node);
  return selected;
 }
 throw std::runtime_error("hardware test has no permitted NUMA node with an eligible CPU");
}
[[nodiscard]] inline mmltk::common::system::ExecutionPolicyRequest selected_test_execution_policy(int ordinal) {
 auto execution = selected_test_device(ordinal, mmltk::common::system::NumaTopology::Capture());
 return {std::move(execution.placement.cpus), {}, 0, execution.placement.numa_node, -10, false};
}
}  // namespace mmltk::frameworks::gpu::test_support
