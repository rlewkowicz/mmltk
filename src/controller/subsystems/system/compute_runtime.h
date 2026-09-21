#pragma once
#include <functional>
#include <optional>
#include "src/common/system/execution_policy.h"
#include "src/controller/contracts/compute.h"
#include "src/frameworks/gpu/device_execution.h"
namespace mmltk::controller {
struct DirectComputeConfiguration final {
 std::optional<mmltk::frameworks::gpu::DeviceExecution> execution;
 [[nodiscard]] bool valid() const noexcept {
  return execution && execution->device >= 0 && execution->placement.numa_node >= 0 && !execution->placement.cpus.empty();
 }
 [[nodiscard]] std::optional<mmltk::common::system::ExecutionPolicyRequest> worker_policy() const;
};
using ComputeProgressSink = std::function<void(const contracts::ComputeProgress&)>;
// The synchronous work boundary admits progress and one terminal. Runtime,
// cancellation, domain results and publication remain with the calling system.
[[nodiscard]] contracts::ComputeTerminal run_checked_compute(std::function_ref<contracts::ComputeTerminal(const ComputeProgressSink&)> work,
                                                             std::function_ref<void(const contracts::ComputeProgress&)> progress);
}  // namespace mmltk::controller
