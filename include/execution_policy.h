#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace mmltk {

struct ExecutionPolicyRequest {
    std::vector<int> cpu_affinity;
    std::string thread_name;
    size_t worker_index = 0;
    bool prefer_realtime_scheduler = false;
    bool prefer_realtime_io = true;
};

struct ExecutionPolicySnapshot {
    std::vector<int> affinity;
    int online_cpu_count = 0;
    int nice_value = 0;
    int scheduler_policy = 0;
    int scheduler_priority = 0;
    int io_class = 0;
    int io_priority_data = 0;
};

[[nodiscard]] ExecutionPolicySnapshot apply_process_execution_policy();
[[nodiscard]] ExecutionPolicySnapshot apply_worker_execution_policy(const ExecutionPolicyRequest& request);
[[nodiscard]] ExecutionPolicySnapshot capture_execution_policy_snapshot();

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int clamp_worker_count_to_cpus(int requested_workers, size_t cpu_count, int reserved_cpus = 0, int minimum_workers = 1);
void log_worker_budget_clamp(const char* subsystem, int requested_workers, int applied_workers,
                             const std::vector<int>& cpus, int reserved_cpus = 0, int minimum_workers = 1);
void log_process_execution_policy(const char* process_label, const ExecutionPolicySnapshot& snapshot,
                                  bool expect_realtime_scheduler, bool expect_realtime_io);

}  
