#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "src/common/system/numa_memory.h"
#include "src/common/system/numa_topology.h"
namespace mmltk::common::system {
struct ExecutionPolicyRequest {
 std::vector<int> cpu_affinity;
 std::string thread_name;
 size_t worker_index = 0;
 int numa_node = -1;
 int target_nice = -10;
 bool storage_worker = true;
};
struct ExecutionPolicySnapshot {
 std::vector<int> affinity;
 int online_cpu_count = 0;
 int nice_value = 0;
 int scheduler_policy = 0;
 int scheduler_priority = 0;
 int io_class = 0;
 int io_priority_data = 0;
 int numa_node = -1;
 MemoryPolicy memory_policy;
 std::string thread_name;
};
class ScopedExecutionPolicy final {
public:
 explicit ScopedExecutionPolicy(const ExecutionPolicyRequest&);
 ~ScopedExecutionPolicy() noexcept;
 ScopedExecutionPolicy(const ScopedExecutionPolicy&) = delete;
 ScopedExecutionPolicy& operator=(const ScopedExecutionPolicy&) = delete;
 void Restore();

private:
 ExecutionPolicySnapshot previous_;
 bool active_ = true;
};
[[nodiscard]] ExecutionPolicySnapshot apply_process_execution_policy();
[[nodiscard]] ExecutionPolicySnapshot apply_worker_execution_policy(const ExecutionPolicyRequest& request);
[[nodiscard]] ExecutionPolicySnapshot capture_execution_policy_snapshot();
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int clamp_worker_count_to_cpus(int requested_workers, size_t cpu_count, int reserved_cpus = 0, int minimum_workers = 1);
}  // namespace mmltk::common::system
