#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <memory>
#include <atomic>
#include <exception>
#include "src/common/system/execution_policy.h"
#include "src/controller/contracts/compute.h"
#include "src/frameworks/gpu/device_execution.h"
#include "src/frameworks/gpu/image_failure.h"
namespace mmltk::controller {
struct DirectComputeConfiguration final {
 std::optional<mmltk::frameworks::gpu::DeviceExecution> execution{};
 int numa_node = -1;
 bool operator==(const DirectComputeConfiguration&) const = default;
 [[nodiscard]] bool valid() const noexcept { return execution && execution->device >= 0 && execution->placement.numa_node >= 0 && !execution->placement.cpus.empty(); }
 [[nodiscard]] std::optional<mmltk::common::system::ExecutionPolicyRequest> worker_policy() const;
};
[[nodiscard]] DirectComputeConfiguration resolve_compute_configuration(int device, int numa_node);
using DirectComputeResolver = std::function<DirectComputeConfiguration(int device, int numa_node)>;
using ComputeArtifactSink = std::function<void(const std::filesystem::path&)>;
using ComputeProgressSink = std::function<void(const contracts::ComputeProgress&)>;
// Construction can fail after acquiring resources but before returning an
// object. Preserve that terminal custody decision alongside the owning system.
template <class Runtime>
[[nodiscard]] std::unique_ptr<Runtime> construct_compute_runtime(
 const std::function<std::unique_ptr<Runtime>(DirectComputeConfiguration)>& factory, const DirectComputeConfiguration& configuration, std::atomic_bool& unsafe) {
 try {
  return factory(configuration);
 } catch (...) {
  if (mmltk::frameworks::gpu::is_image_execution_failure(std::current_exception())) unsafe = true;
  throw;
 }
}
// Failed work and device replacement share the same physical retirement rule.
// Close must finish settlement, session/stream release and caller restoration.
// False preserves the exact runtime and tells its system to seal admission.
template <class Runtime, class Unsafe = decltype([] { return false; })>
[[nodiscard]] bool retire_compute_runtime(std::unique_ptr<Runtime>& runtime, Unsafe unsafe = {}) noexcept {
 if (!runtime) return true;
 if (runtime->HasUnsafeCustody() || unsafe()) return false;
 try {
  runtime->Close();
 } catch (...) { return false; }
 if (runtime->HasUnsafeCustody() || unsafe()) return false;
 runtime.reset();
 return true;
}
// The synchronous work boundary admits progress and one terminal. Runtime,
// cancellation, domain results and publication remain with the calling system.
[[nodiscard]] contracts::ComputeTerminal run_checked_compute(
 std::function_ref<contracts::ComputeTerminal(const ComputeProgressSink&)> work, std::function_ref<void(const contracts::ComputeProgress&)> progress);
}  // namespace mmltk::controller
