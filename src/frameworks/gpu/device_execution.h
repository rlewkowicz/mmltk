#pragma once
#include <array>
#include <cstdint>
#include <string>
#include "src/common/system/numa_topology.h"
namespace mmltk::frameworks::gpu {
struct DeviceExecution final {
 bool operator==(const DeviceExecution&) const = default;
 int device = -1;
 std::string pci_identity{};
 int reported_numa_node = -1;
 mmltk::common::system::ExecutionPlacement placement;
};
[[nodiscard]] DeviceExecution resolve_device_execution(int device, const mmltk::common::system::NumaTopology&, int numa_node = -1,
                                                       const std::string& eligible_cpus = {});
[[nodiscard]] int resolve_device_uuid(const std::array<std::uint8_t, 16U>&);
}  // namespace mmltk::frameworks::gpu
