#include "compute_runtime.h"
#include "src/controller/contracts/application_boundary.h"
#include <atomic>
#include <exception>
#include <stdexcept>
namespace mmltk::controller {
DirectComputeConfiguration resolve_compute_configuration(int device, int numa_node) {
 if (device < 0) throw contracts::UnavailableError("selected CUDA device is unavailable");
 try {
  return {mmltk::frameworks::gpu::resolve_device_execution(device, mmltk::common::system::NumaTopology::Capture(), numa_node), numa_node};
 } catch (const std::exception& error) {
  throw contracts::UnavailableError("selected CUDA GPU " + std::to_string(device) + " is unavailable: " + error.what());
 }
}
std::optional<mmltk::common::system::ExecutionPolicyRequest> DirectComputeConfiguration::worker_policy() const {
 if (!execution) return std::nullopt;
 return mmltk::common::system::ExecutionPolicyRequest{execution->placement.cpus, {}, 0, execution->placement.numa_node, -10, false};
}
contracts::ComputeTerminal run_checked_compute(
 const std::function_ref<contracts::ComputeTerminal(const ComputeProgressSink&)> work, const std::function_ref<void(const contracts::ComputeProgress&)> progress) {
 std::atomic_bool malformed_progress = false;
 try {
  const ComputeProgressSink checked_progress = [&](const contracts::ComputeProgress& value) {
   if (!value.valid()) {
    malformed_progress.store(true, std::memory_order_relaxed);
    return;
   }
   progress(value);
  };
  auto terminal = work(checked_progress);
  if (malformed_progress.load(std::memory_order_relaxed) || !terminal.valid_worker_terminal()) throw std::runtime_error("compute runtime returned an invalid terminal");
  return terminal;
 } catch (...) { return contracts::compute_failure_terminal(std::current_exception(), "compute runtime failed"); }
}
}  // namespace mmltk::controller
