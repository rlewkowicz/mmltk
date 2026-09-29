#pragma once
#include <cstddef>
#include <memory>
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
// Prepare once, then borrow for serial scopes on any requesting thread. Each
// concurrent scope needs its own storage; applying/restoring performs no allocation.
class PreparedExecutionPolicy final {
public:
 explicit PreparedExecutionPolicy(const ExecutionPolicyRequest&);
 ~PreparedExecutionPolicy();
 PreparedExecutionPolicy(const PreparedExecutionPolicy&) = delete;
 PreparedExecutionPolicy& operator=(const PreparedExecutionPolicy&) = delete;

private:
 friend class ScopedExecutionPolicy;
 friend ExecutionPolicySnapshot apply_worker_execution_policy(const ExecutionPolicyRequest&);
 void Apply();
 void Restore();
 struct Impl;
 std::unique_ptr<Impl> impl_;
};
class ScopedExecutionPolicy final {
public:
 explicit ScopedExecutionPolicy(const ExecutionPolicyRequest&);
 explicit ScopedExecutionPolicy(PreparedExecutionPolicy&);
 ~ScopedExecutionPolicy() noexcept;
 ScopedExecutionPolicy(const ScopedExecutionPolicy&) = delete;
 ScopedExecutionPolicy& operator=(const ScopedExecutionPolicy&) = delete;
 void Restore();

private:
 std::unique_ptr<PreparedExecutionPolicy> owned_;
 PreparedExecutionPolicy* policy_;
};
[[nodiscard]] ExecutionPolicySnapshot apply_process_execution_policy();
[[nodiscard]] ExecutionPolicySnapshot apply_worker_execution_policy(const ExecutionPolicyRequest& request);
[[nodiscard]] ExecutionPolicySnapshot capture_execution_policy_snapshot();
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int clamp_worker_count_to_cpus(int requested_workers, size_t cpu_count, int reserved_cpus = 0, int minimum_workers = 1);
}  // namespace mmltk::common::system
